#ifndef MICRONES_MAPPER40_H
#define MICRONES_MAPPER40_H

#include "cart.h"

#include <stdint.h>

void mapper40_cart_init(NesCartridge *cart);
void mapper40_cpu_write(NesCartridge *cart, uint16_t addr, uint8_t value);
void mapper40_tick(NesCartridge *cart, uint32_t cpu_cycles);

#endif
