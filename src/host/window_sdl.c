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
    uint32_t *rgba_pixels;
    int rgba_pitch_bytes;
};

static char g_host_sdl_window_last_error[256];

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

HostSdlWindow *host_sdl_window_create(const char *title, int scale, bool enable_vsync) {
    HostSdlWindow *window;

    host_sdl_set_error(NULL);

    if (scale <= 0) {
        host_sdl_set_error("invalid scale");
        return NULL;
    }

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

    if (!SDL_SetRenderVSync(window->renderer, enable_vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED)) {
        host_sdl_set_sdl_error("SDL_SetRenderVSync failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

    window->texture = SDL_CreateTexture(
        window->renderer,
        SDL_PIXELFORMAT_RGBA8888,
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

    window->rgba_pitch_bytes = NES_FRAME_WIDTH * (int)sizeof(uint32_t);
    window->rgba_pixels = (uint32_t *)malloc((size_t)NES_FRAME_WIDTH * NES_FRAME_HEIGHT * sizeof(uint32_t));
    if (window->rgba_pixels == NULL) {
        host_sdl_set_error("malloc failed");
        host_sdl_window_destroy(window);
        return NULL;
    }

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
    free(window->rgba_pixels);
    free(window);
    SDL_Quit();
}

bool host_sdl_window_upload_frame(HostSdlWindow *window, const uint8_t *pixels, int width, int height) {
    void *texture_pixels = NULL;
    int texture_pitch = 0;

    if (window == NULL || pixels == NULL || width != NES_FRAME_WIDTH || height != NES_FRAME_HEIGHT) {
        host_sdl_set_error("invalid frame upload parameters");
        return false;
    }

    for (int i = 0; i < width * height; ++i) {
        uint8_t gray = pixels[i];
        window->rgba_pixels[i] = 0xff000000u | ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | gray;
    }

    if (!SDL_LockTexture(window->texture, NULL, &texture_pixels, &texture_pitch)) {
        host_sdl_set_sdl_error("SDL_LockTexture failed");
        return false;
    }

    for (int y = 0; y < height; ++y) {
        memcpy(
            (uint8_t *)texture_pixels + (size_t)y * texture_pitch,
            (const uint8_t *)window->rgba_pixels + (size_t)y * window->rgba_pitch_bytes,
            (size_t)window->rgba_pitch_bytes
        );
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
