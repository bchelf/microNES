#include "window_sdl.h"

#include "framebuffer.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HostSdlWindow {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    bool enable_color;
};

static char g_host_sdl_window_last_error[256];
static const float k_host_sdl_brightness_scale = 1.5f;
static const float k_host_sdl_color_brightness_scale = 1.08f;

static const uint8_t k_host_nes_palette[64][3] = {
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

static void host_sdl_set_error(const char *message) {
    if (message == NULL) {
        g_host_sdl_window_last_error[0] = '\0';
        return;
    }

    snprintf(g_host_sdl_window_last_error, sizeof(g_host_sdl_window_last_error), "%s", message);
}

static void host_sdl_set_sdl_error(const char *prefix) {
    snprintf(g_host_sdl_window_last_error, sizeof(g_host_sdl_window_last_error), "%s: %s", prefix, SDL_GetError());
}

static uint8_t host_scale_channel(uint8_t value, float scale) {
    uint32_t scaled = (uint32_t)((float)value * scale);
    return (uint8_t)(scaled > 255u ? 255u : scaled);
}

static void host_convert_gray_to_rgba(uint8_t gray, uint8_t *rgba_out) {
    uint8_t out = host_scale_channel(gray, k_host_sdl_brightness_scale);

    rgba_out[0] = out;
    rgba_out[1] = out;
    rgba_out[2] = out;
    rgba_out[3] = 0xffu;
}

static void host_convert_palette_to_rgba(uint8_t palette_index, uint8_t *rgba_out) {
    const uint8_t *rgb = k_host_nes_palette[palette_index & 0x3fu];

    rgba_out[0] = host_scale_channel(rgb[0], k_host_sdl_color_brightness_scale);
    rgba_out[1] = host_scale_channel(rgb[1], k_host_sdl_color_brightness_scale);
    rgba_out[2] = host_scale_channel(rgb[2], k_host_sdl_color_brightness_scale);
    rgba_out[3] = 0xffu;
}

HostSdlWindow *host_sdl_window_create(const char *title, int scale, bool enable_vsync, bool enable_color) {
    HostSdlWindow *window;

    host_sdl_set_error(NULL);

    if (scale <= 0) {
        host_sdl_set_error("invalid scale");
        return NULL;
    }

    // Bypass the desktop compositor for this window, eliminating 1-2 frames of
    // compositor-induced latency on X11 (KWin, Mutter, etc.).
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        host_sdl_set_sdl_error("SDL_Init failed");
        return NULL;
    }

    window = (HostSdlWindow *)calloc(1, sizeof(*window));
    if (window == NULL) {
        host_sdl_set_error("calloc failed");
        SDL_Quit();
        return NULL;
    }

    window->window = SDL_CreateWindow(title, NES_FRAME_WIDTH * scale, NES_FRAME_HEIGHT * scale, SDL_WINDOW_RESIZABLE);
    if (window->window == NULL) {
        host_sdl_set_sdl_error("SDL_CreateWindow failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    window->renderer = SDL_CreateRenderer(window->window, NULL);
    if (window->renderer == NULL) {
        host_sdl_set_sdl_error("SDL_CreateRenderer failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    if (!SDL_SetRenderLogicalPresentation(
            window->renderer,
            NES_FRAME_WIDTH,
            NES_FRAME_HEIGHT,
            SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
        host_sdl_set_sdl_error("SDL_SetRenderLogicalPresentation failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    if (enable_vsync) {
        // Prefer adaptive vsync (tears instead of stalling when a frame is late)
        // to avoid worst-case +1 frame latency from missing a vblank deadline.
        // Fall back to regular vsync if the driver doesn't support it.
        if (!SDL_SetRenderVSync(window->renderer, SDL_RENDERER_VSYNC_ADAPTIVE)) {
            if (!SDL_SetRenderVSync(window->renderer, 1)) {
                host_sdl_set_sdl_error("SDL_SetRenderVSync failed");
                host_sdl_window_destroy(window);
                return NULL;
            }
        }
    } else {
        if (!SDL_SetRenderVSync(window->renderer, SDL_RENDERER_VSYNC_DISABLED)) {
            host_sdl_set_sdl_error("SDL_SetRenderVSync failed");
            host_sdl_window_destroy(window);
            return NULL;
        }
    }

    window->texture = SDL_CreateTexture(
        window->renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        NES_FRAME_WIDTH,
        NES_FRAME_HEIGHT
    );
    if (window->texture == NULL) {
        host_sdl_set_sdl_error("SDL_CreateTexture failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    if (!SDL_SetTextureScaleMode(window->texture, SDL_SCALEMODE_NEAREST)) {
        host_sdl_set_sdl_error("SDL_SetTextureScaleMode failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    window->enable_color = enable_color;

    return window;
}

void host_sdl_window_destroy(HostSdlWindow *window) {
    if (window == NULL) {
        return;
    }

    if (window->texture != NULL) {
        SDL_DestroyTexture(window->texture);
    }
    if (window->renderer != NULL) {
        SDL_DestroyRenderer(window->renderer);
    }
    if (window->window != NULL) {
        SDL_DestroyWindow(window->window);
    }
    free(window);
    SDL_Quit();
}

bool host_sdl_window_upload_frame(HostSdlWindow *window, const uint8_t *pixels, int width, int height) {
    void *tex_pixels;
    int tex_pitch;

    if (window == NULL || pixels == NULL || width != NES_FRAME_WIDTH || height != NES_FRAME_HEIGHT) {
        host_sdl_set_error("invalid frame upload parameters");
        return false;
    }

    // SDL_LockTexture gives a direct pointer into the streaming texture,
    // eliminating the extra CPU-to-staging-buffer copy that SDL_UpdateTexture does.
    if (!SDL_LockTexture(window->texture, NULL, &tex_pixels, &tex_pitch)) {
        host_sdl_set_sdl_error("SDL_LockTexture failed");
        return false;
    }

    for (int y = 0; y < height; ++y) {
        uint8_t *row = (uint8_t *)tex_pixels + y * tex_pitch;
        for (int x = 0; x < width; ++x) {
            uint8_t *rgba = row + x * 4;
            if (window->enable_color) {
                host_convert_palette_to_rgba(pixels[y * width + x], rgba);
            } else {
                host_convert_gray_to_rgba(pixels[y * width + x], rgba);
            }
        }
    }

    SDL_UnlockTexture(window->texture);
    return true;
}

bool host_sdl_window_present(HostSdlWindow *window) {
    if (window == NULL) {
        host_sdl_set_error("window is null");
        return false;
    }

    if (!SDL_RenderClear(window->renderer)) {
        host_sdl_set_sdl_error("SDL_RenderClear failed");
        return false;
    }
    if (!SDL_RenderTexture(window->renderer, window->texture, NULL, NULL)) {
        host_sdl_set_sdl_error("SDL_RenderTexture failed");
        return false;
    }
    if (!SDL_RenderPresent(window->renderer)) {
        host_sdl_set_sdl_error("SDL_RenderPresent failed");
        return false;
    }

    return true;
}

const char *host_sdl_window_last_error(void) {
    return g_host_sdl_window_last_error;
}

void host_sdl_window_set_title(HostSdlWindow *window, const char *title) {
    if (window == NULL || window->window == NULL || title == NULL) {
        return;
    }

    SDL_SetWindowTitle(window->window, title);
}
