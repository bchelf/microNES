#include "cart.h"
#include "axrom.h"
#include "cnrom.h"
#include "colordreams.h"
#include "gxrom.h"
#include "mmc1.h"
#include "mmc2.h"
#include "mmc3.h"
#include "uxrom.h"

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

#ifndef MICRONES_CHR_ROW_CACHE_MAX_BYTES
#define MICRONES_CHR_ROW_CACHE_MAX_BYTES 0xffffffffu
#endif

#ifndef MICRONES_PICO_PRG_SRAM_COPY_MAX
#define MICRONES_PICO_PRG_SRAM_COPY_MAX 0xffffffffu
#endif

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
    if (total_bytes > (size_t)MICRONES_CHR_ROW_CACHE_MAX_BYTES) {
        cartridge->chr_row_pixels = NULL;
        cartridge->chr_row_count = 0;
        return true;
    }

    /* Prefer internal SRAM: chr_row_pixels is read on every PPU tile render
     * (33 tiles × 240 scanlines = 7920 reads per frame).  PSRAM reads go
     * through DCache and can cause miss pressure.  CART_MALLOC_INTERNAL uses
     * heap_caps_malloc(MALLOC_CAP_INTERNAL) on ESP32 so the allocator doesn't
     * fall back to PSRAM; on other platforms it returns NULL → plain malloc. */
    cartridge->chr_row_pixels = (uint8_t *)CART_MALLOC_INTERNAL(total_bytes);
    if (cartridge->chr_row_pixels == NULL) {
        cartridge->chr_row_pixels = (uint8_t *)malloc(total_bytes);
    }
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
    bool is_nes2;
    uint8_t flags6;
    uint8_t flags7;
    uint32_t mapper;
    uint32_t prg_banks;
    uint32_t chr_banks;
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
    is_nes2 = (flags7 & 0x0cu) == 0x08u;

    cartridge->is_nes2 = is_nes2;
    cartridge->has_battery = (flags6 & 0x02u) != 0;

    mapper = (uint32_t)(flags6 >> 4) | (uint32_t)(flags7 & 0xf0u);
    if (is_nes2) {
        mapper |= (uint32_t)(rom_image[8] & 0x0fu) << 8;
        cartridge->submapper = (uint8_t)(rom_image[8] >> 4);
        cartridge->prg_ram_shift = (uint8_t)(rom_image[10] & 0x0fu);
        cartridge->prg_nvram_shift = (uint8_t)(rom_image[10] >> 4);
        cartridge->chr_ram_shift = (uint8_t)(rom_image[11] & 0x0fu);
        cartridge->chr_nvram_shift = (uint8_t)(rom_image[11] >> 4);
    }

    if (mapper > 255u) {
        cart_set_error(error, error_size, "mapper number exceeds runtime cartridge model");
        return false;
    }
    cartridge->mapper = (uint8_t)mapper;
    switch (cartridge->mapper) {
    case 0: case 1: case 2: case 3: case 4: case 7: case 9: case 11: case 66:
        break;
    default:
        cart_set_error(error, error_size,
            "unsupported mapper (only 0/NROM, 1/MMC1, 2/UxROM, 3/CNROM, "
            "4/MMC3, 7/AxROM, 9/MMC2, 11/ColorDreams, 66/GxROM)");
        return false;
    }

    if (is_nes2 && cartridge->mapper == 1 && cartridge->submapper != 0 && cartridge->submapper != 5) {
        cart_set_error(error, error_size, "only mapper 1 submapper 0 and 5 are supported");
        return false;
    }

    if ((flags6 & 0x08u) != 0) {
        cart_set_error(error, error_size, "four-screen mirroring is not supported");
        return false;
    }

    prg_banks = rom_image[4];
    chr_banks = rom_image[5];
    if (is_nes2) {
        if ((rom_image[9] & 0x0fu) == 0x0fu || (rom_image[9] >> 4) == 0x0fu) {
            cart_set_error(error, error_size, "NES 2.0 exponent/multiplier ROM sizes are not supported");
            return false;
        }
        prg_banks |= (uint32_t)(rom_image[9] & 0x0fu) << 8;
        chr_banks |= (uint32_t)(rom_image[9] >> 4) << 8;
    }
    if (prg_banks > 255u || chr_banks > 255u) {
        cart_set_error(error, error_size, "ROM bank count exceeds runtime cartridge model");
        return false;
    }
    cartridge->prg_banks = (uint8_t)prg_banks;
    cartridge->chr_banks = (uint8_t)chr_banks;
    cartridge->mirror_mode = (flags6 & 0x01u) ? NES_MIRROR_VERTICAL : NES_MIRROR_HORIZONTAL;

    if (cartridge->mapper == 0) {
        if (cartridge->prg_banks != 1 && cartridge->prg_banks != 2) {
            cart_set_error(error, error_size, "NROM expects 16 KiB or 32 KiB of PRG ROM");
            return false;
        }
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

    /* Initialize PRG bank window pointers used by the hot CPU read path */
    switch (cartridge->mapper) {
    case 0:
        cartridge->prg_bank_lo = cartridge->prg_rom;
        cartridge->prg_bank_hi = (cartridge->prg_rom_size == 0x4000u)
            ? cartridge->prg_rom
            : cartridge->prg_rom + 0x4000u;
        break;
    case 1:
        mmc1_cart_init(cartridge);
        break;
    case 2:
        uxrom_cart_init(cartridge);
        break;
    case 3:
        cnrom_cart_init(cartridge);
        break;
    case 4:
        mmc3_cart_init(cartridge);
        break;
    case 7:
        axrom_cart_init(cartridge);
        break;
    case 9:
        mmc2_cart_init(cartridge);
        break;
    case 11:
        colordreams_cart_init(cartridge);
        break;
    case 66:
        gxrom_cart_init(cartridge);
        break;
    }

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

#if defined(ESP_PLATFORM) || defined(MICRONES_PICO_PLATFORM)
    // PRG ROM is the CPU's hot instruction/data path.  ESP32-S3 benefits from
    // keeping it in internal SRAM, and Pico HDMI needs to reduce XIP flash bus
    // pressure so the HSTX scanline IRQ/DMA cadence stays stable.  CHR ROM can
    // remain in the caller's flash-backed image; render-time tile rows use the
    // decoded chr_row_pixels cache built above.
    uint8_t *prg_dram = NULL;
    if (cartridge->prg_rom_size <= (size_t)MICRONES_PICO_PRG_SRAM_COPY_MAX) {
        prg_dram = (uint8_t *)CART_MALLOC_INTERNAL(cartridge->prg_rom_size);
    }
    if (prg_dram == NULL) {
        // Internal SRAM full or not a platform with a special allocator: fall
        // back to the regular heap.
        if (cartridge->prg_rom_size <= (size_t)MICRONES_PICO_PRG_SRAM_COPY_MAX) {
            prg_dram = (uint8_t *)malloc(cartridge->prg_rom_size);
        }
    }
    if (prg_dram != NULL) {
        const uint8_t *old_prg = cartridge->prg_rom;
        memcpy(prg_dram, old_prg, cartridge->prg_rom_size);
        cartridge->prg_rom     = prg_dram;
        /* Rebase precomputed bank pointers into the new DRAM allocation */
        if (cartridge->mapper == 4) {
            mmc3_rebase_banks(cartridge, old_prg);
        } else {
            cartridge->prg_bank_lo = prg_dram + (size_t)(cartridge->prg_bank_lo - old_prg);
            cartridge->prg_bank_hi = prg_dram + (size_t)(cartridge->prg_bank_hi - old_prg);
        }
        cartridge->rom_image   = prg_dram;   // cart_unload will free this
    } else {
        // Not enough heap for PRG copy – run from flash (slower but correct;
        // HDMI may lose signal if XIP contention gets too high).
        cart_set_error(error, error_size,
            "PRG ROM DRAM copy failed; running from flash (expect lower fps)");
        cartridge->rom_image = NULL;       // do not free flash pointer
        // Clear error so caller doesn't treat this as a fatal failure.
        cart_set_error(error, error_size, "");
    }
    // If all mallocs fail, prg_rom stays pointing into flash – slower but correct.
    // rom_image stays NULL so cart_unload never tries to free the flash buffer.
#endif

    return true;
}
