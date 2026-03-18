#ifndef SMB2350_AUDIO_PWM_H
#define SMB2350_AUDIO_PWM_H

#include <stdint.h>

// GP5 is the audio output pin. The signal is PWM-based and intended
// to be passed through a simple resistor or optional passive RC filter.
#define SMB2350_AUDIO_PIN 5u

void audio_pwm_init(uint32_t tone_hz);

#endif
