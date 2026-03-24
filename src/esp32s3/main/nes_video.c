#include "nes_video.h"
#include "framebuffer.h"

// ─────────────────────────────────────────────────────────────
//  Standard NTSC NES palette → RGB565 look-up table
//
//  RGB565 big-endian as required for SPI:
//      stored_value = bswap16( ((r&0xF8)<<8) | ((g&0xFC)<<3) | (b>>3) )
//
//  Source palette: standard NTSC (NesDev wiki reference values).
// ─────────────────────────────────────────────────────────────

// Helper macro: pack RGB888 → RGB565 big-endian
#define C(r,g,b) ( (uint16_t)( (((r)&0xF8u)<<8) | (((g)&0xFCu)<<3) | ((b)>>3) ) )
#define CB(r,g,b) ( (uint16_t)( (C(r,g,b) >> 8) | (C(r,g,b) << 8) ) )

static const uint16_t k_nes_palette_be[64] = {
    /* 0x00 */ CB( 84, 84, 84), CB(  0, 30,116), CB(  8, 16,144), CB( 48,  0,136),
    /* 0x04 */ CB( 68,  0,100), CB( 92,  0, 48), CB( 84,  4,  0), CB( 60, 24,  0),
    /* 0x08 */ CB( 32, 42,  0), CB(  8, 58,  0), CB(  0, 64,  0), CB(  0, 60,  0),
    /* 0x0C */ CB(  0, 50, 60), CB(  0,  0,  0), CB(  0,  0,  0), CB(  0,  0,  0),

    /* 0x10 */ CB(152,150,152), CB(  8, 76,196), CB( 48, 50,236), CB( 92, 30,228),
    /* 0x14 */ CB(136, 20,176), CB(160, 20,100), CB(152, 34, 32), CB(120, 60,  0),
    /* 0x18 */ CB( 84, 90,  0), CB( 40,114,  0), CB(  8,124,  0), CB(  0,118, 40),
    /* 0x1C */ CB(  0,102,120), CB(  0,  0,  0), CB(  0,  0,  0), CB(  0,  0,  0),

    /* 0x20 */ CB(236,238,236), CB( 76,154,236), CB(120,124,236), CB(176, 98,236),
    /* 0x24 */ CB(228, 84,236), CB(236, 88,180), CB(236,106,100), CB(212,136, 32),
    /* 0x28 */ CB(160,170,  0), CB(116,196,  0), CB( 76,208, 32), CB( 56,204,108),
    /* 0x2C */ CB( 56,180,204), CB( 60, 60, 60), CB(  0,  0,  0), CB(  0,  0,  0),

    /* 0x30 */ CB(236,238,236), CB(168,204,236), CB(188,188,236), CB(212,178,236),
    /* 0x34 */ CB(236,174,236), CB(236,174,212), CB(236,180,176), CB(228,196,144),
    /* 0x38 */ CB(204,210,120), CB(180,222,120), CB(168,226,144), CB(152,226,180),
    /* 0x3C */ CB(160,214,228), CB(160,162,160), CB(  0,  0,  0), CB(  0,  0,  0),
};

#undef C
#undef CB

void nes_video_convert_pixels(const uint8_t *palette_src,
                              uint16_t *dst_rgb565_be,
                              uint16_t count)
{
    for (uint16_t x = 0; x < count; ++x) {
        dst_rgb565_be[x] = k_nes_palette_be[palette_src[x] & 0x3Fu];
    }
}

void nes_video_convert_scanline(const uint8_t *palette_row,
                                uint16_t *dst_rgb565_be)
{
    nes_video_convert_pixels(palette_row, dst_rgb565_be, NES_FRAME_WIDTH);
}
