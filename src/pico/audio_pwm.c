/*
 * audio_pwm.c  —  High-frequency PWM audio output on GP9
 *
 * Two PWM slices:
 *
 *   Carrier (GP9, slice 4):
 *     clkdiv = 1.0, wrap = 255  →  315 MHz / 256 ≈ 1.23 MHz
 *     Free-running, no interrupt.  8-bit resolution (sufficient for NES audio).
 *
 *   Sample-rate timer (slice 0, no GPIO):
 *     clkdiv = 1.25, wrap = 5249  →  315 MHz / (1.25 × 5250) = 48,000 Hz
 *     Wrap interrupt pops one sample and writes the carrier level.
 *
 * The ~1.23 MHz carrier is attenuated >50 dB by the two-pole 15.9 kHz RC
 * filter, vs ~20 dB for the old 48 kHz carrier.
 *
 * Sample conversion  int16 → PWM level:
 *   u8  = (uint8_t)((uint16_t)sample >> 8) ^ 0x80)
 *        maps [-32768..32767] → [0..255] with 128 = silence
 *   With wrap = 255, level = u8 directly (no further scaling needed).
 */

#include "audio_pwm.h"
#include "clock_config.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

/* =========================================================================
 * PWM parameters
 * ========================================================================= */

enum {
    AUDIO_CARRIER_WRAP      = MICRONES_AUDIO_PWM_CARRIER_WRAP,  /* 255 */
    AUDIO_SAMPLE_TIMER_SLICE = 0u,   /* PWM slice 0: 48 kHz timer, no GPIO */
    AUDIO_BUF_SIZE           = 2048, /* ~42.7 ms at 48 kHz; power-of-2 for cheap modulo */
};

/* =========================================================================
 * Ring buffer (single-producer Core 0 / single-consumer Core 0 IRQ)
 * ========================================================================= */

static int16_t           audio_buf[AUDIO_BUF_SIZE];
static volatile uint32_t audio_buf_head  = 0;    /* consumer (IRQ) read position  */
static volatile uint32_t audio_buf_tail  = 0;    /* producer (main) write position */
static volatile uint32_t audio_underruns = 0;

static uint  s_audio_slice;
static uint8_t s_audio_last_level = 128u;   /* sample-and-hold across underruns */

/* =========================================================================
 * Sample-rate timer interrupt handler (Core 0)
 *
 * Fires at 48,000 Hz from timer slice 0.  Pops one sample from the ring
 * buffer and writes the level to the carrier slice on GP9.
 *
 * Execution time: ~10-15 instructions ≈ 10-15 sys_clk cycles.
 * At 315 MHz / 48,000 Hz = 6,562.5 cycles between interrupts, the ISR
 * consumes < 0.2% of Core 0's cycle budget.
 * ========================================================================= */

static void __isr pwm_audio_irq_handler(void) {
    pwm_clear_irq(AUDIO_SAMPLE_TIMER_SLICE);

    uint32_t head = audio_buf_head;
    uint32_t level;

    if (head == audio_buf_tail) {
        /* Underrun: hold last output level to suppress buzz */
        level = s_audio_last_level;
        ++audio_underruns;
    } else {
        /*
         * Convert int16 → unsigned 8-bit centred at 128.
         *   raw = (uint16_t)sample            reinterpret bits
         *   u8  = (raw >> 8) ^ 0x80           top byte, flip sign bit
         *   Maps: -32768 → 0,  0 → 128,  32767 → 255
         *
         * With AUDIO_CARRIER_WRAP = 255, level = u8 directly.
         */
        uint16_t raw = (uint16_t)audio_buf[head];
        uint8_t  u8  = (uint8_t)((raw >> 8u) ^ 0x80u);
        level = ((uint32_t)u8 * ((uint32_t)AUDIO_CARRIER_WRAP + 1u)) >> 8u;

        s_audio_last_level = (uint8_t)u8;
        audio_buf_head = (head + 1u) & (AUDIO_BUF_SIZE - 1u);
    }

    pwm_set_gpio_level(MICRONES_AUDIO_PIN, level);
}

/* =========================================================================
 * audio_pwm_init()
 * ========================================================================= */

void audio_pwm_init(uint32_t sample_rate) {
    (void)sample_rate;   /* actual rate is fixed by clock_config.h for this target */

    /* --- Carrier PWM on GP9 (free-running, ~1.23 MHz, 8-bit) ------------- */
    gpio_set_function(MICRONES_AUDIO_PIN, GPIO_FUNC_PWM);
    s_audio_slice = pwm_gpio_to_slice_num(MICRONES_AUDIO_PIN);

    pwm_config carrier_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&carrier_cfg,
                                   MICRONES_AUDIO_PWM_CARRIER_CLKDIV_INT,
                                   MICRONES_AUDIO_PWM_CARRIER_CLKDIV_FRAC);
    pwm_config_set_wrap(&carrier_cfg, AUDIO_CARRIER_WRAP);
    pwm_init(s_audio_slice, &carrier_cfg, /*start=*/true);

    /* Initialise to mid-scale silence */
    pwm_set_gpio_level(MICRONES_AUDIO_PIN, (AUDIO_CARRIER_WRAP + 1u) / 2u);

    audio_buf_head     = 0;
    audio_buf_tail     = 0;
    audio_underruns    = 0;
    s_audio_last_level = 128u;

    /* --- Sample-rate timer (slice 0, 48 kHz, interrupt-driven) ----------- */
    pwm_config timer_cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&timer_cfg,
                                   MICRONES_AUDIO_PWM_CLKDIV_INT,
                                   MICRONES_AUDIO_PWM_CLKDIV_FRAC);
    pwm_config_set_wrap(&timer_cfg, MICRONES_AUDIO_PWM_WRAP);
    pwm_init(AUDIO_SAMPLE_TIMER_SLICE, &timer_cfg, /*start=*/true);

    /* Enable wrap interrupt on the timer slice only */
    pwm_clear_irq(AUDIO_SAMPLE_TIMER_SLICE);
    pwm_set_irq_enabled(AUDIO_SAMPLE_TIMER_SLICE, true);
    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_audio_irq_handler);
    irq_set_enabled(PWM_IRQ_WRAP, true);
}

/* =========================================================================
 * audio_pwm_push_samples()
 * ========================================================================= */

size_t audio_pwm_push_samples(const int16_t *samples, size_t count) {
    size_t   written = 0;
    uint32_t tail    = audio_buf_tail;

    for (size_t i = 0; i < count; ++i) {
        uint32_t next_tail = (tail + 1u) & (AUDIO_BUF_SIZE - 1u);
        if (next_tail == audio_buf_head) {
            break;   /* buffer full — drop remainder silently */
        }
        audio_buf[tail] = samples[i];
        tail = next_tail;
        ++written;
    }

    audio_buf_tail = tail;
    return written;
}

/* =========================================================================
 * Diagnostics
 * ========================================================================= */

uint32_t audio_pwm_underrun_count(void) {
    return audio_underruns;
}

uint32_t audio_pwm_buffer_level(void) {
    uint32_t head = audio_buf_head;
    uint32_t tail = audio_buf_tail;
    return (tail - head) & (AUDIO_BUF_SIZE - 1u);
}
