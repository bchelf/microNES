#include "audio_pwm.h"
#include "core1_video.h"
#include "emulator_video_adapter.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/time.h"
#include "pico_input.h"
#include "video_ntsc.h"

#include "pico/stdlib.h"

#include <stdio.h>

#define SMB2350_PICO_OVERCLOCK_KHZ 252000u
#define SMB2350_PICO_OVERCLOCK_VREG VREG_VOLTAGE_1_20

int main(void) {
#if defined(SMB2350_PICO_VIDEO_MODE_EMULATOR)
    extern unsigned char smb2350_pico_embedded_rom[];
    extern unsigned int smb2350_pico_embedded_rom_len;
    static PicoEmulatorVideoAdapter emulator_video;
    uint64_t report_started_us = 0;
    uint64_t report_render_us = 0;
    uint64_t report_step_us = 0;
    uint64_t report_cpu_exec_us = 0;
    uint64_t report_ppu_step_us = 0;
    uint64_t report_ppu_render_us = 0;
    uint64_t report_bus_reads = 0;
    uint64_t report_bus_writes = 0;
    uint64_t report_ppu_cycles = 0;
    uint64_t report_c1_convert_us = 0;
    uint64_t report_c1_begin_wait_us = 0;
    uint64_t report_c1_frames = 0;
    uint32_t report_q_stall_count = 0;
    uint64_t report_q_stall_us = 0;
    Smb2350VideoNtscPerfStats report_video_stats = { 0 };
#endif

    vreg_set_voltage(SMB2350_PICO_OVERCLOCK_VREG);
    sleep_ms(10);
    if (!set_sys_clock_khz(SMB2350_PICO_OVERCLOCK_KHZ, true)) {
        panic("failed to set sys clock to %u kHz", SMB2350_PICO_OVERCLOCK_KHZ);
    }

    stdio_init_all();
    printf("sys clock: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    pico_input_init();
    video_ntsc_init();
    audio_pwm_init(440);

#if defined(SMB2350_PICO_VIDEO_MODE_EMULATOR)
    if (emulator_video_adapter_init(
            &emulator_video,
            smb2350_pico_embedded_rom,
            (size_t)smb2350_pico_embedded_rom_len)) {
        printf("video mode: emulator (dual-core)\n");

        // Seed the DMA with a blank frame, start scanout, then launch core 1.
        // Core 1 will begin_frame (returns immediately, video_started=true from
        // this point) and wait on the scanline queue.
        video_ntsc_begin_frame();
        video_ntsc_present();
        video_ntsc_start();

        core1_video_launch(k_emulator_video_palette_to_gray, 64);

        report_started_us = time_us_64();

        while (true) {
            nes_set_controller_state(&emulator_video.nes, 0, pico_input_read());
            if (!emulator_video_adapter_render_frame(&emulator_video)) {
                printf("emulator video failed: %s\n", emulator_video_adapter_last_error(&emulator_video));
                break;
            }
            if ((emulator_video.rendered_frames % 60u) == 0u) {
                uint64_t now_us = time_us_64();
                Smb2350VideoNtscPerfStats current_video_stats;
                Core1VideoStats c1_stats;
                ScanlineQueue *q = core1_video_get_queue();
                uint64_t delta_us = now_us - report_started_us;
                uint64_t delta_render_us =
                    emulator_video.profile_render_frame_us_total - report_render_us;
                uint64_t delta_step_us =
                    emulator_video.profile_step_scanline_us_total - report_step_us;
                uint64_t delta_cpu_exec_us =
                    emulator_video.nes.step_profile.cpu_exec_us_total - report_cpu_exec_us;
                uint64_t delta_ppu_step_us =
                    emulator_video.nes.step_profile.ppu_step_us_total - report_ppu_step_us;
                uint64_t delta_ppu_render_us =
                    emulator_video.nes.ppu.step_profile.render_us_total - report_ppu_render_us;
                uint64_t delta_bus_reads =
                    emulator_video.nes.step_profile.bus_read_count - report_bus_reads;
                uint64_t delta_bus_writes =
                    emulator_video.nes.step_profile.bus_write_count - report_bus_writes;
                uint64_t delta_ppu_cycles =
                    emulator_video.nes.ppu.step_profile.cycles_requested - report_ppu_cycles;
                double fps = delta_us != 0 ? (60.0 * 1000000.0) / (double)delta_us : 0.0;
                double frame_ms = delta_us / 60000.0;

                core1_video_get_stats(&c1_stats);
                video_ntsc_perf_get(&current_video_stats);

                uint64_t delta_c1_convert_us = c1_stats.convert_us_total - report_c1_convert_us;
                uint64_t delta_c1_begin_wait_us = c1_stats.frame_begin_wait_us_total - report_c1_begin_wait_us;
                uint64_t delta_c1_frames = c1_stats.frames_converted - report_c1_frames;
                uint32_t delta_q_stall_count = q->producer_stall_count - report_q_stall_count;
                uint64_t delta_q_stall_us = q->producer_stall_us_total - report_q_stall_us;

                printf(
                    "emu perf: frames=%llu fps=%.2f frame_ms=%.2f"
                    " step=%.2fms cpu=%.2fms ppu=%.2fms ppu_render=%.2fms ppu_other=%.2fms"
                    " c1_convert=%.2fms c1_wait=%.2fms c1_frames=%llu"
                    " q_stalls=%u q_stall_ms=%.2f"
                    " ntsc_wait=%.2fms ntsc_wait_max=%.2fms"
                    " bus_r=%llu bus_w=%llu ppu_cycles=%llu cpu_instr=%llu ppu_frame=%llu"
                    " src_nonzero=%u visible=%u gray=%u white=%u colors=%u range=%02x-%02x first_visible=%d,%d\n",
                    emulator_video.rendered_frames,
                    fps,
                    frame_ms,
                    delta_step_us / 60000.0,
                    delta_cpu_exec_us / 60000.0,
                    delta_ppu_step_us / 60000.0,
                    delta_ppu_render_us / 60000.0,
                    (delta_ppu_step_us - delta_ppu_render_us) / 60000.0,
                    delta_c1_frames > 0 ? (double)delta_c1_convert_us / ((double)delta_c1_frames * 1000.0) : 0.0,
                    delta_c1_frames > 0 ? (double)delta_c1_begin_wait_us / ((double)delta_c1_frames * 1000.0) : 0.0,
                    delta_c1_frames,
                    delta_q_stall_count,
                    delta_q_stall_us / 1000.0,
                    (current_video_stats.swap_wait_us_total - report_video_stats.swap_wait_us_total) / 60000.0,
                    (double)current_video_stats.swap_wait_us_max / 1000.0,
                    delta_bus_reads,
                    delta_bus_writes,
                    delta_ppu_cycles,
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
                report_cpu_exec_us = emulator_video.nes.step_profile.cpu_exec_us_total;
                report_ppu_step_us = emulator_video.nes.step_profile.ppu_step_us_total;
                report_ppu_render_us = emulator_video.nes.ppu.step_profile.render_us_total;
                report_bus_reads = emulator_video.nes.step_profile.bus_read_count;
                report_bus_writes = emulator_video.nes.step_profile.bus_write_count;
                report_ppu_cycles = emulator_video.nes.ppu.step_profile.cycles_requested;
                report_c1_convert_us = c1_stats.convert_us_total;
                report_c1_begin_wait_us = c1_stats.frame_begin_wait_us_total;
                report_c1_frames = c1_stats.frames_converted;
                report_q_stall_count = q->producer_stall_count;
                report_q_stall_us = q->producer_stall_us_total;
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
