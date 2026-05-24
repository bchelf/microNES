#ifndef MICRONES_COLORDREAMS_H
#define MICRONES_COLORDREAMS_H

#include "cart.h"

#include <stdint.h>

void colordreams_cart_init(NesCartridge *cart);
void colordreams_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
