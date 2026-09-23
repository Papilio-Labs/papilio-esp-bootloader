#ifndef LOADER_SERIAL_FLASH_H
#define LOADER_SERIAL_FLASH_H

/*
 * serial_flash.h - USB-serial fallback for FPGA JTAG SRAM programming and
 * ESP32 app inactive-slot flashing (Phase 4 of
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md).
 *
 * Ported from FPGA-Companion/src/esp32/serial_flash.c, trimmed to what the
 * loader actually supports today: JTAG SRAM programming (no persistent
 * SPI-flash target -- the loader has no mcu_hw SPI bridge ported, unlike
 * FPGA-Companion) plus a new APP_FLASH_BEGIN command mirroring POST
 * /update's inactive-slot semantics. Shares the JTAG mutex/init-once state
 * and slot-selection logic with http_server.c (see http_server.h) so both
 * transports never race or disagree.
 *
 * Protocol (over the USB Serial/JTAG console, same channel as boot logs):
 *
 *   Host -> Device:  FPGA_FLASH_BEGIN sram <size>\n
 *   Device -> Host:  READY\r\n                  (or FPGA_FLASH_ERROR <reason>\r\n)
 *   Host -> Device:  <raw bitstream bytes, exactly <size> of them>
 *   Device -> Host:  PROGRESS <bytes>\r\n        (periodically)
 *   Device -> Host:  FPGA_FLASH_OK\r\n            (or FPGA_FLASH_ERROR <reason>\r\n)
 *
 *   Host -> Device:  APP_FLASH_BEGIN <size>\n
 *   Device -> Host:  READY\r\n                  (or APP_FLASH_ERROR <reason>\r\n)
 *   Host -> Device:  <raw ESP32 app image bytes, exactly <size> of them>
 *   Device -> Host:  PROGRESS <bytes>\r\n        (periodically)
 *   Device -> Host:  APP_FLASH_OK\r\n             (or APP_FLASH_ERROR <reason>\r\n)
 *                     then the board reboots into the newly flashed slot.
 */

/**
 * Start the background console task that reads lines from stdin (the USB
 * Serial/JTAG console) and dispatches FPGA_FLASH_BEGIN / APP_FLASH_BEGIN
 * commands. Safe to call once at boot, alongside loader_http_server_start().
 */
void serial_flash_start(void);

#endif /* LOADER_SERIAL_FLASH_H */
