#ifndef MICRONES_CLOCK_CONFIG_H
#define MICRONES_CLOCK_CONFIG_H

/*
 * clock_config.h — single toggle for system clock speed.
 *
 * Set MICRONES_SYS_CLK_MHZ to 315, 250, or 157.
 *
 *   315 MHz  — full speed analog target.  Requires VREG 1.20 V.
 *              NTSC: 315 MHz / 22 = 14.318182 MHz  ✓
 *              SPI:  best achievable ≤ 62.5 MHz = 52.5 MHz (CPSDVSR=6) → 53.4 FPS ceiling.
 *              USB enumeration may be marginal on some boards.
 *
 *   250 MHz  — optimal for TFT/SPI digital target.  VREG 1.20 V.
 *              SPI:  250 / 4 = exactly 62.5 MHz (CPSDVSR=4) → 63.6 FPS ceiling.
 *              NTSC: 250 does not divide evenly into 14.318182 MHz; do NOT use
 *              for the analog target.
 *
 *   157      — 157.5 MHz (315/2).  Stable USB enumeration, VREG 1.10 V.
 *              Achieves ~40 fps with current optimisations; useful for debugging.
 *
 * All timing-dependent values are derived below — no other files need editing.
 *
 * MICRONES_SYS_CLK_MHZ may be overridden per build target via a CMake
 * compile definition (e.g. -DMICRONES_SYS_CLK_MHZ=250 on the TFT target).
 * The #ifndef guard below makes the hardcoded 315 the fallback default.
 */
#ifndef MICRONES_SYS_CLK_MHZ
#define MICRONES_SYS_CLK_MHZ 315
#endif

/* -------------------------------------------------------------------------
 * Derived values — do not edit below this line.
 * ------------------------------------------------------------------------- */

#if MICRONES_SYS_CLK_MHZ == 315

/*
 * 315 MHz  —  PLL: VCO=1260 MHz, post_div1=2, post_div2=2
 *   sys_clk = 1260 / (2×2) = 315 MHz
 */
#  define MICRONES_PLL_VCO_HZ       1260000000u
#  define MICRONES_PLL_DIV1         2u
#  define MICRONES_PLL_DIV2         2u
#  define MICRONES_VREG             VREG_VOLTAGE_1_20
#  define MICRONES_VREG_SETTLE_MS   20u

/*
 * PIO: fixed at 'out pins, 5 [10]' = 11 sys_clk cycles/sample.
 * clkdiv=2.0 → effective rate = 315 MHz / (2 × 11) = 14.318182 MHz  ✓
 */
#  define MICRONES_PIO_CLKDIV       2.0f

/*
 * Audio PWM sample-rate timer: clkdiv=1.25, wrap=5249
 * → f_sample = 315,000,000 / (1.25 × 5250) = 48,000 Hz
 */
#  define MICRONES_AUDIO_PWM_CLKDIV_INT   1u
#  define MICRONES_AUDIO_PWM_CLKDIV_FRAC  4u
#  define MICRONES_AUDIO_PWM_WRAP         5249u

#elif MICRONES_SYS_CLK_MHZ == 157

/*
 * 157.5 MHz  —  PLL: VCO=1260 MHz, post_div1=4, post_div2=2
 *   sys_clk = 1260 / (4×2) = 157.5 MHz
 */
#  define MICRONES_PLL_VCO_HZ       1260000000u
#  define MICRONES_PLL_DIV1         4u
#  define MICRONES_PLL_DIV2         2u
#  define MICRONES_VREG             VREG_VOLTAGE_1_10
#  define MICRONES_VREG_SETTLE_MS   10u

/*
 * PIO: fixed at 'out pins, 5 [10]' = 11 sys_clk cycles/sample.
 * clkdiv=1.0 → effective rate = 157.5 MHz / (1 × 11) = 14.318182 MHz  ✓
 */
#  define MICRONES_PIO_CLKDIV       1.0f

/*
 * Audio PWM sample-rate timer: clkdiv=1.25, wrap=2624
 * → f_sample = 157,500,000 / (1.25 × 2625) = 48,000 Hz
 */
#  define MICRONES_AUDIO_PWM_CLKDIV_INT   1u
#  define MICRONES_AUDIO_PWM_CLKDIV_FRAC  4u
#  define MICRONES_AUDIO_PWM_WRAP         2624u

#elif MICRONES_SYS_CLK_MHZ == 250

/*
 * 250 MHz  —  PLL: VCO=1000 MHz, post_div1=4, post_div2=1
 *   sys_clk = 1000 / (4×1) = 250 MHz
 *   NOTE: 250 does not divide evenly into 14.318182 MHz (250/11 = 22.73 MHz,
 *   250/17 = 14.706 MHz). NTSC color will not work. CPU stability test only.
 */
#  define MICRONES_PLL_VCO_HZ       1000000000u
#  define MICRONES_PLL_DIV1         4u
#  define MICRONES_PLL_DIV2         1u
#  define MICRONES_VREG             VREG_VOLTAGE_1_20
#  define MICRONES_VREG_SETTLE_MS   20u

/*
 * PIO: clkdiv=2.0 gives 250/(2×11) = 11.364 MHz — incorrect for NTSC color,
 * but the PIO will run and sync pulses will be approximately timed.
 */
#  define MICRONES_PIO_CLKDIV       2.0f

/*
 * Audio PWM sample-rate timer: clkdiv=1.0, wrap=5207
 * → f_sample = 250,000,000 / 5208 = 48,003 Hz
 * Close enough for debug-only builds; 250 MHz is not the analog target.
 */
#  define MICRONES_AUDIO_PWM_CLKDIV_INT   1u
#  define MICRONES_AUDIO_PWM_CLKDIV_FRAC  0u
#  define MICRONES_AUDIO_PWM_WRAP         5207u

#else
#  error "MICRONES_SYS_CLK_MHZ must be 315, 250, or 157"
#endif

/*
 * Audio PWM carrier — identical for all clock speeds.
 * clkdiv = 1.0, wrap = 255  →  f_carrier = sys_clk / 256
 *   315 MHz → 1,230,469 Hz
 *   250 MHz →   976,563 Hz
 *   157 MHz →   615,234 Hz
 * Resolution = 256 levels = 8 bits (sufficient for NES audio).
 */
#define MICRONES_AUDIO_PWM_CARRIER_CLKDIV_INT   1u
#define MICRONES_AUDIO_PWM_CARRIER_CLKDIV_FRAC  0u
#define MICRONES_AUDIO_PWM_CARRIER_WRAP         255u

#endif /* MICRONES_CLOCK_CONFIG_H */
