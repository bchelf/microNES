#include "nes_hw_controller.h"
#include "pico_input.h"       // PICO_NES_CTL_* pin definitions

#include "hardware/gpio.h"
#include "hardware/timer.h"   // busy_wait_us_32 – non-sleeping busy-wait

// ── GPIO init ────────────────────────────────────────────────────────────

void nes_hw_controller_init(void)
{
    // CLOCK and LATCH are outputs, idle low.
    gpio_init(PICO_NES_CTL_CLOCK);
    gpio_set_dir(PICO_NES_CTL_CLOCK, GPIO_OUT);
    gpio_put(PICO_NES_CTL_CLOCK, 0);

    gpio_init(PICO_NES_CTL_LATCH);
    gpio_set_dir(PICO_NES_CTL_LATCH, GPIO_OUT);
    gpio_put(PICO_NES_CTL_LATCH, 0);

    // DATA is an input with pull-up (guards against floating when no controller
    // is connected – all bits read 1, meaning no button pressed).
    gpio_init(PICO_NES_CTL_DATA);
    gpio_set_dir(PICO_NES_CTL_DATA, GPIO_IN);
    gpio_pull_up(PICO_NES_CTL_DATA);
}

// ── Read cycle ───────────────────────────────────────────────────────────

NesControllerState nes_hw_controller_read(void)
{
    // Step 1: latch all eight button states into the 4021 shift register.
    gpio_put(PICO_NES_CTL_LATCH, 1);
    busy_wait_us_32(NES_HW_CTL_LATCH_US);
    gpio_put(PICO_NES_CTL_LATCH, 0);
    busy_wait_us_32(NES_HW_CTL_CLOCK_US);    // settle before first sample

    // Step 2: shift out 8 bits.
    // DATA is active-low: 0 = button pressed, 1 = released.
    static const uint8_t k_map[8] = {
        NES_BUTTON_A,      NES_BUTTON_B,    NES_BUTTON_SELECT, NES_BUTTON_START,
        NES_BUTTON_UP,     NES_BUTTON_DOWN, NES_BUTTON_LEFT,   NES_BUTTON_RIGHT,
    };

    uint8_t buttons = 0;
    for (int i = 0; i < 8; ++i) {
        if (!gpio_get(PICO_NES_CTL_DATA)) {
            buttons |= k_map[i];
        }
        // Rising edge shifts the 4021 to present the next button on DATA.
        gpio_put(PICO_NES_CTL_CLOCK, 1);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
        gpio_put(PICO_NES_CTL_CLOCK, 0);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
    }

    return (NesControllerState){ .buttons = buttons };
}
