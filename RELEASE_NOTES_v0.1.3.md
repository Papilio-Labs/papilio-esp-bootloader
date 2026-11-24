# Papilio ESP Bootloader v0.1.3

Fixes USB FPGA uploads reporting success while leaving an unbootable core.

## Changes

- Round persistent FPGA erases up to complete 4 KB sectors. A 907,418-byte
  bitstream now erases 909,312 bytes, including its final partial sector.
- Validate erase alignment and capacity, and propagate erase/write failures.
  USB and HTTP uploads stop on failure instead of acknowledging success.
- Preserve the existing 4 MB factory/OTA partition layout and SPI bridge.

## Validation

- ESP-IDF 6.0.1 release build with version v0.1.3 and no WiFi credentials.
- Patched firmware tested on a Papilio Retrocade through installed Papilio
  Loader v0.5.1: USB A2600-to-C64 and WiFi C64-to-A2600 updates completed,
  Companion v2.2.2 booted, and HDMI output was confirmed for both cores.
- One WiFi attempt timed out during loader identification before uploading;
  a successful status probe and retry completed the update.
- FPGA completion is protocol/HTTP acknowledgement, not full-image readback.
  Injected flash errors and interrupted-upload recovery were not hardware tested.

## Installation

The versioned application binary is the resident **factory loader**, not a
normal user application. On an existing compatible 4 MB loader layout it
belongs at `0x20000`; do not upload it into a user OTA slot. Back up the factory
partition before updating it. Do not erase the whole ESP flash or rewrite the
partition table/OTA metadata merely to apply this patch.

The ZIP also contains the ESP second-stage bootloader, partition table, and
initial OTA metadata for provisioning workflows. These are not required for
an existing-board factory-loader-only update.

WiFi credentials are not included. Existing stored settings are preserved by
a factory-loader-only update.
