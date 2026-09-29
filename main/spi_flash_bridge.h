#ifndef LOADER_SPI_FLASH_BRIDGE_H
#define LOADER_SPI_FLASH_BRIDGE_H

/*
 * spi_flash_bridge.h - driver for the FPGA's own external SPI flash chip,
 * reached over a SEPARATE SPI bus/GPIOs from jtag_gowin.c's bit-bang JTAG
 * pins (Phase 6.5, see papilio-works/plans/2026-09-21-papilio-esp-bootloader.md).
 *
 * Ported from FPGA-Companion/src/esp32/mcu_hw.c's external-flash subset
 * (mcu_hw_spi_init/mcu_hw_reinit_flash/mcu_hw_erase_flash_region/
 * mcu_hw_write_flash/mcu_hw_read_flash/mcu_hw_fpga_reset), trimmed to just
 * the flash-programming path -- the loader has no HID/USB-host/general
 * SPI-protocol code, so the IRQ pin and mcu_hw_spi_begin/end "packet"
 * helpers used by that protocol are not ported.
 *
 * IMPORTANT: this bus only becomes electrically live once the FPGA is
 * running a bitstream that bridges these pins to its external flash chip
 * (either the user's own bitstream, or -- while writing a new one --
 * fpga_bootloader.c's temporary JTAG-SRAM-loaded bridge). Calling any
 * write/erase/read function before that requires spi_flash_bridge_reinit()
 * to have succeeded first.
 *
 * Stable SPI mapping from the January 2026 EMI investigation:
 *   MISO=GPIO4, MOSI=GPIO2, CLK=GPIO1, bridge/flash CS=GPIO3,
 *   RECONFIG_N=GPIO13.
 * The previous GPIO3/11/12/4 mapping placed CLK on FPGA A11 next to reset
 * circuitry and caused instability during sustained transfers.
 */

#include <stdint.h>
#include "esp_err.h"

/**
 * One-time SPI bus + flash-device registration. Does NOT probe the chip
 * (the FPGA SPI bridge is not yet active this early at boot) -- call
 * spi_flash_bridge_reinit() after fpga_bootloader_load_to_sram() has run.
 * Safe to call once at app startup, alongside loader_http_server_start().
 */
void spi_flash_bridge_init(void);

/**
 * Probe and initialize the external flash chip. The FPGA's SPI bridge
 * (temporary JTAG-SRAM bootloader, or the user's own bitstream) must
 * already be running. Retries internally (chip needs a moment to settle
 * after the bridge bitstream starts running) -- see spi_flash_bridge.c.
 */
esp_err_t spi_flash_bridge_reinit(void);

/** Read the JEDEC ID and capacity after the bridge has been initialized. */
esp_err_t spi_flash_bridge_read_id(uint32_t *id, uint32_t *size_bytes);

/** Erase `size` bytes starting at `addr`, yielding between 64 KB chunks so a
 *  large erase doesn't trip the task watchdog. No-op (silently) if the
 *  flash isn't ready -- callers should check spi_flash_bridge_reinit()'s
 *  return value first. */
void spi_flash_bridge_erase_region(uint32_t addr, uint32_t size);

/** Write `size` bytes from `data` to the flash at `addr`. */
void spi_flash_bridge_write(uint32_t addr, const uint8_t *data, uint32_t size);

/** Read `size` bytes from the flash at `addr` into `data`. */
void spi_flash_bridge_read(uint32_t addr, uint8_t *data, uint32_t size);

/**
 * Exclusive access to the flash bus for the duration of an erase/write
 * sequence (same mutex jtag_gowin.c's SPI-bit-bang path never touches --
 * these are different buses). Always pair with spi_flash_bridge_unlock().
 */
void spi_flash_bridge_lock(void);
void spi_flash_bridge_unlock(void);

/**
 * Pulse RECONFIG_N for 100 ms so the FPGA cold-boots and auto-loads
 * whatever bitstream now sits at its flash's boot address -- this is what
 * makes a spi_flash_bridge_write() persistent (vs. jtag_gowin.c's SRAM-only
 * programming). Same pin used by http_server.c's brief 5 ms JTAG-recovery
 * pulse, but held low far longer here to guarantee a full cold-boot.
 */
void spi_flash_bridge_fpga_reset(void);

#endif /* LOADER_SPI_FLASH_BRIDGE_H */
