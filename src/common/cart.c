#include "cart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// heap_caps_malloc is available on ESP32 (esp-idf) for MALLOC_CAP_INTERNAL.
// On other platforms it is not available; use a no-op shim so cart.c stays
// portable.
#ifdef ESP_PLATFORM
#  include "esp_heap_caps.h"
#  define CART_MALLOC_INTERNAL(sz) \
       heap_caps_malloc((sz), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#else
#  define CART_MALLOC_INTERNAL(sz) NULL  // fall through to plain malloc below
#endif

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

static void cart_decode_chr_row(NesCartridge *cartridge, size_t low_addr) {
    size_t row_index;
    uint8_t *dst;
    uint8_t low;
    uint8_t high;

    if (cartridge->chr_row_pixels == NULL || cartridge->chr_size == 0) {
        return;
    }

    low_addr %= cartridge->chr_size;
    row_index = ((low_addr >> 4) * 8u) + (low_addr & 0x07u);
    dst = &cartridge->chr_row_pixels[row_index * 8u];
    low = cartridge->chr_data[low_addr];
    high = cartridge->chr_data[(low_addr + 8u) % cartridge->chr_size];

    for (int x = 0; x < 8; ++x) {
        uint8_t bit = (uint8_t)(7 - x);
        dst[x] = (uint8_t)((((high >> bit) & 0x01u) << 1) | ((low >> bit) & 0x01u));
    }
}

static bool cart_build_chr_row_cache(NesCartridge *cartridge, char *error, size_t error_size) {
    size_t row_count;
    size_t total_bytes;

    if (cartridge->chr_size == 0) {
        cartridge->chr_row_pixels = NULL;
        cartridge->chr_row_count = 0;
        return true;
    }

    row_count = (cartridge->chr_size / 16u) * 8u;
    total_bytes = row_count * 8u;
    cartridge->chr_row_pixels = (uint8_t *)malloc(total_bytes);
    if (cartridge->chr_row_pixels == NULL) {
        cart_set_error(error, error_size, "failed to allocate CHR row cache");
        return false;
    }
    cartridge->chr_row_count = row_count;

    for (size_t tile = 0; tile < cartridge->chr_size / 16u; ++tile) {
        size_t tile_base = tile * 16u;
        for (size_t row = 0; row < 8u; ++row) {
            cart_decode_chr_row(cartridge, tile_base + row);
        }
    }

    return true;
}

// transfer_ownership: if true, cartridge->rom_image is set to rom_image and
// cart_unload() will free it.  Pass false when rom_image points into flash or
// another non-heap buffer – cart_unload's free(NULL) is then a safe no-op.
static bool cart_parse_ines_image(
    NesCartridge *cartridge,
    uint8_t *rom_image,
    size_t rom_image_size,
    bool transfer_ownership,
    char *error,
    size_t error_size
) {
    uint8_t flags6;
    uint8_t flags7;
    size_t offset;

    if (rom_image_size < INES_HEADER_SIZE) {
        cart_set_error(error, error_size, "ROM image is too small");
        return false;
    }

    if (memcmp(rom_image, "NES\x1a", 4) != 0) {
        cart_set_error(error, error_size, "file is not an iNES ROM");
        return false;
    }

    flags6 = rom_image[6];
    flags7 = rom_image[7];

    if ((flags7 & 0x0cu) == 0x08u) {
        cart_set_error(error, error_size, "NES 2.0 ROMs are not supported");
        return false;
    }

    cartridge->mapper = (uint8_t)((flags6 >> 4) | (flags7 & 0xf0u));
    if (cartridge->mapper != 0) {
        cart_set_error(error, error_size, "only mapper 0 / NROM is supported");
        return false;
    }

    if ((flags6 & 0x08u) != 0) {
        cart_set_error(error, error_size, "four-screen mirroring is not supported");
        return false;
    }

    cartridge->prg_banks = rom_image[4];
    cartridge->chr_banks = rom_image[5];
    cartridge->mirror_mode = (flags6 & 0x01u) ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL;

    if (cartridge->prg_banks != 1 && cartridge->prg_banks != 2) {
        cart_set_error(error, error_size, "NROM expects 16 KiB or 32 KiB of PRG ROM");
        return false;
    }

    offset = INES_HEADER_SIZE;
    if ((flags6 & 0x04u) != 0) {
        offset += INES_TRAINER_SIZE;
    }

    cartridge->prg_rom_size = (size_t)cartridge->prg_banks * INES_PRG_BANK_SIZE;
    cartridge->chr_size = (size_t)cartridge->chr_banks * INES_CHR_BANK_SIZE;

    if (rom_image_size < offset + cartridge->prg_rom_size + cartridge->chr_size) {
        cart_set_error(error, error_size, "ROM image is truncated");
        return false;
    }

    // Only take ownership when the caller allocated rom_image on the heap.
    // When transfer_ownership is false (e.g. flash-mapped const buffer),
    // cartridge->rom_image stays NULL so cart_unload's free() is a no-op.
    if (transfer_ownership) {
        cartridge->rom_image = rom_image;
        cartridge->rom_image_size = rom_image_size;
    }
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

    cartridge->prg_rom_mask = (uint32_t)(cartridge->prg_rom_size - 1u);
    cartridge->chr_mask = (uint32_t)(cartridge->chr_size - 1u);

    if (!cart_build_chr_row_cache(cartridge, error, error_size)) {
        cart_unload(cartridge);
        return false;
    }

    cart_set_error(error, error_size, "");
    return true;
}

bool cart_is_loaded(const NesCartridge *cartridge) {
    return cartridge->prg_rom != NULL;
}

void cart_unload(NesCartridge *cartridge) {
    free(cartridge->chr_row_pixels);
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

    cart_unload(cartridge);

    file = fopen(path, "rb");
    if (file == NULL) {
        cart_set_error(error, error_size, "failed to open ROM file");
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

    if (!cart_parse_ines_image(cartridge, rom_image, (size_t)file_size, true, error, error_size)) {
        free(rom_image);
        return false;
    }
    return true;
}

bool cart_load_ines_memory(
    NesCartridge *cartridge,
    const uint8_t *rom_image,
    size_t rom_image_size,
    char *error,
    size_t error_size
) {
    uint8_t *owned_copy;

    cart_unload(cartridge);

    if (rom_image == NULL || rom_image_size == 0) {
        cart_set_error(error, error_size, "ROM image is empty");
        return false;
    }

    owned_copy = (uint8_t *)malloc(rom_image_size);
    if (owned_copy == NULL) {
        cart_set_error(error, error_size, "failed to allocate ROM buffer");
        return false;
    }

    memcpy(owned_copy, rom_image, rom_image_size);
    if (!cart_parse_ines_image(cartridge, owned_copy, rom_image_size, true, error, error_size)) {
        free(owned_copy);
        return false;
    }

    return true;
}

bool cart_load_ines_const_memory(
    NesCartridge *cartridge,
    const uint8_t *rom_image,
    size_t rom_image_size,
    char *error,
    size_t error_size
) {
    cart_unload(cartridge);

    if (rom_image == NULL || rom_image_size == 0) {
        cart_set_error(error, error_size, "ROM image is empty");
        return false;
    }

    // Parse directly from the flash-mapped buffer.
    // transfer_ownership=false keeps cartridge->rom_image NULL, so any
    // internal cart_unload error path calls free(NULL) – a harmless no-op
    // instead of the crash caused by trying to free a flash address.
    if (!cart_parse_ines_image(cartridge, (uint8_t *)rom_image, rom_image_size,
                               false, error, error_size)) {
        return false;
    }

    // PRG ROM is on the 6502 instruction-fetch hot path.  Flash-mapped DROM
    // goes through DCache (shared with NES state and stack), causing miss
    // pressure.  Copy PRG into internal SRAM for zero-wait-state fetches.
    //
    // Strategy (in priority order):
    //  1. Internal SRAM  – fastest; use heap_caps_malloc(MALLOC_CAP_INTERNAL)
    //     when PSRAM is active so the allocator doesn't fall back to PSRAM.
    //  2. Any heap (PSRAM) – still 3-4× faster than flash DCache thrash.
    //  3. Flash           – slowest; accepted if all else fails.
    //
    // chr_data stays in flash: its only hot user is the chr_row_pixels cache
    // (already in DRAM), so flash latency there is acceptable.
    uint8_t *prg_dram = (uint8_t *)CART_MALLOC_INTERNAL(cartridge->prg_rom_size);
    if (prg_dram == NULL) {
        // Internal SRAM full (or non-ESP platform) – fall back to any heap.
        prg_dram = (uint8_t *)malloc(cartridge->prg_rom_size);
    }
    if (prg_dram != NULL) {
        memcpy(prg_dram, cartridge->prg_rom, cartridge->prg_rom_size);
        cartridge->prg_rom   = prg_dram;
        cartridge->rom_image = prg_dram;   // cart_unload will free this
    }
    // If all mallocs fail, prg_rom stays pointing into flash – slower but correct.
    // rom_image stays NULL so cart_unload never tries to free the flash buffer.

    return true;
}
