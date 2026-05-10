#include "nes_hw_controller.h"
#include "pico_input.h"       // PICO_NES_CTL_* pin definitions

#include "hardware/gpio.h"
#include "hardware/timer.h"   // busy_wait_us_32 – non-sleeping busy-wait

#include <stdio.h>

/* Edge-triggered printf of the raw 8-bit DATA shift-register sample on
 * every change in button state.  Useful for diagnosing "wrong button
 * registers / all buttons register" symptoms by exposing what the
 * 4021 actually puts on the wire.  Output format:
 *   ctl: raw=10110111 buttons=0x40   (Up only -> bit 4 low)
 * Bits in raw[] are in shift order: 0=A 1=B 2=Sel 3=Start 4=Up 5=Dn 6=L 7=R.
 * Build with -DMICRONES_NES_CTL_DEBUG=0 to disable. */
#ifndef MICRONES_NES_CTL_DEBUG
#define MICRONES_NES_CTL_DEBUG 1
#endif

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
#if MICRONES_NES_CTL_DEBUG
    uint8_t raw[8];
#endif
    for (int i = 0; i < 8; ++i) {
        bool bit = gpio_get(PICO_NES_CTL_DATA);
#if MICRONES_NES_CTL_DEBUG
        raw[i] = bit ? 1u : 0u;
#endif
        if (!bit) {
            buttons |= k_map[i];
        }
        // Rising edge shifts the 4021 to present the next button on DATA.
        gpio_put(PICO_NES_CTL_CLOCK, 1);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
        gpio_put(PICO_NES_CTL_CLOCK, 0);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
    }

#if MICRONES_NES_CTL_DEBUG
    /* Edge-trigger: only emit a line when the button state changes, so
     * the USB CDC log isn't flooded at 60 Hz. */
    static uint8_t s_last_buttons = 0;
    static bool    s_seen = false;
    if (!s_seen || buttons != s_last_buttons) {
        printf("ctl: raw=%u%u%u%u%u%u%u%u buttons=0x%02X\n",
               raw[0], raw[1], raw[2], raw[3],
               raw[4], raw[5], raw[6], raw[7],
               (unsigned)buttons);
        s_last_buttons = buttons;
        s_seen = true;
    }
#endif

    return (NesControllerState){ .buttons = buttons };
}
