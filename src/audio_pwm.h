#ifndef SMB2350_AUDIO_PWM_H
#define SMB2350_AUDIO_PWM_H

#include <stdint.h>

// GP2 is the first audio bring-up pin. The signal is PWM-based and intended
// to be passed through a simple resistor or optional passive RC filter.
#define SMB2350_AUDIO_PIN 2u

void audio_pwm_init(uint32_t tone_hz);

#endif
