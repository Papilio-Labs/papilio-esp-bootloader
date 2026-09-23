#ifndef LOADER_WIFI_LOG_H
#define LOADER_WIFI_LOG_H

/*
 * wifi_log.h - UDP wireless diagnostic log for the Papilio ESP Bootloader
 * (Phase 4 of papilio-works/plans/2026-09-21-papilio-esp-bootloader.md).
 *
 * Forwards all printf()/ESP_LOGx() output to UDP broadcast packets on port
 * 7777, matching FPGA-Companion's wifi_log.c convention -- boot reason,
 * JTAG/OTA progress and errors, and the final handoff message are all
 * visible this way even with no serial monitor attached.
 *
 * On your PC:
 *   Windows:  ncat -u -l 7777
 *   Linux:    nc -u -l -p 7777
 */

/**
 * Installs the write hook + ring buffer so no log output is missed. Call as
 * early as possible in app_main(), before wifi_init_start().
 */
void wifi_log_early_init(void);

/**
 * Opens the UDP broadcast socket and flushes the ring buffer built up since
 * wifi_log_early_init(). Call once WiFi has connected (see wifi_init_start()).
 */
void wifi_log_start(void);

#endif /* LOADER_WIFI_LOG_H */
