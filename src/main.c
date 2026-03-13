#include "audio_pwm.h"
#include "video_ntsc.h"

#include "pico/stdlib.h"

int main(void) {
    stdio_init_all();

    video_ntsc_init();
    video_ntsc_start();

    audio_pwm_init(440);

    while (true) {
        tight_loop_contents();
    }
}
