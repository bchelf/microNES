#include "micrones_rl.h"

#include "framebuffer.h"
#include "input.h"
#include "nes.h"

#include <stdlib.h>
#include <string.h>

struct MicronesRLHandle {
    Nes nes;
    char last_error[256];
};

MicronesRLHandle *micrones_rl_create(void) {
    MicronesRLHandle *h = (MicronesRLHandle *)calloc(1, sizeof(MicronesRLHandle));
    if (h) {
        nes_init(&h->nes);
    }
    return h;
}

void micrones_rl_destroy(MicronesRLHandle *h) {
    if (h) {
        nes_destroy(&h->nes);
        free(h);
    }
}

int micrones_rl_load_rom(MicronesRLHandle *h, const char *path) {
    if (!nes_load_cartridge_file(&h->nes, path)) {
        strncpy(h->last_error, nes_last_error(&h->nes), sizeof(h->last_error) - 1);
        return 0;
    }
    return 1;
}

void micrones_rl_reset(MicronesRLHandle *h) {
    nes_reset(&h->nes);
}

int micrones_rl_step(MicronesRLHandle *h) {
    return nes_step_frame(&h->nes) ? 1 : 0;
}

void micrones_rl_set_buttons(MicronesRLHandle *h, uint8_t buttons) {
    NesControllerState state;
    state.buttons = buttons;
    nes_set_controller_state(&h->nes, 0, state);
}

void micrones_rl_write_ram(MicronesRLHandle *h, uint16_t addr, uint8_t value) {
    h->nes.cpu_ram[addr & 0x07FFu] = value;
}

const uint8_t *micrones_rl_ram(const MicronesRLHandle *h) {
    return h->nes.cpu_ram;
}

const uint8_t *micrones_rl_nametables(const MicronesRLHandle *h) {
    return h->nes.ppu.nametables;
}

const uint8_t *micrones_rl_oam(const MicronesRLHandle *h) {
    return h->nes.ppu.oam;
}

const uint8_t *micrones_rl_framebuffer(const MicronesRLHandle *h) {
    return h->nes.ppu.frame_buffer.pixels;
}

uint64_t micrones_rl_frame_count(const MicronesRLHandle *h) {
    return nes_frame_count(&h->nes);
}

const char *micrones_rl_last_error(const MicronesRLHandle *h) {
    return h->last_error[0] ? h->last_error : nes_last_error(&h->nes);
}

// ---------------------------------------------------------------------------
// Savestate implementation
// ---------------------------------------------------------------------------

size_t micrones_rl_state_size(void) {
    return sizeof(MicroNESSaveState);
}

void micrones_rl_save_state(const MicronesRLHandle *h, MicroNESSaveState *out) {
    const Nes *nes = &h->nes;

    // ---- CPU ----
    out->cpu_cycles      = nes->cpu.cycles;
    out->cpu_pc          = nes->cpu.pc;
    out->cpu_a           = nes->cpu.a;
    out->cpu_x           = nes->cpu.x;
    out->cpu_y           = nes->cpu.y;
    out->cpu_sp          = nes->cpu.sp;
    out->cpu_p           = nes->cpu.p;
    out->cpu_last_opcode = nes->cpu.last_opcode;
    out->cpu_jammed      = (uint8_t)nes->cpu.jammed;

    // ---- CPU RAM ----
    memcpy(out->ram, nes->cpu_ram, 2048);

    // ---- PPU scalar state ----
    out->ppu_frame_count            = nes->ppu.frame_count;
    out->ppu_completed_frame_count  = nes->ppu.completed_frame_count;
    out->ppu_vram_addr              = nes->ppu.vram_addr;
    out->ppu_temp_addr              = nes->ppu.temp_addr;
    out->ppu_render_vram_addr       = nes->ppu.render_vram_addr;
    out->ppu_scanline               = (int32_t)nes->ppu.scanline;
    out->ppu_cycle                  = (int32_t)nes->ppu.cycle;
    out->ppu_ctrl                   = nes->ppu.ctrl;
    out->ppu_mask                   = nes->ppu.mask;
    out->ppu_status                 = nes->ppu.status;
    out->ppu_oam_addr               = nes->ppu.oam_addr;
    out->ppu_read_buffer            = nes->ppu.read_buffer;
    out->ppu_fine_x                 = nes->ppu.fine_x;
    out->ppu_write_toggle           = (uint8_t)nes->ppu.write_toggle;
    out->ppu_scroll_x               = nes->ppu.scroll_x;
    out->ppu_scroll_y               = nes->ppu.scroll_y;
    out->ppu_render_fine_x          = nes->ppu.render_fine_x;
    out->ppu_render_scroll_x        = nes->ppu.render_scroll_x;
    out->ppu_render_scroll_y        = nes->ppu.render_scroll_y;
    out->ppu_render_base_nametable  = nes->ppu.render_base_nametable;
    out->ppu_frame_ready            = (uint8_t)nes->ppu.frame_ready;
    out->ppu_scanline_ready         = (uint8_t)nes->ppu.scanline_ready;
    out->ppu_nmi_pending            = (uint8_t)nes->ppu.nmi_pending;
    out->ppu_completed_frame_ready  = (uint8_t)nes->ppu.completed_frame_ready;

    // ---- PPU memory ----
    memcpy(out->ppu_oam,        nes->ppu.oam,        256);
    memcpy(out->ppu_nametables, nes->ppu.nametables, 2048);
    memcpy(out->ppu_palette,    nes->ppu.palette,    32);

    // ---- Controllers ----
    out->ctrl_buttons[0] = nes->controllers[0].live_state.buttons;
    out->ctrl_shift[0]   = nes->controllers[0].shift_register;
    out->ctrl_strobe[0]  = (uint8_t)nes->controllers[0].strobe;
    out->ctrl_buttons[1] = nes->controllers[1].live_state.buttons;
    out->ctrl_shift[1]   = nes->controllers[1].shift_register;
    out->ctrl_strobe[1]  = (uint8_t)nes->controllers[1].strobe;

    // ---- APU CPU-visible state ----
    memcpy(out->apu_registers, nes->apu.registers, 24);
    out->apu_status               = nes->apu.status;
    out->apu_frame_counter_mode_5 = (uint8_t)nes->apu.frame_counter_mode_5;
    out->apu_frame_irq_inhibit    = (uint8_t)nes->apu.frame_irq_inhibit;
    out->apu_frame_counter_cycle  = nes->apu.frame_counter_cycle;
    out->apu_cpu_cycles           = nes->apu.cpu_cycles;
    // Pulse 0
    out->apu_p0_enabled             = (uint8_t)nes->apu.pulse[0].enabled;
    out->apu_p0_length_halt         = (uint8_t)nes->apu.pulse[0].length_halt;
    out->apu_p0_constant_volume     = (uint8_t)nes->apu.pulse[0].constant_volume;
    out->apu_p0_envelope_start      = (uint8_t)nes->apu.pulse[0].envelope_start;
    out->apu_p0_sweep_enabled       = (uint8_t)nes->apu.pulse[0].sweep_enabled;
    out->apu_p0_sweep_negate        = (uint8_t)nes->apu.pulse[0].sweep_negate;
    out->apu_p0_sweep_reload        = (uint8_t)nes->apu.pulse[0].sweep_reload;
    out->apu_p0_duty                = nes->apu.pulse[0].duty;
    out->apu_p0_duty_step           = nes->apu.pulse[0].duty_step;
    out->apu_p0_volume_period       = nes->apu.pulse[0].volume_period;
    out->apu_p0_envelope_divider    = nes->apu.pulse[0].envelope_divider;
    out->apu_p0_envelope_decay      = nes->apu.pulse[0].envelope_decay;
    out->apu_p0_sweep_period        = nes->apu.pulse[0].sweep_period;
    out->apu_p0_sweep_divider       = nes->apu.pulse[0].sweep_divider;
    out->apu_p0_sweep_shift         = nes->apu.pulse[0].sweep_shift;
    out->apu_p0_length_counter      = nes->apu.pulse[0].length_counter;
    out->apu_p0_timer_period        = nes->apu.pulse[0].timer_period;
    out->apu_p0_timer_counter       = nes->apu.pulse[0].timer_counter;
    out->apu_p0_sweep_ones_complement = (uint8_t)nes->apu.pulse[0].sweep_ones_complement;
    // Pulse 1
    out->apu_p1_enabled             = (uint8_t)nes->apu.pulse[1].enabled;
    out->apu_p1_length_halt         = (uint8_t)nes->apu.pulse[1].length_halt;
    out->apu_p1_constant_volume     = (uint8_t)nes->apu.pulse[1].constant_volume;
    out->apu_p1_envelope_start      = (uint8_t)nes->apu.pulse[1].envelope_start;
    out->apu_p1_sweep_enabled       = (uint8_t)nes->apu.pulse[1].sweep_enabled;
    out->apu_p1_sweep_negate        = (uint8_t)nes->apu.pulse[1].sweep_negate;
    out->apu_p1_sweep_reload        = (uint8_t)nes->apu.pulse[1].sweep_reload;
    out->apu_p1_duty                = nes->apu.pulse[1].duty;
    out->apu_p1_duty_step           = nes->apu.pulse[1].duty_step;
    out->apu_p1_volume_period       = nes->apu.pulse[1].volume_period;
    out->apu_p1_envelope_divider    = nes->apu.pulse[1].envelope_divider;
    out->apu_p1_envelope_decay      = nes->apu.pulse[1].envelope_decay;
    out->apu_p1_sweep_period        = nes->apu.pulse[1].sweep_period;
    out->apu_p1_sweep_divider       = nes->apu.pulse[1].sweep_divider;
    out->apu_p1_sweep_shift         = nes->apu.pulse[1].sweep_shift;
    out->apu_p1_length_counter      = nes->apu.pulse[1].length_counter;
    out->apu_p1_timer_period        = nes->apu.pulse[1].timer_period;
    out->apu_p1_timer_counter       = nes->apu.pulse[1].timer_counter;
    out->apu_p1_sweep_ones_complement = (uint8_t)nes->apu.pulse[1].sweep_ones_complement;
    // Triangle
    out->apu_tri_enabled            = (uint8_t)nes->apu.triangle.enabled;
    out->apu_tri_control_flag       = (uint8_t)nes->apu.triangle.control_flag;
    out->apu_tri_linear_reload_flag = (uint8_t)nes->apu.triangle.linear_reload_flag;
    out->apu_tri_sequence_step      = nes->apu.triangle.sequence_step;
    out->apu_tri_linear_reload_value = nes->apu.triangle.linear_reload_value;
    out->apu_tri_linear_counter     = nes->apu.triangle.linear_counter;
    out->apu_tri_length_counter     = nes->apu.triangle.length_counter;
    out->apu_tri_timer_period       = nes->apu.triangle.timer_period;
    out->apu_tri_timer_counter      = nes->apu.triangle.timer_counter;
    // Noise
    out->apu_noise_enabled          = (uint8_t)nes->apu.noise.enabled;
    out->apu_noise_length_halt      = (uint8_t)nes->apu.noise.length_halt;
    out->apu_noise_constant_volume  = (uint8_t)nes->apu.noise.constant_volume;
    out->apu_noise_envelope_start   = (uint8_t)nes->apu.noise.envelope_start;
    out->apu_noise_mode             = (uint8_t)nes->apu.noise.mode;
    out->apu_noise_volume_period    = nes->apu.noise.volume_period;
    out->apu_noise_envelope_divider = nes->apu.noise.envelope_divider;
    out->apu_noise_envelope_decay   = nes->apu.noise.envelope_decay;
    out->apu_noise_length_counter   = nes->apu.noise.length_counter;
    out->apu_noise_period_index     = nes->apu.noise.period_index;
    out->apu_noise_timer_period     = nes->apu.noise.timer_period;
    out->apu_noise_timer_counter    = nes->apu.noise.timer_counter;
    out->apu_noise_shift_register   = nes->apu.noise.shift_register;

    // ---- NES-level timing ----
    out->pending_apu_cycles = nes->pending_apu_cycles;
}

void micrones_rl_load_state(MicronesRLHandle *h, const MicroNESSaveState *in) {
    Nes *nes = &h->nes;

    // ---- CPU ----
    nes->cpu.cycles      = in->cpu_cycles;
    nes->cpu.pc          = in->cpu_pc;
    nes->cpu.a           = in->cpu_a;
    nes->cpu.x           = in->cpu_x;
    nes->cpu.y           = in->cpu_y;
    nes->cpu.sp          = in->cpu_sp;
    nes->cpu.p           = in->cpu_p;
    nes->cpu.last_opcode = in->cpu_last_opcode;
    nes->cpu.jammed      = (bool)in->cpu_jammed;

    // ---- CPU RAM ----
    memcpy(nes->cpu_ram, in->ram, 2048);

    // ---- PPU scalar state ----
    nes->ppu.frame_count           = in->ppu_frame_count;
    nes->ppu.completed_frame_count = in->ppu_completed_frame_count;
    nes->ppu.vram_addr             = in->ppu_vram_addr;
    nes->ppu.temp_addr             = in->ppu_temp_addr;
    nes->ppu.render_vram_addr      = in->ppu_render_vram_addr;
    nes->ppu.scanline              = (int)in->ppu_scanline;
    nes->ppu.cycle                 = (int)in->ppu_cycle;
    nes->ppu.ctrl                  = in->ppu_ctrl;
    nes->ppu.mask                  = in->ppu_mask;
    nes->ppu.status                = in->ppu_status;
    nes->ppu.oam_addr              = in->ppu_oam_addr;
    nes->ppu.read_buffer           = in->ppu_read_buffer;
    nes->ppu.fine_x                = in->ppu_fine_x;
    nes->ppu.write_toggle          = (bool)in->ppu_write_toggle;
    nes->ppu.scroll_x              = in->ppu_scroll_x;
    nes->ppu.scroll_y              = in->ppu_scroll_y;
    nes->ppu.render_fine_x         = in->ppu_render_fine_x;
    nes->ppu.render_scroll_x       = in->ppu_render_scroll_x;
    nes->ppu.render_scroll_y       = in->ppu_render_scroll_y;
    nes->ppu.render_base_nametable = in->ppu_render_base_nametable;
    nes->ppu.frame_ready           = (bool)in->ppu_frame_ready;
    nes->ppu.scanline_ready        = (bool)in->ppu_scanline_ready;
    nes->ppu.nmi_pending           = (bool)in->ppu_nmi_pending;
    nes->ppu.completed_frame_ready = (bool)in->ppu_completed_frame_ready;

    // ---- PPU memory ----
    memcpy(nes->ppu.oam,        in->ppu_oam,        256);
    memcpy(nes->ppu.nametables, in->ppu_nametables, 2048);
    memcpy(nes->ppu.palette,    in->ppu_palette,    32);

    // ---- Controllers ----
    nes->controllers[0].live_state.buttons = in->ctrl_buttons[0];
    nes->controllers[0].shift_register     = in->ctrl_shift[0];
    nes->controllers[0].strobe             = (bool)in->ctrl_strobe[0];
    nes->controllers[1].live_state.buttons = in->ctrl_buttons[1];
    nes->controllers[1].shift_register     = in->ctrl_shift[1];
    nes->controllers[1].strobe             = (bool)in->ctrl_strobe[1];

    // ---- APU CPU-visible state ----
    memcpy(nes->apu.registers, in->apu_registers, 24);
    nes->apu.status               = in->apu_status;
    nes->apu.frame_counter_mode_5 = (bool)in->apu_frame_counter_mode_5;
    nes->apu.frame_irq_inhibit    = (bool)in->apu_frame_irq_inhibit;
    nes->apu.frame_counter_cycle  = in->apu_frame_counter_cycle;
    nes->apu.cpu_cycles           = in->apu_cpu_cycles;
    // Pulse 0
    nes->apu.pulse[0].enabled             = (bool)in->apu_p0_enabled;
    nes->apu.pulse[0].length_halt         = (bool)in->apu_p0_length_halt;
    nes->apu.pulse[0].constant_volume     = (bool)in->apu_p0_constant_volume;
    nes->apu.pulse[0].envelope_start      = (bool)in->apu_p0_envelope_start;
    nes->apu.pulse[0].sweep_enabled       = (bool)in->apu_p0_sweep_enabled;
    nes->apu.pulse[0].sweep_negate        = (bool)in->apu_p0_sweep_negate;
    nes->apu.pulse[0].sweep_reload        = (bool)in->apu_p0_sweep_reload;
    nes->apu.pulse[0].duty                = in->apu_p0_duty;
    nes->apu.pulse[0].duty_step           = in->apu_p0_duty_step;
    nes->apu.pulse[0].volume_period       = in->apu_p0_volume_period;
    nes->apu.pulse[0].envelope_divider    = in->apu_p0_envelope_divider;
    nes->apu.pulse[0].envelope_decay      = in->apu_p0_envelope_decay;
    nes->apu.pulse[0].sweep_period        = in->apu_p0_sweep_period;
    nes->apu.pulse[0].sweep_divider       = in->apu_p0_sweep_divider;
    nes->apu.pulse[0].sweep_shift         = in->apu_p0_sweep_shift;
    nes->apu.pulse[0].length_counter      = in->apu_p0_length_counter;
    nes->apu.pulse[0].timer_period        = in->apu_p0_timer_period;
    nes->apu.pulse[0].timer_counter       = in->apu_p0_timer_counter;
    nes->apu.pulse[0].sweep_ones_complement = (bool)in->apu_p0_sweep_ones_complement;
    // Pulse 1
    nes->apu.pulse[1].enabled             = (bool)in->apu_p1_enabled;
    nes->apu.pulse[1].length_halt         = (bool)in->apu_p1_length_halt;
    nes->apu.pulse[1].constant_volume     = (bool)in->apu_p1_constant_volume;
    nes->apu.pulse[1].envelope_start      = (bool)in->apu_p1_envelope_start;
    nes->apu.pulse[1].sweep_enabled       = (bool)in->apu_p1_sweep_enabled;
    nes->apu.pulse[1].sweep_negate        = (bool)in->apu_p1_sweep_negate;
    nes->apu.pulse[1].sweep_reload        = (bool)in->apu_p1_sweep_reload;
    nes->apu.pulse[1].duty                = in->apu_p1_duty;
    nes->apu.pulse[1].duty_step           = in->apu_p1_duty_step;
    nes->apu.pulse[1].volume_period       = in->apu_p1_volume_period;
    nes->apu.pulse[1].envelope_divider    = in->apu_p1_envelope_divider;
    nes->apu.pulse[1].envelope_decay      = in->apu_p1_envelope_decay;
    nes->apu.pulse[1].sweep_period        = in->apu_p1_sweep_period;
    nes->apu.pulse[1].sweep_divider       = in->apu_p1_sweep_divider;
    nes->apu.pulse[1].sweep_shift         = in->apu_p1_sweep_shift;
    nes->apu.pulse[1].length_counter      = in->apu_p1_length_counter;
    nes->apu.pulse[1].timer_period        = in->apu_p1_timer_period;
    nes->apu.pulse[1].timer_counter       = in->apu_p1_timer_counter;
    nes->apu.pulse[1].sweep_ones_complement = (bool)in->apu_p1_sweep_ones_complement;
    // Triangle
    nes->apu.triangle.enabled            = (bool)in->apu_tri_enabled;
    nes->apu.triangle.control_flag       = (bool)in->apu_tri_control_flag;
    nes->apu.triangle.linear_reload_flag = (bool)in->apu_tri_linear_reload_flag;
    nes->apu.triangle.sequence_step      = in->apu_tri_sequence_step;
    nes->apu.triangle.linear_reload_value = in->apu_tri_linear_reload_value;
    nes->apu.triangle.linear_counter     = in->apu_tri_linear_counter;
    nes->apu.triangle.length_counter     = in->apu_tri_length_counter;
    nes->apu.triangle.timer_period       = in->apu_tri_timer_period;
    nes->apu.triangle.timer_counter      = in->apu_tri_timer_counter;
    // Noise
    nes->apu.noise.enabled          = (bool)in->apu_noise_enabled;
    nes->apu.noise.length_halt      = (bool)in->apu_noise_length_halt;
    nes->apu.noise.constant_volume  = (bool)in->apu_noise_constant_volume;
    nes->apu.noise.envelope_start   = (bool)in->apu_noise_envelope_start;
    nes->apu.noise.mode             = (bool)in->apu_noise_mode;
    nes->apu.noise.volume_period    = in->apu_noise_volume_period;
    nes->apu.noise.envelope_divider = in->apu_noise_envelope_divider;
    nes->apu.noise.envelope_decay   = in->apu_noise_envelope_decay;
    nes->apu.noise.length_counter   = in->apu_noise_length_counter;
    nes->apu.noise.period_index     = in->apu_noise_period_index;
    nes->apu.noise.timer_period     = in->apu_noise_timer_period;
    nes->apu.noise.timer_counter    = in->apu_noise_timer_counter;
    nes->apu.noise.shift_register   = in->apu_noise_shift_register;

    // ---- NES-level timing ----
    nes->pending_apu_cycles = in->pending_apu_cycles;
}
