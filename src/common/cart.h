#ifndef MICRONES_CART_H
#define MICRONES_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NES_MIRROR_HORIZONTAL = 0,
    NES_MIRROR_VERTICAL = 1,
    NES_MIRROR_ONE_SCREEN_LOWER = 2,
    NES_MIRROR_ONE_SCREEN_UPPER = 3,
} NesMirrorMode;

typedef struct {
    uint8_t *rom_image;
    size_t rom_image_size;
    const uint8_t *prg_rom;
    size_t prg_rom_size;
    uint32_t prg_rom_mask;   /* prg_rom_size - 1; use with & for fast mirroring */
    /* Precomputed 16 KiB window pointers for the hot CPU read path.
     * prg_bank_lo covers $8000-$BFFF; prg_bank_hi covers $C000-$FFFF.
     * For NROM-128 both point to prg_rom[0] (mirrors the single bank).
     * For NROM-256 lo=prg_rom[0], hi=prg_rom[16K].
     * For MMC1 these are updated by mmc1_cpu_write() on every bank switch. */
    const uint8_t *prg_bank_lo;
    const uint8_t *prg_bank_hi;
    uint8_t *chr_data;
    size_t chr_size;
    uint32_t chr_mask;       /* chr_size - 1; use with & instead of % in hot paths */
    uint8_t *chr_row_pixels;
    size_t chr_row_count;
    uint8_t chr_row_scratch[8];
    bool chr_is_ram;
    bool is_nes2;
    bool has_battery;        /* iNES flags6 bit 1: battery-backed PRG-RAM at $6000 */
    uint8_t mapper;
    uint8_t submapper;
    uint8_t prg_banks;
    uint8_t chr_banks;
    uint8_t prg_ram_shift;
    uint8_t prg_nvram_shift;
    uint8_t chr_ram_shift;
    uint8_t chr_nvram_shift;
    NesMirrorMode mirror_mode;
    /* MMC1 (mapper 1) serial shift register state */
    uint8_t mmc1_shift;        /* accumulated bits, LSB-first */
    uint8_t mmc1_shift_count;  /* number of bits written so far (0-4) */
    uint8_t mmc1_control;      /* internal control register */
    uint8_t mmc1_chr0;         /* CHR bank 0 register */
    uint8_t mmc1_chr1;         /* CHR bank 1 register */
    uint8_t mmc1_prg;          /* PRG bank register */
} NesCartridge;

bool cart_load_ines_file(NesCartridge *cartridge, const char *path, char *error, size_t error_size);
bool cart_load_ines_memory(
    NesCartridge *cartridge,
    const uint8_t *rom_image,
    size_t rom_image_size,
    char *error,
    size_t error_size
);
// Zero-copy variant: prg_rom/chr_data point directly into the caller's buffer.
// Use for flash-mapped embedded ROMs to avoid a heap allocation.
bool cart_load_ines_const_memory(
    NesCartridge *cartridge,
    const uint8_t *rom_image,
    size_t rom_image_size,
    char *error,
    size_t error_size
);
void cart_unload(NesCartridge *cartridge);
bool cart_is_loaded(const NesCartridge *cartridge);

#endif
