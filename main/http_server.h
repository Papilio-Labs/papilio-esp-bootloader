#ifndef LOADER_HTTP_SERVER_H
#define LOADER_HTTP_SERVER_H

/**
 * Start the loader's bare-bones HTTP server: JTAG SRAM programming
 * (Phase 2) and inactive-slot ESP32 app flashing (Phase 3). Call after
 * WiFi is connected. Endpoint shape is not final; see Phase 4 of
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md for the
 * USB-serial fallback that extends this later.
 */
void loader_http_server_start(void);

#endif /* LOADER_HTTP_SERVER_H */
