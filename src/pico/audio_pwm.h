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
 *   wrap       = 5249  (5250 levels)
 *   clkdiv     = 1.25  (int=1, frac=4)
 *   f_carrier  = 315,000,000 / (1.25 * 5250) = 48,000 Hz exactly
 *   resolution = 5250 levels ≈ 12.4 bits  (well above 8-bit minimum)
 *
 * Sample rate: PWM wrap interrupt fires at 48,000 Hz.
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
 * PWM rate is fixed by the target clock configuration. Pass the backend's
 * preferred sample rate from main().
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
