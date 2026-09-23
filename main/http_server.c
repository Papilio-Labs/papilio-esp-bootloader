/*
 * http_server.c - Bare-bones HTTP endpoints for the loader
 *
 * POST /fpga-jtag-sram streams a raw Gowin .bin bitstream straight into
 * jtag_gowin_program_sram_*() -- same wire format and endpoint name as
 * FPGA-Companion's ota_server.c (see its JTAG_PROGRAMMING.md), kept
 * identical here only to prove the loader can drive JTAG independently
 * (Phase 2).
 *
 * POST /update streams an ESP32 app image into whichever ota_0/ota_1 slot
 * is NOT currently selected to boot, then reboots into it (Phase 3). Not
 * the final endpoint shape -- Phase 4 (USB-serial fallback) extends this.
 * See papilio-works/plans/2026-09-21-papilio-esp-bootloader.md.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "nvs.h"

#include "jtag_gowin.h"
#include "http_server.h"

static const char *TAG = "http_server";

/* Same isolated partition wifi_init.c uses for WiFi creds -- survives a
 * Tier-1 otadata erase, unlike otadata itself. */
#define LOADER_NVS_PARTITION "nvs_loader"
#define LOADER_NVS_NAMESPACE "loader"
#define LOADER_NVS_KEY_LAST_SLOT "last_slot"

#define RECV_CHUNK_SIZE 4096
#define LOADER_HTTP_PORT 3232  /* matches FPGA-Companion's ota_server port, by convention */

/* Papilio Retrocade/Arcade "FPGA Reconfig" pin -- see FPGA-Companion's
 * mcu_hw.c PIN_NUM_RECONFIG_N / mcu_hw_fpga_reset_brief(). A brief low pulse
 * aborts any in-progress SPI-flash auto-config and tri-states the FPGA's
 * user I/O so the JTAG TAP can respond -- without this, jtag_gowin_program_
 * sram_begin()'s IDCODE read can fail while the config FSM is mid-auto-load. */
#define PIN_NUM_RECONFIG_N 13

static void fpga_reconfig_pulse(void)
{
    gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RECONFIG_N, 0);
    esp_rom_delay_us(5000);
    gpio_set_level(PIN_NUM_RECONFIG_N, 1);
    gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_INPUT);
}

static SemaphoreHandle_t s_jtag_mutex = NULL;
static bool s_jtag_initialized = false;

static SemaphoreHandle_t ensure_jtag_mutex(void)
{
    if (s_jtag_mutex == NULL) {
        s_jtag_mutex = xSemaphoreCreateMutex();
    }
    return s_jtag_mutex;
}

bool loader_jtag_lock(TickType_t timeout_ticks)
{
    return xSemaphoreTake(ensure_jtag_mutex(), timeout_ticks) == pdTRUE;
}

void loader_jtag_unlock(void)
{
    xSemaphoreGive(s_jtag_mutex);
}

esp_err_t loader_jtag_ensure_init(void)
{
    if (!s_jtag_initialized) {
        if (jtag_gowin_init(NULL) != ESP_OK) {
            return ESP_FAIL;
        }
        s_jtag_initialized = true;
    }
    return ESP_OK;
}

/* JTAG SRAM programming can lose a race against the FPGA's own SPI-flash
 * auto-config FSM on the first attempt (mirrors FPGA-Companion's
 * ota_server.c retry loop) -- pulse RECONFIG_N and retry up to 3x with
 * increasing settle time before giving up. Shared by HTTP and USB-serial
 * (Phase 4) so neither reimplements the retry loop. */
esp_err_t loader_jtag_program_sram_begin_with_retry(uint32_t *idcode_out)
{
    const int MAX_JTAG_BEGIN_ATTEMPTS = 3;
    esp_err_t begin_err = ESP_FAIL;
    for (int attempt = 1; attempt <= MAX_JTAG_BEGIN_ATTEMPTS; attempt++) {
        fpga_reconfig_pulse();
        esp_rom_delay_us(2000 + attempt * 1000);  /* 3 ms, 4 ms, 5 ms */

        begin_err = jtag_gowin_program_sram_begin(idcode_out);
        if (begin_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "JTAG sram_begin attempt %d/%d failed: %s",
                 attempt, MAX_JTAG_BEGIN_ATTEMPTS, esp_err_to_name(begin_err));
    }
    return begin_err;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req,
        "Papilio ESP Bootloader\n"
        "POST /fpga-jtag-sram -- program FPGA SRAM via JTAG (.bin bitstream)\n"
        "POST /update -- flash ESP32 app into inactive ota_0/ota_1 slot and boot it\n"
        "GET  /update-target -- show which slot the next /update would target\n"
        "POST /resume -- boot into the already-flashed user app, no re-upload needed\n");
    return ESP_OK;
}

static bool load_last_slot(uint8_t *out_slot_idx)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition(LOADER_NVS_PARTITION, LOADER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u8(nvs, LOADER_NVS_KEY_LAST_SLOT, out_slot_idx);
    nvs_close(nvs);
    return err == ESP_OK;
}

static void save_last_slot(uint8_t slot_idx)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition(LOADER_NVS_PARTITION, LOADER_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "could not open %s NVS to persist last_slot", LOADER_NVS_PARTITION);
        return;
    }
    nvs_set_u8(nvs, LOADER_NVS_KEY_LAST_SLOT, slot_idx);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* The loader always *runs* from `factory`, never from an ota_x slot, so
 * esp_ota_get_next_update_partition(NULL) (which defaults to "next after the
 * running partition") would always resolve to the same first ota slot and
 * never toggle. Prefer esp_ota_get_boot_partition() (otadata's selection)
 * when it's still valid -- authoritative within the current otadata
 * lifetime. otadata goes blank/invalid after a Tier-1 factory-recovery
 * erase (esp_ota_set_boot_partition(factory) always wipes it, no way around
 * that in stock ESP-IDF), which would otherwise reset target selection back
 * to ota_0 every time and never really alternate across recovery cycles --
 * so fall back to the last-flashed slot persisted in nvs_loader (survives
 * the erase) and target its opposite. Never returns `factory` -- it's not
 * an OTA subtype. */
const esp_partition_t *loader_get_target_update_partition(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot && boot->type == ESP_PARTITION_TYPE_APP &&
        boot->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        boot->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        return esp_ota_get_next_update_partition(boot);
    }

    uint8_t last_slot;
    if (load_last_slot(&last_slot)) {
        esp_partition_subtype_t other = (last_slot == 0) ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                                           : ESP_PARTITION_SUBTYPE_APP_OTA_0;
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, other, NULL);
        if (p) {
            return p;
        }
    }

    return esp_ota_get_next_update_partition(NULL); /* fresh board, no memory anywhere -- default ota_0 */
}

void loader_save_last_slot(uint8_t slot_idx)
{
    save_last_slot(slot_idx);
}

/* Phase 5: the slot loader_get_target_update_partition() would write next is
 * always the *inactive* one -- the resume slot is the other one (whichever
 * was actually last booted/flashed). Doesn't validate the image here; the
 * caller checks for a plausible app magic byte before committing to it. */
const esp_partition_t *loader_get_resume_partition(void)
{
    const esp_partition_t *next_target = loader_get_target_update_partition();
    if (!next_target) {
        return NULL;
    }
    esp_partition_subtype_t other = (next_target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
                                        ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                        : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, other, NULL);
}

static esp_err_t handle_update_target(httpd_req_t *req)
{
    const esp_partition_t *target = loader_get_target_update_partition();
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "next /update target: %s (offset=0x%06" PRIx32 ")\n",
                      target ? target->label : "(none)", target ? target->address : 0);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

static esp_err_t handle_update(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    const esp_partition_t *target = loader_get_target_update_partition();
    if (!target) {
        ESP_LOGE(TAG, "No OTA update partition found -- check partition table");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition. Flash with partitions_loader.csv.");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "App update started: %d bytes -> partition '%s'", req->content_len, target->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(RECV_CHUNK_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written = 0;
    bool hdr_ok = false;

    while (remaining > 0) {
        int to_recv = (remaining < RECV_CHUNK_SIZE) ? remaining : RECV_CHUNK_SIZE;
        int recv = httpd_req_recv(req, buf, to_recv);

        if (recv == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        /* Validate image magic on first chunk, same check FPGA-Companion's ota_server.c uses. */
        if (!hdr_ok) {
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid image magic 0x%02x (expected 0xE9)", (uint8_t)buf[0]);
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not a valid ESP32 firmware binary");
                return ESP_FAIL;
            }
            hdr_ok = true;
        }

        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }

        remaining -= recv;
        written += recv;
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Image validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }
    loader_save_last_slot((uint8_t)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0));

    ESP_LOGI(TAG, "App update complete (%d bytes) -> '%s'. Rebooting in 1 s ...", written, target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "App update successful. Rebooting into new slot...\r\n");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

/* Return to the already-flashed user app without re-uploading it -- e.g.
 * after a Tier-1 otadata-erase recovery, or after only flashing an FPGA
 * bitstream via /fpga-jtag-sram and never touching the ESP32 app slots. */
static esp_err_t handle_resume(httpd_req_t *req)
{
    const esp_partition_t *target = loader_get_resume_partition();
    if (!target) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No resume target found");
        return ESP_FAIL;
    }

    uint8_t magic = 0;
    esp_err_t err = esp_partition_read(target, 0, &magic, 1);
    if (err != ESP_OK || magic != 0xE9) {
        ESP_LOGW(TAG, "Resume target '%s' has no valid app image (magic=0x%02x)", target->label, magic);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Resume target has no valid app image");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }
    loader_save_last_slot((uint8_t)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0));

    ESP_LOGI(TAG, "Resuming user app '%s'. Rebooting in 1 s ...", target->label);
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "Resuming '%s'. Rebooting...\r\n", target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, n);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK; /* unreachable */
}

static esp_err_t handle_fpga_jtag_sram(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (!loader_jtag_lock(0)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG programming already in progress");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming started: %d bytes", req->content_len);

    if (loader_jtag_ensure_init() != ESP_OK) {
        ESP_LOGE(TAG, "JTAG initialization failed");
        loader_jtag_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG init failed");
        return ESP_FAIL;
    }

    uint32_t idcode = 0;
    esp_err_t begin_err = loader_jtag_program_sram_begin_with_retry(&idcode);
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin JTAG programming (FPGA not detected?)");
        loader_jtag_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "FPGA not detected via JTAG");
        return ESP_FAIL;
    }

    char *chunk_buf = malloc(RECV_CHUNK_SIZE);
    if (!chunk_buf) {
        loader_jtag_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received = 0;
    bool error = false;

    while (remaining > 0 && !error) {
        int to_recv = (remaining < RECV_CHUNK_SIZE) ? remaining : RECV_CHUNK_SIZE;
        int recv = httpd_req_recv(req, chunk_buf, to_recv);

        if (recv == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (recv <= 0) {
            ESP_LOGE(TAG, "Receive error (%d)", recv);
            error = true;
            break;
        }

        if (jtag_gowin_program_sram_write((uint8_t *)chunk_buf, recv, true) != ESP_OK) {
            ESP_LOGE(TAG, "JTAG write failed");
            error = true;
            break;
        }

        remaining -= recv;
        received += recv;
    }

    free(chunk_buf);

    if (error) {
        loader_jtag_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    if (jtag_gowin_program_sram_end() != ESP_OK) {
        ESP_LOGE(TAG, "JTAG programming end failed");
        loader_jtag_unlock();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming complete! (%d bytes)", received);
    loader_jtag_unlock();

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "FPGA programmed successfully via JTAG!\n");
    return ESP_OK;
}

void loader_http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = LOADER_HTTP_PORT;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/", .method = HTTP_GET, .handler = handle_status,
    };
    static const httpd_uri_t fpga_jtag_sram_uri = {
        .uri = "/fpga-jtag-sram", .method = HTTP_POST, .handler = handle_fpga_jtag_sram,
    };
    static const httpd_uri_t update_uri = {
        .uri = "/update", .method = HTTP_POST, .handler = handle_update,
    };
    static const httpd_uri_t update_target_uri = {
        .uri = "/update-target", .method = HTTP_GET, .handler = handle_update_target,
    };
    static const httpd_uri_t resume_uri = {
        .uri = "/resume", .method = HTTP_POST, .handler = handle_resume,
    };

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &fpga_jtag_sram_uri);
    httpd_register_uri_handler(server, &update_uri);
    httpd_register_uri_handler(server, &update_target_uri);
    httpd_register_uri_handler(server, &resume_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", LOADER_HTTP_PORT);
    ESP_LOGI(TAG, "  curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @bitstream.bin", LOADER_HTTP_PORT);
    ESP_LOGI(TAG, "  curl -X POST http://<device-ip>:%d/update --data-binary @app.bin", LOADER_HTTP_PORT);
    ESP_LOGI(TAG, "  curl http://<device-ip>:%d/update-target", LOADER_HTTP_PORT);
    ESP_LOGI(TAG, "  curl -X POST http://<device-ip>:%d/resume  -- return to the existing user app", LOADER_HTTP_PORT);
}
