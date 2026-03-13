#ifndef SMB2350_PICO_TIME_H
#define SMB2350_PICO_TIME_H

#include <stdint.h>

uint64_t smb2350_pico_clock_now_ns(void);
void smb2350_pico_sleep_until_ns(uint64_t deadline_ns);

#endif
