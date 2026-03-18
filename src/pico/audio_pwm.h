#ifndef SMB2350_AUDIO_PWM_H
#define SMB2350_AUDIO_PWM_H

#include <stddef.h>
#include <stdint.h>

/* GP5 audio output pin.  PWM carrier at clk_sys/256 (~984 kHz at 252 MHz),
 * duty-cycle modulated at sample_rate Hz.  Pass through an RC low-pass filter
 * (e.g. 1 kΩ + 10 nF) before the output jack. */
#define SMB2350_AUDIO_PIN 5u

/* Initialise PWM audio output and start the playback timer.
 * sample_rate is the PCM sample rate in Hz (typically 48000). */
void audio_pwm_init(uint32_t sample_rate);

/* Push signed 16-bit mono PCM samples into the playback ring buffer.
 * Samples that do not fit are silently dropped.
 * Returns the number of samples actually accepted. */
size_t audio_pwm_push_samples(const int16_t *samples, size_t count);

/* Diagnostics. */
uint32_t audio_pwm_underrun_count(void);
uint32_t audio_pwm_buffer_level(void);

#endif
