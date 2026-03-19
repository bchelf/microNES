#ifndef MICRONES_RL_H
#define MICRONES_RL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle — one instance per parallel env.
typedef struct MicronesRLHandle MicronesRLHandle;

MicronesRLHandle *micrones_rl_create(void);
void              micrones_rl_destroy(MicronesRLHandle *h);

// Returns 1 on success, 0 on failure (call micrones_rl_last_error for details).
int  micrones_rl_load_rom(MicronesRLHandle *h, const char *path);
void micrones_rl_reset(MicronesRLHandle *h);

// Step one full NES frame. Returns 1 on success, 0 if the emulator stalled.
int  micrones_rl_step(MicronesRLHandle *h);

// Set controller 0 button mask (NES_BUTTON_* flags from input.h).
void micrones_rl_set_buttons(MicronesRLHandle *h, uint8_t buttons);

// Direct read-only pointers into emulator memory — valid until next call.
const uint8_t *micrones_rl_ram(const MicronesRLHandle *h);         // 2048 bytes
const uint8_t *micrones_rl_nametables(const MicronesRLHandle *h);  // 2048 bytes
const uint8_t *micrones_rl_oam(const MicronesRLHandle *h);         // 256 bytes
const uint8_t *micrones_rl_framebuffer(const MicronesRLHandle *h); // 256*240 bytes

uint64_t    micrones_rl_frame_count(const MicronesRLHandle *h);
const char *micrones_rl_last_error(const MicronesRLHandle *h);

#ifdef __cplusplus
}
#endif

#endif
