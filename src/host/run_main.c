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
};

typedef struct {
    const char *rom_path;
    int scale;
    bool enable_vsync;
    bool enable_color;
    uint64_t max_frames;
} RunOptions;

typedef struct {
    int counts[8];
} HostInputState;

static void print_usage(const char *argv0) {
    printf("Usage: %s [rom_path] [--scale N] [--vsync] [--no-vsync] [--color] [--grayscale] [--max-frames N]\n", argv0);
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
    options->enable_vsync = true;
    options->enable_color = true;
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

int main(int argc, char **argv) {
    RunOptions options;
    HostSdlWindow *window = NULL;
    HostInputState input = { { 0 } };
    Nes nes;
    bool running = true;
    uint64_t presented_frames = 0;
    uint64_t fps_window_frames = 0;
    uint64_t fps_window_start_ms = 0;

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
    fps_window_start_ms = SDL_GetTicks();

    printf("ROM: %s\n", options.rom_path);
    printf("window scale: %d\n", options.scale);
    printf("vsync: %s\n", options.enable_vsync ? "on" : "off");
    printf("display mode: %s\n", options.enable_color ? "color" : "grayscale");
    if (options.max_frames != 0) {
        printf("max frames: %" PRIu64 "\n", options.max_frames);
    }

    while (running) {
        uint64_t target_completed_frame = nes.ppu.completed_frame_count + 1;
        uint64_t now_ms;
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

        ++presented_frames;
        ++fps_window_frames;
        now_ms = SDL_GetTicks();
        if (now_ms - fps_window_start_ms >= HOST_FPS_SAMPLE_MS) {
            double seconds = (double)(now_ms - fps_window_start_ms) / 1000.0;
            double fps = seconds > 0.0 ? (double)fps_window_frames / seconds : 0.0;

            printf("fps: %.2f frames=%" PRIu64 " emu_frames=%" PRIu64 "\n", fps, presented_frames, nes.ppu.completed_frame_count);
            snprintf(title, sizeof(title), "smb2350 - SMB1 | %.2f fps", fps);
            host_sdl_window_set_title(window, title);
            fps_window_frames = 0;
            fps_window_start_ms = now_ms;
        }

        if (options.max_frames != 0 && presented_frames >= options.max_frames) {
            break;
        }
    }

    nes_destroy(&nes);
    host_sdl_window_destroy(window);
    return 0;
}
