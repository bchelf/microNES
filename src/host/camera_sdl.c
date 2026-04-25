#include "camera_sdl.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HostCameraSdl {
    SDL_Camera *camera;
    SDL_CameraID instance_id;
    SDL_CameraSpec active_spec;
    uint8_t *rgba_pixels;
    size_t rgba_capacity_bytes;
    uint64_t last_frame_timestamp_ns;
    uint64_t total_frames_acquired;
    uint64_t total_frames_discarded;
    uint64_t total_polls;
    uint64_t polls_with_frame;
    uint64_t stats_window_start_ns;
    uint64_t stats_window_frames;
    double measured_fps;
    int device_index;
    bool enabled;
    bool approved;
    bool has_frame;
    char device_name[128];
};

static char g_host_camera_last_error[256];

static void host_camera_set_error(const char *message) {
    if (message == NULL) {
        g_host_camera_last_error[0] = '\0';
        return;
    }

    snprintf(g_host_camera_last_error, sizeof(g_host_camera_last_error), "%s", message);
}

static void host_camera_set_sdl_error(const char *prefix) {
    snprintf(g_host_camera_last_error, sizeof(g_host_camera_last_error), "%s: %s", prefix, SDL_GetError());
}

static bool host_camera_copy_surface_rgba(HostCameraSdl *camera, SDL_Surface *surface, uint64_t timestamp_ns) {
    SDL_Surface *converted = NULL;
    SDL_Surface *source = surface;
    const uint8_t *src_row;
    uint8_t *dst_row;
    size_t row_bytes;
    size_t required_bytes;

    if (surface->format != SDL_PIXELFORMAT_RGBA32) {
        converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
        if (converted == NULL) {
            host_camera_set_sdl_error("SDL_ConvertSurface failed");
            return false;
        }
        source = converted;
    }

    required_bytes = (size_t)source->pitch * (size_t)source->h;
    if (camera->rgba_capacity_bytes < required_bytes) {
        uint8_t *new_pixels = (uint8_t *)realloc(camera->rgba_pixels, required_bytes);
        if (new_pixels == NULL) {
            host_camera_set_error("realloc failed");
            SDL_DestroySurface(converted);
            return false;
        }
        camera->rgba_pixels = new_pixels;
        camera->rgba_capacity_bytes = required_bytes;
    }

    if (SDL_MUSTLOCK(source)) {
        if (!SDL_LockSurface(source)) {
            host_camera_set_sdl_error("SDL_LockSurface failed");
            SDL_DestroySurface(converted);
            return false;
        }
    }

    row_bytes = (size_t)source->w * 4u;
    for (int y = 0; y < source->h; ++y) {
        src_row = (const uint8_t *)source->pixels + (size_t)y * (size_t)source->pitch;
        dst_row = camera->rgba_pixels + (size_t)y * (size_t)source->pitch;
        memcpy(dst_row, src_row, row_bytes);
    }

    if (SDL_MUSTLOCK(source)) {
        SDL_UnlockSurface(source);
    }

    camera->active_spec.format = SDL_PIXELFORMAT_RGBA32;
    camera->active_spec.width = source->w;
    camera->active_spec.height = source->h;
    camera->active_spec.colorspace = SDL_COLORSPACE_SRGB;
    camera->last_frame_timestamp_ns = timestamp_ns;
    camera->has_frame = true;

    SDL_DestroySurface(converted);
    return true;
}

HostCameraSdl *host_camera_sdl_create(bool enabled, int device_index, int requested_width, int requested_height, int requested_fps) {
    HostCameraSdl *camera = NULL;
    SDL_CameraID *camera_ids = NULL;
    int camera_count = 0;
    SDL_CameraSpec requested_spec;
    SDL_CameraPermissionState permission_state;
    const char *device_name;

    host_camera_set_error(NULL);

    camera = (HostCameraSdl *)calloc(1, sizeof(*camera));
    if (camera == NULL) {
        host_camera_set_error("calloc failed");
        return NULL;
    }

    camera->enabled = enabled;
    camera->device_index = device_index;

    if (!enabled) {
        return camera;
    }

    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        host_camera_set_sdl_error("SDL_InitSubSystem(camera) failed");
        free(camera);
        return NULL;
    }

    camera_ids = SDL_GetCameras(&camera_count);
    if (camera_ids == NULL) {
        host_camera_set_sdl_error("SDL_GetCameras failed");
        host_camera_sdl_destroy(camera);
        return NULL;
    }
    if (camera_count <= 0) {
        host_camera_set_error("no camera devices found");
        SDL_free(camera_ids);
        host_camera_sdl_destroy(camera);
        return NULL;
    }
    if (device_index < 0 || device_index >= camera_count) {
        snprintf(
            g_host_camera_last_error,
            sizeof(g_host_camera_last_error),
            "camera index %d is out of range (found %d camera%s)",
            device_index,
            camera_count,
            camera_count == 1 ? "" : "s"
        );
        SDL_free(camera_ids);
        host_camera_sdl_destroy(camera);
        return NULL;
    }

    camera->instance_id = camera_ids[device_index];
    device_name = SDL_GetCameraName(camera->instance_id);
    snprintf(
        camera->device_name,
        sizeof(camera->device_name),
        "%s",
        device_name != NULL ? device_name : "unknown camera"
    );
    SDL_free(camera_ids);

    SDL_zero(requested_spec);
    requested_spec.format = SDL_PIXELFORMAT_RGBA32;
    requested_spec.colorspace = SDL_COLORSPACE_SRGB;
    requested_spec.width = requested_width;
    requested_spec.height = requested_height;
    requested_spec.framerate_numerator = requested_fps;
    requested_spec.framerate_denominator = 1;

    camera->camera = SDL_OpenCamera(camera->instance_id, &requested_spec);
    if (camera->camera == NULL) {
        camera->camera = SDL_OpenCamera(camera->instance_id, NULL);
        if (camera->camera == NULL) {
            host_camera_set_sdl_error("SDL_OpenCamera failed");
            host_camera_sdl_destroy(camera);
            return NULL;
        }
    }

    permission_state = SDL_GetCameraPermissionState(camera->camera);
    camera->approved = permission_state == SDL_CAMERA_PERMISSION_STATE_APPROVED;

    if (camera->approved) {
        if (!SDL_GetCameraFormat(camera->camera, &camera->active_spec)) {
            host_camera_set_sdl_error("SDL_GetCameraFormat failed");
            host_camera_sdl_destroy(camera);
            return NULL;
        }
        camera->active_spec.format = SDL_PIXELFORMAT_RGBA32;
    } else {
        camera->active_spec.format = SDL_PIXELFORMAT_RGBA32;
        camera->active_spec.width = requested_width;
        camera->active_spec.height = requested_height;
        camera->active_spec.colorspace = SDL_COLORSPACE_SRGB;
        camera->active_spec.framerate_numerator = requested_fps;
        camera->active_spec.framerate_denominator = 1;
    }

    return camera;
}

void host_camera_sdl_destroy(HostCameraSdl *camera) {
    if (camera == NULL) {
        return;
    }

    if (camera->camera != NULL) {
        SDL_CloseCamera(camera->camera);
    }
    free(camera->rgba_pixels);
    if (camera->enabled) {
        SDL_QuitSubSystem(SDL_INIT_CAMERA);
    }
    free(camera);
}

bool host_camera_sdl_poll(HostCameraSdl *camera, uint64_t now_ns) {
    SDL_Surface *frame;
    uint64_t timestamp_ns;
    bool saw_frame = false;
    uint64_t frames_this_poll = 0;

    if (camera == NULL || !camera->enabled) {
        return true;
    }

    ++camera->total_polls;
    if (camera->stats_window_start_ns == 0) {
        camera->stats_window_start_ns = now_ns;
    }

    if (!camera->approved) {
        SDL_CameraPermissionState permission_state = SDL_GetCameraPermissionState(camera->camera);
        if (permission_state == SDL_CAMERA_PERMISSION_STATE_DENIED) {
            host_camera_set_error("camera permission denied");
            return false;
        }
        camera->approved = permission_state == SDL_CAMERA_PERMISSION_STATE_APPROVED;
        if (camera->approved && !SDL_GetCameraFormat(camera->camera, &camera->active_spec)) {
            host_camera_set_sdl_error("SDL_GetCameraFormat failed");
            return false;
        }
    }

    for (;;) {
        timestamp_ns = 0;
        frame = SDL_AcquireCameraFrame(camera->camera, &timestamp_ns);
        if (frame == NULL) {
            break;
        }

        saw_frame = true;
        ++frames_this_poll;
        ++camera->total_frames_acquired;
        ++camera->stats_window_frames;

        if (!host_camera_copy_surface_rgba(camera, frame, timestamp_ns)) {
            SDL_ReleaseCameraFrame(camera->camera, frame);
            return false;
        }

        SDL_ReleaseCameraFrame(camera->camera, frame);
    }

    if (saw_frame) {
        ++camera->polls_with_frame;
        if (frames_this_poll > 1) {
            camera->total_frames_discarded += frames_this_poll - 1;
        }
    }

    if (camera->stats_window_start_ns != 0 && now_ns > camera->stats_window_start_ns) {
        uint64_t elapsed_ns = now_ns - camera->stats_window_start_ns;
        if (elapsed_ns >= 1000000000ull) {
            camera->measured_fps = (double)camera->stats_window_frames * 1000000000.0 / (double)elapsed_ns;
            camera->stats_window_start_ns = now_ns;
            camera->stats_window_frames = 0;
        }
    }

    return true;
}

bool host_camera_sdl_is_enabled(const HostCameraSdl *camera) {
    return camera != NULL && camera->enabled;
}

bool host_camera_sdl_has_frame(const HostCameraSdl *camera) {
    return camera != NULL && camera->has_frame;
}

const uint8_t *host_camera_sdl_latest_rgba(
    const HostCameraSdl *camera,
    int *width_out,
    int *height_out,
    int *pitch_bytes_out,
    uint64_t *timestamp_ns_out
) {
    if (camera == NULL || !camera->has_frame) {
        return NULL;
    }

    if (width_out != NULL) {
        *width_out = camera->active_spec.width;
    }
    if (height_out != NULL) {
        *height_out = camera->active_spec.height;
    }
    if (pitch_bytes_out != NULL) {
        *pitch_bytes_out = camera->active_spec.width * 4;
    }
    if (timestamp_ns_out != NULL) {
        *timestamp_ns_out = camera->last_frame_timestamp_ns;
    }

    return camera->rgba_pixels;
}

void host_camera_sdl_get_stats(const HostCameraSdl *camera, HostCameraSdlStats *stats_out) {
    if (stats_out == NULL) {
        return;
    }

    memset(stats_out, 0, sizeof(*stats_out));
    if (camera == NULL) {
        return;
    }

    stats_out->enabled = camera->enabled;
    stats_out->approved = camera->approved;
    stats_out->device_index = camera->device_index;
    stats_out->frame_width = camera->active_spec.width;
    stats_out->frame_height = camera->active_spec.height;
    stats_out->frame_pitch_bytes = camera->active_spec.width * 4;
    stats_out->pixel_format = SDL_PIXELFORMAT_RGBA32;
    stats_out->last_frame_timestamp_ns = camera->last_frame_timestamp_ns;
    stats_out->total_frames_acquired = camera->total_frames_acquired;
    stats_out->total_frames_discarded = camera->total_frames_discarded;
    stats_out->total_polls = camera->total_polls;
    stats_out->polls_with_frame = camera->polls_with_frame;
    stats_out->measured_fps = camera->measured_fps;
    snprintf(stats_out->device_name, sizeof(stats_out->device_name), "%s", camera->device_name);
}

const char *host_camera_sdl_last_error(void) {
    return g_host_camera_last_error;
}
