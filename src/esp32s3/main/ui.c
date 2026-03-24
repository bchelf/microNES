#include "ui.h"
#include "board.h"
#include "display.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────────
//  Colour helpers
// ─────────────────────────────────────────────────────────────
// Pack RGB888 → RGB565
#define RGB(r,g,b) ((uint16_t)(((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3))

// Byte-swap for big-endian SPI wire format
#define BE(v) ((uint16_t)(((v)>>8)|((v)<<8)))

// ─────────────────────────────────────────────────────────────
//  Palette (all pre-swapped for SPI)
// ─────────────────────────────────────────────────────────────
static const uint16_t kBG         = BE(RGB( 18,  18,  22));  // near-black bg
static const uint16_t kDpadBase   = BE(RGB( 30,  28,  36));  // D-pad cross fill
static const uint16_t kDpadHiL   = BE(RGB( 80,  76,  92));  // top/left bevel
static const uint16_t kDpadShdw  = BE(RGB( 12,  11,  16));  // bot/right bevel
static const uint16_t kDpadCtr   = BE(RGB( 22,  20,  28));  // centre hub
static const uint16_t kDpadArrow = BE(RGB(140, 136, 160));  // arrow marks

static const uint16_t kBtnRed    = BE(RGB(210,  30,  30));  // A/B button face
static const uint16_t kBtnHiL    = BE(RGB(240,  80,  70));  // top-left shine
static const uint16_t kBtnShdw   = BE(RGB(120,  10,  10));  // bottom-right shadow
static const uint16_t kBtnRim    = BE(RGB( 60,   6,   6));  // outer ring

static const uint16_t kSS        = BE(RGB( 60,  56,  72));  // Start/Select pill
static const uint16_t kSSHiL     = BE(RGB( 90,  86, 108));  // pill highlight
static const uint16_t kLabel     = BE(RGB(200, 196, 220));  // button labels

// ─────────────────────────────────────────────────────────────
//  Per-zone scanline buffer (max zone width = 140)
// ─────────────────────────────────────────────────────────────
#define ZONE_W  140
static uint16_t s_line[ZONE_W];

static void fill_line(int w, uint16_t col)
{
    for (int i = 0; i < w; i++) s_line[i] = col;
}

// ─────────────────────────────────────────────────────────────
//  Integer sqrt (no libm dependency in ISR-safe path)
// ─────────────────────────────────────────────────────────────
static int isqrt(int v)
{
    return (int)__builtin_sqrt((float)v);
}

// ─────────────────────────────────────────────────────────────
//  Filled oval helper
//  Draws the portion of an axis-aligned ellipse (rx,ry) centred
//  at (cx,cy) that falls on scanline sy, using three colours:
//    inner    – body fill
//    hilight  – top-left 1-px bevel (when dy < 0 and x near left edge)
//    shadow   – bottom-right bevel
//    rim      – outermost ring
// ─────────────────────────────────────────────────────────────
static void draw_oval_scanline(int cx, int cy, int rx, int ry, int sy,
                               uint16_t rim, uint16_t hilight,
                               uint16_t inner, uint16_t shadow)
{
    int dy = sy - cy;
    if (dy < -ry || dy > ry) return;
    // x half-span at this dy: (dy/ry)^2 + (dx/rx)^2 = 1
    int rx2 = rx * rx, ry2 = ry * ry;
    int dx = isqrt(rx2 - rx2 * dy * dy / ry2);
    int x0 = cx - dx, x1 = cx + dx;
    bool upper_half = (dy <= 0);

    for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= ZONE_W) continue;
        bool edge  = (x == x0 || x == x1 ||
                      x == x0 + 1 || x == x1 - 1 ||
                      sy == cy - ry || sy == cy + ry);
        bool rim1  = (x == x0 || x == x1 ||
                      sy == cy - ry || sy == cy + ry);
        if (rim1) {
            s_line[x] = rim;
        } else if (edge) {
            // 2-px wide bevel
            bool is_hi = upper_half && (x - x0 < (x1 - x0) / 2);
            s_line[x] = is_hi ? hilight : shadow;
        } else {
            // Subtle radial shade: lighten top-left quadrant slightly
            bool quad_hi = (dy <= -ry/3) && (x <= cx);
            s_line[x] = quad_hi ? hilight : inner;
        }
    }
}

// ─────────────────────────────────────────────────────────────
//  D-pad (left zone 140×240, centre at (70, 130))
// ─────────────────────────────────────────────────────────────
#define DP_CX    70
#define DP_CY   130
#define DP_AW    28    // arm width
#define DP_AL    44    // arm half-length (tip to centre)
#define DP_BEVEL  2    // bevel thickness
#define DP_HUB_R  9    // centre hub radius

// Returns true if (x,y) is inside the cross shape
static bool in_cross(int x, int y)
{
    int ax = x - DP_CX, ay = y - DP_CY;
    bool in_v = (ax >= -(DP_AW/2) && ax <= (DP_AW/2));
    bool in_h = (ay >= -(DP_AW/2) && ay <= (DP_AW/2));
    bool in_varm = in_v && (ay >= -DP_AL && ay <= DP_AL);
    bool in_harm = in_h && (ax >= -DP_AL && ax <= DP_AL);
    return in_varm || in_harm;
}

// Directional arrow (small chevron/triangle) drawn as filled triangle
static void draw_arrow_scanline(int cx, int cy, int sz, int dir, int sy,
                                uint16_t col)
{
    int dy = sy - cy;
    switch (dir) {
        case 0: { // UP – apex at (cx, cy-sz), base at cy
            if (dy < -sz || dy > 0) return;
            int half = (-dy) * sz / sz;  // widest at base (dy=0)
            half = sz - (-dy);
            if (half < 0) half = 0;
            for (int x = cx - half; x <= cx + half; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = col;
            break;
        }
        case 1: { // DOWN
            if (dy < 0 || dy > sz) return;
            int half = sz - dy;
            if (half < 0) half = 0;
            for (int x = cx - half; x <= cx + half; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = col;
            break;
        }
        case 2: { // LEFT – apex left
            if (abs(dy) > sz) return;
            int depth = sz - abs(dy);
            int x0 = cx - sz, x1 = cx - sz + depth;
            for (int x = x0; x <= x1; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = col;
            break;
        }
        case 3: { // RIGHT – apex right
            if (abs(dy) > sz) return;
            int depth = sz - abs(dy);
            int x0 = cx + sz - depth, x1 = cx + sz;
            for (int x = x0; x <= x1; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = col;
            break;
        }
    }
}

static void draw_dpad_scanline(int sy)
{
    fill_line(UI_LEFT_W, kBG);

    // Draw every pixel in the cross shape with bevel shading
    for (int x = 0; x < UI_LEFT_W; x++) {
        int ax = x - DP_CX, ay = sy - DP_CY;

        if (!in_cross(x, sy)) continue;

        // Determine which edges this pixel is on
        bool in_v = (ax >= -(DP_AW/2) && ax <= (DP_AW/2));
        bool in_h = (ay >= -(DP_AW/2) && ay <= (DP_AW/2));

        // Top or left edge → highlight; bottom or right edge → shadow
        bool top_edge   = (in_v && ay == -DP_AL);
        bool bot_edge   = (in_v && ay ==  DP_AL);
        bool left_edge  = (in_h && ax == -DP_AL);
        bool right_edge = (in_h && ax ==  DP_AL);

        // Inner vertical/horizontal bevel lines
        bool v_left  = (in_v && ax == -(DP_AW/2));
        bool v_right = (in_v && ax ==  (DP_AW/2));
        bool h_top   = (in_h && ay == -(DP_AW/2));
        bool h_bot   = (in_h && ay ==  (DP_AW/2));

        // Centre hub (small inset circle)
        int dist2 = ax*ax + ay*ay;
        bool hub = (dist2 <= DP_HUB_R * DP_HUB_R);

        if (top_edge || left_edge || v_left || h_top) {
            s_line[x] = kDpadHiL;
        } else if (bot_edge || right_edge || v_right || h_bot) {
            s_line[x] = kDpadShdw;
        } else if (hub) {
            s_line[x] = kDpadCtr;
        } else {
            s_line[x] = kDpadBase;
        }
    }

    // Arrow indicators in each arm (small filled triangles)
    int arrow_sz = 7;
    draw_arrow_scanline(DP_CX, DP_CY - DP_AL + arrow_sz + 3, arrow_sz, 0, sy, kDpadArrow);
    draw_arrow_scanline(DP_CX, DP_CY + DP_AL - arrow_sz - 3, arrow_sz, 1, sy, kDpadArrow);
    draw_arrow_scanline(DP_CX - DP_AL + arrow_sz + 3, DP_CY, arrow_sz, 2, sy, kDpadArrow);
    draw_arrow_scanline(DP_CX + DP_AL - arrow_sz - 3, DP_CY, arrow_sz, 3, sy, kDpadArrow);
}

// ─────────────────────────────────────────────────────────────
//  Button zone (140×240, display_x_offset = UI_RIGHT_X = 396)
//
//  Layout (zone-local coords):
//    SELECT pill: centre (35, 48), 30×14
//    START  pill: centre (105, 48), 30×14
//    B button:    centre (40, 160), r=34
//    A button:    centre (100, 140), r=34
//    Labels "B"/"A" drawn below the buttons using a tiny 3×5 font
// ─────────────────────────────────────────────────────────────
#define BTN_B_CX   40
#define BTN_B_CY  160
#define BTN_B_R    34

#define BTN_A_CX  100
#define BTN_A_CY  140
#define BTN_A_R    34

#define SS_SEL_CX   35
#define SS_SEL_CY   50
#define SS_START_CX 105
#define SS_START_CY  50
#define SS_RX        18   // pill half-width
#define SS_RY         8   // pill half-height

// ─────────────────────────────────────────────────────────────
//  Tiny 3×5 bitmap font for A/B/S/E labels
//  Each character = 5 rows of 3-bit column masks (MSB=left)
// ─────────────────────────────────────────────────────────────
typedef struct { uint8_t rows[5]; } Glyph3x5;

static const Glyph3x5 kGlyphA = {{ 0b010, 0b101, 0b111, 0b101, 0b101 }};
static const Glyph3x5 kGlyphB = {{ 0b110, 0b101, 0b110, 0b101, 0b110 }};
static const Glyph3x5 kGlyphS = {{ 0b011, 0b100, 0b010, 0b001, 0b110 }};  // "S" for SELECT
static const Glyph3x5 kGlyphT = {{ 0b111, 0b010, 0b010, 0b010, 0b010 }};  // "T" for START (abbreviated)

// Draw a 3×5 glyph at (gx, gy) for scanline sy
static void draw_glyph(const Glyph3x5 *g, int gx, int gy, int sy, uint16_t col)
{
    int row = sy - gy;
    if (row < 0 || row >= 5) return;
    uint8_t mask = g->rows[row];
    for (int bit = 2; bit >= 0; bit--) {
        if (mask & (1u << bit)) {
            int x = gx + (2 - bit);
            if (x >= 0 && x < ZONE_W) s_line[x] = col;
        }
    }
}

// Draw oval button at (cx,cy) with radius r
static void draw_button(int cx, int cy, int r, int sy)
{
    int dy = sy - cy;
    if (dy < -r || dy > r) return;
    int dx = isqrt(r*r - dy*dy);
    int x0 = cx - dx, x1 = cx + dx;

    for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= ZONE_W) continue;
        int ax = x - cx;
        bool rim = (x == x0 || x == x1 || dy == -r || dy == r);
        bool bev_in = (x == x0+1 || x == x0+2 || x == x1-1 || x == x1-2 ||
                       dy == -r+1 || dy == -r+2 || dy == r-1 || dy == r-2);
        if (rim) {
            s_line[x] = kBtnRim;
        } else if (bev_in) {
            bool is_hi = (dy < 0) && (ax < 0);
            s_line[x] = is_hi ? kBtnHiL : kBtnShdw;
        } else {
            // Dome highlight: a small bright spot top-left
            bool spot = (dy <= -r/3) && (ax <= -r/4) &&
                        (ax*ax + dy*dy <= (r*r*4/9));
            s_line[x] = spot ? kBtnHiL : kBtnRed;
        }
    }
}

// Draw a rounded pill (horizontal) at (cx,cy) half-size (rx,ry)
static void draw_pill_scanline(int cx, int cy, int rx, int ry, int sy)
{
    int dy = sy - cy;
    if (dy < -ry || dy > ry) return;

    // Horizontal extent: straight body + semicircle caps
    int cap_dx = isqrt(ry*ry - dy*dy);
    int x0 = cx - rx + ry - cap_dx;   // left cap edge
    int x1 = cx + rx - ry + cap_dx;   // right cap edge
    if (x0 > x1) return;

    for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= ZONE_W) continue;
        bool top_row = (dy == -ry || (dy == -ry+1 && (x == x0 || x == x1)));
        bool bot_row = (dy ==  ry || (dy ==  ry-1 && (x == x0 || x == x1)));
        bool left_col  = (x == x0);
        bool right_col = (x == x1);
        if (top_row || left_col) {
            s_line[x] = kSSHiL;
        } else if (bot_row || right_col) {
            s_line[x] = kSS;
        } else {
            s_line[x] = kSS;
        }
    }
}

static void draw_buttons_scanline(int sy)
{
    fill_line(UI_RIGHT_W, kBG);

    // A/B large round buttons
    draw_button(BTN_B_CX, BTN_B_CY, BTN_B_R, sy);
    draw_button(BTN_A_CX, BTN_A_CY, BTN_A_R, sy);

    // "B" / "A" labels centred below each button (5px below rim)
    draw_glyph(&kGlyphB, BTN_B_CX - 1, BTN_B_CY + BTN_B_R + 4, sy, kLabel);
    draw_glyph(&kGlyphA, BTN_A_CX - 1, BTN_A_CY + BTN_A_R + 4, sy, kLabel);

    // Start / Select pills
    draw_pill_scanline(SS_SEL_CX,   SS_SEL_CY,   SS_RX, SS_RY, sy);
    draw_pill_scanline(SS_START_CX, SS_START_CY, SS_RX, SS_RY, sy);

    // "S" / "T" labels below pills
    draw_glyph(&kGlyphS, SS_SEL_CX   - 1, SS_SEL_CY   + SS_RY + 3, sy, kLabel);
    draw_glyph(&kGlyphT, SS_START_CX - 1, SS_START_CY + SS_RY + 3, sy, kLabel);
}

// ─────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────
static uint16_t s_black_row[NES_DISPLAY_W];

void ui_draw_overlay(void)
{
    for (int i = 0; i < NES_DISPLAY_W; i++) s_black_row[i] = 0x0000u;

    // Left zone: D-pad
    display_stream_begin(UI_LEFT_X, 0, UI_LEFT_W, DISPLAY_H);
    for (int y = 0; y < DISPLAY_H; y++) {
        draw_dpad_scanline(y);
        display_stream_row(s_line, UI_LEFT_W);
    }
    display_stream_end();

    // Centre: black (overwritten by emulator each frame)
    display_stream_begin(NES_DISPLAY_X, 0, NES_DISPLAY_W, NES_DISPLAY_H);
    for (int y = 0; y < NES_DISPLAY_H; y++)
        display_stream_row(s_black_row, NES_DISPLAY_W);
    display_stream_end();

    // Right zone: A/B/Start/Select
    display_stream_begin(UI_RIGHT_X, 0, UI_RIGHT_W, DISPLAY_H);
    for (int y = 0; y < DISPLAY_H; y++) {
        draw_buttons_scanline(y);
        display_stream_row(s_line, UI_RIGHT_W);
    }
    display_stream_end();
}
