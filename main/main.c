/*
 * Phase 1 proved the three-slot partition table + otadata-fallback mechanism
 * (see papilio-works/plans/2026-09-21-papilio-esp-bootloader.md, Phase 1).
 *
 * Phase 2 adds just enough to validate JTAG programming standalone in the
 * loader: WiFi station bring-up (wifi_init.c) and a bare-bones HTTP endpoint
 * (http_server.c) wired to the ported jtag_gowin.c driver. FPGA-Companion
 * keeps its own unmodified JTAG/WiFi code throughout this phase -- nothing
 * is removed there until Phase 6.
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
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_reset_reason_t reason = esp_reset_reason();

    loader_led_set_purple();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Papilio ESP Bootloader -- Phase 1 test app");
    ESP_LOGI(TAG, "Running partition: label='%s' subtype=0x%02x offset=0x%06" PRIx32
                  " size=0x%06" PRIx32,
             running->label, running->subtype, running->address, running->size);
    ESP_LOGI(TAG, "Reset reason: %d", reason);
    ESP_LOGI(TAG, "================================================");

    wifi_init_start();
    loader_http_server_start();

    while (1) {
        ESP_LOGI(TAG, "alive -- running from '%s' -- wifi=%s", running->label,
                 wifi_init_is_connected() ? "connected" : "disconnected");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
