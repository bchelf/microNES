#ifndef SMB2350_RUNTIME_CONFIG_H
#define SMB2350_RUNTIME_CONFIG_H

#include <stdint.h>

// SMB2350_HOT_FUNC(name): place the function in SRAM on Pico builds to avoid
// flash XIP latency on the hottest emulator paths. Falls back to a no-op on
// host builds.
#ifndef SMB2350_HOT_FUNC
#ifdef SMB2350_PICO_PLATFORM
#include "pico.h"
#define SMB2350_HOT_FUNC(name) __not_in_flash_func(name)
#else
#define SMB2350_HOT_FUNC(name) name
#endif
#endif

#ifndef SMB2350_ENABLE_RUNTIME_DIAGNOSTICS
#define SMB2350_ENABLE_RUNTIME_DIAGNOSTICS 1
#endif

#ifndef SMB2350_ENABLE_APU_DEBUG_METRICS
#define SMB2350_ENABLE_APU_DEBUG_METRICS SMB2350_ENABLE_RUNTIME_DIAGNOSTICS
#endif

#ifndef SMB2350_ENABLE_APU_PCM_OUTPUT
#define SMB2350_ENABLE_APU_PCM_OUTPUT 1
#endif

#ifndef SMB2350_ENABLE_APU_EMULATION
#define SMB2350_ENABLE_APU_EMULATION 1
#endif

#ifndef SMB2350_ENABLE_FRAMEBUFFER
#define SMB2350_ENABLE_FRAMEBUFFER 1
#endif

#ifndef SMB2350_ENABLE_STEP_PROFILING
#define SMB2350_ENABLE_STEP_PROFILING 0
#endif

#ifndef SMB2350_ENABLE_PICO_VIDEO_STATS
#define SMB2350_ENABLE_PICO_VIDEO_STATS 1
#endif

typedef uint64_t (*smb2350_profile_now_us_fn)(void *user);

#endif
