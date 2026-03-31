#ifndef MICRONES_AUDIO_PWM_H
#define MICRONES_AUDIO_PWM_H

#include <stddef.h>
#include <stdint.h>

/*
 * PWM audio output on GP9.
 *
 * Hardware:
 *   GP9 → two-pole RC low-pass filter (R=1kΩ, C=10nF each pole, f_c=15.9 kHz)
 *       → 100µF DC-blocking cap → 10kΩ bleeder to GND → output jack.
 *   BAT85 ESD diode pair on the output.
 *
 * PWM configuration at 315 MHz system clock:
 *   wrap       = 7142  (7143 levels)
 *   clkdiv     = 1.0   (integer, no fractional division)
 *   f_carrier  = 315,000,000 / 7143 = 44,103.3 Hz  (75 ppm from 44,100 Hz)
 *   resolution = 7143 levels ≈ 12.8 bits  (well above 8-bit minimum)
 *
 *   Note: 315,000,000 / 44,100 = 50000/7 = 7142.857 — not an integer.
 *   44,100 Hz does not divide evenly into 315 MHz (GCD = 6,300 → irreducible).
 *   wrap=7142 gives the closest attainable rate (error 75 ppm, negligible).
 *
 * Sample rate: PWM wrap interrupt fires at 44,103 Hz ≈ 44.1 kHz.
 * The interrupt handler pops one sample from the ring buffer each wrap.
 *
 * The ring buffer is filled from Core 0's main loop via audio_pwm_push_samples().
 * The PWM wrap interrupt runs on Core 0 (same core as the NES CPU loop);
 * interrupt latency and execution time are well within the emulator's per-cycle
 * budget (~7,000 CPU cycles between interrupts at 315 MHz / 44.1 kHz).
 */

#define MICRONES_AUDIO_PIN  9u   /* GP9 */

/*
 * Initialise PWM audio on GP9.  sample_rate is stored but the actual
 * PWM rate is fixed at 44,103 Hz regardless of the value passed
 * (nearest achievable rate at 315 MHz).  Pass 44100 from main().
 */
void audio_pwm_init(uint32_t sample_rate);

/*
 * Push signed 16-bit mono PCM samples into the playback ring buffer.
 * Samples that do not fit are silently dropped.
 * Returns the number of samples actually accepted.
 * Thread-safe: producer on Core 0, consumer in PWM interrupt on Core 0.
 */
size_t audio_pwm_push_samples(const int16_t *samples, size_t count);

/* Diagnostics (unchanged signatures). */
uint32_t audio_pwm_underrun_count(void);
uint32_t audio_pwm_buffer_level(void);

#endif /* MICRONES_AUDIO_PWM_H */
