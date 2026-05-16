#include "nes_hw_controller.h"
#include "pico_input.h"       // PICO_NES_CTL_* pin definitions

#include "hardware/gpio.h"
#include "hardware/timer.h"   // busy_wait_us_32 – non-sleeping busy-wait

#include <stdio.h>

/* Edge-triggered printf of the raw 8-bit DATA shift-register sample on
 * every change in button state.  Useful for diagnosing "wrong button
 * registers / all buttons register" symptoms by exposing what the
 * 4021 actually puts on the wire.  Output format:
 *   ctl: p1 raw=10110111 buttons=0x40   (Up only -> bit 4 low)
 *        p2 raw=11111111 buttons=0x00
 * Bits in raw[] are in shift order: 0=A 1=B 2=Sel 3=Start 4=Up 5=Dn 6=L 7=R.
 * Build with -DMICRONES_NES_CTL_DEBUG=0 to disable. */
#ifndef MICRONES_NES_CTL_DEBUG
#define MICRONES_NES_CTL_DEBUG 1
#endif

// ── GPIO init ────────────────────────────────────────────────────────────

void nes_hw_controller_init(void)
{
    // CLOCK and LATCH are outputs, idle low.  Default 4 mA drive — the
    // 12 mA bump made things worse on the breadboard (likely overshoot
    // / reflections on long jumpers).
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

#ifdef PICO_NES_CTL_DATA_P2
    gpio_init(PICO_NES_CTL_DATA_P2);
    gpio_set_dir(PICO_NES_CTL_DATA_P2, GPIO_IN);
    gpio_pull_up(PICO_NES_CTL_DATA_P2);
#endif
}

// ── Read cycle ───────────────────────────────────────────────────────────

void nes_hw_controller_read_pair(NesControllerState *p1_out,
                                 NesControllerState *p2_out)
{
    // Step 1: latch all eight button states into the 4021 shift register(s).
    // CLOCK and LATCH are shared between both pads on the v0.1 PCB, so a
    // single LATCH pulse captures both controllers simultaneously.
    gpio_put(PICO_NES_CTL_LATCH, 1);
    busy_wait_us_32(NES_HW_CTL_LATCH_US);
    gpio_put(PICO_NES_CTL_LATCH, 0);
    busy_wait_us_32(NES_HW_CTL_CLOCK_US);    // settle before first sample

    // Step 2: shift out 8 bits per port.
    // DATA is active-low: 0 = button pressed, 1 = released.
    static const uint8_t k_map[8] = {
        NES_BUTTON_A,      NES_BUTTON_B,    NES_BUTTON_SELECT, NES_BUTTON_START,
        NES_BUTTON_UP,     NES_BUTTON_DOWN, NES_BUTTON_LEFT,   NES_BUTTON_RIGHT,
    };

    uint8_t buttons_p1 = 0;
    uint8_t buttons_p2 = 0;
#if MICRONES_NES_CTL_DEBUG
    uint8_t raw_p1[8];
    uint8_t raw_p2[8];
#endif
    for (int i = 0; i < 8; ++i) {
        bool bit_p1 = gpio_get(PICO_NES_CTL_DATA);
#ifdef PICO_NES_CTL_DATA_P2
        bool bit_p2 = gpio_get(PICO_NES_CTL_DATA_P2);
#else
        bool bit_p2 = true;  /* no port wired → reads "no buttons" */
#endif
#if MICRONES_NES_CTL_DEBUG
        raw_p1[i] = bit_p1 ? 1u : 0u;
        raw_p2[i] = bit_p2 ? 1u : 0u;
#endif
        if (!bit_p1) {
            buttons_p1 |= k_map[i];
        }
        if (!bit_p2) {
            buttons_p2 |= k_map[i];
        }
        // Rising edge shifts both 4021s to present the next button on DATA.
        gpio_put(PICO_NES_CTL_CLOCK, 1);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
        gpio_put(PICO_NES_CTL_CLOCK, 0);
        busy_wait_us_32(NES_HW_CTL_CLOCK_US);
    }

#if MICRONES_NES_CTL_DEBUG
    /* Edge-trigger: only emit a line when either port's state changes, so
     * the USB CDC log isn't flooded at 60 Hz. */
    static uint8_t s_last_p1 = 0;
    static uint8_t s_last_p2 = 0;
    static bool    s_seen = false;
    if (!s_seen || buttons_p1 != s_last_p1 || buttons_p2 != s_last_p2) {
        printf("ctl: p1 raw=%u%u%u%u%u%u%u%u buttons=0x%02X | "
               "p2 raw=%u%u%u%u%u%u%u%u buttons=0x%02X\n",
               raw_p1[0], raw_p1[1], raw_p1[2], raw_p1[3],
               raw_p1[4], raw_p1[5], raw_p1[6], raw_p1[7],
               (unsigned)buttons_p1,
               raw_p2[0], raw_p2[1], raw_p2[2], raw_p2[3],
               raw_p2[4], raw_p2[5], raw_p2[6], raw_p2[7],
               (unsigned)buttons_p2);
        s_last_p1 = buttons_p1;
        s_last_p2 = buttons_p2;
        s_seen = true;
    }
#endif

    if (p1_out) {
        p1_out->buttons = buttons_p1;
    }
    if (p2_out) {
        p2_out->buttons = buttons_p2;
    }
}

NesControllerState nes_hw_controller_read(void)
{
    NesControllerState p1, p2;
    nes_hw_controller_read_pair(&p1, &p2);
    (void)p2;
    return p1;
}
