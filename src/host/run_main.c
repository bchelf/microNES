#include "audio_sdl.h"
#include "frame_pacer.h"
#include "nes.h"
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
};

typedef struct {
    const char *rom_path;
    int scale;
    bool enable_vsync;
    bool enable_color;
    bool enable_audio;
    bool throttled;
    uint64_t max_frames;
} RunOptions;

typedef struct {
    int counts[8];
} HostInputState;

static void print_usage(const char *argv0) {
    printf(
        "Usage: %s [rom_path] [--scale N] [--vsync] [--no-vsync] [--color] [--grayscale] [--audio] [--no-audio] [--throttled] [--unthrottled] [--max-frames N]\n",
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

static bool parse_args(int argc, char **argv, RunOptions *options) {
    bool rom_set = false;

    options->rom_path = "roms/smb1.nes";
    options->scale = HOST_DEFAULT_SCALE;
    options->enable_vsync = false;
    options->enable_color = true;
    options->enable_audio = true;
    options->throttled = true;
    options->max_frames = 0;

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

    for (int i = 0; i < 8; ++i) {
        if (input->counts[i] > 0) {
            state.buttons |= host_button_mask_for_index(i);
        }
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
                host_update_button_state(input, event.key.scancode, true);
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
    Smb2350FramePacer pacer;
    Smb2350FramePacerStats pacer_stats;
    Nes nes;
    bool running = true;
    uint64_t presented_frames = 0;
    uint64_t stats_window_start_ns = 0;
    uint64_t now_ns;
    int16_t audio_samples[2048];

    if (!parse_args(argc, argv, &options)) {
        return 1;
    }

    window = host_sdl_window_create("smb2350 - SMB1", options.scale, options.enable_vsync, options.enable_color);
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
    audio = host_audio_sdl_create((int)nes_audio_sample_rate(&nes), options.enable_audio);
    if (audio == NULL) {
        fprintf(stderr, "SDL audio init failed: %s\n", host_audio_sdl_last_error());
        host_sdl_window_destroy(window);
        nes_destroy(&nes);
        return 4;
    }

    now_ns = host_now_ns();
    smb2350_frame_pacer_init(&pacer, options.throttled, now_ns);
    stats_window_start_ns = now_ns;

    printf("ROM: %s\n", options.rom_path);
    printf("window scale: %d\n", options.scale);
    printf("pacing: %s\n", options.throttled ? "throttled" : "unthrottled");
    printf("target fps: %.4f\n", smb2350_frame_pacer_target_fps());
    printf("vsync: %s\n", options.enable_vsync ? "on" : "off");
    printf("display mode: %s\n", options.enable_color ? "color" : "grayscale");
    printf("audio: %s", host_audio_sdl_is_enabled(audio) ? "on" : "off");
    if (host_audio_sdl_is_enabled(audio)) {
        printf(" sample_rate=%u", nes_audio_sample_rate(&nes));
    }
    printf("\n");
    if (options.max_frames != 0) {
        printf("max frames: %" PRIu64 "\n", options.max_frames);
    }

    while (running) {
        uint64_t target_completed_frame = nes.ppu.completed_frame_count + 1;
        char title[128];

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
            if (sample_count == 0) {
                break;
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

        ++presented_frames;
        now_ns = host_now_ns();
        smb2350_frame_pacer_frame_done(&pacer, now_ns);

        if (now_ns - stats_window_start_ns >= HOST_FPS_SAMPLE_MS * 1000000ull) {
            smb2350_frame_pacer_get_stats(&pacer, now_ns, &pacer_stats);
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
            snprintf(
                title,
                sizeof(title),
                "smb2350 - SMB1 | %.2f fps | late=%" PRIu64,
                pacer_stats.measured_fps,
                pacer_stats.late_frame_count
            );
            host_sdl_window_set_title(window, title);
            stats_window_start_ns = now_ns;
        }

        if (options.max_frames != 0 && presented_frames >= options.max_frames) {
            break;
        }

        if (smb2350_frame_pacer_should_wait(&pacer, now_ns, NULL)) {
            host_wait_until_ns(pacer.wait_until_ns);
            smb2350_frame_pacer_note_wait_complete(&pacer, host_now_ns());
        }
    }

    nes_destroy(&nes);
    host_audio_sdl_destroy(audio);
    host_sdl_window_destroy(window);
    return 0;
}
