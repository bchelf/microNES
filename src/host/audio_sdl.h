#ifndef SMB2350_HOST_AUDIO_SDL_H
#define SMB2350_HOST_AUDIO_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HostAudioSdl HostAudioSdl;

HostAudioSdl *host_audio_sdl_create(int sample_rate, bool enabled);
void host_audio_sdl_destroy(HostAudioSdl *audio);
bool host_audio_sdl_submit_samples(HostAudioSdl *audio, const int16_t *samples, size_t sample_count);
bool host_audio_sdl_is_enabled(const HostAudioSdl *audio);
uint32_t host_audio_sdl_queued_bytes(const HostAudioSdl *audio);
uint64_t host_audio_sdl_submitted_samples(const HostAudioSdl *audio);
uint64_t host_audio_sdl_dropped_samples(const HostAudioSdl *audio);
const char *host_audio_sdl_last_error(void);

#endif
