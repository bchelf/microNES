#ifndef MICRONES_FONT_5X7_H
#define MICRONES_FONT_5X7_H

#include "framebuffer.h"

#include <stdbool.h>
#include <stdint.h>

/* 5x7 monochrome bitmap font for ASCII 0x20..0x7E.  Each glyph occupies a
 * 6x8 cell (5px glyph + 1px right gutter + 1px bottom gutter) so adjacent
 * characters and lines naturally separate without extra spacing logic. */

enum {
    FONT5X7_GLYPH_W = 5,
    FONT5X7_GLYPH_H = 7,
    FONT5X7_CELL_W  = 6,
    FONT5X7_CELL_H  = 8,
};

void font5x7_draw_char(NesFrameBuffer *fb, int x, int y, char ch, uint8_t color);
void font5x7_draw_text(NesFrameBuffer *fb, int x, int y, const char *text, uint8_t color);
int  font5x7_text_width(const char *text); /* pixels including inter-glyph gutter */

#endif
