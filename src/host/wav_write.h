#ifndef SMB2350_HOST_WAV_WRITE_H
#define SMB2350_HOST_WAV_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool host_write_wav_mono_s16(
    const char *path,
    const int16_t *samples,
    size_t sample_count,
    uint32_t sample_rate
);

const char *host_wav_write_last_error(void);

#endif
