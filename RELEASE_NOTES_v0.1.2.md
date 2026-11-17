# Papilio ESP Bootloader v0.1.2

This patch release fixes the Retrocade RGB LED status shown while the loader
programs the FPGA SPI flash.

## Changes

- Embed the updated Papilio SPI bridge (`papilio_spi_bridge` `7c17284`). Its
  WS2812 driver now holds the data line low for 300 us between frames. The
  Retrocade's WS2812B-2020 needs at least 280 us to latch a frame, so with the
  previous 50 us gap the LED only showed the first frame: the purple
  programming blink appeared as solid purple.
- Hold the LED-clear sideband (GPIO9) low for 20 ms before reconfiguring the
  FPGA. The previous 5 ms delay rounded down to zero ticks at the loader's
  100 Hz tick rate, so the LED stayed purple after programming.
- Preserve the existing 4 MB factory/OTA partition layout.

## Validation

- ESP-IDF 6.0.1 release build; WiFi credentials are not included.
- On a Papilio Retrocade, flashing the A2600 core through `/fpga-update`
  blinked the RGB LED purple while programming, turned it off when
  programming finished, and the core then set its own color.

WiFi credentials are not included in the release build. Configure WiFi through
the board recovery flow.
