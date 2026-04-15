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
 * Two PWM slices are used:
 *
 *   1. Carrier slice (GP9, slice 4):
 *      Free-running PWM at ~1.23 MHz (315 MHz / 256) with 8-bit resolution.
 *      No interrupt — just holds whatever level is written.
 *
 *   2. Sample-rate timer slice (slice 0, no GPIO):
 *      Wraps at exactly 48,000 Hz.  Its wrap interrupt pops one sample from
 *      the ring buffer and writes the new level to the carrier slice.
 *
 * The high carrier frequency is attenuated >50 dB by the two-pole RC filter
 * (f_c = 15.9 kHz), vs ~20 dB for the old 48 kHz carrier.
 *
 * The ring buffer is filled from Core 0's main loop via audio_pwm_push_samples().
 * The timer interrupt runs on Core 0 (same core as the NES CPU loop);
 * interrupt latency and execution time are well within the emulator's per-cycle
 * budget (~6,500 cycles between interrupts at 315 MHz / 48 kHz).
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
