#ifndef MICRONES_HOST_WINDOW_SDL_H
#define MICRONES_HOST_WINDOW_SDL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct HostSdlWindow HostSdlWindow;

/* opacity_percent: 0 = fully invisible, 100 = fully opaque.
 * Only has a visible effect when enable_transparent is true. */
HostSdlWindow *host_sdl_window_create(const char *title, int scale, bool enable_vsync, bool enable_color, bool enable_transparent, int opacity_percent);
void host_sdl_window_destroy(HostSdlWindow *window);
bool host_sdl_window_upload_frame(HostSdlWindow *window, const uint8_t *pixels, int width, int height);
bool host_sdl_window_upload_scanlines(HostSdlWindow *window, const uint8_t *pixels, int width, int start_y, int count);
bool host_sdl_window_present(HostSdlWindow *window);
const char *host_sdl_window_last_error(void);
void host_sdl_window_set_title(HostSdlWindow *window, const char *title);

#endif
