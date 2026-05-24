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
#include "rom_source_flash_cache.h"
#include "rom_source.h"
#include "rom_source_sd.h"
#if defined(MICRONES_PICO_VIDEO_BACKEND_TFT)
#include "display/video_tft.h"
#endif

#include "pico/stdlib.h"

#include <stdio.h>

#if MICRONES_PICO_ENABLE_FLASH_ROM_CACHE && defined(MICRONES_PICO_VIDEO_BACKEND_HDMI)
#include "rom_menu.h"
typedef struct {
    PicoEmulatorVideoAdapter *adapter;
    NesFrameBuffer *fb;
} FlashProgressCtx;

static void flash_progress_cb(size_t done, size_t total, void *user) {
    FlashProgressCtx *ctx = (FlashProgressCtx *)user;
    int pct = (total > 0) ? (int)((done * 100u) / total) : 0;
    rom_menu_render_loading(ctx->fb, NULL, pct);
    emulator_video_adapter_present_framebuffer(ctx->adapter, ctx->fb);
}
#endif

int main(void) {
    const uint32_t audio_sample_rate = pico_audio_backend_preferred_sample_rate();
#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    static PicoEmulatorVideoAdapter emulator_video;
    static AppShell                 shell;
    static RomSource                sd_rom_source;
    static RomSource                rom_source;
    MicronesFramePacer frame_pacer;
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

    pico_input_init();
    pico_status_init();    /* lights power LED, arms reset-button edge detect */
    if (!pico_video_backend_init()) {
        printf("video backend init failed: %s\n", pico_video_backend_last_error());
        return 1;
    }
    pico_audio_backend_init(audio_sample_rate);

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR) && defined(MICRONES_PICO_VIDEO_BACKEND_HDMI)
    /* Bring HDMI up before SD scanning / ROM menu setup so a missing display
     * signal can be diagnosed independently from storage or cartridge load. */
    pico_video_backend_start_emulator();
#endif

#if defined(MICRONES_PICO_VIDEO_MODE_EMULATOR)
    /* Bring up the adapter with no cart loaded — the ROM picker (AppShell)
     * will load cartridges on demand from the SD card. */
    if (emulator_video_adapter_init_empty(&emulator_video)) {
        /* Initialize the SD-card-backed ROM source.  If the card is missing
         * or the volume can't be mounted, the menu still runs and shows
         * "No ROMs found" until the user inserts a card and we can refresh. */
        if (rom_source_sd_init(&sd_rom_source)) {
#if MICRONES_PICO_ENABLE_FLASH_ROM_CACHE
            if (!rom_source_flash_cache_init(&rom_source, &sd_rom_source)) {
                rom_source = sd_rom_source;
            } else {
#if defined(MICRONES_PICO_VIDEO_BACKEND_HDMI)
                static FlashProgressCtx s_progress_ctx;
                s_progress_ctx.adapter = &emulator_video;
                s_progress_ctx.fb = &shell.menu_fb;
                rom_source_flash_cache_set_progress(flash_progress_cb, &s_progress_ctx);
#endif
            }
#else
            rom_source = sd_rom_source;
#endif
        } else {
            /* Fallback: empty source so the menu renders the "no ROMs"
             * screen.  Hot-insert support would re-init the SD source on
             * an explicit user trigger; not implemented here. */
            rom_source_make_empty(&rom_source);
        }

        app_shell_init(&shell, &rom_source, &emulator_video.nes);

#if !defined(MICRONES_PICO_VIDEO_BACKEND_HDMI)
        pico_video_backend_start_emulator();
#endif
        micrones_frame_pacer_init(&frame_pacer, true, micrones_pico_clock_now_ns());

#if MICRONES_ENABLE_PERF_LOG
        report_started_us = time_us_64();
#endif

        bool reset_button_was_down = pico_status_reset_button_down();

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

            /* Front-panel reset button (v0.1 PCB).  Turn the power LED off
             * while the button is held, then restore it and reset the current
             * game on release.  In the menu the reset request is a no-op, but
             * the LED still gives button feedback. */
            bool reset_button_down = pico_status_reset_button_down();
            if (reset_button_down) {
                pico_status_set_led(false);
            } else if (reset_button_was_down) {
                pico_status_set_led(true);
                app_shell_request_reset(&shell);
            }
            reset_button_was_down = reset_button_down;

            AppShellFrame frame = app_shell_begin_frame(&shell, input_pair.players[0]);

            if (frame.just_entered_run) {
                micrones_frame_pacer_init(&frame_pacer, true, micrones_pico_clock_now_ns());
            }

            if (!frame.stepping_nes) {
                emulator_video_adapter_present_framebuffer(
                    &emulator_video, app_shell_menu_framebuffer(&shell));
                micrones_frame_pacer_frame_done(&frame_pacer,
                                                micrones_pico_clock_now_ns());
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
#elif defined(MICRONES_PICO_VIDEO_BACKEND_HDMI)
            {
                if (!emulator_video_adapter_step_frame(&emulator_video)) {
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

                emulator_video_adapter_present_frame(&emulator_video);
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
        }
        pico_video_backend_start_test_pattern();
        while (true) {
            tight_loop_contents();
        }
    }

#endif

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
