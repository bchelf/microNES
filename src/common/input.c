#include "input.h"

static void input_controller_latch(NesController *controller) {
    controller->shift_register = controller->live_state.buttons;
}

void input_controller_init(NesController *controller) {
    controller->live_state.buttons = 0;
    controller->shift_register = 0;
    controller->strobe = false;
}

void input_controller_reset(NesController *controller) {
    controller->shift_register = controller->live_state.buttons;
    controller->strobe = false;
}

void input_controller_set_state(NesController *controller, NesControllerState state) {
    controller->live_state = state;
    if (controller->strobe) {
        input_controller_latch(controller);
    }
}

void input_controller_write_strobe(NesController *controller, uint8_t value) {
    bool next_strobe = (value & 0x01u) != 0;
    if (next_strobe) {
        input_controller_latch(controller);
    } else if (controller->strobe) {
        input_controller_latch(controller);
    }
    controller->strobe = next_strobe;
}

uint8_t input_controller_read(NesController *controller) {
    uint8_t value;
    if (controller->strobe) {
        value = controller->live_state.buttons & 0x01u;
    } else {
        value = controller->shift_register & 0x01u;
        controller->shift_register = (controller->shift_register >> 1) | 0x80u;
    }
    return (uint8_t)(0x40u | value);
}
