#include "nes.h"

#include <stdio.h>
#include <stdlib.h>

static void print_cpu_state(const Cpu6502 *cpu) {
    printf(
        "PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu OPC=%02X\n",
        cpu->pc,
        cpu->a,
        cpu->x,
        cpu->y,
        cpu->p,
        cpu->sp,
        (unsigned long long)cpu->cycles,
        cpu->last_opcode
    );
}

int main(int argc, char **argv) {
    const char *rom_path = (argc > 1) ? argv[1] : "roms/smb1.nes";
    unsigned long steps = (argc > 2) ? strtoul(argv[2], NULL, 10) : 64ul;
    Nes nes;

    nes_init(&nes);

    if (!nes_load_cartridge_file(&nes, rom_path)) {
        fprintf(stderr, "ROM load failed: %s\n", nes_last_error(&nes));
        nes_destroy(&nes);
        return 1;
    }

    nes_reset(&nes);

    printf("ROM: %s\n", rom_path);
    printf(
        "PRG=%zu CHR=%zu mapper=%u mirror=%s\n",
        nes.cartridge.prg_rom_size,
        nes.cartridge.chr_size,
        nes.cartridge.mapper,
        nes.cartridge.mirror_mode == NES_MIRROR_VERTICAL ? "vertical" : "horizontal"
    );
    printf("Reset vector: %04X\n", nes.cpu.pc);
    print_cpu_state(nes_cpu_state(&nes));

    for (unsigned long i = 0; i < steps; ++i) {
        if (!nes_step_instruction(&nes)) {
            fprintf(stderr, "CPU stopped after %lu steps: %s\n", i, nes_last_error(&nes));
            nes_destroy(&nes);
            return 2;
        }
    }

    printf("After %lu instruction steps:\n", steps);
    print_cpu_state(nes_cpu_state(&nes));
    printf(
        "PPU frame=%llu scanline=%d scanline_ready=%d\n",
        (unsigned long long)nes_frame_count(&nes),
        nes_scanline(&nes),
        nes_scanline_buffer(&nes)->ready ? 1 : 0
    );

    nes_destroy(&nes);
    return 0;
}
