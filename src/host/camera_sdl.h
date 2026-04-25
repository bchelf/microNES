#ifndef MICRONES_HOST_CAMERA_SDL_H
#define MICRONES_HOST_CAMERA_SDL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct HostCameraSdl HostCameraSdl;

typedef struct {
    bool enabled;
    bool approved;
    int device_index;
    int frame_width;
    int frame_height;
    int frame_pitch_bytes;
    uint32_t pixel_format;
    uint64_t last_frame_timestamp_ns;
    uint64_t total_frames_acquired;
    uint64_t total_frames_discarded;
    uint64_t total_polls;
    uint64_t polls_with_frame;
    double measured_fps;
    char device_name[128];
} HostCameraSdlStats;

HostCameraSdl *host_camera_sdl_create(bool enabled, int device_index, int requested_width, int requested_height, int requested_fps);
void host_camera_sdl_destroy(HostCameraSdl *camera);
bool host_camera_sdl_poll(HostCameraSdl *camera, uint64_t now_ns);
bool host_camera_sdl_is_enabled(const HostCameraSdl *camera);
bool host_camera_sdl_has_frame(const HostCameraSdl *camera);
const uint8_t *host_camera_sdl_latest_rgba(
    const HostCameraSdl *camera,
    int *width_out,
    int *height_out,
    int *pitch_bytes_out,
    uint64_t *timestamp_ns_out
);
void host_camera_sdl_get_stats(const HostCameraSdl *camera, HostCameraSdlStats *stats_out);
const char *host_camera_sdl_last_error(void);

#endif
