/*
 * wifi_init.c - Minimal WiFi station bring-up for the Papilio ESP Bootloader
 *
 * Phase 2 (see papilio-works/plans/2026-09-21-papilio-esp-bootloader.md):
 * just enough WiFi to reach the loader's bare-bones JTAG HTTP endpoint.
 * The UDP diagnostic log channel (port 7777, matching FPGA-Companion's
 * wifi_log.c) is added in Phase 4 alongside the USB-serial flashing path --
 * intentionally not duplicated here yet.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_init.h"

static const char *TAG = "wifi_init";

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_MAX_RETRIES        5

/* Isolated from the user app's own "nvs" partition -- see partitions_loader.csv
 * and the NVS-isolation decision in the plan doc. */
#define LOADER_NVS_PARTITION "nvs_loader"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;
static volatile bool s_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA_START -- connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        ESP_LOGW(TAG, "STA_DISCONNECTED reason=%d", disc ? disc->reason : -1);
        if (s_retry_num < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)...", s_retry_num, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRIES);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Prefers the loader's own isolated NVS partition over compiled-in Kconfig defaults. */
static void load_credentials(wifi_config_t *wifi_cfg)
{
    memset(wifi_cfg, 0, sizeof(*wifi_cfg));
    strncpy((char *)wifi_cfg->sta.ssid, CONFIG_LOADER_WIFI_SSID, sizeof(wifi_cfg->sta.ssid) - 1);
    strncpy((char *)wifi_cfg->sta.password, CONFIG_LOADER_WIFI_PASSWORD, sizeof(wifi_cfg->sta.password) - 1);
    wifi_cfg->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    nvs_handle_t nvs;
    if (nvs_open_from_partition(LOADER_NVS_PARTITION, "wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;  /* no override present -- stick with the compiled-in defaults */
    }

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    if (nvs_get_str(nvs, "ssid", ssid, &len) == ESP_OK && ssid[0] != '\0') {
        memset(wifi_cfg->sta.ssid, 0, sizeof(wifi_cfg->sta.ssid));
        strncpy((char *)wifi_cfg->sta.ssid, ssid, sizeof(wifi_cfg->sta.ssid) - 1);
        ESP_LOGI(TAG, "Using SSID from nvs_loader override");
    }

    char password[65] = {0};
    len = sizeof(password);
    if (nvs_get_str(nvs, "pass", password, &len) == ESP_OK) {
        memset(wifi_cfg->sta.password, 0, sizeof(wifi_cfg->sta.password));
        strncpy((char *)wifi_cfg->sta.password, password, sizeof(wifi_cfg->sta.password) - 1);
    }

    nvs_close(nvs);
}

void wifi_init_start(void)
{
    esp_err_t err = nvs_flash_init_partition(LOADER_NVS_PARTITION);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(LOADER_NVS_PARTITION);
        err = nvs_flash_init_partition(LOADER_NVS_PARTITION);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init_partition(%s) failed: %s", LOADER_NVS_PARTITION, esp_err_to_name(err));
    }

    s_wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* The WiFi driver's own PHY/calibration storage defaults to the "nvs"
     * partition -- but that partition is reserved for the user app (see the
     * NVS-isolation decision in the plan doc), and the loader never calls
     * nvs_flash_init() on it. Without this, esp_wifi_init() fails internally
     * (osi_nvs_open fail ret=ESP_ERR_NVS_PART_NOT_FOUND) and every connection
     * attempt silently no-ops. The loader doesn't need persisted calibration
     * data, so just disable it. */
    cfg.nvs_enable = false;
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                         &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_cfg;
    load_credentials(&wifi_cfg);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    ESP_LOGI(TAG, "Connecting to SSID '%s'...", (char *)wifi_cfg.sta.ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi did not connect within %d ms -- HTTP endpoint unreachable until it does",
                 WIFI_CONNECT_TIMEOUT_MS);
    }
}

bool wifi_init_is_connected(void)
{
    return s_connected;
}
