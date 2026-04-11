/*
 * audio_pwm.c  —  PWM audio output on GP9 at 48,000 Hz
 *
 * System clock: 315 MHz
 *
 * PWM configuration (analytically derived):
 *   Target sample rate: 48,000 Hz
 *   315,000,000 / (1.25 × 5,250) = 48,000 exactly.
 *
 *   clkdiv_int  = 1, clkdiv_frac = 4  (1.25 total divider)
 *   wrap        = 5,249
 *   Resolution  = 5,250 levels ≈ 12.4 bits  (> 8-bit minimum ✓)
 *   Carrier     = 48,000 Hz  (edge-aligned PWM, carrier = sample interrupt rate)
 *
 * Architecture:
 *   • PWM wrap interrupt fires at 48,000 Hz on Core 0.
 *   • Interrupt handler pops one sample from a ring buffer, scales it to
 *     [0..AUDIO_PWM_WRAP], and writes the PWM level.
 *   • Main loop (Core 0) pushes int16 PCM samples via audio_pwm_push_samples().
 *   • Ring buffer is single-producer (Core 0 main) / single-consumer (Core 0 IRQ);
 *     volatile indices provide sufficient ordering on a single core.
 *
 * Sample conversion  int16 → PWM level:
 *   u8  = (uint8_t)((uint16_t)sample >> 8) ^ 0x80)
 *        maps [-32768..32767] → [0..255] with 128 = silence
 *   level = (u8 * (AUDIO_PWM_WRAP + 1)) >> 8
 *        maps [0..255] → [0..AUDIO_PWM_WRAP]
 */

#include "audio_pwm.h"
#include "clock_config.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"

/* =========================================================================
 * PWM parameters
 * ========================================================================= */

/*
 * MICRONES_AUDIO_PWM_WRAP is derived from MICRONES_SYS_CLK_MHZ in clock_config.h:
 *   315 MHz → clkdiv=1.25, wrap=5249 → 48,000 Hz exactly
 *   157.5 MHz → clkdiv=1.25, wrap=2624 → 48,000 Hz exactly
 * The interrupt fires once per PWM period (on wrap).
 */
enum {
    AUDIO_PWM_WRAP  = MICRONES_AUDIO_PWM_WRAP,
    AUDIO_BUF_SIZE  = 2048,    /* ~42.7 ms at 48 kHz; power-of-2 for cheap modulo */
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
 * PWM wrap interrupt handler (Core 0)
 *
 * Execution time: ~10-15 instructions ≈ 10-15 sys_clk cycles.
 * At 315 MHz / 48,000 Hz = 6,562.5 cycles between interrupts, the ISR
 * consumes < 0.2% of Core 0's cycle budget.
 * ========================================================================= */

static void __isr pwm_audio_irq_handler(void) {
    pwm_clear_irq(s_audio_slice);

    uint32_t head = audio_buf_head;
    uint32_t level;

    if (head == audio_buf_tail) {
        /* Underrun: hold last output level to suppress buzz */
        level = s_audio_last_level;
        ++audio_underruns;
    } else {
        /*
         * Convert int16 → unsigned 8-bit centred at 128, then scale to
         * [0..AUDIO_PWM_WRAP].
         *
         * Step 1: int16 → uint8 (sign-flip without UB)
         *   raw = (uint16_t)sample            reinterpret bits
         *   u8  = (raw >> 8) ^ 0x80           top byte, flip sign bit
         *   Maps: -32768 → 0x80^0x80 = 0,  0 → 0x00^0x80 = 128,  32767 → 0x7F^0x80 = 255
         *
         * Step 2: uint8 → PWM level  (0-255 → 0-AUDIO_PWM_WRAP)
         *   level = (u8 * (AUDIO_PWM_WRAP + 1)) >> 8
         */
        uint16_t raw = (uint16_t)audio_buf[head];
        uint8_t  u8  = (uint8_t)((raw >> 8u) ^ 0x80u);
        level = ((uint32_t)u8 * ((uint32_t)AUDIO_PWM_WRAP + 1u)) >> 8u;

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

    gpio_set_function(MICRONES_AUDIO_PIN, GPIO_FUNC_PWM);
    s_audio_slice = pwm_gpio_to_slice_num(MICRONES_AUDIO_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int_frac(&config,
                                   MICRONES_AUDIO_PWM_CLKDIV_INT,
                                   MICRONES_AUDIO_PWM_CLKDIV_FRAC);
    pwm_config_set_wrap(&config, AUDIO_PWM_WRAP);
    pwm_init(s_audio_slice, &config, /*start=*/true);

    /* Initialise to mid-scale silence */
    pwm_set_gpio_level(MICRONES_AUDIO_PIN, (AUDIO_PWM_WRAP + 1u) / 2u);

    audio_buf_head     = 0;
    audio_buf_tail     = 0;
    audio_underruns    = 0;
    s_audio_last_level = 128u;

    /* Enable PWM wrap interrupt on Core 0 */
    pwm_clear_irq(s_audio_slice);
    pwm_set_irq_enabled(s_audio_slice, true);
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
