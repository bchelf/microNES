#include "audio_pwm.h"
#include "emulator_video_adapter.h"
#include "pico/time.h"
#include "video_ntsc.h"

#include "pico/stdlib.h"

#include <stdio.h>

int main(void) {
#if defined(SMB2350_PICO_VIDEO_MODE_EMULATOR)
    extern unsigned char smb2350_pico_embedded_rom[];
    extern unsigned int smb2350_pico_embedded_rom_len;
    PicoEmulatorVideoAdapter emulator_video;
    uint64_t report_started_us = 0;
    uint64_t report_render_us = 0;
    uint64_t report_step_us = 0;
    uint64_t report_convert_us = 0;
    Smb2350VideoNtscPerfStats report_video_stats = { 0 };
#endif

    stdio_init_all();

    video_ntsc_init();
    audio_pwm_init(440);

#if defined(SMB2350_PICO_VIDEO_MODE_EMULATOR)
    if (emulator_video_adapter_init(
            &emulator_video,
            smb2350_pico_embedded_rom,
            (size_t)smb2350_pico_embedded_rom_len)) {
        printf("video mode: emulator\n");
        video_ntsc_begin_frame();
        video_ntsc_present();
        video_ntsc_start();
        report_started_us = time_us_64();

        while (true) {
            if (!emulator_video_adapter_render_frame(&emulator_video)) {
                printf("emulator video failed: %s\n", emulator_video_adapter_last_error(&emulator_video));
                break;
            }
            if ((emulator_video.rendered_frames % 60u) == 0u) {
                uint64_t now_us = time_us_64();
                Smb2350VideoNtscPerfStats current_video_stats;
                uint64_t delta_us = now_us - report_started_us;
                uint64_t delta_render_us =
                    emulator_video.profile_render_frame_us_total - report_render_us;
                uint64_t delta_step_us =
                    emulator_video.profile_step_scanline_us_total - report_step_us;
                uint64_t delta_convert_us =
                    emulator_video.profile_convert_scanline_us_total - report_convert_us;
                double fps = delta_us != 0 ? (60.0 * 1000000.0) / (double)delta_us : 0.0;
                double frame_ms = delta_us / 60000.0;

                video_ntsc_perf_get(&current_video_stats);
                printf(
                    "emu perf: frames=%llu fps=%.2f frame_ms=%.2f render=%.2fms step=%.2fms convert=%.2fms wait=%.2fms wait_max=%.2fms cpu_instr=%llu ppu_frame=%llu src_nonzero=%u visible=%u gray=%u white=%u colors=%u range=%02x-%02x first_visible=%d,%d\n",
                    emulator_video.rendered_frames,
                    fps,
                    frame_ms,
                    delta_render_us / 60000.0,
                    delta_step_us / 60000.0,
                    delta_convert_us / 60000.0,
                    (current_video_stats.swap_wait_us_total - report_video_stats.swap_wait_us_total) / 60000.0,
                    (double)(current_video_stats.swap_wait_us_max - report_video_stats.swap_wait_us_max) / 1000.0,
                    emulator_video.nes.stats.instruction_count,
                    emulator_video.nes.ppu.frame_count,
                    emulator_video.last_frame_source_nonzero_pixels,
                    emulator_video.last_frame_visible_nonblack_pixels,
                    emulator_video.last_frame_visible_gray_pixels,
                    emulator_video.last_frame_visible_white_pixels,
                    emulator_video.last_frame_unique_color_count,
                    emulator_video.last_frame_min_pixel,
                    emulator_video.last_frame_max_pixel,
                    emulator_video.last_frame_first_visible_x,
                    emulator_video.last_frame_first_visible_y
                );
                report_started_us = now_us;
                report_render_us = emulator_video.profile_render_frame_us_total;
                report_step_us = emulator_video.profile_step_scanline_us_total;
                report_convert_us = emulator_video.profile_convert_scanline_us_total;
                report_video_stats = current_video_stats;
            }
        }
        video_ntsc_build_test_pattern_frame();
        video_ntsc_start();
        while (true) {
            tight_loop_contents();
        }
    }

    printf("video mode: emulator init failed, falling back to test pattern\n");
    printf("reason: %s\n", emulator_video_adapter_last_error(&emulator_video));
#endif

    printf("video mode: test pattern\n");
    video_ntsc_build_test_pattern_frame();
    video_ntsc_start();

    while (true) {
        tight_loop_contents();
    }
}
