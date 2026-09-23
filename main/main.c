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
#include "led_strip.h"

#include "wifi_init.h"
#include "http_server.h"
#include "serial_flash.h"
#include "wifi_log.h"

static const char *TAG = "loader-phase1";

/* Same WS2812 LED as FPGA-Companion (wifi_log.c): GPIO48, single pixel. */
#define LOADER_LED_GPIO 48

static void loader_led_set_purple(void)
{
    led_strip_handle_t strip;
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LOADER_LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip) != ESP_OK) {
        return;
    }
    /* Solid purple identifies the bootloader/factory app (vs. FPGA-Companion's green). */
    led_strip_set_pixel(strip, 0, 16, 0, 16);
    led_strip_refresh(strip);
}

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
    loader_http_server_start();
    serial_flash_start();

    while (1) {
        ESP_LOGI(TAG, "alive -- running from '%s' -- wifi=%s", running->label,
                 wifi_init_is_connected() ? "connected" : "disconnected");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
