/*
 * micrones_test - headless test runner for AccuracyCoin and other test ROMs.
 *
 * Supports:
 *   --test-mode <rom>       : run in headless test mode (required)
 *   --input-script <file>   : script of FRAME,BUTTON inputs
 *   --screenshot-dir <dir>  : save PNG screenshots when script ends + settling
 *   --screenshot-at <frames>: comma-separated frame numbers to screenshot
 *   --settling-frames <N>   : extra frames after last script event (default 300)
 *   --max-frames <N>        : hard cap on total frames run
 *
 * Input script format: one entry per line, "FRAME,BUTTON"
 * Buttons: A B SELECT START UP DOWN LEFT RIGHT
 * The button is held for exactly one frame.
 */

#include "nes.h"
#include "png_write.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* NES palette for color screenshots */
static const uint8_t k_nes_palette[64][3] = {
    { 0x7c, 0x7c, 0x7c }, { 0x00, 0x00, 0xfc }, { 0x00, 0x00, 0xbc }, { 0x44, 0x28, 0xbc },
    { 0x94, 0x00, 0x84 }, { 0xa8, 0x00, 0x20 }, { 0xa8, 0x10, 0x00 }, { 0x88, 0x14, 0x00 },
    { 0x50, 0x30, 0x00 }, { 0x00, 0x78, 0x00 }, { 0x00, 0x68, 0x00 }, { 0x00, 0x58, 0x00 },
    { 0x00, 0x40, 0x58 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
    { 0xbc, 0xbc, 0xbc }, { 0x00, 0x78, 0xf8 }, { 0x00, 0x58, 0xf8 }, { 0x68, 0x44, 0xfc },
    { 0xd8, 0x00, 0xcc }, { 0xe4, 0x00, 0x58 }, { 0xf8, 0x38, 0x00 }, { 0xe4, 0x5c, 0x10 },
    { 0xac, 0x7c, 0x00 }, { 0x00, 0xb8, 0x00 }, { 0x00, 0xa8, 0x00 }, { 0x00, 0xa8, 0x44 },
    { 0x00, 0x88, 0x88 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
    { 0xf8, 0xf8, 0xf8 }, { 0x3c, 0xbc, 0xfc }, { 0x68, 0x88, 0xfc }, { 0x98, 0x78, 0xf8 },
    { 0xf8, 0x78, 0xf8 }, { 0xf8, 0x58, 0x98 }, { 0xf8, 0x78, 0x58 }, { 0xfc, 0xa0, 0x44 },
    { 0xf8, 0xb8, 0x00 }, { 0xb8, 0xf8, 0x18 }, { 0x58, 0xd8, 0x54 }, { 0x58, 0xf8, 0x98 },
    { 0x00, 0xe8, 0xd8 }, { 0x78, 0x78, 0x78 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
    { 0xfc, 0xfc, 0xfc }, { 0xa4, 0xe4, 0xfc }, { 0xb8, 0xb8, 0xf8 }, { 0xd8, 0xb8, 0xf8 },
    { 0xf8, 0xb8, 0xf8 }, { 0xf8, 0xa4, 0xc0 }, { 0xf0, 0xd0, 0xb0 }, { 0xfc, 0xe0, 0xa8 },
    { 0xf8, 0xd8, 0x78 }, { 0xd8, 0xf8, 0x78 }, { 0xb8, 0xf8, 0xb8 }, { 0xb8, 0xf8, 0xd8 },
    { 0x00, 0xfc, 0xfc }, { 0xf8, 0xd8, 0xf8 }, { 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00 },
};

enum {
    HOST_DEFAULT_SETTLING_FRAMES = 300,
};

typedef struct {
    const char *rom_path;
    const char *input_script_path;
    const char *screenshot_at_arg;
    const char *screenshot_dir;
    uint64_t settling_frames;
    uint64_t max_frames;
} TestOptions;

typedef struct {
    uint64_t frame;
    uint8_t buttons;
} InputEvent;

typedef struct {
    InputEvent *events;
    size_t count;
    size_t capacity;
} InputScript;

typedef struct {
    uint64_t *frames;
    size_t count;
    size_t capacity;
} ScreenshotList;

static void print_usage(const char *argv0) {
    printf(
        "Usage: %s --test-mode <rom> [--input-script file] [--screenshot-dir dir]"
        " [--screenshot-at frames] [--settling-frames N] [--max-frames N]\n",
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

static bool parse_args(int argc, char **argv, TestOptions *options) {
    bool rom_set = false;

    options->rom_path = NULL;
    options->input_script_path = NULL;
    options->screenshot_at_arg = NULL;
    options->screenshot_dir = "test-screenshots";
    options->settling_frames = HOST_DEFAULT_SETTLING_FRAMES;
    options->max_frames = 0;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return false;
        }
        if (strcmp(arg, "--test-mode") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--test-mode requires a ROM path\n");
                return false;
            }
            options->rom_path = argv[++i];
            rom_set = true;
            continue;
        }
        if (strcmp(arg, "--input-script") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--input-script requires a file path\n");
                return false;
            }
            options->input_script_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--screenshot-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--screenshot-dir requires a directory path\n");
                return false;
            }
            options->screenshot_dir = argv[++i];
            continue;
        }
        if (strcmp(arg, "--screenshot-at") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--screenshot-at requires frame numbers\n");
                return false;
            }
            options->screenshot_at_arg = argv[++i];
            continue;
        }
        if (strcmp(arg, "--settling-frames") == 0) {
            if (i + 1 >= argc || !parse_u64_arg(argv[i + 1], &options->settling_frames)) {
                fprintf(stderr, "--settling-frames requires an integer\n");
                return false;
            }
            ++i;
            continue;
        }
        if (strcmp(arg, "--max-frames") == 0) {
            if (i + 1 >= argc || !parse_u64_arg(argv[i + 1], &options->max_frames)) {
                fprintf(stderr, "--max-frames requires an integer\n");
                return false;
            }
            ++i;
            continue;
        }
        fprintf(stderr, "Unknown argument: %s\n", arg);
        return false;
    }

    if (!rom_set || options->rom_path == NULL) {
        fprintf(stderr, "--test-mode <rom> is required\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

/* ---------- input script ---------- */

static bool parse_button_name(const char *name, uint8_t *mask_out) {
    if (strcmp(name, "A") == 0)      { *mask_out = NES_BUTTON_A;      return true; }
    if (strcmp(name, "B") == 0)      { *mask_out = NES_BUTTON_B;      return true; }
    if (strcmp(name, "SELECT") == 0) { *mask_out = NES_BUTTON_SELECT; return true; }
    if (strcmp(name, "START") == 0)  { *mask_out = NES_BUTTON_START;  return true; }
    if (strcmp(name, "UP") == 0)     { *mask_out = NES_BUTTON_UP;     return true; }
    if (strcmp(name, "DOWN") == 0)   { *mask_out = NES_BUTTON_DOWN;   return true; }
    if (strcmp(name, "LEFT") == 0)   { *mask_out = NES_BUTTON_LEFT;   return true; }
    if (strcmp(name, "RIGHT") == 0)  { *mask_out = NES_BUTTON_RIGHT;  return true; }
    return false;
}

static bool input_script_push(InputScript *script, uint64_t frame, uint8_t buttons) {
    if (script->count >= script->capacity) {
        size_t new_cap = script->capacity == 0 ? 16 : script->capacity * 2;
        InputEvent *new_events = (InputEvent *)realloc(script->events, new_cap * sizeof(*new_events));
        if (new_events == NULL) {
            return false;
        }
        script->events = new_events;
        script->capacity = new_cap;
    }
    script->events[script->count].frame = frame;
    script->events[script->count].buttons = buttons;
    ++script->count;
    return true;
}

static bool load_input_script(const char *path, InputScript *script) {
    FILE *f = fopen(path, "r");
    char line[256];

    if (f == NULL) {
        fprintf(stderr, "Cannot open input script: %s\n", path);
        return false;
    }

    while (fgets(line, (int)sizeof(line), f)) {
        size_t len = strlen(line);
        char *comma;
        uint64_t frame;
        uint8_t buttons;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }

        comma = strchr(line, ',');
        if (comma == NULL) {
            fprintf(stderr, "Invalid input script line (missing comma): %s\n", line);
            fclose(f);
            return false;
        }
        *comma = '\0';

        if (!parse_u64_arg(line, &frame) || frame == 0) {
            fprintf(stderr, "Invalid frame number in input script: %s\n", line);
            fclose(f);
            return false;
        }

        if (!parse_button_name(comma + 1, &buttons)) {
            fprintf(stderr, "Unknown button name in input script: %s\n", comma + 1);
            fclose(f);
            return false;
        }

        if (!input_script_push(script, frame, buttons)) {
            fprintf(stderr, "Out of memory loading input script\n");
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

static uint8_t get_script_buttons(const InputScript *script, uint64_t frame) {
    uint8_t buttons = 0;
    for (size_t i = 0; i < script->count; ++i) {
        if (script->events[i].frame == frame) {
            buttons |= script->events[i].buttons;
        }
    }
    return buttons;
}

static uint64_t input_script_last_frame(const InputScript *script) {
    uint64_t last = 0;
    for (size_t i = 0; i < script->count; ++i) {
        if (script->events[i].frame > last) {
            last = script->events[i].frame;
        }
    }
    return last;
}

/* ---------- screenshot list ---------- */

static bool screenshot_list_push(ScreenshotList *list, uint64_t frame) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        uint64_t *new_frames = (uint64_t *)realloc(list->frames, new_cap * sizeof(*new_frames));
        if (new_frames == NULL) {
            return false;
        }
        list->frames = new_frames;
        list->capacity = new_cap;
    }
    list->frames[list->count++] = frame;
    return true;
}

static bool parse_screenshot_frames(const char *arg, ScreenshotList *list) {
    size_t len = strlen(arg);

    if (len > 4 && strcmp(arg + len - 4, ".txt") == 0) {
        FILE *f = fopen(arg, "r");
        char line[64];

        if (f == NULL) {
            fprintf(stderr, "Cannot open screenshot-at file: %s\n", arg);
            return false;
        }

        while (fgets(line, (int)sizeof(line), f)) {
            size_t llen = strlen(line);
            uint64_t frame;

            while (llen > 0 && (line[llen - 1] == '\n' || line[llen - 1] == '\r' || line[llen - 1] == ' ')) {
                line[--llen] = '\0';
            }
            if (llen == 0 || line[0] == '#') {
                continue;
            }
            if (!parse_u64_arg(line, &frame)) {
                fprintf(stderr, "Invalid frame number in screenshot-at file: %s\n", line);
                fclose(f);
                return false;
            }
            if (!screenshot_list_push(list, frame)) {
                fprintf(stderr, "Out of memory\n");
                fclose(f);
                return false;
            }
        }
        fclose(f);
    } else {
        const char *p = arg;
        while (*p != '\0') {
            char *end;
            uint64_t frame = strtoull(p, &end, 10);
            if (end == p) {
                fprintf(stderr, "Invalid frame number in --screenshot-at: %s\n", arg);
                return false;
            }
            if (!screenshot_list_push(list, frame)) {
                fprintf(stderr, "Out of memory\n");
                return false;
            }
            p = end;
            if (*p == ',') {
                ++p;
            } else if (*p != '\0') {
                fprintf(stderr, "Invalid --screenshot-at format\n");
                return false;
            }
        }
    }
    return true;
}

/* ---------- screenshot writing ---------- */

static bool ensure_dir_exists(const char *path) {
    struct stat st;
    if (mkdir(path, 0755) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return false;
}

static void save_screenshot(const char *dir, uint64_t frame, const NesFrameBuffer *fb) {
    char path[1024];
    uint8_t rgb[NES_FRAME_WIDTH * NES_FRAME_HEIGHT * 3];
    int pixel_count = NES_FRAME_WIDTH * NES_FRAME_HEIGHT;

    snprintf(path, sizeof(path), "%s/frame_%06" PRIu64 ".png", dir, frame);

    for (int i = 0; i < pixel_count; ++i) {
        uint8_t idx = fb->pixels[i] & 0x3fu;
        rgb[i * 3 + 0] = k_nes_palette[idx][0];
        rgb[i * 3 + 1] = k_nes_palette[idx][1];
        rgb[i * 3 + 2] = k_nes_palette[idx][2];
    }

    if (!host_write_png_rgb24(path, rgb, NES_FRAME_WIDTH, NES_FRAME_HEIGHT, NES_FRAME_WIDTH * 3)) {
        fprintf(stderr, "Screenshot write failed: %s\n", path);
        return;
    }
    printf("SCREENSHOT: %s\n", path);
    fflush(stdout);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    TestOptions options;
    InputScript input_script = { NULL, 0, 0 };
    ScreenshotList screenshot_list = { NULL, 0, 0 };
    Nes nes;
    uint64_t test_mode_exit_frame;
    int16_t audio_drain[256];
    int ret = 0;

    if (!parse_args(argc, argv, &options)) {
        return 1;
    }

    if (options.input_script_path != NULL) {
        if (!load_input_script(options.input_script_path, &input_script)) {
            return 1;
        }
    }

    if (options.screenshot_at_arg != NULL) {
        if (!parse_screenshot_frames(options.screenshot_at_arg, &screenshot_list)) {
            free(input_script.events);
            return 1;
        }
    }

    if (screenshot_list.count > 0) {
        if (!ensure_dir_exists(options.screenshot_dir)) {
            fprintf(stderr, "Cannot create screenshot directory: %s\n", options.screenshot_dir);
            free(input_script.events);
            free(screenshot_list.frames);
            return 1;
        }
    }

    /* compute exit frame */
    {
        uint64_t last_script_frame = input_script_last_frame(&input_script);
        uint64_t headroom = options.settling_frames;
        test_mode_exit_frame = (last_script_frame > UINT64_MAX - headroom)
            ? UINT64_MAX
            : last_script_frame + headroom;
        if (test_mode_exit_frame == 0) {
            test_mode_exit_frame = headroom;
        }
    }

    nes_init(&nes);
    if (!nes_load_cartridge_file(&nes, options.rom_path)) {
        fprintf(stderr, "ROM load failed: %s\n", nes_last_error(&nes));
        free(input_script.events);
        free(screenshot_list.frames);
        return 3;
    }
    nes_reset(&nes);

    printf("ROM: %s\n", options.rom_path);
    printf("test mode: exit_frame=%" PRIu64 " settling=%" PRIu64 "\n",
           test_mode_exit_frame, options.settling_frames);
    if (input_script.count > 0) {
        printf("input script: %s events=%zu last_frame=%" PRIu64 "\n",
               options.input_script_path,
               input_script.count,
               input_script_last_frame(&input_script));
    }
    if (screenshot_list.count > 0) {
        printf("screenshot dir: %s frames=%zu\n", options.screenshot_dir, screenshot_list.count);
    }

    uint64_t presented_frames = 0;

    while (true) {
        uint64_t current_frame = nes.ppu.completed_frame_count + 1;

        /* apply input for this frame */
        {
            NesControllerState ctrl = { 0 };
            ctrl.buttons = get_script_buttons(&input_script, current_frame);
            nes_set_controller_state(&nes, 0, ctrl);
        }

        /* run one NES frame */
        while (nes.ppu.completed_frame_count < current_frame) {
            if (!nes_step_instruction(&nes)) {
                fprintf(stderr, "Emulation stopped at frame %" PRIu64 ": %s\n",
                        current_frame, nes_last_error(&nes));
                ret = 1;
                goto done;
            }
        }

        /* drain audio to prevent overflow */
        while (nes_audio_available_samples(&nes) > 0) {
            if (nes_audio_read_samples(&nes, audio_drain, sizeof(audio_drain) / sizeof(audio_drain[0])) == 0) {
                break;
            }
        }

        /* take screenshots */
        for (size_t si = 0; si < screenshot_list.count; ++si) {
            if (screenshot_list.frames[si] == current_frame) {
                save_screenshot(options.screenshot_dir, current_frame, &nes.ppu.frame_buffer);
            }
        }

        ++presented_frames;

        if (current_frame >= test_mode_exit_frame) {
            break;
        }
        if (options.max_frames != 0 && presented_frames >= options.max_frames) {
            break;
        }
    }

    printf("Done: %" PRIu64 " frames\n", presented_frames);

done:
    free(input_script.events);
    free(screenshot_list.frames);
    nes_destroy(&nes);
    return ret;
}
