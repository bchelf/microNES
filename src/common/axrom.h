#ifndef MICRONES_AXROM_H
#define MICRONES_AXROM_H

#include "cart.h"

#include <stdint.h>

void axrom_cart_init(NesCartridge *cart);
void axrom_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);

#endif
