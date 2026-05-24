#ifndef MICRONES_GXROM_H
#define MICRONES_GXROM_H

#include "cart.h"

#include <stdint.h>

void gxrom_cart_init(NesCartridge *cart);
void gxrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
