#include "app_shell.h"
#include "clock_config.h"
#include "emulator_video_adapter.h"
#include "frame_pacer.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico_audio_backend.h"
#include "pico_time.h"
#include "pico_video_backend.h"
#include "pico_input.h"
#include "pico_status.h"
#include "rom_source.h"
#include "rom_source_sd.h"
#include "sd_spi.h"
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
#include "display/video_tft.h"
#endif

#include "pico/stdlib.h"

#include <stdio.h>

typedef struct {
    uint64_t started_us;
    uint64_t frames;
    uint64_t presented;
} PicoFpsLogger;

static const char *sd_card_type_name(SdCardType type) {
    switch (type) {
    case SD_TYPE_SDV1: return "SDv1";
    case SD_TYPE_SDV2: return "SDv2";
    case SD_TYPE_SDHC: return "SDHC/SDXC";
    default: return "none";
    }
}

static void log_rom_source_entries(RomSource *source) {
    if (source == NULL || source->count == NULL || source->entry == NULL) {
        printf("ROM list: source unavailable\n");
        return;
    }

    size_t count = source->count(source);
    printf("ROM list: %u entr%s\n",
           (unsigned)count,
           count == 1u ? "y" : "ies");
    for (size_t i = 0; i < count; ++i) {
        const RomSourceEntry *entry = source->entry(source, i);
        if (entry == NULL) {
            continue;
        }
        printf("ROM list: [%u] \"%s\" size=%lu mapper=%u supported=%u prg=%lu chr=%lu battery=%u\n",
               (unsigned)i,
               entry->name,
               (unsigned long)entry->file_size,
               (unsigned)entry->mapper,
               entry->supported ? 1u : 0u,
               (unsigned long)entry->prg_size,
               (unsigned long)entry->chr_size,
               entry->has_battery ? 1u : 0u);
    }
}

static void pico_fps_logger_init(PicoFpsLogger *logger, uint64_t now_us,
                                 uint64_t frames, uint64_t presented) {
    logger->started_us = now_us;
    logger->frames = frames;
    logger->presented = presented;
}

static void pico_fps_logger_maybe_report(PicoFpsLogger *logger,
                                         const PicoEmulatorVideoAdapter *adapter) {
    uint64_t now_us = time_us_64();
    uint64_t elapsed_us = now_us - logger->started_us;
    if (elapsed_us < 1000000ull) {
        return;
    }

    PicoVideoBackendStats stats;
    pico_video_backend_get_stats(&stats);

    uint64_t frame_delta = adapter->rendered_frames - logger->frames;
    uint64_t present_delta = stats.frames_presented - logger->presented;
    double seconds = (double)elapsed_us / 1000000.0;
    double fps = seconds > 0.0 ? (double)frame_delta / seconds : 0.0;
    double present_fps = seconds > 0.0 ? (double)present_delta / seconds : 0.0;

    printf("pico fps: frames=%llu fps=%.2f presented=%llu present_fps=%.2f audio_buf=%lu underruns=%lu\n",
           (unsigned long long)adapter->rendered_frames,
           fps,
           (unsigned long long)stats.frames_presented,
           present_fps,
           (unsigned long)pico_audio_backend_buffer_level(),
           (unsigned long)pico_audio_backend_underrun_count());

    logger->started_us = now_us;
    logger->frames = adapter->rendered_frames;
    logger->presented = stats.frames_presented;
}

int main(void) {
    const uint32_t audio_sample_rate = pico_audio_backend_preferred_sample_rate();
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    static PicoEmulatorVideoAdapter emulator_video;
    static AppShell                 shell;
    static RomSource                rom_source;
    MicronesFramePacer frame_pacer;
    PicoFpsLogger fps_logger;
#if MICRONES_ENABLE_PERF_LOG
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
    PicoVideoBackendStats report_video_stats = { 0 };
    /* Exp A/B: audio pipeline shape + sample correctness */
    uint64_t report_samples_drained = 0;
    uint64_t report_samples_dropped = 0;
    uint64_t report_apu_internal_dropped = 0;
    bool     report_saw_nonzero_sample = false;
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
    /* TFT profiling: per-component breakdown inside present_frame */
    uint64_t report_tft_convert_us = 0;
    uint64_t report_tft_diff_us = 0;
    uint64_t report_tft_spi_us = 0;
    uint64_t report_tft_spans = 0;
#endif
#endif /* MICRONES_ENABLE_PERF_LOG */
#endif

    /* System clock — speed controlled by MICRONES_SYS_CLK_MHZ in clock_config.h.
     * PIO is fixed at 'out pins, 4 [10]' (11 cycles); clkdiv scales to 14.318182 MHz.
     *   315 MHz: PLL 1260/(2×2), VREG 1.20 V, clkdiv=2.0 → 315M/22 = 14.318 MHz
     *   157.5 MHz: PLL 1260/(4×2), VREG 1.10 V, clkdiv=1.0 → 157.5M/11 = 14.318 MHz */
    vreg_set_voltage(MICRONES_VREG);
    sleep_ms(MICRONES_VREG_SETTLE_MS);
    set_sys_clock_pll(MICRONES_PLL_VCO_HZ, MICRONES_PLL_DIV1, MICRONES_PLL_DIV2);

    /* Explicitly re-configure clk_peri to follow the sys PLL at the new frequency.
     *
     * set_sys_clock_pll() only updates configured_freq[clk_sys].  It does NOT
     * update configured_freq[clk_peri], so clock_get_hz(clk_peri) still returns
     * the SDK-init value (125 MHz or 48 MHz depending on RP2350 defaults).
     *
     * SPI baud-rate selection calls clock_get_hz(clk_peri) to compute CPSDVSR/SCR.
     * If that value is wrong, the SPI clock is wildly off from the requested rate —
     * either far too slow (24 MHz from 48 MHz peri) or too fast (157.5 MHz from a
     * stale 125 MHz peri when actual is 315 MHz).  Either way the display runs wrong.
     *
     * Calling clock_configure here:
     *   1. Points the clk_peri aux mux directly at PLL_SYS (same source as clk_sys).
     *   2. Updates the SDK's configured_freq[clk_peri] to the true sys frequency.
     * After this, spi_init() will compute correct divisors.
     *
     * This is safe for the analog target too: analog uses PIO (clk_sys, not clk_peri)
     * and PWM (clk_sys).  No peripheral that the analog path uses is clk_peri-gated. */
    {
        const uint32_t sys_hz = MICRONES_PLL_VCO_HZ / (MICRONES_PLL_DIV1 * MICRONES_PLL_DIV2);
        clock_configure(clk_peri, 0,
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                        sys_hz, sys_hz);
    }

    stdio_init_all();
    sleep_ms(5000);
    printf("pico init: stdio ready\n");
    printf("pico init: power rails and system clock ready\n");
    printf("pico init: sys clock=%lu Hz peri clock=%lu Hz\n",
           (unsigned long)clock_get_hz(clk_sys),
           (unsigned long)clock_get_hz(clk_peri));

    printf("pico init: controller input\n");
    pico_input_init();
    printf("pico init: front panel (LED + reset button)\n");
    pico_status_init();
    printf("pico init: video backend %s\n", pico_video_backend_name());
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
    printf("pico init: TFT controller %s\n", video_tft_controller_name());
#endif
    if (!pico_video_backend_init()) {
        printf("video backend init failed: %s\n", pico_video_backend_last_error());
        return 1;
    }
    printf("pico init: video backend ready\n");
    printf("pico init: audio backend %s @ %lu Hz\n",
           pico_audio_backend_name(),
           (unsigned long)audio_sample_rate);
    pico_audio_backend_init(audio_sample_rate);
    printf("pico init: audio backend ready\n");

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    /* Bring up the adapter with no cart loaded — the ROM picker (AppShell)
     * will load cartridges on demand from the SD card. */
    printf("pico init: emulator core\n");
    if (emulator_video_adapter_init_empty(&emulator_video)) {
        printf("video mode: emulator (%s)\n", pico_video_backend_name());

        /* Initialize the SD-card-backed ROM source.  If the card is missing
         * or the volume can't be mounted, the menu still runs and shows
         * "No ROMs found" until the user inserts a card and we can refresh. */
        printf("pico init: SD card reader\n");
        if (rom_source_sd_init(&rom_source)) {
            printf("pico init: SD mounted type=%s blocks=%lu (%lu MiB), %u ROM(s) listed\n",
                   sd_card_type_name(sd_card_type()),
                   (unsigned long)sd_block_count(),
                   (unsigned long)(sd_block_count() / 2048u),
                   (unsigned)rom_source.count(&rom_source));
            log_rom_source_entries(&rom_source);
        } else {
            printf("SD: %s\n", rom_source_sd_last_error());
            sd_print_init_diag();
            /* Fallback: empty source so the menu renders the "no ROMs"
             * screen.  Hot-insert support would re-init the SD source on
             * an explicit user trigger; not implemented here. */
            rom_source_make_empty(&rom_source);
        }

        printf("pico init: app shell\n");
        app_shell_init(&shell, &rom_source, &emulator_video.nes);
        if (rom_source.count != NULL && rom_source.count(&rom_source) > 0u) {
            const RomSourceEntry *first = rom_source.entry != NULL
                ? rom_source.entry(&rom_source, 0u)
                : NULL;
            printf("pico init: autostart first SD ROM: %s\n",
                   first != NULL ? first->name : "(unknown)");
            if (!app_shell_launch_index(&shell, 0u)) {
                printf("pico init: autostart failed: %s\n", app_shell_status(&shell));
            }
        } else {
            printf("pico init: autostart skipped, no SD ROMs listed\n");
        }

        printf("pico init: video start\n");
        pico_video_backend_start_emulator();
        micrones_frame_pacer_init(&frame_pacer, true, micrones_pico_clock_now_ns());
        {
            PicoVideoBackendStats stats;
            pico_video_backend_get_stats(&stats);
            pico_fps_logger_init(&fps_logger, time_us_64(),
                                 emulator_video.rendered_frames,
                                 stats.frames_presented);
        }
        printf("pico init: complete\n");

#if MICRONES_ENABLE_PERF_LOG
        report_started_us = time_us_64();
#endif

        while (true) {
            /* Pace emulation to the NTSC NES frame cadence (~16.639 ms,
             * 60.10 Hz) so wall-clock audio production stays aligned with the
             * 48 kHz backend. On the TFT path, capture the next frame
             * deadline so the skip-present logic can decide whether the
             * current frame used too much of the available budget. */
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
            uint64_t frame_deadline_us;
#endif
            {
                uint64_t wait_until_ns = 0u;
                uint64_t now_ns = micrones_pico_clock_now_ns();

                if (micrones_frame_pacer_should_wait(&frame_pacer, now_ns, &wait_until_ns)) {
                    micrones_pico_sleep_until_ns(wait_until_ns);
                    micrones_frame_pacer_note_wait_complete(
                        &frame_pacer,
                        micrones_pico_clock_now_ns());
                }
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
                frame_deadline_us = (frame_pacer.next_deadline_ns + 999ull) / 1000ull;
#endif
            }

            /* Drive the AppShell state machine (menu vs. running).  When in
             * MENU the shell renders into its own framebuffer and we just
             * present that — no NES step, no audio.  When in RUNNING the
             * shell forwards (and combo-masks) the input and we run the
             * normal step+present path below.
             *
             * Player 1 drives the shell (menu navigation, exit combo).
             * Player 2 bypasses the shell and is wired directly to NES
             * controller port 2 — its state should not affect menu
             * navigation, but games that support two players (e.g. SMB1's
             * Luigi mode) will see it on $4017. */
            PicoControllerPair input_pair = pico_input_read_pair();

            /* Front-panel reset button (v0.1 PCB).  Acts like the original
             * NES RESET button: short press while running drops the user
             * back to the ROM picker.  In the menu it's a no-op (the shell
             * silently stays put). */
            if (pico_status_reset_button_pressed()) {
                app_shell_request_menu(&shell);
            }

            AppShellFrame frame = app_shell_begin_frame(&shell, input_pair.players[0]);
            if (!frame.stepping_nes) {
                emulator_video_adapter_present_framebuffer(
                    &emulator_video, app_shell_menu_framebuffer(&shell));
                micrones_frame_pacer_frame_done(&frame_pacer,
                                                micrones_pico_clock_now_ns());
                pico_fps_logger_maybe_report(&fps_logger, &emulator_video);
                continue;
            }
            nes_set_controller_state(&emulator_video.nes, 0, frame.forwarded);
            nes_set_controller_state(&emulator_video.nes, 1, input_pair.players[1]);

#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
            /* TFT path: step and present are separated so audio is pushed
             * between them.  This ensures audio is refilled on every NES
             * frame regardless of how long the display present takes. */
            if (!emulator_video_adapter_step_frame(&emulator_video)) {
                printf("emulator video failed: %s\n", emulator_video_adapter_last_error(&emulator_video));
                break;
            }

            /* Drain APU PCM samples into the audio backend immediately after
             * the NES step, before the display present. */
            {
                static int16_t pcm_tmp[256];
                size_t n;
                while ((n = nes_audio_read_samples(&emulator_video.nes, pcm_tmp,
                                                   sizeof(pcm_tmp) / sizeof(pcm_tmp[0]))) > 0) {
#if MICRONES_ENABLE_PERF_LOG
                    size_t pushed = pico_audio_backend_push_samples(pcm_tmp, n);
                    report_samples_drained += n;
                    report_samples_dropped += (n - pushed);
                    if (!report_saw_nonzero_sample) {
                        for (size_t i = 0; i < n; ++i) {
                            if (pcm_tmp[i] != 0) {
                                report_saw_nonzero_sample = true;
                                break;
                            }
                        }
                    }
#else
                    pico_audio_backend_push_samples(pcm_tmp, n);
#endif
                }
#if MICRONES_ENABLE_PERF_LOG
                report_apu_internal_dropped += emulator_video.nes.apu.dropped_samples;
#endif
            }

            /* Present the frame to the display — but skip if the previous
             * frame already overran the 16.67 ms budget.  Skipping one
             * present lets the loop recover to the next deadline without
             * accumulating drift.  The display drops during heavy scenes but
             * NES emulation and audio remain on the NTSC cadence. */
            {
                static bool s_skip_present = false;
                if (!s_skip_present) {
                    emulator_video_adapter_present_frame(&emulator_video);
                }
                /* Record whether this frame (step + audio + present) overran
                 * so the next iteration can decide whether to skip present. */
                s_skip_present = (time_us_64() > frame_deadline_us);
            }
#else
            /* Analog path: render_frame handles step + present together,
             * matching the original loop structure. */
            if (!emulator_video_adapter_render_frame(&emulator_video)) {
                printf("emulator video failed: %s\n", emulator_video_adapter_last_error(&emulator_video));
                break;
            }

            /* Drain APU PCM samples into the audio backend.
             * The APU produces about 799 samples per NTSC frame at 48 kHz. */
            {
                static int16_t pcm_tmp[256];
                size_t n;
                while ((n = nes_audio_read_samples(&emulator_video.nes, pcm_tmp,
                                                   sizeof(pcm_tmp) / sizeof(pcm_tmp[0]))) > 0) {
#if MICRONES_ENABLE_PERF_LOG
                    size_t pushed = pico_audio_backend_push_samples(pcm_tmp, n);
                    report_samples_drained += n;
                    report_samples_dropped += (n - pushed);
                    if (!report_saw_nonzero_sample) {
                        for (size_t i = 0; i < n; ++i) {
                            if (pcm_tmp[i] != 0) {
                                report_saw_nonzero_sample = true;
                                break;
                            }
                        }
                    }
#else
                    pico_audio_backend_push_samples(pcm_tmp, n);
#endif
                }
#if MICRONES_ENABLE_PERF_LOG
                report_apu_internal_dropped += emulator_video.nes.apu.dropped_samples;
#endif
            }
#endif
            micrones_frame_pacer_frame_done(&frame_pacer, micrones_pico_clock_now_ns());
            pico_fps_logger_maybe_report(&fps_logger, &emulator_video);

#if MICRONES_ENABLE_PERF_LOG
            if ((emulator_video.rendered_frames % 60u) == 0u) {
                uint64_t now_us = time_us_64();
                PicoVideoBackendStats backend_stats;
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

                pico_video_backend_get_stats(&backend_stats);
#if defined(MICRONES_PICO_VIDEO_BACKEND_ANALOG)
                {
                    uint64_t delta_c1_convert_us = backend_stats.convert_us_total - report_c1_convert_us;
                    uint64_t delta_c1_begin_wait_us = backend_stats.frame_begin_wait_us_total - report_c1_begin_wait_us;
                    uint64_t delta_c1_frames = backend_stats.frames_presented - report_c1_frames;
                    uint32_t delta_q_stall_count = backend_stats.queue_stall_count - report_q_stall_count;
                    uint64_t delta_q_stall_us = backend_stats.queue_stall_us_total - report_q_stall_us;

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
                        (backend_stats.swap_wait_us_total - report_video_stats.swap_wait_us_total) / 60000.0,
                        (double)backend_stats.swap_wait_us_max / 1000.0,
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
                        pico_audio_backend_buffer_level(),
                        pico_audio_backend_underrun_count()
                    );
                }
#else
                {
                    uint64_t delta_present_frames = backend_stats.frames_presented - report_c1_frames;
                    uint64_t delta_present_us = backend_stats.convert_us_total - report_c1_convert_us;
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
                    PicoTftStats tft_raw;
                    video_tft_get_stats(&tft_raw);
                    uint64_t delta_tft_convert_us = tft_raw.present_convert_us_total - report_tft_convert_us;
                    uint64_t delta_tft_diff_us = tft_raw.present_diff_us_total - report_tft_diff_us;
                    uint64_t delta_tft_spi_us = tft_raw.present_bus_us_total - report_tft_spi_us;
                    uint64_t delta_tft_spans = tft_raw.spans_sent_total - report_tft_spans;
#endif

                    printf(
                        "emu perf: backend=%s frames=%llu fps=%.2f frame_ms=%.2f"
                        " step=%.2fms cpu=%.2fms ppu=%.2fms ppu_render=%.2fms ppu_other=%.2fms"
                        " present=%.2fms present_frames=%llu present_max=%.2fms"
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
                        " convert=%.2fms diff=%.2fms spi=%.2fms spans_pf=%.0f"
#endif
                        " bus_r=%llu bus_w=%llu ppu_cycles=%llu cpu_instr=%llu ppu_frame=%llu"
                        " audio_buf=%u audio_underruns=%u\n",
                        pico_video_backend_name(),
                        emulator_video.rendered_frames,
                        fps,
                        frame_ms,
                        delta_step_us / 60000.0,
                        delta_cpu_exec_us / 60000.0,
                        delta_ppu_step_us / 60000.0,
                        delta_ppu_render_us / 60000.0,
                        (delta_ppu_step_us - delta_ppu_render_us) / 60000.0,
                        delta_present_frames > 0 ? (double)delta_present_us / ((double)delta_present_frames * 1000.0) : 0.0,
                        delta_present_frames,
                        (double)backend_stats.swap_wait_us_max / 1000.0,
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
                        delta_present_frames > 0 ? (double)delta_tft_convert_us / ((double)delta_present_frames * 1000.0) : 0.0,
                        delta_present_frames > 0 ? (double)delta_tft_diff_us / ((double)delta_present_frames * 1000.0) : 0.0,
                        delta_present_frames > 0 ? (double)delta_tft_spi_us / ((double)delta_present_frames * 1000.0) : 0.0,
                        delta_present_frames > 0 ? (double)delta_tft_spans / (double)delta_present_frames : 0.0,
#endif
                        delta_bus_reads,
                        delta_bus_writes,
                        delta_ppu_cycles,
                        emulator_video.nes.stats.instruction_count,
                        emulator_video.nes.ppu.frame_count,
                        pico_audio_backend_buffer_level(),
                        pico_audio_backend_underrun_count()
                    );
                }
#endif
                report_started_us = now_us;
                report_render_us = emulator_video.profile_render_frame_us_total;
                report_step_us = emulator_video.profile_step_scanline_us_total;
                report_cpu_exec_us = emulator_video.nes.step_profile.cpu_exec_us_total;
                report_ppu_step_us = emulator_video.nes.step_profile.ppu_step_us_total;
                report_ppu_render_us = emulator_video.nes.ppu.step_profile.render_us_total;
                report_bus_reads = emulator_video.nes.step_profile.bus_read_count;
                report_bus_writes = emulator_video.nes.step_profile.bus_write_count;
                report_ppu_cycles = emulator_video.nes.ppu.step_profile.cycles_requested;
                report_c1_convert_us = backend_stats.convert_us_total;
                report_c1_begin_wait_us = backend_stats.frame_begin_wait_us_total;
                report_c1_frames = backend_stats.frames_presented;
                report_q_stall_count = backend_stats.queue_stall_count;
                report_q_stall_us = backend_stats.queue_stall_us_total;
                report_video_stats.swap_wait_us_total = backend_stats.swap_wait_us_total;
                report_video_stats.swap_wait_us_max = backend_stats.swap_wait_us_max;
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
                {
                    PicoTftStats tft_raw;
                    video_tft_get_stats(&tft_raw);
                    report_tft_convert_us = tft_raw.present_convert_us_total;
                    report_tft_diff_us = tft_raw.present_diff_us_total;
                    report_tft_spi_us = tft_raw.present_bus_us_total;
                    report_tft_spans = tft_raw.spans_sent_total;
                }
#endif

                /* Exp A: audio pipeline shape — how many samples were produced
                 * and pushed vs dropped over the last 60 NES frames.
                 * Exp B: report whether any non-silent PCM was seen.
                 * Expected at NTSC cadence: drained≈799/frame, dropped=0, nonzero=1.
                 * If drained=0 → APU not producing (apu_step not running).
                 * If nonzero=0 → APU producing silence (channels muted/not init).
                 * If dropped>0  → PWM buffer backing up (unexpected at <60fps). */
                printf(
                    "audio diag:"
                    " drained=%llu dropped=%llu nonzero=%u"
                    " apu_int_drop=%llu apu_pcm_buf=%u"
                    " underruns=%u overruns=%u buf_level=%u"
                    " apu_samples=%llu apu_p0_en=%u apu_p0_lc=%u"
                    " $4015_writes=%llu $4015_last=%02x"
                    " $4003_writes=%llu fc_steps=%llu"
                    " clip=%llu\n",
                    report_samples_drained,
                    report_samples_dropped,
                    (unsigned)report_saw_nonzero_sample,
                    report_apu_internal_dropped,
                    emulator_video.nes.apu.pcm_count,
                    pico_audio_backend_underrun_count(),
                    pico_audio_backend_overrun_count(),
                    pico_audio_backend_buffer_level(),
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
#endif /* MICRONES_ENABLE_PERF_LOG */
        }
        pico_video_backend_start_test_pattern();
        while (true) {
            tight_loop_contents();
        }
    }

    printf("video mode: emulator init failed, falling back to test pattern\n");
    printf("reason: %s\n", emulator_video_adapter_last_error(&emulator_video));
#endif

    printf("video mode: test pattern\n");
    pico_video_backend_start_test_pattern();

    /* 440 Hz square-wave test tone via PCM ring buffer. */
    {
        static int16_t s_tone_sample;
        uint32_t tone_phase = 0;
        const uint32_t tone_step = (uint32_t)((440ull << 32) / audio_sample_rate);
        while (true) {
            s_tone_sample = (tone_phase & 0x80000000u) ? 16384 : -16384;
            if (pico_audio_backend_push_samples(&s_tone_sample, 1) == 1) {
                tone_phase += tone_step;
            }
        }
    }
}
