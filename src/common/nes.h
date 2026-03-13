#ifndef SMB2350_NES_H
#define SMB2350_NES_H

#include "apu.h"
#include "cart.h"
#include "cpu6502.h"
#include "framebuffer.h"
#include "input.h"
#include "ppu.h"
#include "scanline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Nes {
    Cpu6502 cpu;
    Ppu ppu;
    Apu apu;
    NesCartridge cartridge;
    NesController controllers[2];
    uint8_t cpu_ram[2048];
    char last_error[160];
} Nes;

void nes_init(Nes *nes);
void nes_destroy(Nes *nes);
bool nes_load_cartridge_file(Nes *nes, const char *path);
void nes_reset(Nes *nes);
void nes_set_controller_state(Nes *nes, unsigned controller_index, NesControllerState state);
bool nes_step_instruction(Nes *nes);
bool nes_step_scanline(Nes *nes);
bool nes_step_frame(Nes *nes);

const Cpu6502 *nes_cpu_state(const Nes *nes);
uint64_t nes_frame_count(const Nes *nes);
int nes_scanline(const Nes *nes);
const NesFrameBuffer *nes_framebuffer(const Nes *nes);
const NesScanline *nes_scanline_buffer(const Nes *nes);
const char *nes_last_error(const Nes *nes);

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr);
void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value);

#endif
