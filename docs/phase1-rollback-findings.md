# Phase 1 — partition table & rollback mechanism findings

Findings from hardware-validating Phase 1 of the Papilio ESP Bootloader plan (3-slot partition
table + `otadata`-based factory/OTA fallback and automatic app-rollback) on a real Papilio
Retrocade board (ESP32-S3, native USB Serial/JTAG, 4MB flash). ESP-IDF v6.0.1.

## Finalized partition table (`partitions_loader.csv`)

App-type partition `Offset` column MUST be left blank — `gen_esp32part.py` enforces 0x10000
alignment for ALL app partitions unconditionally (not just secure boot). Layout (auto-computed,
validated by build + real flash):

```
nvs_loader, data, nvs, 0x9000,  0x3000
otadata,    data, ota, 0xC000,  0x2000
nvs,        data, nvs, 0xE000,  0x6000
factory,    app,  factory, (auto=0x20000),  0xC0000
ota_0,      app,  ota_0,   (auto=0xE0000),  0x190000
ota_1,      app,  ota_1,   (auto=0x270000), 0x190000
```

Exactly fills 4MB (0x400000), zero slack. ota slot size 0x190000 (1.5625MB) is based on measured
FPGA-Companion v1.1.1 binary size (1,522,528 bytes).

## `otadata` format (32 bytes/record, 2 records per 8KB partition, ping-pong wear-leveled)

- Bytes 0-3: `ota_seq` (uint32). Bytes 4-23: seq_label (unused, 0xFF). Bytes 24-27: `ota_state`.
  Bytes 28-31: `crc` = `crc32(pack('<I', ota_seq), 0xFFFFFFFF)` — CRC only covers `ota_seq`.
- `esp_ota_img_states_t`: NEW=0x0, PENDING_VERIFY=0x1, VALID=0x2, INVALID=0x3, ABORTED=0x4,
  UNDEFINED=0xFFFFFFFF.
- `otatool.py`'s `switch_ota_partition` only rewrites seq+crc, NEVER touches `ota_state`. So a
  slot selected via `otatool` is left with whatever state it last had (often UNDEFINED on a
  blank/erased otadata) — this is NOT the same as a real confirmed VALID state that
  `esp_ota_mark_app_valid_cancel_rollback()` would set.

## How the bootloader picks a slot

(`bootloader_support/src/bootloader_common_loader.c` + `bootloader_utility.c`, ESP-IDF 6.0.1)

- `bootloader_common_ota_select_invalid(s)`: true if `seq==UINT32_MAX || state==INVALID ||
  state==ABORTED`.
- `bootloader_common_ota_select_valid(s)`: true only if NOT `select_invalid` AND crc matches.
- Of the two `otadata` records (ping-pong copies, NOT one-per-app-slot — both copies encode the
  SAME logical "which slot is active" selection, kept redundantly for atomic-update safety), the
  bootloader picks the one with the HIGHEST `ota_seq` **among the ones that are `select_valid`**.
  If only one is valid, that one wins regardless of seq. If BOTH are `select_invalid` (e.g. both
  blank/erased, or both ABORTED), the bootloader logs "Defaulting to factory image" and boots
  `factory` unconditionally (the ultimate safety net — requires a `factory` partition to exist).
- On every boot (with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), the bootloader first scans BOTH
  records and flips any record in `PENDING_VERIFY` state to `ABORTED` (this is what "detects a bad
  boot" — the app never called `esp_ota_mark_app_valid_cancel_rollback()` before rebooting or
  crashing). It then re-runs otadata selection using the (now updated) validity of both records.
- **Consequence (the actual rollback mechanism):** true "roll back to the previous slot" behavior
  depends entirely on the OTHER (non-active) otadata record still holding a valid
  (`select_valid==true`) crc/seq from a PRIOR real selection. If that other record was
  erased/blank (`seq==UINT32_MAX`), there is nothing to roll back to, and the bootloader falls
  through to `factory` instead of the "previous" ota slot — even though `factory` was never the
  previously-running app. `esp_ota_set_boot_partition()` marks the new target record as
  `ESP_OTA_IMG_NEW` (not `PENDING_VERIFY` yet — that transition happens at the next boot's
  selection code, right before the app is loaded), leaving the OTHER record untouched — so as
  long as you never manually erase the whole `otadata` partition, the "previous" record naturally
  survives as the rollback target.

## Pitfall: don't erase the whole `otadata` partition to force a rollback test

Erasing `otadata` (`esptool erase-region 0xC000 0x2000`) to force a factory boot for a pivot app
wipes BOTH ping-pong records, destroying the "previously active" record that the rollback
mechanism needs as its fallback target. Symptom observed: pivot (factory) sets target `ota_1`
(`NEW`) → crashy `ota_1` app boots, never confirms, reboots → bootloader marks that record
`ABORTED` → but the OTHER record is also invalid (blank from the erase) → bootloader falls
through to `factory` again, not `ota_0` → the pivot app runs again → infinite `factory`/`ota_1`
loop (confirmed via serial capture, 8 identical cycles). This is NOT a bug in the rollback
mechanism — it's an artifact of destroying the fallback record via manual erase before the test.

## Confirmed-pass test: rollback to previous slot (byte-crafted `otadata`)

Rather than relying on live pivot/crash app code (which requires 2 reboots and can loop if the
fallback record is destroyed), directly craft the 8KB `otadata` partition as raw bytes and write
it with `esptool` — this exercises exactly the bootloader logic in one deterministic step:

- record0 @0xC000: `seq=1` (→ `(1-1)%2=0` → `ota_0`), `state=0xFFFFFFFF` (UNDEFINED),
  `crc=crc32(seq)`.
- record1 @0xE000 (2nd half of partition): `seq=2` (→ `ota_1`), `state=0x1` (PENDING_VERIFY),
  `crc=crc32(seq)`.

This simulates "`ota_1` was booted once via `esp_ota_set_boot_partition`, went `PENDING_VERIFY`,
and the app rebooted without confirming" while `ota_0`'s older record is still intact as
fallback.

**Result after reset** (serial capture): the board booted DIRECTLY into `ota_0` — the literal
"FPGA Companion for ESP32-S2/S3" ASCII banner appeared — with NO "Defaulting to factory image"
and no `ota_1`/crashy boot at all. Reading back `otadata` afterward (`esptool read-flash 0xC000
0x2000` + manual struct unpack) showed record1 (`ota_1`) had been rewritten by the bootloader
from `state=0x1` (PENDING_VERIFY) to `state=0x4` (ABORTED), confirming the exact internal
state transition documented in `bootloader_utility.c`. This is definitive, byte-level proof of
the Phase 1 "automatic rollback to previous slot" checklist item — **PASSED**.

Python snippet used to craft the fixture (reusable for future regression tests — see
`otadata_rollback_test.bin` / `otadata_after_rollback.bin` in the repo root):

```python
import struct, binascii
def record(seq, state):
    crc = binascii.crc32(struct.pack('<I', seq), 0xFFFFFFFF) & 0xFFFFFFFF
    return struct.pack('<I', seq) + b'\xff'*20 + struct.pack('<I', state) + struct.pack('<I', crc)
rec0 = record(1, 0xFFFFFFFF)   # -> ota_0, UNDEFINED (valid fallback)
rec1 = record(2, 0x1)          # -> ota_1, PENDING_VERIFY (never confirmed)
with open('otadata_rollback_test.bin', 'wb') as f:
    f.write(rec0.ljust(4096, b'\xff') + rec1.ljust(4096, b'\xff'))
# flash with: esptool --port COM10 --chip esp32s3 -b 460800 --before usb-reset
#             --after watchdog-reset write-flash 0xC000 otadata_rollback_test.bin
```

This craft-the-bytes-directly approach is much more reliable than live pivot-app testing for
future regression checks of the rollback mechanism — no multi-reboot timing, no risk of
destroying the fallback record, and it directly asserts the bootloader's own state-transition
behavior.

## LED convention (`main/main.c`)

Added `espressif/led_strip` component dependency (`main/idf_component.yml`) and
`loader_led_set_purple()` in `main.c`'s `app_main()`, driving the GPIO48 WS2812 solid purple
(16, 0, 16) to visually distinguish the bootloader/factory app from FPGA-Companion's green
(same LED/GPIO/component `FPGA-Companion`'s `wifi_log.c` uses for its own status indication).
Requires `idf.py reconfigure` after adding `idf_component.yml` — the component manager doesn't
refetch on a plain `idf.py build` if the manifest was added after the first cmake configure.

## Board state as left at end of this test session

- `factory` (0x20000): blinky test app with solid-purple WS2812 LED indicator.
- `ota_0` (0xE0000): FPGA-Companion v1.1.1, untouched during the corrected test.
- `ota_1` (0x270000): still contains an old "crashy" test binary from an earlier flawed test
  (harmless — `otadata` doesn't point there).
- `otadata`: the crafted test fixture (`seq=1`/UNDEFINED → `ota_0`, `seq=2`/ABORTED → `ota_1`) —
  board boots `ota_0` (FPGA-Companion) by default.
