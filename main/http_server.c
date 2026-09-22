/*
 * http_server.c - Bare-bones HTTP endpoint for Phase 2 JTAG validation
 *
 * POST /fpga-jtag-sram streams a raw Gowin .bin bitstream straight into
 * jtag_gowin_program_sram_*() -- same wire format and endpoint name as
 * FPGA-Companion's ota_server.c (see its JTAG_PROGRAMMING.md), kept
 * identical here only to prove the loader can drive JTAG independently.
 * Not the final endpoint shape -- Phase 3 (inactive-slot app flashing) and
 * Phase 4 (USB-serial fallback) extend this. See
 * papilio-works/plans/2026-09-21-papilio-esp-bootloader.md.
 */
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "jtag_gowin.h"
#include "http_server.h"

static const char *TAG = "http_server";

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

static esp_err_t handle_status(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req,
        "Papilio ESP Bootloader (Phase 2 -- JTAG only)\n"
        "POST /fpga-jtag-sram -- program FPGA SRAM via JTAG (.bin bitstream)\n");
    return ESP_OK;
}

static esp_err_t handle_fpga_jtag_sram(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(ensure_jtag_mutex(), 0) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG programming already in progress");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming started: %d bytes", req->content_len);

    if (!s_jtag_initialized) {
        if (jtag_gowin_init(NULL) != ESP_OK) {
            ESP_LOGE(TAG, "JTAG initialization failed");
            xSemaphoreGive(s_jtag_mutex);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JTAG init failed");
            return ESP_FAIL;
        }
        s_jtag_initialized = true;
    }

    /* JTAG SRAM programming can lose a race against the FPGA's own SPI-flash
     * auto-config FSM on the first attempt (mirrors FPGA-Companion's
     * ota_server.c retry loop) -- pulse RECONFIG_N and retry up to 3x with
     * increasing settle time before giving up. */
    const int MAX_JTAG_BEGIN_ATTEMPTS = 3;
    uint32_t idcode = 0;
    esp_err_t begin_err = ESP_FAIL;
    for (int attempt = 1; attempt <= MAX_JTAG_BEGIN_ATTEMPTS; attempt++) {
        fpga_reconfig_pulse();
        esp_rom_delay_us(2000 + attempt * 1000);  /* 3 ms, 4 ms, 5 ms */

        begin_err = jtag_gowin_program_sram_begin(&idcode);
        if (begin_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "JTAG sram_begin attempt %d/%d failed: %s",
                 attempt, MAX_JTAG_BEGIN_ATTEMPTS, esp_err_to_name(begin_err));
    }
    if (begin_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to begin JTAG programming (FPGA not detected?)");
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "FPGA not detected via JTAG");
        return ESP_FAIL;
    }

    char *chunk_buf = malloc(RECV_CHUNK_SIZE);
    if (!chunk_buf) {
        xSemaphoreGive(s_jtag_mutex);
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
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    if (jtag_gowin_program_sram_end() != ESP_OK) {
        ESP_LOGE(TAG, "JTAG programming end failed");
        xSemaphoreGive(s_jtag_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Programming failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "FPGA JTAG SRAM programming complete! (%d bytes)", received);
    xSemaphoreGive(s_jtag_mutex);

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

    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &fpga_jtag_sram_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", LOADER_HTTP_PORT);
    ESP_LOGI(TAG, "  curl -X POST http://<device-ip>:%d/fpga-jtag-sram --data-binary @bitstream.bin", LOADER_HTTP_PORT);
}
