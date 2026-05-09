#include "font_5x7.h"

#include <string.h>

/* Bit ordering: each row is one byte, the leftmost glyph pixel is bit 4 and
 * the rightmost is bit 0.  These macros let the table below be read as a
 * picture: '_' is off, 'X' is on. */
#define _____ 0x00
#define ____X 0x01
#define ___X_ 0x02
#define ___XX 0x03
#define __X__ 0x04
#define __X_X 0x05
#define __XX_ 0x06
#define __XXX 0x07
#define _X___ 0x08
#define _X__X 0x09
#define _X_X_ 0x0A
#define _X_XX 0x0B
#define _XX__ 0x0C
#define _XX_X 0x0D
#define _XXX_ 0x0E
#define _XXXX 0x0F
#define X____ 0x10
#define X___X 0x11
#define X__X_ 0x12
#define X__XX 0x13
#define X_X__ 0x14
#define X_X_X 0x15
#define X_XX_ 0x16
#define X_XXX 0x17
#define XX___ 0x18
#define XX__X 0x19
#define XX_X_ 0x1A
#define XX_XX 0x1B
#define XXX__ 0x1C
#define XXX_X 0x1D
#define XXXX_ 0x1E
#define XXXXX 0x1F

/* 95 printable ASCII glyphs (0x20..0x7E), 7 rows each. */
static const uint8_t k_font5x7[95][7] = {
    /* 0x20 ' ' */ { _____, _____, _____, _____, _____, _____, _____ },
    /* 0x21 '!' */ { __X__, __X__, __X__, __X__, __X__, _____, __X__ },
    /* 0x22 '"' */ { _X_X_, _X_X_, _X_X_, _____, _____, _____, _____ },
    /* 0x23 '#' */ { _X_X_, _X_X_, XXXXX, _X_X_, XXXXX, _X_X_, _X_X_ },
    /* 0x24 '$' */ { __X__, _XXXX, X_X__, _XXX_, __X_X, XXXX_, __X__ },
    /* 0x25 '%' */ { XX___, XX__X, ___X_, __X__, _X___, X__XX, ___XX },
    /* 0x26 '&' */ { _XX__, X__X_, X_X__, _X___, X_X_X, X__X_, _XX_X },
    /* 0x27 ''' */ { __X__, __X__, __X__, _____, _____, _____, _____ },
    /* 0x28 '(' */ { ___X_, __X__, _X___, _X___, _X___, __X__, ___X_ },
    /* 0x29 ')' */ { _X___, __X__, ___X_, ___X_, ___X_, __X__, _X___ },
    /* 0x2A '*' */ { _____, _X_X_, __X__, XXXXX, __X__, _X_X_, _____ },
    /* 0x2B '+' */ { _____, __X__, __X__, XXXXX, __X__, __X__, _____ },
    /* 0x2C ',' */ { _____, _____, _____, _____, _____, __X__, _X___ },
    /* 0x2D '-' */ { _____, _____, _____, XXXXX, _____, _____, _____ },
    /* 0x2E '.' */ { _____, _____, _____, _____, _____, _____, __X__ },
    /* 0x2F '/' */ { ____X, ___X_, ___X_, __X__, _X___, _X___, X____ },
    /* 0x30 '0' */ { _XXX_, X___X, X__XX, X_X_X, XX__X, X___X, _XXX_ },
    /* 0x31 '1' */ { __X__, _XX__, __X__, __X__, __X__, __X__, _XXX_ },
    /* 0x32 '2' */ { _XXX_, X___X, ____X, ___X_, __X__, _X___, XXXXX },
    /* 0x33 '3' */ { _XXX_, X___X, ____X, __XX_, ____X, X___X, _XXX_ },
    /* 0x34 '4' */ { ___X_, __XX_, _X_X_, X__X_, XXXXX, ___X_, ___X_ },
    /* 0x35 '5' */ { XXXXX, X____, XXXX_, ____X, ____X, X___X, _XXX_ },
    /* 0x36 '6' */ { _XXX_, X____, X____, XXXX_, X___X, X___X, _XXX_ },
    /* 0x37 '7' */ { XXXXX, ____X, ___X_, __X__, _X___, _X___, _X___ },
    /* 0x38 '8' */ { _XXX_, X___X, X___X, _XXX_, X___X, X___X, _XXX_ },
    /* 0x39 '9' */ { _XXX_, X___X, X___X, _XXXX, ____X, ____X, _XXX_ },
    /* 0x3A ':' */ { _____, __X__, _____, _____, _____, __X__, _____ },
    /* 0x3B ';' */ { _____, __X__, _____, _____, _____, __X__, _X___ },
    /* 0x3C '<' */ { ___X_, __X__, _X___, X____, _X___, __X__, ___X_ },
    /* 0x3D '=' */ { _____, _____, XXXXX, _____, XXXXX, _____, _____ },
    /* 0x3E '>' */ { _X___, __X__, ___X_, ____X, ___X_, __X__, _X___ },
    /* 0x3F '?' */ { _XXX_, X___X, ____X, ___X_, __X__, _____, __X__ },
    /* 0x40 '@' */ { _XXX_, X___X, X_XXX, X_X_X, X_XXX, X____, _XXX_ },
    /* 0x41 'A' */ { _XXX_, X___X, X___X, X___X, XXXXX, X___X, X___X },
    /* 0x42 'B' */ { XXXX_, X___X, X___X, XXXX_, X___X, X___X, XXXX_ },
    /* 0x43 'C' */ { _XXX_, X___X, X____, X____, X____, X___X, _XXX_ },
    /* 0x44 'D' */ { XXXX_, X___X, X___X, X___X, X___X, X___X, XXXX_ },
    /* 0x45 'E' */ { XXXXX, X____, X____, XXXX_, X____, X____, XXXXX },
    /* 0x46 'F' */ { XXXXX, X____, X____, XXXX_, X____, X____, X____ },
    /* 0x47 'G' */ { _XXX_, X___X, X____, X_XXX, X___X, X___X, _XXX_ },
    /* 0x48 'H' */ { X___X, X___X, X___X, XXXXX, X___X, X___X, X___X },
    /* 0x49 'I' */ { _XXX_, __X__, __X__, __X__, __X__, __X__, _XXX_ },
    /* 0x4A 'J' */ { ____X, ____X, ____X, ____X, ____X, X___X, _XXX_ },
    /* 0x4B 'K' */ { X___X, X__X_, X_X__, XX___, X_X__, X__X_, X___X },
    /* 0x4C 'L' */ { X____, X____, X____, X____, X____, X____, XXXXX },
    /* 0x4D 'M' */ { X___X, XX_XX, X_X_X, X_X_X, X___X, X___X, X___X },
    /* 0x4E 'N' */ { X___X, X___X, XX__X, X_X_X, X__XX, X___X, X___X },
    /* 0x4F 'O' */ { _XXX_, X___X, X___X, X___X, X___X, X___X, _XXX_ },
    /* 0x50 'P' */ { XXXX_, X___X, X___X, XXXX_, X____, X____, X____ },
    /* 0x51 'Q' */ { _XXX_, X___X, X___X, X___X, X_X_X, X__X_, _XX_X },
    /* 0x52 'R' */ { XXXX_, X___X, X___X, XXXX_, X_X__, X__X_, X___X },
    /* 0x53 'S' */ { _XXX_, X___X, X____, _XXX_, ____X, X___X, _XXX_ },
    /* 0x54 'T' */ { XXXXX, __X__, __X__, __X__, __X__, __X__, __X__ },
    /* 0x55 'U' */ { X___X, X___X, X___X, X___X, X___X, X___X, _XXX_ },
    /* 0x56 'V' */ { X___X, X___X, X___X, X___X, X___X, _X_X_, __X__ },
    /* 0x57 'W' */ { X___X, X___X, X___X, X_X_X, X_X_X, X_X_X, _X_X_ },
    /* 0x58 'X' */ { X___X, X___X, _X_X_, __X__, _X_X_, X___X, X___X },
    /* 0x59 'Y' */ { X___X, X___X, _X_X_, __X__, __X__, __X__, __X__ },
    /* 0x5A 'Z' */ { XXXXX, ____X, ___X_, __X__, _X___, X____, XXXXX },
    /* 0x5B '[' */ { _XXX_, _X___, _X___, _X___, _X___, _X___, _XXX_ },
    /* 0x5C '\' */ { X____, _X___, _X___, __X__, ___X_, ___X_, ____X },
    /* 0x5D ']' */ { _XXX_, ___X_, ___X_, ___X_, ___X_, ___X_, _XXX_ },
    /* 0x5E '^' */ { __X__, _X_X_, X___X, _____, _____, _____, _____ },
    /* 0x5F '_' */ { _____, _____, _____, _____, _____, _____, XXXXX },
    /* 0x60 '`' */ { _X___, __X__, _____, _____, _____, _____, _____ },
    /* 0x61 'a' */ { _____, _____, _XXX_, ____X, _XXXX, X___X, _XXXX },
    /* 0x62 'b' */ { X____, X____, XXXX_, X___X, X___X, X___X, XXXX_ },
    /* 0x63 'c' */ { _____, _____, _XXX_, X____, X____, X___X, _XXX_ },
    /* 0x64 'd' */ { ____X, ____X, _XXXX, X___X, X___X, X___X, _XXXX },
    /* 0x65 'e' */ { _____, _____, _XXX_, X___X, XXXXX, X____, _XXX_ },
    /* 0x66 'f' */ { __XX_, _X__X, _X___, XXX__, _X___, _X___, _X___ },
    /* 0x67 'g' */ { _____, _____, _XXXX, X___X, _XXXX, ____X, _XXX_ },
    /* 0x68 'h' */ { X____, X____, XXXX_, X___X, X___X, X___X, X___X },
    /* 0x69 'i' */ { __X__, _____, _XX__, __X__, __X__, __X__, _XXX_ },
    /* 0x6A 'j' */ { ___X_, _____, ___X_, ___X_, ___X_, X__X_, _XX__ },
    /* 0x6B 'k' */ { X____, X____, X__X_, X_X__, XX___, X_X__, X__X_ },
    /* 0x6C 'l' */ { _XX__, __X__, __X__, __X__, __X__, __X__, _XXX_ },
    /* 0x6D 'm' */ { _____, _____, XX_X_, X_X_X, X_X_X, X___X, X___X },
    /* 0x6E 'n' */ { _____, _____, XXXX_, X___X, X___X, X___X, X___X },
    /* 0x6F 'o' */ { _____, _____, _XXX_, X___X, X___X, X___X, _XXX_ },
    /* 0x70 'p' */ { _____, _____, XXXX_, X___X, XXXX_, X____, X____ },
    /* 0x71 'q' */ { _____, _____, _XXXX, X___X, _XXXX, ____X, ____X },
    /* 0x72 'r' */ { _____, _____, X_XX_, XX__X, X____, X____, X____ },
    /* 0x73 's' */ { _____, _____, _XXXX, X____, _XXX_, ____X, XXXX_ },
    /* 0x74 't' */ { _X___, _X___, XXX__, _X___, _X___, _X__X, __XX_ },
    /* 0x75 'u' */ { _____, _____, X___X, X___X, X___X, X___X, _XXXX },
    /* 0x76 'v' */ { _____, _____, X___X, X___X, X___X, _X_X_, __X__ },
    /* 0x77 'w' */ { _____, _____, X___X, X___X, X_X_X, X_X_X, _X_X_ },
    /* 0x78 'x' */ { _____, _____, X___X, _X_X_, __X__, _X_X_, X___X },
    /* 0x79 'y' */ { _____, _____, X___X, X___X, _XXXX, ____X, _XXX_ },
    /* 0x7A 'z' */ { _____, _____, XXXXX, ___X_, __X__, _X___, XXXXX },
    /* 0x7B '{' */ { ___X_, __X__, __X__, _X___, __X__, __X__, ___X_ },
    /* 0x7C '|' */ { __X__, __X__, __X__, _____, __X__, __X__, __X__ },
    /* 0x7D '}' */ { _X___, __X__, __X__, ___X_, __X__, __X__, _X___ },
    /* 0x7E '~' */ { _X__X, X_XX_, _____, _____, _____, _____, _____ },
};

static const uint8_t *font5x7_glyph(char ch) {
    unsigned u = (unsigned char)ch;
    if (u < 0x20u || u > 0x7Eu) {
        u = (unsigned)'?';
    }
    return k_font5x7[u - 0x20u];
}

void font5x7_draw_char(NesFrameBuffer *fb, int x, int y, char ch, uint8_t color) {
    const uint8_t *glyph = font5x7_glyph(ch);
    if (fb == NULL) {
        return;
    }
    for (int row = 0; row < FONT5X7_GLYPH_H; ++row) {
        int py = y + row;
        if (py < 0 || py >= NES_FRAME_HEIGHT) {
            continue;
        }
        uint8_t bits = glyph[row];
        uint8_t *line = nes_framebuffer_scanline(fb, (uint16_t)py);
        for (int col = 0; col < FONT5X7_GLYPH_W; ++col) {
            if (((bits >> (FONT5X7_GLYPH_W - 1 - col)) & 0x01u) == 0) {
                continue;
            }
            int px = x + col;
            if (px < 0 || px >= NES_FRAME_WIDTH) {
                continue;
            }
            line[px] = color;
        }
    }
}

void font5x7_draw_text(NesFrameBuffer *fb, int x, int y, const char *text, uint8_t color) {
    if (text == NULL) {
        return;
    }
    int cx = x;
    for (const char *p = text; *p != '\0'; ++p) {
        font5x7_draw_char(fb, cx, y, *p, color);
        cx += FONT5X7_CELL_W;
    }
}

int font5x7_text_width(const char *text) {
    if (text == NULL) {
        return 0;
    }
    return (int)strlen(text) * FONT5X7_CELL_W;
}
