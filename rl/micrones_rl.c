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
