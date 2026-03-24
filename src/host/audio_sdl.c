#include "audio_sdl.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HOST_AUDIO_CHANNELS = 1,
    HOST_AUDIO_BYTES_PER_SAMPLE = 2,
    HOST_AUDIO_MAX_QUEUED_MS = 100,
};

struct HostAudioSdl {
    SDL_AudioStream *stream;
    bool enabled;
    uint32_t max_queued_bytes;
    uint64_t submitted_samples;
    uint64_t dropped_samples;
};

static char g_host_audio_last_error[256];

static void host_audio_set_error(const char *message) {
    if (message == NULL) {
        g_host_audio_last_error[0] = '\0';
        return;
    }

    snprintf(g_host_audio_last_error, sizeof(g_host_audio_last_error), "%s", message);
}

static void host_audio_set_sdl_error(const char *prefix) {
    snprintf(g_host_audio_last_error, sizeof(g_host_audio_last_error), "%s: %s", prefix, SDL_GetError());
}

HostAudioSdl *host_audio_sdl_create(int sample_rate, bool enabled) {
    HostAudioSdl *audio;
    SDL_AudioSpec spec;

    host_audio_set_error(NULL);

    audio = (HostAudioSdl *)calloc(1, sizeof(*audio));
    if (audio == NULL) {
        host_audio_set_error("calloc failed");
        return NULL;
    }

    audio->enabled = enabled;
    audio->max_queued_bytes = (uint32_t)(
        ((uint64_t)sample_rate * HOST_AUDIO_BYTES_PER_SAMPLE * HOST_AUDIO_MAX_QUEUED_MS) / 1000ull
    );

    if (!enabled) {
        return audio;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        host_audio_set_sdl_error("SDL_InitSubSystem(audio) failed");
        free(audio);
        return NULL;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = HOST_AUDIO_CHANNELS;
    spec.freq = sample_rate;

    audio->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio->stream == NULL) {
        host_audio_set_sdl_error("SDL_OpenAudioDeviceStream failed");
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        free(audio);
        return NULL;
    }

    if (!SDL_ResumeAudioStreamDevice(audio->stream)) {
        host_audio_set_sdl_error("SDL_ResumeAudioStreamDevice failed");
        host_audio_sdl_destroy(audio);
        return NULL;
    }

    return audio;
}

void host_audio_sdl_destroy(HostAudioSdl *audio) {
    if (audio == NULL) {
        return;
    }

    if (audio->stream != NULL) {
        SDL_DestroyAudioStream(audio->stream);
    }
    if (audio->enabled) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    free(audio);
}

bool host_audio_sdl_submit_samples(HostAudioSdl *audio, const int16_t *samples, size_t sample_count) {
    uint32_t queued_bytes;
    uint32_t bytes_to_submit;

    if (audio == NULL || !audio->enabled || samples == NULL || sample_count == 0) {
        return true;
    }

    queued_bytes = (uint32_t)SDL_GetAudioStreamQueued(audio->stream);
    bytes_to_submit = (uint32_t)(sample_count * HOST_AUDIO_BYTES_PER_SAMPLE);

    /* Drop the entire batch if it would exceed the cap. Partial submission
     * (trimming the batch to fill the remaining space) would create a
     * mid-batch discontinuity: SDL plays samples 0..N, then the next frame
     * picks up from N+dropped onward, producing an audible click. All-or-
     * nothing drops only at batch boundaries and is far less likely to pop. */
    if (queued_bytes + bytes_to_submit > audio->max_queued_bytes) {
        audio->dropped_samples += sample_count;
        return true;
    }

    if (!SDL_PutAudioStreamData(audio->stream, samples, (int)bytes_to_submit)) {
        host_audio_set_sdl_error("SDL_PutAudioStreamData failed");
        return false;
    }

    audio->submitted_samples += sample_count;
    return true;
}

bool host_audio_sdl_is_enabled(const HostAudioSdl *audio) {
    return audio != NULL && audio->enabled;
}

uint32_t host_audio_sdl_queued_bytes(const HostAudioSdl *audio) {
    if (audio == NULL || !audio->enabled || audio->stream == NULL) {
        return 0;
    }

    return (uint32_t)SDL_GetAudioStreamQueued(audio->stream);
}

uint32_t host_audio_sdl_max_queued_bytes(const HostAudioSdl *audio) {
    return audio != NULL ? audio->max_queued_bytes : 0;
}

uint64_t host_audio_sdl_submitted_samples(const HostAudioSdl *audio) {
    return audio != NULL ? audio->submitted_samples : 0;
}

uint64_t host_audio_sdl_dropped_samples(const HostAudioSdl *audio) {
    return audio != NULL ? audio->dropped_samples : 0;
}

const char *host_audio_sdl_last_error(void) {
    return g_host_audio_last_error;
}
