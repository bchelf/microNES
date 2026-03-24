#include "nes_input.h"
#include "board.h"

// ─────────────────────────────────────────────────────────────
//  Touch zones in landscape coordinates (absolute, 536×240)
// ─────────────────────────────────────────────────────────────

// D-pad – left zone (0..139)
// Centre at (70, 120).  Each direction gets a 50-pixel-wide wedge,
// with a 14-pixel dead-zone ring around the centre.
#define DPAD_CX   70
#define DPAD_CY  120
#define DPAD_DEAD  14   // radius of dead zone

// Rectangular hit regions for each direction
#define UP_X1    20  // x range shared with L/R
#define UP_X2   120
#define UP_Y1    15
#define UP_Y2    76

#define DOWN_X1  20
#define DOWN_X2 120
#define DOWN_Y1 164
#define DOWN_Y2 225

#define LEFT_X1   2
#define LEFT_X2  56
#define LEFT_Y1  76
#define LEFT_Y2 164

#define RIGHT_X1  84
#define RIGHT_X2 138
#define RIGHT_Y1  76
#define RIGHT_Y2 164

// Buttons – right zone (396..535)  relative coords +396
#define RZ  396   // right zone origin

// Start / Select are small and placed near the top to avoid accidental presses
#define SELECT_X1  (RZ +  2)
#define SELECT_X2  (RZ + 56)
#define SELECT_Y1   4
#define SELECT_Y2  34

#define START_X1   (RZ + 82)
#define START_X2   (RZ + 138)
#define START_Y1   4
#define START_Y2  34

// B button – lower-left of right zone, large circular-ish region
#define B_CX  (RZ + 38)
#define B_CY  160
#define B_R   36   // radius

// A button – lower-right of right zone, large circular-ish region
#define A_CX  (RZ + 102)
#define A_CY  135
#define A_R   38   // radius

static inline bool in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static inline bool in_circle(int x, int y, int cx, int cy, int r)
{
    int dx = x - cx, dy = y - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

NesControllerState nes_input_from_touch(const TouchData *touch)
{
    uint8_t buttons = 0;

    for (uint8_t i = 0; i < touch->count; i++) {
        if (!touch->points[i].valid) continue;
        int lx = (int)touch->points[i].x;
        int ly = (int)touch->points[i].y;

        // ── D-pad ──────────────────────────────────────────────
        if (lx < UI_RIGHT_X) {
            if (in_rect(lx, ly, UP_X1, UP_Y1, UP_X2, UP_Y2))
                buttons |= NES_BUTTON_UP;
            if (in_rect(lx, ly, DOWN_X1, DOWN_Y1, DOWN_X2, DOWN_Y2))
                buttons |= NES_BUTTON_DOWN;
            if (in_rect(lx, ly, LEFT_X1, LEFT_Y1, LEFT_X2, LEFT_Y2))
                buttons |= NES_BUTTON_LEFT;
            if (in_rect(lx, ly, RIGHT_X1, RIGHT_Y1, RIGHT_X2, RIGHT_Y2))
                buttons |= NES_BUTTON_RIGHT;
        }

        // ── Action buttons ────────────────────────────────────
        if (lx >= UI_RIGHT_X) {
            if (in_rect(lx, ly, SELECT_X1, SELECT_Y1, SELECT_X2, SELECT_Y2))
                buttons |= NES_BUTTON_SELECT;
            if (in_rect(lx, ly, START_X1, START_Y1, START_X2, START_Y2))
                buttons |= NES_BUTTON_START;
            if (in_circle(lx, ly, B_CX, B_CY, B_R))
                buttons |= NES_BUTTON_B;
            if (in_circle(lx, ly, A_CX, A_CY, A_R))
                buttons |= NES_BUTTON_A;
        }
    }

    return (NesControllerState){ .buttons = buttons };
}
