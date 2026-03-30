#ifndef MICRONES_CLOCK_CONFIG_H
#define MICRONES_CLOCK_CONFIG_H

/*
 * clock_config.h — single toggle for system clock speed.
 *
 * Set MICRONES_SYS_CLK_MHZ to 315 or 157.
 *
 *   315 MHz  — full speed, 60 fps.  Requires VREG 1.20 V.
 *              USB enumeration may be marginal on some boards; if it fails,
 *              plug the USB cable in after the board is already powered via VSYS,
 *              or switch to 157.
 *
 *   157      — 157.5 MHz (315/2).  Stable USB enumeration, VREG 1.10 V.
 *              Achieves ~40 fps with current optimisations; useful for debugging.
 *
 * All timing-dependent values are derived below — no other files need editing.
 */
#define MICRONES_SYS_CLK_MHZ  315

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
 * PIO: fixed at 'out pins, 4 [10]' = 11 sys_clk cycles/sample.
 * clkdiv=2.0 → effective rate = 315 MHz / (2 × 11) = 14.318182 MHz  ✓
 */
#  define MICRONES_PIO_CLKDIV       2.0f

/*
 * Audio PWM: wrap=7142 → f = 315,000,000 / 7143 = 44,103 Hz  (75 ppm from 44,100 Hz)
 */
#  define MICRONES_AUDIO_PWM_WRAP   7142u

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
 * PIO: fixed at 'out pins, 4 [10]' = 11 sys_clk cycles/sample.
 * clkdiv=1.0 → effective rate = 157.5 MHz / (1 × 11) = 14.318182 MHz  ✓
 */
#  define MICRONES_PIO_CLKDIV       1.0f

/*
 * Audio PWM: wrap=3570 → f = 157,500,000 / 3571 = 44,106 Hz  (136 ppm from 44,100 Hz)
 */
#  define MICRONES_AUDIO_PWM_WRAP   3570u

#else
#  error "MICRONES_SYS_CLK_MHZ must be 315 or 157"
#endif

#endif /* MICRONES_CLOCK_CONFIG_H */
