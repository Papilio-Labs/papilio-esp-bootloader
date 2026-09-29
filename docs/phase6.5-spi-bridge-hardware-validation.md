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
- Regenerated `main/bootloader_data.h` using raw deflate compression.
- Compressed payload size: 5,197 bytes.
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