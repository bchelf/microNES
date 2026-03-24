#include "pico_input.h"
#include "nes_hw_controller.h"

void pico_input_init(void) {
    nes_hw_controller_init();
}

NesControllerState pico_input_read(void) {
    return nes_hw_controller_read();
}
