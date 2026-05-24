#ifndef MICRONES_UXROM_H
#define MICRONES_UXROM_H

#include "cart.h"

#include <stdint.h>

void uxrom_cart_init(NesCartridge *cart);
void uxrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
