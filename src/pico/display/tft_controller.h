#ifndef MICRONES_TFT_CONTROLLER_H
#define MICRONES_TFT_CONTROLLER_H

#include "display_transport.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t len;
    uint16_t delay_ms;
} TftControllerInitCommand;

typedef struct {
    const char *name;
    uint16_t width;
    uint16_t height;
    uint16_t viewport_x;
    uint16_t viewport_y;
    void (*init)(const TftTransportOps *transport);
} TftController;

const TftController *tft_controller_get(void);

void tft_controller_run_init_sequence(
    const TftTransportOps *transport,
    const TftControllerInitCommand *sequence,
    size_t count
);

void tft_controller_write_standard_window(
    const TftTransportOps *transport,
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1
);

#endif
