/*
 * serial_flash.c - see serial_flash.h for the protocol and Phase 4 context.
 *
 * Reuses the exact JTAG lock/init/retry-begin helpers and the inactive-slot
 * selection/bookkeeping exposed by http_server.h -- this module never talks
 * to jtag_gowin.c or esp_ota_ops directly without going through those, so
 * HTTP and serial can never disagree about which slot is next or race for
 * the JTAG pins.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "serial_flash.h"
#include "http_server.h"
#include "jtag_gowin.h"

static const char *TAG = "serial_flash";

#define SERIAL_FLASH_CHUNK         4096
#define SERIAL_FLASH_MAX_SRAM_SIZE (2 * 1024 * 1024)  /* generous cap for a Gowin bitstream */
#define SERIAL_FLASH_MAX_APP_SIZE  (2 * 1024 * 1024)  /* larger than any ota_0/ota_1 slot */
#define SERIAL_FLASH_STALL_MS      30000
#define SERIAL_FLASH_EOF_DELAY_MS  20
#define SERIAL_FLASH_LINE_MAX      64

/* RX ring buffer for the low-level usb_serial_jtag driver used during the
 * raw payload phase (see io_begin/io_end below) -- sized well above the
 * 64-byte USB-FS packet size so the ISR can absorb bursts between
 * read_raw_bytes() calls without stalling the host. */
#define SERIAL_FLASH_DRIVER_RX_BUF 65536
#define SERIAL_FLASH_DRIVER_TX_BUF 256

/* Console's default RX line-ending mode -- must be restored after reading
 * the raw payload or the next line-based command stops parsing correctly. */
#define CONSOLE_DEFAULT_RX_LINE_ENDINGS ESP_LINE_ENDINGS_CR

static bool s_driver_installed = false;

/* Bytes over-read into stdio's internal FILE* buffer while the getchar()
 * based line reader parsed the *_BEGIN command line, drained by io_begin()
 * before the low-level driver takes over the RX FIFO. */
static uint8_t s_prefix_buf[64];
static size_t  s_prefix_len = 0;
static size_t  s_prefix_off = 0;

/* Prepares for the raw payload transfer -- see FPGA-Companion's
 * serial_flash.c for the full rationale (default console VFS caps
 * throughput at a few KB/s; the low-level driver's ISR-fed ring buffer
 * does not). */
static void io_begin(void)
{
    s_prefix_len = 0;
    s_prefix_off = 0;
    size_t r;
    while (s_prefix_len < sizeof(s_prefix_buf) &&
           (r = fread(s_prefix_buf + s_prefix_len, 1, sizeof(s_prefix_buf) - s_prefix_len, stdin)) > 0) {
        s_prefix_len += r;
    }
    clearerr(stdin);

    usb_serial_jtag_driver_config_t drv_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    drv_cfg.rx_buffer_size = SERIAL_FLASH_DRIVER_RX_BUF;
    drv_cfg.tx_buffer_size = SERIAL_FLASH_DRIVER_TX_BUF;
    esp_err_t err = usb_serial_jtag_driver_install(&drv_cfg);
    if (err == ESP_OK) {
        s_driver_installed = true;
    } else {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install failed (%s), falling back to slow stdio reads", esp_err_to_name(err));
        s_driver_installed = false;
    }
}

static void io_end(void)
{
    if (s_driver_installed) {
        usb_serial_jtag_driver_uninstall();
        s_driver_installed = false;
    }
}

/* Blocks reading exactly `n` raw bytes -- see FPGA-Companion's serial_flash.c
 * for the detailed rationale. Gives up after SERIAL_FLASH_STALL_MS of no
 * data rather than hanging forever with flash/JTAG state half-open. */
static bool read_raw_bytes(uint8_t *buf, size_t n)
{
    size_t got = 0;

    if (s_prefix_off < s_prefix_len) {
        size_t avail = s_prefix_len - s_prefix_off;
        size_t take  = avail < n ? avail : n;
        memcpy(buf, s_prefix_buf + s_prefix_off, take);
        s_prefix_off += take;
        got = take;
    }

    int stalled_ms = 0;
    while (got < n) {
        size_t r;
        if (s_driver_installed) {
            r = (size_t)usb_serial_jtag_read_bytes(buf + got, n - got, pdMS_TO_TICKS(SERIAL_FLASH_EOF_DELAY_MS));
        } else {
            r = fread(buf + got, 1, n - got, stdin);
            if (r == 0) clearerr(stdin);
        }
        if (r > 0) {
            got += r;
            stalled_ms = 0;
        } else {
            if (!s_driver_installed) vTaskDelay(pdMS_TO_TICKS(SERIAL_FLASH_EOF_DELAY_MS));
            stalled_ms += SERIAL_FLASH_EOF_DELAY_MS;
            if (stalled_ms >= SERIAL_FLASH_STALL_MS) return false;
        }
    }
    return true;
}

/* =========================================================================
 * target=sram — stream bytes to FPGA SRAM via JTAG. Mirrors http_server.c's
 * handle_fpga_jtag_sram(), just fed by read_raw_bytes() instead of
 * httpd_req_recv().
 * ========================================================================= */

static esp_err_t serial_flash_write_sram(size_t size)
{
    ESP_LOGI(TAG, "Serial FPGA JTAG SRAM program: %zu bytes", size);

    if (!loader_jtag_lock(0)) {
        ESP_LOGW(TAG, "JTAG busy - another programming request in progress");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = loader_jtag_ensure_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JTAG init failed: %s", esp_err_to_name(err));
        loader_jtag_unlock();
        return err;
    }

    uint32_t idcode = 0;
    err = loader_jtag_program_sram_begin_with_retry(&idcode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FPGA not detected via JTAG");
        loader_jtag_unlock();
        return err;
    }

    uint8_t *buf = malloc(SERIAL_FLASH_CHUNK);
    if (!buf) {
        jtag_gowin_program_sram_end();
        loader_jtag_unlock();
        return ESP_ERR_NO_MEM;
    }

    size_t    remaining   = size;
    size_t    received    = 0;
    size_t    last_logged = 0;
    esp_err_t write_err   = ESP_OK;

    while (remaining > 0) {
        size_t to_read = remaining < SERIAL_FLASH_CHUNK ? remaining : SERIAL_FLASH_CHUNK;
        if (!read_raw_bytes(buf, to_read)) {
            ESP_LOGE(TAG, "Serial read stalled after %zu / %zu bytes", received, size);
            write_err = ESP_ERR_TIMEOUT;
            break;
        }

        write_err = jtag_gowin_program_sram_write(buf, to_read, true);
        if (write_err != ESP_OK) {
            ESP_LOGE(TAG, "JTAG write failed at offset %zu", received);
            break;
        }

        remaining -= to_read;
        received  += to_read;

        if (received - last_logged >= 131072 || remaining == 0) {
            printf("PROGRESS %zu\r\n", received);
            fflush(stdout);
            last_logged = received;
        }
    }

    free(buf);

    esp_err_t end_err = jtag_gowin_program_sram_end();
    if (write_err == ESP_OK) write_err = end_err;

    if (write_err == ESP_OK) {
        ESP_LOGI(TAG, "Serial FPGA JTAG SRAM programming complete!");
    }

    loader_jtag_unlock();
    return write_err;
}

/* =========================================================================
 * ESP32 app image — stream bytes into whichever ota_0/ota_1 slot is
 * inactive, then boot into it. Mirrors http_server.c's handle_update(),
 * just fed by read_raw_bytes() instead of httpd_req_recv().
 * ========================================================================= */

static esp_err_t serial_flash_write_app(size_t size)
{
    const esp_partition_t *target = loader_get_target_update_partition();
    if (!target) {
        ESP_LOGE(TAG, "No OTA update partition found -- check partition table");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Serial app flash: %zu bytes -> partition '%s'", size, target->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = malloc(SERIAL_FLASH_CHUNK);
    if (!buf) {
        esp_ota_abort(ota_handle);
        return ESP_ERR_NO_MEM;
    }

    size_t remaining = size;
    size_t written   = 0;
    bool   hdr_ok    = false;

    while (remaining > 0) {
        size_t to_read = remaining < SERIAL_FLASH_CHUNK ? remaining : SERIAL_FLASH_CHUNK;
        if (!read_raw_bytes(buf, to_read)) {
            ESP_LOGE(TAG, "Serial read stalled after %zu / %zu bytes", written, size);
            free(buf);
            esp_ota_abort(ota_handle);
            return ESP_ERR_TIMEOUT;
        }

        if (!hdr_ok) {
            if (buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid image magic 0x%02x (expected 0xE9)", buf[0]);
                free(buf);
                esp_ota_abort(ota_handle);
                return ESP_ERR_INVALID_ARG;
            }
            hdr_ok = true;
        }

        err = esp_ota_write(ota_handle, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            return err;
        }

        remaining -= to_read;
        written   += to_read;
        printf("PROGRESS %zu\r\n", written);
        fflush(stdout);
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }
    loader_save_last_slot((uint8_t)(target->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0));

    ESP_LOGI(TAG, "Serial app flash complete (%zu bytes) -> '%s'. Rebooting...", written, target->label);
    return ESP_OK;
}

/* =========================================================================
 * Command dispatch + console task
 * ========================================================================= */

static bool try_handle_fpga_flash_begin(const char *line)
{
    static const char prefix[] = "FPGA_FLASH_BEGIN ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;

    char          target[8] = {0};
    unsigned long size_ul   = 0;
    if (sscanf(line + sizeof(prefix) - 1, "%7s %lu", target, &size_ul) != 2) {
        printf("\r\nFPGA_FLASH_ERROR bad_command\r\n");
        fflush(stdout);
        return true;
    }

    if (strcmp(target, "sram") != 0) {
        /* target=flash (persistent SPI) isn't wired up yet -- the loader has
         * no mcu_hw SPI-flash bridge ported (unlike FPGA-Companion). */
        printf("\r\nFPGA_FLASH_ERROR bad_target\r\n");
        fflush(stdout);
        return true;
    }

    size_t size = (size_t)size_ul;
    if (size == 0 || size > SERIAL_FLASH_MAX_SRAM_SIZE) {
        printf("\r\nFPGA_FLASH_ERROR bad_size\r\n");
        fflush(stdout);
        return true;
    }

    printf("\r\nREADY\r\n");
    fflush(stdout);

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    io_begin();
    esp_err_t err = serial_flash_write_sram(size);
    io_end();
    usb_serial_jtag_vfs_set_rx_line_endings(CONSOLE_DEFAULT_RX_LINE_ENDINGS);

    if (err == ESP_OK) {
        printf("FPGA_FLASH_OK\r\n");
    } else {
        printf("FPGA_FLASH_ERROR %s\r\n", esp_err_to_name(err));
    }
    fflush(stdout);
    return true;
}

static bool try_handle_app_flash_begin(const char *line)
{
    static const char prefix[] = "APP_FLASH_BEGIN ";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;

    unsigned long size_ul = 0;
    if (sscanf(line + sizeof(prefix) - 1, "%lu", &size_ul) != 1) {
        printf("\r\nAPP_FLASH_ERROR bad_command\r\n");
        fflush(stdout);
        return true;
    }

    size_t size = (size_t)size_ul;
    if (size == 0 || size > SERIAL_FLASH_MAX_APP_SIZE) {
        printf("\r\nAPP_FLASH_ERROR bad_size\r\n");
        fflush(stdout);
        return true;
    }

    printf("\r\nREADY\r\n");
    fflush(stdout);

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    io_begin();
    esp_err_t err = serial_flash_write_app(size);
    io_end();
    usb_serial_jtag_vfs_set_rx_line_endings(CONSOLE_DEFAULT_RX_LINE_ENDINGS);

    if (err == ESP_OK) {
        printf("APP_FLASH_OK\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        printf("APP_FLASH_ERROR %s\r\n", esp_err_to_name(err));
        fflush(stdout);
    }
    return true;
}

/* Blocks reading one newline-terminated line from stdin. getchar() is
 * expected to block until a byte arrives, but some VFS backends return EOF
 * immediately when no console is attached -- back off briefly on EOF rather
 * than spinning and starving the idle task. */
static size_t read_line(char *buf, size_t buf_size)
{
    size_t len = 0;
    int c;

    for (;;) {
        c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') break;
        if (len < buf_size - 1) buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return len;
}

static void serial_console_task(void *arg)
{
    char line[SERIAL_FLASH_LINE_MAX];

    ESP_LOGI(TAG, "Listening on USB serial for FPGA_FLASH_BEGIN / APP_FLASH_BEGIN commands");

    for (;;) {
        size_t len = read_line(line, sizeof(line));
        if (len == 0) continue;

        if (try_handle_fpga_flash_begin(line)) continue;
        if (try_handle_app_flash_begin(line)) continue;
    }
}

void serial_flash_start(void)
{
    xTaskCreate(serial_console_task, "serial_flash", 4096, NULL, 5, NULL);
}
