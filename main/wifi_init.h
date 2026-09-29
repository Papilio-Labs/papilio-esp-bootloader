#ifndef WIFI_INIT_H
#define WIFI_INIT_H

#include <stdbool.h>
#include "esp_netif.h"

/**
 * Bring up WiFi in station mode and block (up to ~15s) until connected or
 * the attempt is given up. Safe to call once at boot -- always started,
 * no USB-presence gating (see the "always enable WiFi" decision in
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md).
 *
 * Credentials come from an NVS override (partition "nvs_loader", namespace
 * "wifi_cfg", keys "ssid"/"pass") if present, otherwise the compiled-in
 * Kconfig defaults (LOADER_WIFI_SSID/LOADER_WIFI_PASSWORD). The nvs_loader
 * partition is physically separate from the user app's own "nvs" partition
 * so a user-app factory reset can never wipe the loader's WiFi credentials.
 */
void wifi_init_start(void);

/** True once WiFi STA has an IP address. */
bool wifi_init_is_connected(void);

/** Return the current station IPv4 address, or 0.0.0.0 when disconnected. */
esp_ip4_addr_t wifi_init_get_ip(void);

#endif /* WIFI_INIT_H */
