#ifndef LOADER_HTTP_SERVER_H
#define LOADER_HTTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"

/**
 * Start the loader's bare-bones HTTP server: JTAG SRAM programming
 * (Phase 2) and inactive-slot ESP32 app flashing (Phase 3). Call after
 * WiFi is connected. Endpoint shape is not final; see Phase 4 of
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md for the
 * USB-serial fallback that extends this later.
 */
void loader_http_server_start(void);

/*
 * Shared JTAG access, so the HTTP (/fpga-jtag-sram) and USB-serial
 * (FPGA_FLASH_BEGIN sram, serial_flash.c) transports serialize through the
 * same mutex/init-once state and never race for the JTAG pins (Phase 4).
 */
bool loader_jtag_lock(TickType_t timeout_ticks);
void loader_jtag_unlock(void);
esp_err_t loader_jtag_ensure_init(void);

/*
 * jtag_gowin_program_sram_begin() wrapped with the RECONFIG_N pulse + 3x
 * retry loop proven in Phase 2 -- shared so serial_flash.c doesn't
 * reimplement it (Phase 4).
 */
esp_err_t loader_jtag_program_sram_begin_with_retry(uint32_t *idcode_out);

/*
 * Shared inactive-slot selection + bookkeeping (Phase 3/4), so HTTP's
 * /update and serial's APP_FLASH_BEGIN always agree on which ota_0/ota_1
 * slot is next and both persist the choice across otadata-erase recovery
 * cycles.
 */
const esp_partition_t *loader_get_target_update_partition(void);
void loader_save_last_slot(uint8_t slot_idx);

#endif /* LOADER_HTTP_SERVER_H */
