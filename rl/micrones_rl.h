#ifndef MICRONES_RL_H
#define MICRONES_RL_H

#include <stddef.h>
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

// Write one byte to CPU RAM (addr must be in range 0x0000–0x07FF).
void micrones_rl_write_ram(MicronesRLHandle *h, uint16_t addr, uint8_t value);

// Direct read-only pointers into emulator memory — valid until next call.
const uint8_t *micrones_rl_ram(const MicronesRLHandle *h);         // 2048 bytes
const uint8_t *micrones_rl_nametables(const MicronesRLHandle *h);  // 2048 bytes
const uint8_t *micrones_rl_oam(const MicronesRLHandle *h);         // 256 bytes
const uint8_t *micrones_rl_framebuffer(const MicronesRLHandle *h); // 256*240 bytes

uint64_t    micrones_rl_frame_count(const MicronesRLHandle *h);
const char *micrones_rl_last_error(const MicronesRLHandle *h);

// ---------------------------------------------------------------------------
// Savestate support
//
// MicroNESSaveState is a packed, fixed-size snapshot of all deterministic
// emulator state (CPU, RAM, PPU, controllers, APU CPU-visible state).
// The cartridge ROM is NOT included — it is immutable after load_rom().
//
// Typical usage:
//   size_t sz = micrones_rl_state_size();
//   void  *buf = malloc(sz);
//   micrones_rl_save_state(h, buf);
//   // ... take actions ...
//   micrones_rl_load_state(h, buf);   // deterministic restore
//   free(buf);
// ---------------------------------------------------------------------------

// Savestate blob — all uint8_t/uint16_t/uint32_t/uint64_t, no padding.
// All bool fields from the emulator are stored as uint8_t (0 or 1).
typedef struct __attribute__((packed)) {
    // ---- CPU (17 bytes) ----
    uint64_t cpu_cycles;
    uint16_t cpu_pc;
    uint8_t  cpu_a;
    uint8_t  cpu_x;
    uint8_t  cpu_y;
    uint8_t  cpu_sp;
    uint8_t  cpu_p;            // processor flags
    uint8_t  cpu_last_opcode;
    uint8_t  cpu_jammed;       // bool → uint8_t

    // ---- CPU RAM (2048 bytes) ----
    uint8_t  ram[2048];

    // ---- PPU scalar state (47 bytes) ----
    uint64_t ppu_frame_count;
    uint64_t ppu_completed_frame_count;
    uint16_t ppu_vram_addr;
    uint16_t ppu_temp_addr;
    uint16_t ppu_render_vram_addr;
    int32_t  ppu_scanline;
    int32_t  ppu_cycle;
    uint8_t  ppu_ctrl;
    uint8_t  ppu_mask;
    uint8_t  ppu_status;
    uint8_t  ppu_oam_addr;
    uint8_t  ppu_read_buffer;
    uint8_t  ppu_fine_x;
    uint8_t  ppu_write_toggle;         // bool → uint8_t
    uint8_t  ppu_scroll_x;
    uint8_t  ppu_scroll_y;
    uint8_t  ppu_render_fine_x;
    uint8_t  ppu_render_scroll_x;
    uint8_t  ppu_render_scroll_y;
    uint8_t  ppu_render_base_nametable;
    uint8_t  ppu_frame_ready;          // bool → uint8_t
    uint8_t  ppu_scanline_ready;       // bool → uint8_t
    uint8_t  ppu_nmi_pending;          // bool → uint8_t
    uint8_t  ppu_completed_frame_ready;// bool → uint8_t

    // ---- PPU memory (2336 bytes) ----
    uint8_t  ppu_oam[256];
    uint8_t  ppu_nametables[2048];
    uint8_t  ppu_palette[32];

    // ---- Controllers (6 bytes) ----
    uint8_t  ctrl_buttons[2];
    uint8_t  ctrl_shift[2];
    uint8_t  ctrl_strobe[2];           // bool → uint8_t

    // ---- APU CPU-visible state (120 bytes) ----
    // Raw register mirror (written by CPU to $4000–$4017)
    uint8_t  apu_registers[24];
    uint8_t  apu_status;               // $4015 readable value
    uint8_t  apu_frame_counter_mode_5; // bool → uint8_t
    uint8_t  apu_frame_irq_inhibit;    // bool → uint8_t
    uint64_t apu_frame_counter_cycle;
    uint64_t apu_cpu_cycles;
    // Pulse channel 0
    uint8_t  apu_p0_enabled;
    uint8_t  apu_p0_length_halt;
    uint8_t  apu_p0_constant_volume;
    uint8_t  apu_p0_envelope_start;
    uint8_t  apu_p0_sweep_enabled;
    uint8_t  apu_p0_sweep_negate;
    uint8_t  apu_p0_sweep_reload;
    uint8_t  apu_p0_duty;
    uint8_t  apu_p0_duty_step;
    uint8_t  apu_p0_volume_period;
    uint8_t  apu_p0_envelope_divider;
    uint8_t  apu_p0_envelope_decay;
    uint8_t  apu_p0_sweep_period;
    uint8_t  apu_p0_sweep_divider;
    uint8_t  apu_p0_sweep_shift;
    uint8_t  apu_p0_length_counter;
    uint16_t apu_p0_timer_period;
    uint16_t apu_p0_timer_counter;
    uint8_t  apu_p0_sweep_ones_complement;
    // Pulse channel 1
    uint8_t  apu_p1_enabled;
    uint8_t  apu_p1_length_halt;
    uint8_t  apu_p1_constant_volume;
    uint8_t  apu_p1_envelope_start;
    uint8_t  apu_p1_sweep_enabled;
    uint8_t  apu_p1_sweep_negate;
    uint8_t  apu_p1_sweep_reload;
    uint8_t  apu_p1_duty;
    uint8_t  apu_p1_duty_step;
    uint8_t  apu_p1_volume_period;
    uint8_t  apu_p1_envelope_divider;
    uint8_t  apu_p1_envelope_decay;
    uint8_t  apu_p1_sweep_period;
    uint8_t  apu_p1_sweep_divider;
    uint8_t  apu_p1_sweep_shift;
    uint8_t  apu_p1_length_counter;
    uint16_t apu_p1_timer_period;
    uint16_t apu_p1_timer_counter;
    uint8_t  apu_p1_sweep_ones_complement;
    // Triangle channel
    uint8_t  apu_tri_enabled;
    uint8_t  apu_tri_control_flag;
    uint8_t  apu_tri_linear_reload_flag;
    uint8_t  apu_tri_sequence_step;
    uint8_t  apu_tri_linear_reload_value;
    uint8_t  apu_tri_linear_counter;
    uint8_t  apu_tri_length_counter;
    uint16_t apu_tri_timer_period;
    uint16_t apu_tri_timer_counter;
    // Noise channel
    uint8_t  apu_noise_enabled;
    uint8_t  apu_noise_length_halt;
    uint8_t  apu_noise_constant_volume;
    uint8_t  apu_noise_envelope_start;
    uint8_t  apu_noise_mode;
    uint8_t  apu_noise_volume_period;
    uint8_t  apu_noise_envelope_divider;
    uint8_t  apu_noise_envelope_decay;
    uint8_t  apu_noise_length_counter;
    uint8_t  apu_noise_period_index;
    uint16_t apu_noise_timer_period;
    uint16_t apu_noise_timer_counter;
    uint16_t apu_noise_shift_register;

    // ---- NES-level timing (4 bytes) ----
    uint32_t pending_apu_cycles;
} MicroNESSaveState;

// Returns sizeof(MicroNESSaveState). Use to allocate the buffer in Python.
size_t micrones_rl_state_size(void);

// Capture all deterministic emulator state into *out.
// *out must point to a buffer of at least micrones_rl_state_size() bytes.
void micrones_rl_save_state(const MicronesRLHandle *h, MicroNESSaveState *out);

// Restore emulator state from *in.
// After this call, subsequent frames are identical to what would have occurred
// had micrones_rl_save_state never been called.
void micrones_rl_load_state(MicronesRLHandle *h, const MicroNESSaveState *in);

#ifdef __cplusplus
}
#endif

#endif
