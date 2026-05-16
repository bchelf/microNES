#ifndef MICRONES_AUDIO_PWM_H
#define MICRONES_AUDIO_PWM_H

#include <stddef.h>
#include <stdint.h>

/*
 * PWM audio output.
 *
 * Hardware (default breadboard wiring; GP16):
 *   GP16 → two-pole RC low-pass filter (R=1kΩ, C=10nF each pole, f_c=15.9 kHz)
 *        → 100µF DC-blocking cap → 10kΩ bleeder to GND → output jack.
 *   BAT85 ESD diode pair on the output.
 *
 * Hardware (v0.1 PCB; build with -DMICRONES_BOARD=v0_1):
 *   GP23 (GP_ADAC_IN in the schematic) → on-board analog DAC / filter
 *        → AUD_A / AUD_B line outputs.  The two-pole RC filter is on the
 *        PCB; this code only drives the PWM input.
 *
 * Two PWM slices are used:
 *
 *   1. Carrier slice (GP16, slice 8):
 *      Free-running PWM at ~1.23 MHz (315 MHz / 256) with 8-bit resolution.
 *      No interrupt — just holds whatever level is written.
 *
 *   2. Sample-rate timer slice (slice 0, no GPIO):
 *      Wraps at exactly 48,000 Hz.  Its wrap interrupt pops one sample from
 *      the ring buffer and writes the new level to the carrier slice.
 *
 * The slice number is derived from MICRONES_AUDIO_PIN at runtime via
 * pwm_gpio_to_slice_num(), so retargeting to a different GPIO is just a
 * #define change.
 *
 * The high carrier frequency is attenuated >50 dB by the two-pole RC filter
 * (f_c = 15.9 kHz), vs ~20 dB for the old 48 kHz carrier.
 *
 * The ring buffer is filled from Core 0's main loop via audio_pwm_push_samples().
 * The timer interrupt runs on Core 0 (same core as the NES CPU loop);
 * interrupt latency and execution time are well within the emulator's per-cycle
 * budget (~6,500 cycles between interrupts at 315 MHz / 48 kHz).
 */

#ifdef MICRONES_BOARD_V0_1
#include "board_pinout_v0_1.h"
#endif

#ifndef MICRONES_AUDIO_PIN
#ifdef MICRONES_BOARD_V0_1
#define MICRONES_AUDIO_PIN  MICRONES_V0_1_PIN_AUDIO_PWM
#else
#define MICRONES_AUDIO_PIN  16u  /* GP16, breadboard default */
#endif
#endif

/*
 * Initialise PWM audio.  sample_rate is stored but the actual PWM rate is
 * fixed by the target clock configuration. Pass the backend's preferred
 * sample rate from main().
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
