#ifndef SMB2350_CART_H
#define SMB2350_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NES_MIRROR_HORIZONTAL = 0,
    NES_MIRROR_VERTICAL = 1,
} NesMirrorMode;

typedef struct {
    uint8_t *rom_image;
    size_t rom_image_size;
    const uint8_t *prg_rom;
    size_t prg_rom_size;
    uint8_t *chr_data;
    size_t chr_size;
    uint8_t *chr_row_pixels;
    size_t chr_row_count;
    bool chr_is_ram;
    uint8_t mapper;
    uint8_t prg_banks;
    uint8_t chr_banks;
    NesMirrorMode mirror_mode;
} NesCartridge;

bool cart_load_ines_file(NesCartridge *cartridge, const char *path, char *error, size_t error_size);
bool cart_load_ines_memory(
    NesCartridge *cartridge,
    const uint8_t *rom_image,
    size_t rom_image_size,
    char *error,
    size_t error_size
);
void cart_unload(NesCartridge *cartridge);
bool cart_is_loaded(const NesCartridge *cartridge);

#endif
