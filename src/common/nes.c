#include "nes.h"

#include "nrom.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void nes_set_error(Nes *nes, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(nes->last_error, sizeof(nes->last_error), fmt, args);
    va_end(args);
}

static bool nes_has_cartridge(const Nes *nes) {
    return cart_is_loaded(&nes->cartridge);
}

void nes_init(Nes *nes) {
    memset(nes, 0, sizeof(*nes));
    cpu6502_init(&nes->cpu);
    ppu_init(&nes->ppu);
    apu_init(&nes->apu);
    input_controller_init(&nes->controllers[0]);
    input_controller_init(&nes->controllers[1]);
    nes_set_error(nes, "");
}

void nes_destroy(Nes *nes) {
    cart_unload(&nes->cartridge);
}

bool nes_load_cartridge_file(Nes *nes, const char *path) {
    char error[160];

    if (!cart_load_ines_file(&nes->cartridge, path, error, sizeof(error))) {
        nes_set_error(nes, "%s", error);
        return false;
    }

    nes_set_error(nes, "");
    return true;
}

void nes_reset(Nes *nes) {
    memset(nes->cpu_ram, 0, sizeof(nes->cpu_ram));
    ppu_reset(&nes->ppu);
    apu_reset(&nes->apu);
    input_controller_reset(&nes->controllers[0]);
    input_controller_reset(&nes->controllers[1]);
    cpu6502_reset(&nes->cpu, nes);
    nes_set_error(nes, "");
}

void nes_set_controller_state(Nes *nes, unsigned controller_index, NesControllerState state) {
    if (controller_index < 2) {
        input_controller_set_state(&nes->controllers[controller_index], state);
    }
}

bool nes_step_instruction(Nes *nes) {
    if (!nes_has_cartridge(nes)) {
        nes_set_error(nes, "no cartridge loaded");
        return false;
    }
    return cpu6502_step(&nes->cpu, nes);
}

bool nes_step_scanline(Nes *nes) {
    uint64_t frame_before = nes->ppu.frame_count;
    uint64_t token = ((uint64_t)frame_before << 16) | (uint16_t)(nes->ppu.scanline_buffer.y & 0xffff);

    nes->ppu.scanline_ready = false;
    nes->ppu.scanline_buffer.ready = false;

    do {
        if (!nes_step_instruction(nes)) {
            return false;
        }
    } while (!nes->ppu.scanline_ready ||
             ((((uint64_t)nes->ppu.scanline_buffer.frame_index << 16) |
               nes->ppu.scanline_buffer.y) == token));

    return true;
}

bool nes_step_frame(Nes *nes) {
    uint64_t target = nes->ppu.frame_count + 1;

    nes->ppu.frame_ready = false;
    while (nes->ppu.frame_count < target) {
        if (!nes_step_instruction(nes)) {
            return false;
        }
    }
    return true;
}

const Cpu6502 *nes_cpu_state(const Nes *nes) {
    return &nes->cpu;
}

uint64_t nes_frame_count(const Nes *nes) {
    return nes->ppu.frame_count;
}

int nes_scanline(const Nes *nes) {
    return nes->ppu.scanline;
}

const NesFrameBuffer *nes_framebuffer(const Nes *nes) {
    return ppu_framebuffer(&nes->ppu);
}

const NesScanline *nes_scanline_buffer(const Nes *nes) {
    return ppu_scanline(&nes->ppu);
}

const char *nes_last_error(const Nes *nes) {
    return nes->last_error;
}

uint8_t nes_cpu_bus_read(Nes *nes, uint16_t addr) {
    if (addr < 0x2000u) {
        return nes->cpu_ram[addr & 0x07ffu];
    }
    if (addr < 0x4000u) {
        return ppu_cpu_read(&nes->ppu, &nes->cartridge, (uint16_t)(0x2000u + (addr & 0x0007u)));
    }
    if (addr == 0x4016u) {
        return input_controller_read(&nes->controllers[0]);
    }
    if (addr == 0x4017u) {
        return input_controller_read(&nes->controllers[1]);
    }
    if (addr >= 0x4000u && addr <= 0x4017u) {
        return apu_cpu_read(&nes->apu, addr);
    }
    if (addr >= 0x8000u) {
        return nrom_cpu_read(&nes->cartridge, addr);
    }
    return 0;
}

void nes_cpu_bus_write(Nes *nes, uint16_t addr, uint8_t value) {
    if (addr < 0x2000u) {
        nes->cpu_ram[addr & 0x07ffu] = value;
        return;
    }
    if (addr < 0x4000u) {
        ppu_cpu_write(&nes->ppu, &nes->cartridge, (uint16_t)(0x2000u + (addr & 0x0007u)), value);
        return;
    }
    if (addr == 0x4014u) {
        uint16_t base = (uint16_t)value << 8;
        for (uint16_t i = 0; i < 256u; ++i) {
            nes->ppu.oam[(uint8_t)(nes->ppu.oam_addr + i)] = nes_cpu_bus_read(nes, (uint16_t)(base + i));
        }
        return;
    }
    if (addr == 0x4016u) {
        input_controller_write_strobe(&nes->controllers[0], value);
        input_controller_write_strobe(&nes->controllers[1], value);
        return;
    }
    if (addr >= 0x4000u && addr <= 0x4017u) {
        apu_cpu_write(&nes->apu, addr, value);
        return;
    }
    if (addr >= 0x8000u) {
        nrom_cpu_write(&nes->cartridge, addr, value);
    }
}
