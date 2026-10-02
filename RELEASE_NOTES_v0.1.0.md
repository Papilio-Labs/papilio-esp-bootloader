# Papilio ESP Bootloader v0.1.0

This is the first tagged release of the always-resident Papilio ESP32-S3
loader. It owns USB serial programming, OTA updates, FPGA programming, WiFi
provisioning, and recovery for a board's user application slots.

## Flash layout

The release targets 4 MB flash boards using `partitions_loader.csv`:

- `factory` at `0x20000`: the loader itself
- `ota_0` at `0x100000`: the active user application
- `ota_1` at `0x280000`: the rollback user application slot

The loader is `898,448` bytes (`0xdb590`) and leaves approximately 2% free in
the `factory` partition. The companion application must fit within the
`0x180000` OTA slot.

## Migration

Existing pre-Phase-6 boards must be migrated once by USB using the merged
image published with `FPGA-Companion v2.0.0`. Do not update the loader through
the normal application OTA path: the `factory` partition has no A/B fallback.

## Validation

The ESP-IDF 6.0.1 build passes from the tagged source tree. Hardware
regression coverage for USB recovery, interrupted writes, and migrated
A2600/C64/NES boards remains a release follow-up when the target hardware is
available.