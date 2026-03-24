#include "ui.h"
#include "board.h"
#include "display.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────────
//  Colour palette (RGB565 big-endian)
// ─────────────────────────────────────────────────────────────
#define COL_BG          0x18C3u  // very dark blue-grey  #181818 ish
#define COL_DPAD_BASE   0x2945u  // mid-dark grey  #282828
#define COL_DPAD_ARROW  0x632Cu  // lighter grey for the arrow shapes
#define COL_BTN_A       0x03E0u  // green  (0x03E0 = 00000 111111 00000 = pure green)
#define COL_BTN_B       0xF800u  // red
#define COL_BTN_START   0x8C71u  // mid grey
#define COL_BTN_SELECT  0x8C71u
#define COL_LABEL       0xFFFFu  // white

// Big-endian swap
#define BE(v) ((uint16_t)(((v)>>8)|((v)<<8)))

// All colours already in big-endian for SPI
static const uint16_t kBG     = BE(COL_BG);
static const uint16_t kDBase  = BE(COL_DPAD_BASE);
static const uint16_t kDArrow = BE(COL_DPAD_ARROW);
static const uint16_t kBtnA   = BE(COL_BTN_A);
static const uint16_t kBtnB   = BE(COL_BTN_B);
static const uint16_t kBtnSS  = BE(COL_BTN_START);
static const uint16_t kLabel  = BE(COL_LABEL);

// ─────────────────────────────────────────────────────────────
//  Per-zone line buffer
// ─────────────────────────────────────────────────────────────
// Maximum zone width is 140 pixels
#define ZONE_W  140
static uint16_t s_line[ZONE_W];

// ─────────────────────────────────────────────────────────────
//  Drawing helpers  (all x/y are zone-relative, 0-based)
// ─────────────────────────────────────────────────────────────

static void fill_line(int w, uint16_t colour)
{
    for (int i = 0; i < w; i++) s_line[i] = colour;
}

static void draw_pixel(int x, int y, uint16_t colour, int zone_x_offset)
{
    (void)zone_x_offset;  // unused; caller writes via display_write_row
    if (x >= 0 && x < ZONE_W) s_line[x] = colour;
}

// Draw a filled circle into the current s_line buffer for scanline y.
// cx, cy, r: circle centre and radius (zone-relative).
static void draw_circle_scanline(int cx, int cy, int r, int scan_y,
                                 uint16_t fill, uint16_t border, int border_w)
{
    int dy = scan_y - cy;
    if (abs(dy) > r) return;
    int dx = (int)__builtin_sqrt((float)((r) * (r) - dy * dy));
    int x0 = cx - dx, x1 = cx + dx;
    // fill
    for (int x = x0 + border_w; x <= x1 - border_w; x++) {
        if (x >= 0 && x < ZONE_W) s_line[x] = fill;
    }
    // border
    for (int w = 0; w < border_w; w++) {
        int bx0 = x0 + w, bx1 = x1 - w;
        if (bx0 >= 0 && bx0 < ZONE_W) s_line[bx0] = border;
        if (bx1 >= 0 && bx1 < ZONE_W) s_line[bx1] = border;
    }
}

// Draw a filled rectangle into s_line for the given scanline y.
static void draw_rect_scanline(int rx, int ry, int rw, int rh, int scan_y,
                               uint16_t fill, uint16_t border)
{
    if (scan_y < ry || scan_y >= ry + rh) return;
    bool is_top    = (scan_y == ry);
    bool is_bottom = (scan_y == ry + rh - 1);
    for (int x = rx; x < rx + rw; x++) {
        if (x < 0 || x >= ZONE_W) continue;
        bool is_side = (x == rx || x == rx + rw - 1);
        s_line[x] = (is_top || is_bottom || is_side) ? border : fill;
    }
}

// Draw a directional arrow triangle into s_line for scanline y.
// dir: 0=up,1=down,2=left,3=right
static void draw_arrow_scanline(int cx, int cy, int size, int dir, int scan_y,
                                uint16_t colour)
{
    // Simple filled triangle approximation
    int half;
    int base_y;
    switch (dir) {
        case 0: // UP: apex at top, base at bottom
            base_y = cy + size / 2;
            if (scan_y < cy - size / 2 || scan_y > base_y) return;
            half = (scan_y - (cy - size / 2)) * (size / 2) / size;
            for (int x = cx - half; x <= cx + half; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = colour;
            break;
        case 1: // DOWN
            base_y = cy - size / 2;
            if (scan_y < base_y || scan_y > cy + size / 2) return;
            half = ((cy + size / 2) - scan_y) * (size / 2) / size;
            for (int x = cx - half; x <= cx + half; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = colour;
            break;
        case 2: // LEFT: apex left, base right
            if (abs(scan_y - cy) > size / 2) return;
            half = (size / 2 - abs(scan_y - cy)) * (size / 2) / (size / 2);
            for (int x = cx - size / 2; x <= cx - size / 2 + half; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = colour;
            break;
        case 3: // RIGHT
            if (abs(scan_y - cy) > size / 2) return;
            half = (size / 2 - abs(scan_y - cy)) * (size / 2) / (size / 2);
            for (int x = cx + size / 2 - half; x <= cx + size / 2; x++)
                if (x >= 0 && x < ZONE_W) s_line[x] = colour;
            break;
    }
}

// ─────────────────────────────────────────────────────────────
//  D-pad zone (140×240), D-pad centre at (70, 120)
// ─────────────────────────────────────────────────────────────
#define DP_CX  70
#define DP_CY 120
#define DP_ARM_W  28   // width of each arm
#define DP_ARM_H  48   // length of each arm
#define DP_BG_R   54   // outer radius of circular D-pad background

static void draw_dpad_scanline(int y)
{
    fill_line(UI_LEFT_W, kBG);

    // Circular D-pad background
    {
        int dy = y - DP_CY;
        if (dy * dy <= DP_BG_R * DP_BG_R) {
            int dx = (int)__builtin_sqrt((float)(DP_BG_R * DP_BG_R - dy * dy));
            for (int x = DP_CX - dx; x <= DP_CX + dx; x++)
                if (x >= 0 && x < UI_LEFT_W) s_line[x] = kDBase;
        }
    }

    // Cross shape: vertical arm
    bool in_v_arm = (y >= DP_CY - DP_ARM_H && y <= DP_CY + DP_ARM_H);
    if (in_v_arm) {
        int x0 = DP_CX - DP_ARM_W / 2;
        int x1 = DP_CX + DP_ARM_W / 2;
        for (int x = x0; x <= x1; x++)
            if (x >= 0 && x < UI_LEFT_W) s_line[x] = kDBase;
        // Border highlight
        if (x0 >= 0 && x0 < UI_LEFT_W) s_line[x0] = kDArrow;
        if (x1 >= 0 && x1 < UI_LEFT_W) s_line[x1] = kDArrow;
    }

    // Cross shape: horizontal arm
    bool in_h_arm = (y >= DP_CY - DP_ARM_W / 2 && y <= DP_CY + DP_ARM_W / 2);
    if (in_h_arm) {
        int x0 = DP_CX - DP_ARM_H;
        int x1 = DP_CX + DP_ARM_H;
        for (int x = x0; x <= x1; x++)
            if (x >= 0 && x < UI_LEFT_W) s_line[x] = kDBase;
        if (x0 >= 0 && x0 < UI_LEFT_W) s_line[x0] = kDArrow;
        if (x1 >= 0 && x1 < UI_LEFT_W) s_line[x1] = kDArrow;
    }

    // Direction arrows (triangles)
    draw_arrow_scanline(DP_CX, DP_CY - DP_ARM_H / 2 - 4, 18, 0, y, kDArrow); // UP
    draw_arrow_scanline(DP_CX, DP_CY + DP_ARM_H / 2 + 4, 18, 1, y, kDArrow); // DOWN
    draw_arrow_scanline(DP_CX - DP_ARM_H / 2 - 4, DP_CY, 18, 2, y, kDArrow); // LEFT
    draw_arrow_scanline(DP_CX + DP_ARM_H / 2 + 4, DP_CY, 18, 3, y, kDArrow); // RIGHT
}

// ─────────────────────────────────────────────────────────────
//  Button zone (140×240), zone-relative coordinates
//  (display_x_offset = 396)
// ─────────────────────────────────────────────────────────────
// Select: top-left,  x 2..56,  y 4..34
// Start:  top-right, x 84..138, y 4..34
// B:      large,  centre (38, 155), r=34
// A:      large,  centre (102, 130), r=38
#define BTN_B_CX  38
#define BTN_B_CY 155
#define BTN_B_R   34

#define BTN_A_CX 102
#define BTN_A_CY 130
#define BTN_A_R   38

static void draw_buttons_scanline(int y)
{
    fill_line(UI_RIGHT_W, kBG);

    // START  (small, top)
    draw_rect_scanline(84, 4, 54, 26, y, kBtnSS, kDArrow);
    // SELECT (small, top)
    draw_rect_scanline( 2, 4, 54, 26, y, kBtnSS, kDArrow);

    // B button
    draw_circle_scanline(BTN_B_CX, BTN_B_CY, BTN_B_R, y, kBtnB, kDArrow, 2);
    // A button
    draw_circle_scanline(BTN_A_CX, BTN_A_CY, BTN_A_R, y, kBtnA, kDArrow, 2);
}

// ─────────────────────────────────────────────────────────────
//  NES centre: black fill
// ─────────────────────────────────────────────────────────────
static uint16_t s_black_row[NES_DISPLAY_W];

// ─────────────────────────────────────────────────────────────
//  Public entry point
// ─────────────────────────────────────────────────────────────
void ui_draw_overlay(void)
{
    // Prepare black row for NES area
    for (int i = 0; i < NES_DISPLAY_W; i++) s_black_row[i] = 0x0000u;

    // Draw left zone (D-pad): 140 columns × 240 rows
    display_stream_begin(UI_LEFT_X, 0, UI_LEFT_W, DISPLAY_H);
    for (int y = 0; y < DISPLAY_H; y++) {
        draw_dpad_scanline(y);
        display_stream_row(s_line, UI_LEFT_W);
    }
    display_stream_end();

    // Draw NES area as solid black (will be overwritten by emulator)
    display_stream_begin(NES_DISPLAY_X, 0, NES_DISPLAY_W, NES_DISPLAY_H);
    for (int y = 0; y < NES_DISPLAY_H; y++) {
        display_stream_row(s_black_row, NES_DISPLAY_W);
    }
    display_stream_end();

    // Draw right zone (buttons): 140 columns × 240 rows
    display_stream_begin(UI_RIGHT_X, 0, UI_RIGHT_W, DISPLAY_H);
    for (int y = 0; y < DISPLAY_H; y++) {
        draw_buttons_scanline(y);
        display_stream_row(s_line, UI_RIGHT_W);
    }
    display_stream_end();
}
