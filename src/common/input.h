#ifndef MICRONES_INPUT_H
#define MICRONES_INPUT_H

#include <stdbool.h>
#include <stdint.h>

enum {
    NES_BUTTON_A      = 1u << 0,
    NES_BUTTON_B      = 1u << 1,
    NES_BUTTON_SELECT = 1u << 2,
    NES_BUTTON_START  = 1u << 3,
    NES_BUTTON_UP     = 1u << 4,
    NES_BUTTON_DOWN   = 1u << 5,
    NES_BUTTON_LEFT   = 1u << 6,
    NES_BUTTON_RIGHT  = 1u << 7,
};

typedef struct {
    uint8_t buttons;
} NesControllerState;

typedef struct {
    NesControllerState live_state;
    uint8_t shift_register;
    bool strobe;
} NesController;

void input_controller_init(NesController *controller);
void input_controller_reset(NesController *controller);
void input_controller_set_state(NesController *controller, NesControllerState state);
void input_controller_write_strobe(NesController *controller, uint8_t value);
uint8_t input_controller_read(NesController *controller);

#endif
