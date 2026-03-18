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
    /* Exp A/B: audio pipeline shape + sample correctness */
    uint64_t report_samples_drained = 0;
    uint64_t report_samples_dropped = 0;
    uint64_t report_apu_internal_dropped = 0;
    bool     report_saw_nonzero_sample = false;
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
    audio_pwm_init(48000);

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

            /* Drain APU PCM samples into the PWM audio backend.
             * The APU produces ~800 samples per frame at 48 kHz / 60 fps. */
            {
                static int16_t pcm_tmp[256];
                size_t n;
                while ((n = nes_audio_read_samples(&emulator_video.nes, pcm_tmp,
                                                   sizeof(pcm_tmp) / sizeof(pcm_tmp[0]))) > 0) {
                    size_t pushed = audio_pwm_push_samples(pcm_tmp, n);
                    report_samples_drained += n;
                    report_samples_dropped += (n - pushed);
                    /* Exp B: detect whether any non-silent samples exist. */
                    if (!report_saw_nonzero_sample) {
                        for (size_t i = 0; i < n; ++i) {
                            if (pcm_tmp[i] != 0) {
                                report_saw_nonzero_sample = true;
                                break;
                            }
                        }
                    }
                }
                report_apu_internal_dropped += emulator_video.nes.apu.dropped_samples;
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
                    " src_nonzero=%u visible=%u gray=%u white=%u colors=%u range=%02x-%02x first_visible=%d,%d"
                    " audio_buf=%u audio_underruns=%u\n",
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
                    emulator_video.last_frame_first_visible_y,
                    audio_pwm_buffer_level(),
                    audio_pwm_underrun_count()
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

                /* Exp A: audio pipeline shape — how many samples were produced
                 * and pushed vs dropped over the last 60 NES frames.
                 * Exp B: report whether any non-silent PCM was seen.
                 * Expected at 60fps: drained≈800/frame, dropped=0, nonzero=1.
                 * If drained=0 → APU not producing (apu_step not running).
                 * If nonzero=0 → APU producing silence (channels muted/not init).
                 * If dropped>0  → PWM buffer backing up (unexpected at <60fps). */
                printf(
                    "audio diag:"
                    " drained=%llu dropped=%llu nonzero=%u"
                    " apu_int_drop=%llu apu_pcm_buf=%u"
                    " underruns=%u buf_level=%u"
                    " apu_samples=%llu apu_p0_en=%u apu_p0_lc=%u"
                    " $4015_writes=%llu $4015_last=%02x"
                    " $4003_writes=%llu fc_steps=%llu"
                    " clip=%llu\n",
                    report_samples_drained,
                    report_samples_dropped,
                    (unsigned)report_saw_nonzero_sample,
                    report_apu_internal_dropped,
                    emulator_video.nes.apu.pcm_count,
                    audio_pwm_underrun_count(),
                    audio_pwm_buffer_level(),
                    emulator_video.nes.apu.sample_count,
                    (unsigned)emulator_video.nes.apu.pulse[0].enabled,
                    (unsigned)emulator_video.nes.apu.pulse[0].length_counter,
                    emulator_video.nes.apu.register_write_count[0x15],
                    (unsigned)emulator_video.nes.apu.registers[0x15],
                    emulator_video.nes.apu.register_write_count[0x03],
                    emulator_video.nes.apu.frame_counter_steps,
                    emulator_video.nes.apu.clip_count
                );
                report_samples_drained = 0;
                report_samples_dropped = 0;
                report_apu_internal_dropped = 0;
                report_saw_nonzero_sample = false;
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

    /* 440 Hz square-wave test tone via PCM ring buffer. */
    {
        static int16_t s_tone_sample;
        uint32_t tone_phase = 0;
        const uint32_t tone_step = (uint32_t)((440ull << 32) / 48000u);
        while (true) {
            s_tone_sample = (tone_phase & 0x80000000u) ? 16384 : -16384;
            if (audio_pwm_push_samples(&s_tone_sample, 1) == 1) {
                tone_phase += tone_step;
            }
        }
    }
}
