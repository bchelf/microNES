#ifndef MICRONES_HOST_VIDEO_CAPTURE_H
#define MICRONES_HOST_VIDEO_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct host_video_capture host_video_capture_t;

host_video_capture_t *host_video_start(const char *path, int width, int height, int fps);
bool host_video_write_frame(host_video_capture_t *capture, const uint8_t *pixels);
bool host_video_close(host_video_capture_t *capture);
const char *host_video_last_error(void);

#endif
