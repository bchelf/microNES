#ifndef SMB2350_PICO_INPUT_H
#define SMB2350_PICO_INPUT_H

#include "input.h"

// GPIO assignments
#define PICO_INPUT_GPIO_START 2u

// Initialize all button GPIOs (call once at startup).
void pico_input_init(void);

// Read all buttons and return the current controller state.
NesControllerState pico_input_read(void);

#endif
