#include "video_capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

struct host_video_capture {
    FILE *pipe;
    int width;
    int height;
    int fps;
    size_t frame_bytes;
    bool write_failed;
};

static char g_host_video_last_error[256];

static void host_video_set_error(const char *message) {
    size_t len;

    if (message == NULL) {
        g_host_video_last_error[0] = '\0';
        return;
    }

    len = strlen(message);
    if (len >= sizeof(g_host_video_last_error)) {
        len = sizeof(g_host_video_last_error) - 1;
    }
    memcpy(g_host_video_last_error, message, len);
    g_host_video_last_error[len] = '\0';
}

static void host_video_set_errno_error(const char *prefix) {
    char buffer[256];

    snprintf(buffer, sizeof(buffer), "%s: %s", prefix, strerror(errno));
    host_video_set_error(buffer);
}

static char *host_shell_quote(const char *value) {
    size_t length = 2;
    char *quoted;
    char *dst;

    for (const char *src = value; *src != '\0'; ++src) {
        length += (*src == '\'') ? 4u : 1u;
    }

    quoted = (char *)malloc(length + 1);
    if (quoted == NULL) {
        host_video_set_errno_error("malloc failed");
        return NULL;
    }

    dst = quoted;
    *dst++ = '\'';
    for (const char *src = value; *src != '\0'; ++src) {
        if (*src == '\'') {
            memcpy(dst, "'\\''", 4);
            dst += 4;
        } else {
            *dst++ = *src;
        }
    }
    *dst++ = '\'';
    *dst = '\0';
    return quoted;
}

static bool host_ffmpeg_available(void) {
    int status = system("command -v ffmpeg >/dev/null 2>&1");

    if (status == 0) {
        return true;
    }

    host_video_set_error("ffmpeg not found on PATH");
    return false;
}

host_video_capture_t *host_video_start(const char *path, int width, int height, int fps) {
    const char *command_format =
        "ffmpeg -y -loglevel error "
        "-f rawvideo -pixel_format gray "
        "-video_size %dx%d -framerate %d "
        "-i - -vf format=yuv420p "
        "-vcodec libx264 -preset veryfast %s";
    char *quoted_path;
    int command_length;
    char *command;
    host_video_capture_t *capture;

    host_video_set_error(NULL);

    if (path == NULL || width <= 0 || height <= 0 || fps <= 0) {
        host_video_set_error("invalid video capture parameters");
        return NULL;
    }
    if (!host_ffmpeg_available()) {
        return NULL;
    }

    quoted_path = host_shell_quote(path);
    if (quoted_path == NULL) {
        return NULL;
    }

    command_length = snprintf(NULL, 0, command_format, width, height, fps, quoted_path);
    if (command_length < 0) {
        free(quoted_path);
        host_video_set_error("failed to size ffmpeg command");
        return NULL;
    }

    command = (char *)malloc((size_t)command_length + 1u);
    if (command == NULL) {
        free(quoted_path);
        host_video_set_errno_error("malloc failed");
        return NULL;
    }
    snprintf(command, (size_t)command_length + 1u, command_format, width, height, fps, quoted_path);
    free(quoted_path);

    capture = (host_video_capture_t *)calloc(1, sizeof(*capture));
    if (capture == NULL) {
        free(command);
        host_video_set_errno_error("calloc failed");
        return NULL;
    }

    capture->pipe = popen(command, "w");
    free(command);
    if (capture->pipe == NULL) {
        free(capture);
        host_video_set_errno_error("popen(ffmpeg) failed");
        return NULL;
    }

    capture->width = width;
    capture->height = height;
    capture->fps = fps;
    capture->frame_bytes = (size_t)width * (size_t)height;
    capture->write_failed = false;
    return capture;
}

bool host_video_write_frame(host_video_capture_t *capture, const uint8_t *pixels) {
    size_t written;

    if (capture == NULL || pixels == NULL || capture->pipe == NULL || capture->write_failed) {
        return false;
    }

    written = fwrite(pixels, 1, capture->frame_bytes, capture->pipe);
    if (written != capture->frame_bytes) {
        capture->write_failed = true;
        if (ferror(capture->pipe)) {
            host_video_set_errno_error("ffmpeg pipe write failed");
        } else {
            host_video_set_error("short write to ffmpeg pipe");
        }
        return false;
    }

    return true;
}

bool host_video_close(host_video_capture_t *capture) {
    int status;
    bool ok = true;

    if (capture == NULL) {
        return true;
    }

    if (capture->pipe != NULL) {
        if (fflush(capture->pipe) != 0) {
            host_video_set_errno_error("ffmpeg pipe flush failed");
            ok = false;
        }

        status = pclose(capture->pipe);
        capture->pipe = NULL;
        if (status != 0) {
            char buffer[256];

            if (WIFEXITED(status)) {
                snprintf(buffer, sizeof(buffer), "ffmpeg exited with status %d", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                snprintf(buffer, sizeof(buffer), "ffmpeg terminated by signal %d", WTERMSIG(status));
            } else {
                snprintf(buffer, sizeof(buffer), "ffmpeg exited unexpectedly (status=%d)", status);
            }
            host_video_set_error(buffer);
            ok = false;
        }
    }

    if (capture->write_failed) {
        ok = false;
    }

    free(capture);
    return ok;
}

const char *host_video_last_error(void) {
    return g_host_video_last_error;
}
