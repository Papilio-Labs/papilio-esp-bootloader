#ifndef LOADER_FPGA_BOOTLOADER_H
#define LOADER_FPGA_BOOTLOADER_H

/*
 * fpga_bootloader.h - loads the embedded Papilio SPI bridge bitstream
 * (a temporary SPI-flash-bridge, see main/bootloader_data.h) into FPGA SRAM
 * via JTAG, so the external SPI flash chip's pins are bridged through to
 * the ESP32 and spi_flash_bridge.c can reach it (Phase 6.5, see
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md).
 *
 * Ported from FPGA-Companion's ota_server.c
 * (ota_server_load_bootloader_to_sram()), simplified to reuse this repo's
 * existing loader_jtag_lock/unlock/ensure_init/program_sram_begin_with_retry
 * helpers (http_server.h) instead of re-implementing the JTAG mutex and
 * RECONFIG_N-pulse retry loop a second time.
 */

#include "esp_err.h"

/**
 * Decompress and stream the embedded bootloader bitstream to FPGA SRAM via
 * JTAG. Caller must NOT hold the JTAG lock -- this function acquires it
 * itself (loader_jtag_lock()) for the duration of the load and releases it
 * before returning, success or failure.
 */
esp_err_t fpga_bootloader_load_to_sram(void);

#endif /* LOADER_FPGA_BOOTLOADER_H */
