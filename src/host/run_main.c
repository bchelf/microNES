#include "audio_sdl.h"
#include "frame_pacer.h"
#include "nes.h"
#include "smb1_bg_classifier.h"
#include "wav_write.h"
#include "window_sdl.h"

#include <SDL3/SDL.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HOST_DEFAULT_SCALE = 4,
    HOST_FPS_SAMPLE_MS = 1000,
    HOST_COARSE_SLEEP_THRESHOLD_NS = 2000000,
    HOST_COARSE_SLEEP_GUARD_NS = 250000,
    HOST_INPUT_POLL_INSTRUCTION_INTERVAL = 32,
};

typedef struct {
    const char *rom_path;
    int scale;
    bool enable_vsync;
    bool enable_color;
    bool enable_audio;
    bool throttled;
    bool apu_stats;
    bool apu_write_summary;
    bool transparent_bg;
    int opacity;
    uint64_t max_frames;
    const char *dump_wav_path;
    double dump_wav_seconds;
    uint8_t audio_mix_mask;
    ApuDebugTestTone test_tone;
} RunOptions;

typedef struct {
    int counts[8];
    bool dump_nametable;
} HostInputState;

static void print_usage(const char *argv0) {
    printf(
        "Usage: %s [rom_path] [--scale N] [--vsync] [--no-vsync] [--color] [--grayscale] [--audio] [--no-audio] [--throttled] [--unthrottled] [--max-frames N] [--audio-solo channel] [--audio-mute channel] [--apu-test-tone mode] [--apu-stats] [--apu-write-summary] [--transparent-bg] [--opacity 0-100] [--dump-wav path] [--dump-wav-seconds N]\n",
        argv0
    );
}

static bool parse_u64_arg(const char *text, uint64_t *value_out) {
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return false;
    }

    *value_out = (uint64_t)parsed;
    return true;
}

static bool parse_int_arg(const char *text, int *value_out) {
    char *end = NULL;
    long parsed = strtol(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool parse_double_arg(const char *text, double *value_out) {
    char *end = NULL;
    double parsed = strtod(text, &end);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return false;
    }

    *value_out = parsed;
    return true;
}

static bool parse_audio_channel_mask(const char *text, uint8_t *mask_out) {
    if (strcmp(text, "pulse1") == 0) {
        *mask_out = APU_DEBUG_MASK_PULSE1;
    } else if (strcmp(text, "pulse2") == 0) {
        *mask_out = APU_DEBUG_MASK_PULSE2;
    } else if (strcmp(text, "triangle") == 0) {
        *mask_out = APU_DEBUG_MASK_TRIANGLE;
    } else if (strcmp(text, "noise") == 0) {
        *mask_out = APU_DEBUG_MASK_NOISE;
    } else if (strcmp(text, "dmc") == 0) {
        *mask_out = APU_DEBUG_MASK_DMC;
    } else {
        return false;
    }
    return true;
}

static bool parse_test_tone_mode(const char *text, ApuDebugTestTone *mode_out) {
    if (strcmp(text, "off") == 0 || strcmp(text, "none") == 0) {
        *mode_out = APU_DEBUG_TEST_TONE_NONE;
    } else if (strcmp(text, "pulse1") == 0) {
        *mode_out = APU_DEBUG_TEST_TONE_PULSE1;
    } else if (strcmp(text, "triangle") == 0) {
        *mode_out = APU_DEBUG_TEST_TONE_TRIANGLE;
    } else {
        return false;
    }
    return true;
}

static bool parse_args(int argc, char **argv, RunOptions *options) {
    bool rom_set = false;

    options->rom_path = "roms/smb1.nes";
    options->scale = HOST_DEFAULT_SCALE;
    options->enable_vsync = false;
    options->enable_color = true;
    options->enable_audio = true;
    options->throttled = true;
    options->apu_stats = false;
    options->apu_write_summary = false;
    options->transparent_bg = false;
    options->opacity = 100;
    options->max_frames = 0;
    options->dump_wav_path = NULL;
    options->dump_wav_seconds = 2.0;
    options->audio_mix_mask = APU_DEBUG_MASK_ALL;
    options->test_tone = APU_DEBUG_TEST_TONE_NONE;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return false;
        }
        if (strcmp(arg, "--scale") == 0) {
            if (i + 1 >= argc || !parse_int_arg(argv[i + 1], &options->scale) || options->scale <= 0) {
                fprintf(stderr, "--scale requires a positive integer\n");
                return false;
            }
            ++i;
            continue;
        }
        if (strcmp(arg, "--vsync") == 0) {
            options->enable_vsync = true;
            continue;
        }
        if (strcmp(arg, "--no-vsync") == 0) {
            options->enable_vsync = false;
            continue;
        }
        if (strcmp(arg, "--color") == 0) {
            options->enable_color = true;
            continue;
        }
        if (strcmp(arg, "--grayscale") == 0 || strcmp(arg, "--no-color") == 0) {
            options->enable_color = false;
            continue;
        }
        if (strcmp(arg, "--audio") == 0) {
            options->enable_audio = true;
            continue;
        }
        if (strcmp(arg, "--no-audio") == 0) {
            options->enable_audio = false;
            continue;
        }
        if (strcmp(arg, "--throttled") == 0) {
            options->throttled = true;
            continue;
        }
        if (strcmp(arg, "--unthrottled") == 0 || strcmp(arg, "--no-throttle") == 0) {
            options->throttled = false;
            continue;
        }
        if (strcmp(arg, "--max-frames") == 0) {
            if (i + 1 >= argc || !parse_u64_arg(argv[i + 1], &options->max_frames)) {
                fprintf(stderr, "--max-frames requires an integer value\n");
                return false;
            }
            ++i;
            continue;
        }
        if (strcmp(arg, "--audio-solo") == 0) {
            uint8_t mask;
            if (i + 1 >= argc || !parse_audio_channel_mask(argv[i + 1], &mask)) {
                fprintf(stderr, "--audio-solo requires one of: pulse1 pulse2 triangle noise dmc\n");
                return false;
            }
            options->audio_mix_mask = mask;
            ++i;
            continue;
        }
        if (strcmp(arg, "--audio-mute") == 0) {
            uint8_t mask;
            if (i + 1 >= argc || !parse_audio_channel_mask(argv[i + 1], &mask)) {
                fprintf(stderr, "--audio-mute requires one of: pulse1 pulse2 triangle noise dmc\n");
                return false;
            }
            options->audio_mix_mask = (uint8_t)(options->audio_mix_mask & (uint8_t)~mask);
            ++i;
            continue;
        }
        if (strcmp(arg, "--apu-test-tone") == 0) {
            if (i + 1 >= argc || !parse_test_tone_mode(argv[i + 1], &options->test_tone)) {
                fprintf(stderr, "--apu-test-tone requires one of: off pulse1 triangle\n");
                return false;
            }
            ++i;
            continue;
        }
        if (strcmp(arg, "--apu-stats") == 0) {
            options->apu_stats = true;
            continue;
        }
        if (strcmp(arg, "--apu-write-summary") == 0) {
            options->apu_write_summary = true;
            continue;
        }
        if (strcmp(arg, "--transparent-bg") == 0) {
            options->transparent_bg = true;
            continue;
        }
        if (strcmp(arg, "--no-transparent-bg") == 0) {
            options->transparent_bg = false;
            continue;
        }
        if (strcmp(arg, "--opacity") == 0) {
            if (i + 1 >= argc || !parse_int_arg(argv[i + 1], &options->opacity) ||
                options->opacity < 0 || options->opacity > 100) {
                fprintf(stderr, "--opacity requires an integer 0-100\n");
                return false;
            }
            ++i;
            continue;
        }
        if (strcmp(arg, "--dump-wav") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--dump-wav requires a path\n");
                return false;
            }
            options->dump_wav_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--dump-wav-seconds") == 0) {
            if (i + 1 >= argc || !parse_double_arg(argv[i + 1], &options->dump_wav_seconds) ||
                options->dump_wav_seconds <= 0.0) {
                fprintf(stderr, "--dump-wav-seconds requires a positive number\n");
                return false;
            }
            ++i;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }
        if (!rom_set) {
            options->rom_path = arg;
            rom_set = true;
            continue;
        }

        fprintf(stderr, "Unexpected positional argument: %s\n", arg);
        return false;
    }

    return true;
}

static double host_stats_average_abs(const ApuDebugSampleStats *stats) {
    if (stats->sample_count == 0) {
        return 0.0;
    }
    return (double)stats->abs_sum / (double)stats->sample_count;
}

static void host_print_apu_stats(const ApuDebugReport *report) {
    printf(
        "apu: mix_mask=%02X test_tone=%s clips=%" PRIu64 " dropped_pcm=%" PRIu64 "\n",
        report->mix_enable_mask,
        apu_debug_test_tone_name(report->test_tone),
        report->clip_count,
        report->dropped_samples
    );
    for (int i = 0; i < APU_DEBUG_CHANNEL_COUNT; ++i) {
        const ApuDebugSampleStats *stats = &report->channel_stats[i];
        printf(
            "  %-8s samples=%" PRIu64 " nonzero=%" PRIu64 " min=%d max=%d avg_abs=%.2f\n",
            apu_debug_channel_name((ApuDebugChannel)i),
            stats->sample_count,
            stats->nonzero_sample_count,
            (int)stats->min_value,
            (int)stats->max_value,
            host_stats_average_abs(stats)
        );
    }
    printf(
        "  pulse1 state: en=%d len=%u timer=%u duty=%u step=%u env=%u\n",
        report->pulse[0].enabled ? 1 : 0,
        report->pulse[0].length_counter,
        report->pulse[0].timer_period,
        report->pulse[0].duty,
        report->pulse[0].duty_step,
        report->pulse[0].envelope_decay
    );
    printf(
        "  pulse2 state: en=%d len=%u timer=%u duty=%u step=%u env=%u\n",
        report->pulse[1].enabled ? 1 : 0,
        report->pulse[1].length_counter,
        report->pulse[1].timer_period,
        report->pulse[1].duty,
        report->pulse[1].duty_step,
        report->pulse[1].envelope_decay
    );
    printf(
        "  triangle state: en=%d len=%u lin=%u reload=%u timer=%u seq=%u\n",
        report->triangle.enabled ? 1 : 0,
        report->triangle.length_counter,
        report->triangle.linear_counter,
        report->triangle.linear_reload_value,
        report->triangle.timer_period,
        report->triangle.sequence_step
    );
    printf(
        "  noise state: en=%d len=%u vol=%u period_idx=%u timer=%u shift=%04X\n",
        report->noise.enabled ? 1 : 0,
        report->noise.length_counter,
        report->noise.envelope_decay,
        report->noise.period_index,
        report->noise.timer_period,
        report->noise.shift_register
    );
}

static void host_print_apu_write_summary(const ApuDebugReport *report) {
    static const uint16_t k_interesting_registers[] = {
        0x4000u, 0x4001u, 0x4002u, 0x4003u,
        0x4004u, 0x4005u, 0x4006u, 0x4007u,
        0x4008u, 0x400au, 0x400bu,
        0x400cu, 0x400eu, 0x400fu,
        0x4010u, 0x4011u, 0x4012u, 0x4013u,
        0x4015u, 0x4017u,
    };

    printf("apu writes:\n");
    for (size_t i = 0; i < sizeof(k_interesting_registers) / sizeof(k_interesting_registers[0]); ++i) {
        uint16_t addr = k_interesting_registers[i];
        const ApuDebugRegisterSummary *summary = &report->register_summary[addr - 0x4000u];
        if (summary->write_count == 0) {
            continue;
        }
        printf("  %04X count=%" PRIu64 " last=%02X\n", addr, summary->write_count, summary->last_value);
    }
}

static int host_button_index_for_scancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_W:
        return 4;
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_S:
        return 5;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_A:
        return 6;
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_D:
        return 7;
    case SDL_SCANCODE_L:
        return 0;
    case SDL_SCANCODE_K:
        return 1;
    case SDL_SCANCODE_TAB:
    case SDL_SCANCODE_RSHIFT:
        return 2;
    case SDL_SCANCODE_RETURN:
        return 3;
    default:
        return -1;
    }
}

static uint8_t host_button_mask_for_index(int index) {
    static const uint8_t k_button_masks[8] = {
        NES_BUTTON_A,
        NES_BUTTON_B,
        NES_BUTTON_SELECT,
        NES_BUTTON_START,
        NES_BUTTON_UP,
        NES_BUTTON_DOWN,
        NES_BUTTON_LEFT,
        NES_BUTTON_RIGHT,
    };

    return (index >= 0 && index < 8) ? k_button_masks[index] : 0;
}

static void host_update_button_state(HostInputState *input, SDL_Scancode scancode, bool down) {
    int index = host_button_index_for_scancode(scancode);

    if (index < 0) {
        return;
    }

    if (down) {
        ++input->counts[index];
    } else if (input->counts[index] > 0) {
        --input->counts[index];
    }
}

static NesControllerState host_build_controller_state(const HostInputState *input) {
    NesControllerState state = { 0 };
    bool up_down;
    bool down_down;
    bool left_down;
    bool right_down;

    for (int i = 0; i < 8; ++i) {
        if (input->counts[i] > 0) {
            state.buttons |= host_button_mask_for_index(i);
        }
    }

    up_down = (state.buttons & NES_BUTTON_UP) != 0;
    down_down = (state.buttons & NES_BUTTON_DOWN) != 0;
    left_down = (state.buttons & NES_BUTTON_LEFT) != 0;
    right_down = (state.buttons & NES_BUTTON_RIGHT) != 0;
    if (up_down && down_down) {
        state.buttons &= (uint8_t)~(NES_BUTTON_UP | NES_BUTTON_DOWN);
    }
    if (left_down && right_down) {
        state.buttons &= (uint8_t)~(NES_BUTTON_LEFT | NES_BUTTON_RIGHT);
    }

    return state;
}

static bool host_process_events(bool *running, HostInputState *input) {
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            *running = false;
            return true;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_F1) {
                    input->dump_nametable = true;
                } else {
                    host_update_button_state(input, event.key.scancode, true);
                }
            }
            break;
        case SDL_EVENT_KEY_UP:
            host_update_button_state(input, event.key.scancode, false);
            break;
        default:
            break;
        }
    }

    return true;
}

static uint64_t host_now_ns(void) {
    return (uint64_t)SDL_GetTicksNS();
}

static void host_wait_until_ns(uint64_t deadline_ns) {
    uint64_t now_ns = host_now_ns();

    while (now_ns < deadline_ns) {
        uint64_t remaining_ns = deadline_ns - now_ns;

        if (remaining_ns > HOST_COARSE_SLEEP_THRESHOLD_NS) {
            SDL_DelayNS(remaining_ns - HOST_COARSE_SLEEP_GUARD_NS);
        } else {
            SDL_DelayPrecise(remaining_ns);
        }

        now_ns = host_now_ns();
    }
}

int main(int argc, char **argv) {
    RunOptions options;
    HostSdlWindow *window = NULL;
    HostAudioSdl *audio = NULL;
    HostInputState input = { { 0 } };
    MicronesFramePacer pacer;
    MicronesFramePacerStats pacer_stats;
    Nes nes;
    bool running = true;
    uint64_t presented_frames = 0;
    uint64_t stats_window_start_ns = 0;
    uint64_t now_ns;
    int16_t audio_samples[2048];
    ApuDebugReport apu_report;
    int16_t *wav_samples = NULL;
    size_t wav_capacity = 0;
    size_t wav_count = 0;

    if (!parse_args(argc, argv, &options)) {
        return 1;
    }

    window = host_sdl_window_create(
        "micrones - SMB1",
        options.scale,
        options.enable_vsync,
        options.enable_color,
        options.transparent_bg,
        options.opacity
    );
    if (window == NULL) {
        fprintf(stderr, "SDL window init failed: %s\n", host_sdl_window_last_error());
        return 2;
    }

    nes_init(&nes);
    if (!nes_load_cartridge_file(&nes, options.rom_path)) {
        fprintf(stderr, "ROM load failed: %s\n", nes_last_error(&nes));
        host_sdl_window_destroy(window);
        return 3;
    }

    nes_reset(&nes);
    if (options.transparent_bg) {
        ppu_set_bg_tile_classifier(&nes.ppu, smb1_bg_tile_is_interactive, NULL);
    }
    nes_audio_set_mix_enable_mask(&nes, options.audio_mix_mask);
    nes_audio_set_test_tone(&nes, options.test_tone);
    nes_audio_debug_reset_metrics(&nes);
    audio = host_audio_sdl_create((int)nes_audio_sample_rate(&nes), options.enable_audio);
    if (audio == NULL) {
        fprintf(stderr, "SDL audio init failed: %s\n", host_audio_sdl_last_error());
        host_sdl_window_destroy(window);
        nes_destroy(&nes);
        return 4;
    }

    now_ns = host_now_ns();
    micrones_frame_pacer_init(&pacer, options.throttled, now_ns);
    stats_window_start_ns = now_ns;

    printf("ROM: %s\n", options.rom_path);
    printf("window scale: %d\n", options.scale);
    printf("pacing: %s\n", options.throttled ? "throttled" : "unthrottled");
    printf("target fps: %.4f\n", micrones_frame_pacer_target_fps());
    printf("vsync: %s\n", options.enable_vsync ? "on" : "off");
    printf("display mode: %s\n", options.enable_color ? "color" : "grayscale");
    if (options.transparent_bg) {
        printf("opacity: %d%%\n", options.opacity);
    }
    printf("audio: %s", host_audio_sdl_is_enabled(audio) ? "on" : "off");
    if (host_audio_sdl_is_enabled(audio)) {
        printf(" sample_rate=%u", nes_audio_sample_rate(&nes));
    }
    printf("\n");
    printf("audio mix mask: %02X\n", nes_audio_mix_enable_mask(&nes));
    printf("apu test tone: %s\n", apu_debug_test_tone_name(nes_audio_test_tone(&nes)));
    if (options.dump_wav_path != NULL) {
        wav_capacity = (size_t)((double)nes_audio_sample_rate(&nes) * options.dump_wav_seconds);
        wav_samples = (int16_t *)calloc(wav_capacity != 0 ? wav_capacity : 1u, sizeof(*wav_samples));
        if (wav_samples == NULL) {
            fprintf(stderr, "WAV buffer allocation failed\n");
            host_audio_sdl_destroy(audio);
            host_sdl_window_destroy(window);
            nes_destroy(&nes);
            return 5;
        }
        printf("wav dump: %s seconds=%.2f samples=%zu\n", options.dump_wav_path, options.dump_wav_seconds, wav_capacity);
    }
    if (options.max_frames != 0) {
        printf("max frames: %" PRIu64 "\n", options.max_frames);
    }

    while (running) {
        uint64_t target_completed_frame = nes.ppu.completed_frame_count + 1;
        char title[128];
        uint32_t instructions_until_input_poll = HOST_INPUT_POLL_INSTRUCTION_INTERVAL;

        if (!host_process_events(&running, &input)) {
            break;
        }
        nes_set_controller_state(&nes, 0, host_build_controller_state(&input));

        while (running && nes.ppu.completed_frame_count < target_completed_frame) {
            if (!nes_step_instruction(&nes)) {
                fprintf(stderr, "Emulation stopped: %s\n", nes_last_error(&nes));
                running = false;
                break;
            }
            if (--instructions_until_input_poll == 0) {
                if (!host_process_events(&running, &input)) {
                    break;
                }
                nes_set_controller_state(&nes, 0, host_build_controller_state(&input));
                instructions_until_input_poll = HOST_INPUT_POLL_INSTRUCTION_INTERVAL;
            }
        }
        if (!running) {
            break;
        }

        if (!host_sdl_window_upload_frame(window, nes.ppu.frame_buffer.pixels, NES_FRAME_WIDTH, NES_FRAME_HEIGHT)) {
            fprintf(stderr, "Frame upload failed: %s\n", host_sdl_window_last_error());
            running = false;
            break;
        }
        if (!host_sdl_window_present(window)) {
            fprintf(stderr, "Render failed: %s\n", host_sdl_window_last_error());
            running = false;
            break;
        }

        while (nes_audio_available_samples(&nes) > 0) {
            size_t sample_count = nes_audio_read_samples(&nes, audio_samples, sizeof(audio_samples) / sizeof(audio_samples[0]));
            size_t remaining;
            if (sample_count == 0) {
                break;
            }
            if (wav_samples != NULL && wav_count < wav_capacity) {
                remaining = wav_capacity - wav_count;
                if (remaining > sample_count) {
                    remaining = sample_count;
                }
                memcpy(&wav_samples[wav_count], audio_samples, remaining * sizeof(audio_samples[0]));
                wav_count += remaining;
            }
            if (!host_audio_sdl_submit_samples(audio, audio_samples, sample_count)) {
                fprintf(stderr, "Audio submit failed: %s\n", host_audio_sdl_last_error());
                running = false;
                break;
            }
        }
        if (!running) {
            break;
        }

        if (input.dump_nametable) {
            input.dump_nametable = false;
            fprintf(stderr, "=== nametable dump (frame %" PRIu64 ") ===\n", nes.ppu.completed_frame_count);
            fprintf(stderr, "     ");
            for (int col = 0; col < 32; ++col) {
                fprintf(stderr, "%02X ", col);
            }
            fprintf(stderr, "\n");
            for (int row = 0; row < 30; ++row) {
                fprintf(stderr, "r%02d: ", row);
                for (int col = 0; col < 32; ++col) {
                    fprintf(stderr, "%02X ", nes.ppu.nametables[row * 32 + col]);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "=== end nametable dump ===\n");
        }

        ++presented_frames;
        now_ns = host_now_ns();
        micrones_frame_pacer_frame_done(&pacer, now_ns);

        if (now_ns - stats_window_start_ns >= HOST_FPS_SAMPLE_MS * 1000000ull) {
            micrones_frame_pacer_get_stats(&pacer, now_ns, &pacer_stats);
#if MICRONES_ENABLE_PERF_LOG
            printf(
                "fps: %.2f/%.2f frame_ms(avg/last/worst)=%.3f/%.3f/%.3f late=%" PRIu64 " max_late=%.3fms frames=%" PRIu64 "\n",
                pacer_stats.measured_fps,
                pacer_stats.target_fps,
                pacer_stats.average_frame_ms,
                pacer_stats.last_frame_ms,
                pacer_stats.worst_frame_ms,
                pacer_stats.late_frame_count,
                pacer_stats.max_late_ms,
                presented_frames
            );
            if (host_audio_sdl_is_enabled(audio)) {
                printf(
                    "audio: queued=%.1fms submitted=%" PRIu64 " dropped=%" PRIu64 "\n",
                    ((double)host_audio_sdl_queued_bytes(audio) / 2.0 / (double)nes_audio_sample_rate(&nes)) * 1000.0,
                    host_audio_sdl_submitted_samples(audio),
                    host_audio_sdl_dropped_samples(audio)
                );
            }
#endif
            snprintf(
                title,
                sizeof(title),
                "micrones - SMB1 | %.2f fps | late=%" PRIu64,
                pacer_stats.measured_fps,
                pacer_stats.late_frame_count
            );
            host_sdl_window_set_title(window, title);
            stats_window_start_ns = now_ns;
        }

        if (options.max_frames != 0 && presented_frames >= options.max_frames) {
            break;
        }

        if (micrones_frame_pacer_should_wait(&pacer, now_ns, NULL)) {
            host_wait_until_ns(pacer.wait_until_ns);
            micrones_frame_pacer_note_wait_complete(&pacer, host_now_ns());
        }
    }

    if (wav_samples != NULL) {
        if (!host_write_wav_mono_s16(options.dump_wav_path, wav_samples, wav_count, nes_audio_sample_rate(&nes))) {
            fprintf(stderr, "WAV dump failed: %s\n", host_wav_write_last_error());
        } else {
            printf("wav written: %s samples=%zu\n", options.dump_wav_path, wav_count);
        }
    }
    if (options.apu_stats || options.apu_write_summary) {
        nes_audio_debug_get_report(&nes, &apu_report);
        if (options.apu_stats) {
            host_print_apu_stats(&apu_report);
        }
        if (options.apu_write_summary) {
            host_print_apu_write_summary(&apu_report);
        }
    }

    free(wav_samples);
    nes_destroy(&nes);
    host_audio_sdl_destroy(audio);
    host_sdl_window_destroy(window);
    return 0;
}
