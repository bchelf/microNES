#include "pico_input.h"

#include "hardware/gpio.h"

void pico_input_init(void) {
    gpio_init(PICO_INPUT_GPIO_START);
    gpio_set_dir(PICO_INPUT_GPIO_START, GPIO_IN);
    gpio_pull_up(PICO_INPUT_GPIO_START);
}

NesControllerState pico_input_read(void) {
    uint8_t buttons = 0;
    if (!gpio_get(PICO_INPUT_GPIO_START)) {
//        buttons |= NES_BUTTON_START;
    }
    return (NesControllerState){ .buttons = buttons };
}
