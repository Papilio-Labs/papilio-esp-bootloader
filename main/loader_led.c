#include "led_strip.h"

#include "loader_led.h"

#define LOADER_LED_GPIO 48

static led_strip_handle_t s_strip;

static bool loader_led_init(void)
{
    if (s_strip) {
        return true;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LOADER_LED_GPIO,
        .max_leds       = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) == ESP_OK;
}

void loader_led_set_purple(void)
{
    if (!loader_led_init()) {
        return;
    }

    led_strip_set_pixel(s_strip, 0, 16, 0, 16);
    led_strip_refresh(s_strip);
}

void loader_led_clear(void)
{
    if (!s_strip) {
        return;
    }

    led_strip_clear(s_strip);
    led_strip_del(s_strip);
    s_strip = NULL;
}
