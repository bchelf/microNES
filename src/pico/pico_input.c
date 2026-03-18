#include "pico_input.h"

#include "hardware/gpio.h"

void pico_input_init(void) {
    gpio_init(PICO_INPUT_GPIO_START);
    gpio_set_dir(PICO_INPUT_GPIO_START, GPIO_IN);
    gpio_pull_up(PICO_INPUT_GPIO_START);
}

uint8_t pressed_start = 0;

NesControllerState pico_input_read(void) {
    uint8_t buttons = 0;
    if (!gpio_get(PICO_INPUT_GPIO_START)) {
        if (/*pressed_start == 0*/1) { buttons |= NES_BUTTON_START; pressed_start = 1; } else { }
    }
    return (NesControllerState){ .buttons = buttons };
}
