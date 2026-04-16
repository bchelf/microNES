#ifndef MICRONES_HOST_WINDOW_SDL_H
#define MICRONES_HOST_WINDOW_SDL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct HostSdlWindow HostSdlWindow;

HostSdlWindow *host_sdl_window_create(const char *title, int scale, bool enable_vsync, bool enable_color, bool enable_transparent);
void host_sdl_window_destroy(HostSdlWindow *window);
bool host_sdl_window_upload_frame(HostSdlWindow *window, const uint8_t *pixels, int width, int height);
bool host_sdl_window_present(HostSdlWindow *window);
const char *host_sdl_window_last_error(void);
void host_sdl_window_set_title(HostSdlWindow *window, const char *title);

#endif
