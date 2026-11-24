# Phase 6.5 - standalone SPI bridge hardware validation

Validation record for replacing the legacy `papilio_tang_bootloader` FPGA
project with the standalone public repository:

`https://github.com/Papilio-Labs/papilio_spi_bridge`

## Build and integration

- Built the standalone bridge with Gowin EDA `gw_sh`.
- Generated bitstream: `impl/pnr/project.bin`.
- Bitstream size: 577,178 bytes.
- Updated `generate_bootloader_header.py` to use the sibling
  `../papilio_spi_bridge/impl/pnr/project.bin` path.
- Regenerated `main/bootloader_data.h` from the merged `main` branch using
  raw deflate compression.
- Compressed payload size: 9,295 bytes.
- ESP-IDF build completed successfully with ESP-IDF v6.0.1.

## Hardware upload

Validated on a Papilio Retrocade ESP32-S3 using native USB Serial/JTAG on
`COM10`:

```text
python -m esptool --port COM10 --chip esp32s3 \
  --before usb-reset --after watchdog-reset write-flash @flash_args
```

All bootloader, partition-table, OTA-data, and application writes completed
with hash verification. The board returned to the running application after
the watchdog reset.

## FPGA flash and live reconfiguration

The updated firmware accepted both tested 577,178-byte FPGA images through
`POST /fpga-update` at `http://10.0.4.88:3232`:

- `orange_led.bin`: HTTP 200, successful reconfiguration, orange LED observed.
- `green_led.bin`: HTTP 200, successful reconfiguration, green LED observed.

Both color changes were visible on the physical board without a power cycle.
This confirms the standalone bridge, embedded bitstream, ESP32 firmware, SPI
flash write path, and runtime FPGA reconfiguration path work together.

On September 29, 2026, the loader was restored over native USB Serial/JTAG on
`COM10` and `green_led.bin` was written through the same endpoint. DHCP assigned
the loader `10.0.4.100` for this run and the request returned HTTP 200. After a
physical power removal and reapplication, the green FPGA image returned without
another loader update, confirming persistent FPGA SPI-flash programming across
a real cold boot. USB-serial persistent flashing and interrupted-write recovery
remain separate validation items.

## RGB blink integration

The bridge `experiment/purple-rgb-blink` branch was tested in SRAM and then
merged into `main`. The WS2812B output on `P9` now shows dim purple for one
second and turns off for one second. The ESP bootloader embeds this merged
bridge image so the behavior is also available whenever it preloads the bridge
for an FPGA flash update.

The integrated bridge was loaded through `POST /fpga-jtag-sram`, persisted
through `POST /fpga-update`, and continued to report JEDEC `0x0b4017` with an
8 MiB flash after reconfiguration.

## USB erase alignment regression (November 23, 2026)

A 907,418-byte A2600 RetroCade image was acknowledged over USB, but Companion
reported "FPGA not ready after 5 seconds!" and HDMI remained blank across
power cycles. Writing the identical image over HTTP restored HDMI.

The USB path passed the raw image length to the erase helper, leaving its
final erase chunk unaligned to 4 KB. Erase and write errors were discarded.
The helper now rounds the erase length up to a sector boundary (909,312 bytes
for this image), checks capacity, and returns failures. Both USB and HTTP
callers stop on erase/write errors without acknowledging success.

Validation:

- ESP-IDF 6.0.1 build passed and the image fits the factory partition.
- Two full reads of the original factory partition produced identical SHA-256
  hashes before updating only that partition. Esptool verified the new image's
  write hash; the partition table, settings, and Companion app were not flashed.
- The installed Papilio Loader v0.5.1 completed the same A2600 upload over USB
  using the patched factory loader, without resetting OTA metadata.
- Companion boot was observed, and the user confirmed A2600 HDMI was working.

Follow-up cross-core checks with the patched loader and installed Papilio
Loader v0.5.1:

- USB: replaced A2600 with the 907,418-byte C64 RetroCade image; completion
  was acknowledged, Companion boot was observed, and the user confirmed C64 HDMI.
- Wi-Fi: replaced C64 with the 907,418-byte A2600 image; the HTTP success
  response and Companion boot were observed, and the user confirmed A2600 HDMI.
  The first network attempt timed out during loader identification before any
  upload began. After a successful loader status probe, the retry completed.
- Loader entry used Companion's network handoff; neither test reset OTA
  metadata. The board was left running A2600 with Companion v2.2.2.

The FPGA upload was protocol-acknowledged, not verified by full-image readback.
Injected erase/write failures and interrupted-upload recovery were not tested
on hardware.