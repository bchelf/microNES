#include "wav_write.h"

#include <stdio.h>
#include <string.h>

static char g_host_wav_error[256];

static void host_wav_set_error(const char *message) {
    if (message == NULL) {
        g_host_wav_error[0] = '\0';
        return;
    }

    snprintf(g_host_wav_error, sizeof(g_host_wav_error), "%s", message);
}

static bool host_wav_write_u16(FILE *f, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)(value & 0xffu), (uint8_t)(value >> 8) };
    return fwrite(bytes, sizeof(bytes), 1, f) == 1;
}

static bool host_wav_write_u32(FILE *f, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)(value & 0xffu),
        (uint8_t)((value >> 8) & 0xffu),
        (uint8_t)((value >> 16) & 0xffu),
        (uint8_t)((value >> 24) & 0xffu),
    };
    return fwrite(bytes, sizeof(bytes), 1, f) == 1;
}

bool host_write_wav_mono_s16(
    const char *path,
    const int16_t *samples,
    size_t sample_count,
    uint32_t sample_rate
) {
    FILE *f;
    uint32_t data_bytes;
    uint32_t riff_size;

    host_wav_set_error(NULL);
    if (path == NULL || samples == NULL) {
        host_wav_set_error("invalid wav write arguments");
        return false;
    }
    if (sample_count > 0xffffffffu / sizeof(int16_t)) {
        host_wav_set_error("wav too large");
        return false;
    }

    data_bytes = (uint32_t)(sample_count * sizeof(int16_t));
    riff_size = 36u + data_bytes;

    f = fopen(path, "wb");
    if (f == NULL) {
        host_wav_set_error("fopen failed");
        return false;
    }

    if (fwrite("RIFF", 4, 1, f) != 1 ||
        !host_wav_write_u32(f, riff_size) ||
        fwrite("WAVE", 4, 1, f) != 1 ||
        fwrite("fmt ", 4, 1, f) != 1 ||
        !host_wav_write_u32(f, 16u) ||
        !host_wav_write_u16(f, 1u) ||
        !host_wav_write_u16(f, 1u) ||
        !host_wav_write_u32(f, sample_rate) ||
        !host_wav_write_u32(f, sample_rate * sizeof(int16_t)) ||
        !host_wav_write_u16(f, sizeof(int16_t)) ||
        !host_wav_write_u16(f, 16u) ||
        fwrite("data", 4, 1, f) != 1 ||
        !host_wav_write_u32(f, data_bytes) ||
        (data_bytes > 0 && fwrite(samples, data_bytes, 1, f) != 1)) {
        fclose(f);
        host_wav_set_error("wav write failed");
        return false;
    }

    fclose(f);
    return true;
}

const char *host_wav_write_last_error(void) {
    return g_host_wav_error;
}
