#ifndef MICRONES_DISPLAY_TRANSPORT_H
#define MICRONES_DISPLAY_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    bool (*init)(void);
    const char *(*last_error)(void);
    void (*write_command)(uint8_t cmd);
    void (*write_data_blocking)(const uint8_t *data, size_t len);
    void (*begin_pixels)(void);
    void (*write_pixels_blocking)(const uint8_t *data, size_t len);
    void (*write_pixels_dma)(const uint8_t *data, size_t len);
    void (*wait_idle)(void);
    void (*end_pixels)(void);
} TftTransportOps;

const TftTransportOps *tft_display_transport_get(void);

#endif
