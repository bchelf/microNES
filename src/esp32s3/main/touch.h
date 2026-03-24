#pragma once

#include <stdbool.h>
#include <stdint.h>

// Maximum simultaneous touch points tracked
#define TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x;  // Landscape x coordinate (0 .. DISPLAY_W-1)
    uint16_t y;  // Landscape y coordinate (0 .. DISPLAY_H-1)
    bool     valid;
} TouchPoint;

typedef struct {
    TouchPoint points[TOUCH_MAX_POINTS];
    uint8_t    count;  // Number of active touch points
} TouchData;

// Initialise I2C bus and FT3168 touch controller.
bool touch_init(void);

// Read current touch state.
// Coordinates are converted to landscape space automatically.
void touch_read(TouchData *out);
