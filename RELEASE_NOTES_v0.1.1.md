# Papilio ESP Bootloader v0.1.1

This patch release includes the latest factory loader fixes for Papilio ESP32-S3
boards.

## Changes

- Suppress ESP-IDF log output on USB serial and UDP while flash transfers are
  active, while keeping serial progress and completion/error responses available.
- Reset the external FPGA SPI flash before probing it.
- Preserve the existing 4 MB factory/OTA partition layout.
- The loader app occupies 913,872 bytes of the 917,504-byte factory partition,
  leaving 3,632 bytes free.

## Validation

- ESP-IDF 6.0.1 build completed successfully.
- The merged image was written to a Papilio Retrocade and its bootloader HTTP
  endpoints responded after reset.

WiFi credentials are not included in the release build. Configure WiFi through
the board recovery flow.
