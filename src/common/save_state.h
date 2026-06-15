#ifndef MICRONES_SAVE_STATE_H
#define MICRONES_SAVE_STATE_H

#include "nes.h"

#include <stdbool.h>
#include <stdint.h>

/* Portable binary save-state format.
 *
 * A SaveStateBlob is a fixed-size snapshot of everything needed to resume
 * emulation: CPU registers, CPU RAM, battery/work RAM, controller shift
 * state, the PPU registers/VRAM/OAM/palette, the APU channel state, and the
 * cartridge's mapper registers.
 *
 * Mapper bank-select pointers (prg_bank_lo/hi, prg_banks_8k[], m40_prg_6000)
 * are stored as byte offsets relative to cartridge.prg_rom rather than raw
 * pointers, since the PRG ROM base address can differ across boots even when
 * the same ROM bytes are reloaded.  SAVE_STATE_PTR_NONE marks an offset that
 * does not apply to the loaded mapper. */

#define SAVE_STATE_MAGIC   0x53565331u /* "SVS1" */
#define SAVE_STATE_VERSION 1u
#define SAVE_STATE_PTR_NONE 0xFFFFFFFFu

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t rom_checksum;
    uint32_t rom_image_size;
    uint32_t elapsed_seconds;
} SaveStateHeader;

typedef struct {
    uint8_t  a, x, y, sp, p;
    uint16_t pc;
    uint64_t cycles;
    uint8_t  last_opcode;
    uint8_t  jammed;
    uint32_t insn_count;
} SaveStateCpu;

typedef struct {
    uint8_t  shift_register;
    uint8_t  strobe;
    uint8_t  live_buttons;
} SaveStateController;

typedef struct {
    int32_t  scanline;
    int32_t  cycle;
    int32_t  cycles_remaining;
    uint8_t  frame_ready;
    uint8_t  scanline_ready;
    uint8_t  nmi_pending;
    uint8_t  completed_frame_ready;
    uint8_t  ctrl;
    uint8_t  mask;
    uint8_t  status;
    uint8_t  oam_addr;
    uint8_t  read_buffer;
    uint8_t  fine_x;
    uint8_t  write_toggle;
    uint8_t  scroll_x;
    uint8_t  scroll_y;
    uint8_t  render_fine_x;
    uint8_t  render_scroll_x;
    uint8_t  render_scroll_y;
    uint8_t  render_base_nametable;
    uint8_t  sprite0_hit_ever;
    uint8_t  max_scanline_sprite_count;
    uint16_t vram_addr;
    uint16_t temp_addr;
    uint16_t render_vram_addr;
    uint64_t frame_count;
    uint64_t completed_frame_count;
    uint8_t  oam[256];
    uint8_t  nametables[2048];
    uint8_t  palette[32];
} SaveStatePpu;

typedef struct {
    uint8_t  enabled;
    uint8_t  length_halt;
    uint8_t  constant_volume;
    uint8_t  envelope_start;
    uint8_t  sweep_enabled;
    uint8_t  sweep_negate;
    uint8_t  sweep_reload;
    uint8_t  sweep_ones_complement;
    uint8_t  duty;
    uint8_t  duty_step;
    uint8_t  volume_period;
    uint8_t  envelope_divider;
    uint8_t  envelope_decay;
    uint8_t  sweep_period;
    uint8_t  sweep_divider;
    uint8_t  sweep_shift;
    uint8_t  length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} SaveStatePulse;

typedef struct {
    uint8_t  enabled;
    uint8_t  control_flag;
    uint8_t  linear_reload_flag;
    uint8_t  sequence_step;
    uint8_t  linear_reload_value;
    uint8_t  linear_counter;
    uint8_t  length_counter;
    uint16_t timer_period;
    uint16_t timer_counter;
} SaveStateTriangle;

typedef struct {
    uint8_t  enabled;
    uint8_t  length_halt;
    uint8_t  constant_volume;
    uint8_t  envelope_start;
    uint8_t  mode;
    uint8_t  volume_period;
    uint8_t  envelope_divider;
    uint8_t  envelope_decay;
    uint8_t  length_counter;
    uint8_t  period_index;
    uint16_t timer_period;
    uint16_t timer_counter;
    uint16_t shift_register;
} SaveStateNoise;

typedef struct {
    uint8_t  enabled;
    uint8_t  irq_enabled;
    uint8_t  loop_flag;
    uint8_t  silence_flag;
    uint8_t  sample_buffer_filled;
    uint8_t  rate_index;
    uint8_t  output_level;
    uint8_t  sample_buffer;
    uint8_t  shift_register;
    uint8_t  bits_remaining;
    uint16_t timer_period;
    uint16_t timer_counter;
    uint16_t sample_address;
    uint16_t sample_length;
    uint16_t current_address;
    uint16_t bytes_remaining;
} SaveStateDmc;

typedef struct {
    uint8_t  registers[0x18];
    uint32_t cpu_cycles;
    uint32_t frame_counter_cycle;
    uint64_t frame_counter_steps;
    uint32_t sample_phase;
    uint8_t  status;
    uint8_t  frame_counter_mode_5;
    uint8_t  frame_irq_inhibit;
    float    hp_prev_x;
    float    hp_prev_y;
    float    lp_prev_y;
    SaveStatePulse    pulse[2];
    SaveStateTriangle triangle;
    SaveStateNoise    noise;
    SaveStateDmc      dmc;
} SaveStateApu;

typedef struct {
    uint32_t mirror_mode;

    /* MMC1 */
    uint8_t mmc1_shift;
    uint8_t mmc1_shift_count;
    uint8_t mmc1_control;
    uint8_t mmc1_chr0;
    uint8_t mmc1_chr1;
    uint8_t mmc1_prg;

    /* MMC3 */
    uint8_t mmc3_bank_select;
    uint8_t mmc3_bank_data[8];
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    uint8_t mmc3_irq_reload;
    uint8_t mmc3_irq_enabled;
    uint8_t irq_pending;

    /* CNROM / ColorDreams / GxROM CHR bank */
    uint8_t cnrom_chr_bank;

    /* MMC2 */
    uint8_t mmc2_chr0_fd;
    uint8_t mmc2_chr0_fe;
    uint8_t mmc2_chr1_fd;
    uint8_t mmc2_chr1_fe;
    uint8_t mmc2_latch0;
    uint8_t mmc2_latch1;

    /* Mapper 40 */
    uint16_t m40_irq_counter;
    uint8_t  m40_irq_enabled;

    /* PRG bank pointer caches, stored as offsets from cartridge.prg_rom.
     * prg_bank_lo/hi are always valid; prg_banks_8k[]/m40_prg_6000 use
     * SAVE_STATE_PTR_NONE when the active mapper doesn't populate them. */
    uint32_t prg_bank_lo_offset;
    uint32_t prg_bank_hi_offset;
    uint32_t prg_banks_8k_offset[4];
    uint32_t m40_prg_6000_offset;

    /* CHR RAM contents (chr_is_ram == true).  chr_ram_size is 0 when the
     * cartridge uses CHR ROM, in which case chr_ram[] is unused. */
    uint32_t chr_ram_size;
    uint8_t  chr_ram[8192];
} SaveStateCartridge;

typedef struct {
    SaveStateHeader      header;
    SaveStateCpu         cpu;
    uint8_t              cpu_ram[2048];
    uint8_t              wram[8192];
    SaveStateController  controllers[2];
    SaveStatePpu         ppu;
    SaveStateApu         apu;
    SaveStateCartridge   cartridge;
    uint32_t             crc32;
} SaveStateBlob;

/* CRC32 (polynomial 0xEDB88320, reflected) over data[0..size). */
uint32_t save_state_crc32(const uint8_t *data, size_t size);

/* Snapshot the current emulator state into *out.  rom_checksum/rom_image_size
 * are recorded in the header so save_state_apply() can verify the blob was
 * captured for the ROM currently loaded. */
void save_state_capture(const Nes *nes, uint32_t rom_checksum, uint32_t rom_image_size,
                        uint32_t elapsed_seconds, SaveStateBlob *out);

/* Restore *nes from *blob.  Returns false (leaving nes untouched) if the
 * blob's magic/version don't match, its CRC32 doesn't match its contents, or
 * rom_checksum/rom_image_size don't match the currently loaded ROM. */
bool save_state_apply(Nes *nes, const SaveStateBlob *blob,
                      uint32_t rom_checksum, uint32_t rom_image_size);

/* Format elapsed seconds as "MM:SS", growing past 99 minutes if needed
 * (e.g. "125:34").  out_size should be at least 8. */
void save_state_format_elapsed(uint32_t elapsed_seconds, char *out, size_t out_size);

#endif
