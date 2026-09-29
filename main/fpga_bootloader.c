/*
 * fpga_bootloader.c - see fpga_bootloader.h for context.
 *
 * Ported from FPGA-Companion's ota_server.c
 * (ota_server_load_bootloader_to_sram()). The RECONFIG_N-pulse + 3x retry
 * JTAG begin() and the JTAG mutex are NOT duplicated here -- both already
 * exist in http_server.c (loader_jtag_lock/unlock/ensure_init/
 * program_sram_begin_with_retry) and are reused as-is, same lesson already
 * applied by serial_flash.c in Phase 4.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "miniz.h"

#include "fpga_bootloader.h"
#include "bootloader_data.h"
#include "http_server.h"
#include "jtag_gowin.h"

static const char *TAG = "fpga_bootloader";

/* 32 KB ring buffer (DEFLATE's max look-back window) in BSS -- reused on
 * every call, avoids fragmenting the heap with repeated large allocations. */
#define BOOTLOADER_RING_SIZE 32768
static uint8_t            s_ring[BOOTLOADER_RING_SIZE];
static tinfl_decompressor s_decomp;

esp_err_t fpga_bootloader_load_to_sram(void)
{
    if (!loader_jtag_lock(pdMS_TO_TICKS(30000))) {
        ESP_LOGE(TAG, "Timeout acquiring JTAG mutex for bootloader load");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = loader_jtag_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG init failed: %s", esp_err_to_name(err));
        loader_jtag_unlock();
        return err;
    }

    ESP_LOGI(TAG, "Loading embedded bootloader (%u B compressed) to FPGA SRAM",
             BOOTLOADER_COMPRESSED_SIZE);

    uint32_t idcode = 0;
    err = loader_jtag_program_sram_begin_with_retry(&idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG sram_begin failed: %s", esp_err_to_name(err));
        loader_jtag_unlock();
        return err;
    }

    tinfl_init(&s_decomp);
    const uint8_t *src      = bootloader_compressed_data;
    size_t         src_left = BOOTLOADER_COMPRESSED_SIZE;
    size_t         ring_pos = 0;
    size_t         total_out = 0;
    esp_err_t      write_err = ESP_OK;

    for (;;) {
        size_t in_bytes  = src_left;
        size_t out_bytes = BOOTLOADER_RING_SIZE - ring_pos;

        tinfl_status status = tinfl_decompress(
            &s_decomp,
            src, &in_bytes,
            s_ring,
            s_ring + ring_pos,
            &out_bytes,
            0);

        src      += in_bytes;
        src_left -= in_bytes;

        if (out_bytes > 0 && write_err == ESP_OK) {
            write_err  = jtag_gowin_program_sram_write(s_ring + ring_pos, out_bytes, false);
            total_out += out_bytes;
            ring_pos   = (ring_pos + out_bytes) % BOOTLOADER_RING_SIZE;
        }

        if (status == TINFL_STATUS_DONE) break;

        if (status < TINFL_STATUS_DONE) {
            ESP_LOGE(TAG, "Decompression error: %d", (int)status);
            if (write_err == ESP_OK) write_err = ESP_FAIL;
            break;
        }

        if (in_bytes == 0 && out_bytes == 0) {
            ESP_LOGE(TAG, "Decompressor stalled -- corrupt embedded data?");
            if (write_err == ESP_OK) write_err = ESP_FAIL;
            break;
        }
    }

    esp_err_t end_err = jtag_gowin_program_sram_end();
    if (write_err == ESP_OK) write_err = end_err;

    loader_jtag_unlock();

    if (write_err == ESP_OK) {
        ESP_LOGI(TAG, "Bootloader in FPGA SRAM (%zu bytes). SPI flash now accessible.", total_out);
    } else {
        ESP_LOGE(TAG, "Bootloader SRAM load failed: %s", esp_err_to_name(write_err));
    }
    return write_err;
}
