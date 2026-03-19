#ifndef MICRONES_PICO_TIME_H
#define MICRONES_PICO_TIME_H

#include <stdint.h>

uint64_t micrones_pico_clock_now_ns(void);
void micrones_pico_sleep_until_ns(uint64_t deadline_ns);

#endif
