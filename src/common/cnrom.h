#ifndef MICRONES_CNROM_H
#define MICRONES_CNROM_H

#include "cart.h"

#include <stdint.h>

void cnrom_cart_init(NesCartridge *cart);
void cnrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
