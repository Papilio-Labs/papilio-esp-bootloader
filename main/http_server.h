#ifndef LOADER_HTTP_SERVER_H
#define LOADER_HTTP_SERVER_H

/**
 * Start the loader's bare-bones HTTP server (Phase 2 -- JTAG validation
 * only). Call after WiFi is connected. Endpoint shape is not final; see
 * Phase 3/4 of papilio-works/plans/2026-09-21-papilio-esp-bootloader.md
 * for the inactive-slot app flashing and USB-serial fallback that extend
 * this later.
 */
void loader_http_server_start(void);

#endif /* LOADER_HTTP_SERVER_H */
