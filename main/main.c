/*
 * Phase 1 proved the three-slot partition table + otadata-fallback mechanism
 * (see papilio-works/plans/2026-09-21-papilio-esp-bootloader.md, Phase 1).
 *
 * Phase 2 added WiFi station bring-up (wifi_init.c) and a bare-bones HTTP
 * endpoint (http_server.c) wired to the ported jtag_gowin.c driver, proving
 * standalone JTAG programming. Phase 3 adds POST /update, which flashes an
 * ESP32 app image into whichever ota_0/ota_1 slot isn't currently selected
 * to boot, then reboots into it. Phase 4 adds the USB-serial fallback
 * (serial_flash.c) for both of those, plus a UDP diagnostic log (wifi_log.c)
 * so progress/errors are visible even with WiFi as the only reachable
 * transport. FPGA-Companion keeps its own unmodified JTAG/OTA/WiFi code
 * throughout -- nothing is removed there until Phase 6.
 */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "loader_led.h"
#include "wifi_init.h"
#include "http_server.h"
#include "serial_flash.h"
#include "wifi_log.h"
#include "spi_flash_bridge.h"

static const char *TAG = "loader-phase1";

void app_main(void)
{
    wifi_log_early_init();  /* install the write hook before any other output */

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_reset_reason_t reason = esp_reset_reason();

    loader_led_set_purple();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Papilio ESP Bootloader -- Phase 4 (USB-serial fallback)");
    ESP_LOGI(TAG, "Running partition: label='%s' subtype=0x%02x offset=0x%06" PRIx32
                  " size=0x%06" PRIx32,
             running->label, running->subtype, running->address, running->size);
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    ESP_LOGI(TAG, "================================================");

    wifi_init_start();
    wifi_log_start();
    spi_flash_bridge_init();
    loader_http_server_start();
    serial_flash_start();

    while (1) {
        esp_ip4_addr_t ip_addr = wifi_init_get_ip();
        ESP_LOGI(TAG, "alive -- running from '%s' -- wifi=%s ip=" IPSTR,
                 running->label,
                 wifi_init_is_connected() ? "connected" : "disconnected",
                 IP2STR(&ip_addr));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
