#include "cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    INES_HEADER_SIZE = 16,
    INES_TRAINER_SIZE = 512,
    INES_PRG_BANK_SIZE = 16 * 1024,
    INES_CHR_BANK_SIZE = 8 * 1024,
};

static void cart_set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

bool cart_is_loaded(const NesCartridge *cartridge) {
    return cartridge->prg_rom != NULL;
}

void cart_unload(NesCartridge *cartridge) {
    if (cartridge->chr_is_ram) {
        free(cartridge->chr_data);
    }
    free(cartridge->rom_image);
    memset(cartridge, 0, sizeof(*cartridge));
}

bool cart_load_ines_file(NesCartridge *cartridge, const char *path, char *error, size_t error_size) {
    FILE *file;
    long file_size;
    uint8_t *rom_image;
    uint8_t header[INES_HEADER_SIZE];
    uint8_t flags6;
    uint8_t flags7;
    size_t offset;

    cart_unload(cartridge);

    file = fopen(path, "rb");
    if (file == NULL) {
        cart_set_error(error, error_size, "failed to open ROM file");
        return false;
    }

    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        cart_set_error(error, error_size, "failed to read iNES header");
        return false;
    }

    if (memcmp(header, "NES\x1a", 4) != 0) {
        fclose(file);
        cart_set_error(error, error_size, "file is not an iNES ROM");
        return false;
    }

    flags6 = header[6];
    flags7 = header[7];

    if ((flags7 & 0x0cu) == 0x08u) {
        fclose(file);
        cart_set_error(error, error_size, "NES 2.0 ROMs are not supported");
        return false;
    }

    cartridge->mapper = (uint8_t)((flags6 >> 4) | (flags7 & 0xf0u));
    if (cartridge->mapper != 0) {
        fclose(file);
        cart_set_error(error, error_size, "only mapper 0 / NROM is supported");
        return false;
    }

    if ((flags6 & 0x08u) != 0) {
        fclose(file);
        cart_set_error(error, error_size, "four-screen mirroring is not supported");
        return false;
    }

    cartridge->prg_banks = header[4];
    cartridge->chr_banks = header[5];
    cartridge->mirror_mode = (flags6 & 0x01u) ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL;

    if (cartridge->prg_banks != 1 && cartridge->prg_banks != 2) {
        fclose(file);
        cart_set_error(error, error_size, "NROM expects 16 KiB or 32 KiB of PRG ROM");
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        cart_set_error(error, error_size, "failed to seek ROM file");
        return false;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        cart_set_error(error, error_size, "failed to determine ROM size");
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        cart_set_error(error, error_size, "failed to rewind ROM file");
        return false;
    }

    rom_image = (uint8_t *)malloc((size_t)file_size);
    if (rom_image == NULL) {
        fclose(file);
        cart_set_error(error, error_size, "failed to allocate ROM buffer");
        return false;
    }

    if (fread(rom_image, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(rom_image);
        cart_set_error(error, error_size, "failed to read ROM image");
        return false;
    }
    fclose(file);

    offset = INES_HEADER_SIZE;
    if ((flags6 & 0x04u) != 0) {
        offset += INES_TRAINER_SIZE;
    }

    cartridge->prg_rom_size = (size_t)cartridge->prg_banks * INES_PRG_BANK_SIZE;
    cartridge->chr_size = (size_t)cartridge->chr_banks * INES_CHR_BANK_SIZE;

    if ((size_t)file_size < offset + cartridge->prg_rom_size + cartridge->chr_size) {
        free(rom_image);
        cart_set_error(error, error_size, "ROM image is truncated");
        return false;
    }

    cartridge->rom_image = rom_image;
    cartridge->rom_image_size = (size_t)file_size;
    cartridge->prg_rom = rom_image + offset;
    offset += cartridge->prg_rom_size;

    if (cartridge->chr_size == 0) {
        cartridge->chr_size = INES_CHR_BANK_SIZE;
        cartridge->chr_data = (uint8_t *)calloc(cartridge->chr_size, 1);
        cartridge->chr_is_ram = true;
        if (cartridge->chr_data == NULL) {
            cart_unload(cartridge);
            cart_set_error(error, error_size, "failed to allocate CHR RAM");
            return false;
        }
    } else {
        cartridge->chr_data = rom_image + offset;
        cartridge->chr_is_ram = false;
    }

    cart_set_error(error, error_size, "");
    return true;
}
