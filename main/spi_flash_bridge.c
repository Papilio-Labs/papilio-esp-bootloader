/*
 * spi_flash_bridge.c - see spi_flash_bridge.h for context.
 *
 * Ported from FPGA-Companion/src/esp32/mcu_hw.c's external-flash subset,
 * kept behaviorally identical (same retry counts/delays, same chunked-erase
 * watchdog fix) since that code is already proven on real hardware -- only
 * the HID/USB-host/general SPI-protocol pieces it shared a file with were
 * dropped, they're irrelevant to the loader.
 */
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "spi_flash_bridge.h"

static const char *TAG = "spi_flash_bridge";

#define SPI_HOST_ID SPI3_HOST
#define PIN_NUM_MISO 4        /* Stable GPIO1-4 relocation; see header comment */
#define PIN_NUM_MOSI 2
#define PIN_NUM_CLK  1
#define PIN_NUM_FLASH_CS 3
#define PIN_NUM_LED_CLEAR_N 9
#define PIN_NUM_RECONFIG_N 13

static esp_flash_t *s_ext_flash = NULL;
static bool s_ext_flash_ready = false;
static volatile bool s_spi_bus_ready = false;
static SemaphoreHandle_t s_flash_mutex = NULL;

static const esp_flash_spi_device_config_t s_flash_device_cfg = {
    .host_id   = SPI_HOST_ID,
    .cs_id     = 0,
    .cs_io_num = PIN_NUM_FLASH_CS,
    .io_mode   = SPI_FLASH_SLOWRD,  /* avoids dummy-byte issues through the FPGA SPI bridge */
    .freq_mhz  = 20
};

static bool wait_for_spi_bus_ready(void)
{
    const int SPI_BUS_READY_TIMEOUT_MS = 30000;
    int waited_ms = 0;
    while (!s_spi_bus_ready) {
        if (waited_ms >= SPI_BUS_READY_TIMEOUT_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    return true;
}

void spi_flash_bridge_init(void)
{
    s_flash_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing external SPI flash bus: MISO=%d SCK=%d MOSI=%d CS=%d RECONFIG_N=%d",
             PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_MOSI, PIN_NUM_FLASH_CS, PIN_NUM_RECONFIG_N);

    gpio_set_direction(PIN_NUM_LED_CLEAR_N, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_LED_CLEAR_N, 1);

    spi_bus_config_t buscfg = {
        .miso_io_num     = PIN_NUM_MISO,
        .mosi_io_num     = PIN_NUM_MOSI,
        .sclk_io_num     = PIN_NUM_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .intr_flags      = ESP_INTR_FLAG_LOWMED,
        .max_transfer_sz = 32,
    };
    spi_bus_initialize(SPI_HOST_ID, &buscfg, SPI_DMA_CH_AUTO);

    /* Registered now, but not probed (esp_flash_init) -- the FPGA SPI bridge
     * isn't running yet this early at boot. Actual probing happens in
     * spi_flash_bridge_reinit(), called after the bridge bitstream is
     * loaded to SRAM via JTAG. */
    esp_err_t err = spi_bus_add_flash_device(&s_ext_flash, &s_flash_device_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_flash_device failed: %s", esp_err_to_name(err));
        return;
    }
    s_spi_bus_ready = true;
}

esp_err_t spi_flash_bridge_reinit(void)
{
    const int MAX_RETRIES = 5;
    const int RETRY_DELAY_MS[] = { 500, 1000, 1500, 2000, 2000 };

    if (!wait_for_spi_bus_ready()) {
        ESP_LOGE(TAG, "Reinit aborted: SPI bus never became ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_ext_flash_ready = false;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        /* Full remove+re-add for a clean handle -- field-clearing alone
         * doesn't reset host-side SPI state. */
        spi_bus_remove_flash_device(s_ext_flash);
        s_ext_flash = NULL;

        esp_err_t add_err = spi_bus_add_flash_device(&s_ext_flash, &s_flash_device_cfg);
        if (add_err != ESP_OK) {
            ESP_LOGW(TAG, "Flash device re-add attempt %d/%d failed: %s",
                     attempt + 1, MAX_RETRIES, esp_err_to_name(add_err));
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS[attempt]));
            continue;
        }

        esp_err_t err = esp_flash_init(s_ext_flash);
        if (err == ESP_OK) {
            uint32_t id = 0, flash_size = 0;
            esp_flash_read_id(s_ext_flash, &id);
            esp_flash_get_size(s_ext_flash, &flash_size);
            ESP_LOGI(TAG, "External flash ready: size=%" PRIu32 " KB, ID=0x%" PRIx32,
                     flash_size / 1024, id);
            s_ext_flash_ready = true;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Flash init attempt %d/%d failed: %s -- retrying in %d ms",
                 attempt + 1, MAX_RETRIES, esp_err_to_name(err), RETRY_DELAY_MS[attempt]);
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS[attempt]));
    }

    if (!s_ext_flash) spi_bus_add_flash_device(&s_ext_flash, &s_flash_device_cfg);
    ESP_LOGE(TAG, "External flash init failed after %d attempts", MAX_RETRIES);
    return ESP_ERR_FLASH_UNSUPPORTED_CHIP;
}

static esp_err_t ensure_flash_ready(void)
{
    if (s_ext_flash_ready) return ESP_OK;
    return spi_flash_bridge_reinit();
}

esp_err_t spi_flash_bridge_read_id(uint32_t *id, uint32_t *size_bytes)
{
    if (!id || !size_bytes) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ensure_flash_ready();
    if (err != ESP_OK) return err;

    err = esp_flash_read_id(s_ext_flash, id);
    if (err != ESP_OK) return err;
    return esp_flash_get_size(s_ext_flash, size_bytes);
}

/* Erasing the full 2 MB FPGA bitstream region in one esp_flash_erase_region()
 * call busy-polls the SPI bus back-to-back with no scheduler yield, long
 * enough to trip the task watchdog. Erase in chunks and yield between them. */
#define ERASE_CHUNK_SIZE 65536

void spi_flash_bridge_erase_region(uint32_t addr, uint32_t size)
{
    if (ensure_flash_ready() != ESP_OK) return;

    uint32_t offset = 0;
    while (offset < size) {
        uint32_t chunk = (size - offset < ERASE_CHUNK_SIZE) ? (size - offset) : ERASE_CHUNK_SIZE;
        esp_flash_erase_region(s_ext_flash, addr + offset, chunk);
        offset += chunk;
        vTaskDelay(1);  /* let the idle task run so the watchdog is fed */
    }
}

void spi_flash_bridge_write(uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (ensure_flash_ready() != ESP_OK) return;
    esp_flash_write(s_ext_flash, data, addr, size);
}

void spi_flash_bridge_read(uint32_t addr, uint8_t *data, uint32_t size)
{
    if (ensure_flash_ready() != ESP_OK) return;
    esp_flash_read(s_ext_flash, data, addr, size);
}

void spi_flash_bridge_lock(void)
{
    xSemaphoreTake(s_flash_mutex, portMAX_DELAY);
}

void spi_flash_bridge_unlock(void)
{
    xSemaphoreGive(s_flash_mutex);
}

void spi_flash_bridge_fpga_reset(void)
{
    ESP_LOGI(TAG, "FPGA RESET (cold boot from flash)");

    /* The bridge LED is a latched WS2812 state. Pull its sideband low long
     * enough for the bridge to transmit a complete zero-color frame. */
    gpio_set_level(PIN_NUM_LED_CLEAR_N, 0);
    vTaskDelay(pdMS_TO_TICKS(5));

    gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_RECONFIG_N, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_NUM_RECONFIG_N, 1);
    gpio_set_direction(PIN_NUM_RECONFIG_N, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_NUM_LED_CLEAR_N, GPIO_MODE_INPUT);
}
