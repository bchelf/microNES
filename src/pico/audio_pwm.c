#include "audio_pwm.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "pico/time.h"

enum {
    AUDIO_PWM_WRAP = 255,
    AUDIO_SAMPLE_RATE = 22050,
    AUDIO_TABLE_SIZE = 32,
};

static struct repeating_timer audio_timer;
static const uint8_t audio_table[AUDIO_TABLE_SIZE] = {
    128, 152, 176, 198, 218, 234, 246, 253,
    255, 253, 246, 234, 218, 198, 176, 152,
    128, 104, 80, 58, 38, 22, 10, 3,
    1, 3, 10, 22, 38, 58, 80, 104,
};
static uint phase;
static uint phase_step;

static bool audio_timer_cb(struct repeating_timer *timer) {
    (void)timer;

    phase += phase_step;
    pwm_set_gpio_level(SMB2350_AUDIO_PIN, audio_table[(phase >> 24) & (AUDIO_TABLE_SIZE - 1)]);
    return true;
}

void audio_pwm_init(uint32_t tone_hz) {
    gpio_set_function(SMB2350_AUDIO_PIN, GPIO_FUNC_PWM);
    uint audio_slice = pwm_gpio_to_slice_num(SMB2350_AUDIO_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv(&config, 1.0f);
    pwm_config_set_wrap(&config, AUDIO_PWM_WRAP);
    pwm_init(audio_slice, &config, true);

    // The PWM carrier runs at clk_sys / 256, well above the audible tone.
    pwm_set_gpio_level(SMB2350_AUDIO_PIN, AUDIO_PWM_WRAP / 2);

    phase = 0;
    phase_step = (uint)(((uint64_t)tone_hz << 24) / AUDIO_SAMPLE_RATE);
    add_repeating_timer_us(-(1000000 / AUDIO_SAMPLE_RATE), audio_timer_cb, NULL, &audio_timer);
}
