#include "pico_status.h"

#ifdef MICRONES_BOARD_V0_1

#include "board_pinout_v0_1.h"

#include "hardware/gpio.h"

static bool s_last_pressed = false;

static inline bool read_button_pressed(void) {
    bool level = gpio_get(MICRONES_V0_1_PIN_RST_BUTTON);
#if MICRONES_RST_BTN_ACTIVE_LOW
    return !level;
#else
    return level;
#endif
}

void pico_status_init(void) {
    /* Power LED — output, driven to "on" so the LED lights as soon as the
     * firmware has reached this point in boot. */
    gpio_init(MICRONES_V0_1_PIN_PWR_LED);
    gpio_set_dir(MICRONES_V0_1_PIN_PWR_LED, GPIO_OUT);
    pico_status_set_led(true);

    /* Reset button — input with pull-up so an absent panel reads "not
     * pressed" even though the connector also has its own pull-up. */
    gpio_init(MICRONES_V0_1_PIN_RST_BUTTON);
    gpio_set_dir(MICRONES_V0_1_PIN_RST_BUTTON, GPIO_IN);
    gpio_pull_up(MICRONES_V0_1_PIN_RST_BUTTON);

    s_last_pressed = read_button_pressed();
}

void pico_status_set_led(bool on) {
#if MICRONES_PWR_LED_ACTIVE_LOW
    gpio_put(MICRONES_V0_1_PIN_PWR_LED, on ? 0 : 1);
#else
    gpio_put(MICRONES_V0_1_PIN_PWR_LED, on ? 1 : 0);
#endif
}

bool pico_status_reset_button_pressed(void) {
    bool now = read_button_pressed();
    bool edge = (now && !s_last_pressed);
    s_last_pressed = now;
    return edge;
}

#else  /* MICRONES_BOARD_V0_1 */

void pico_status_init(void)                    { /* no panel on this board */ }
void pico_status_set_led(bool on)              { (void)on; }
bool pico_status_reset_button_pressed(void)    { return false; }

#endif
