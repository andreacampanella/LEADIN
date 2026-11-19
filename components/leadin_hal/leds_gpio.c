/* leds_gpio.c - LEDs on GPIO pins */
#include "hal_drivers.h"

static const leds_gpio_cfg_t *cfg;
static hal_leds_t part;

static void set(uint32_t on)
{
    for (int i = 0; i < cfg->count; i++)
        gpio_set_level(cfg->pins[i], ((on >> i) & 1) != cfg->active_low);
}

const hal_leds_t *leds_gpio(const leds_gpio_cfg_t *c)
{
    cfg = c;
    for (int i = 0; i < c->count; i++) {
        gpio_reset_pin(c->pins[i]);
        gpio_set_direction(c->pins[i], GPIO_MODE_OUTPUT);
    }
    set(0);
    part.count = c->count;
    part.set = set;
    return &part;
}
