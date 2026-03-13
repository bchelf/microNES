#include "pico_time.h"

#include "pico/time.h"

uint64_t smb2350_pico_clock_now_ns(void) {
    return time_us_64() * 1000ull;
}

void smb2350_pico_sleep_until_ns(uint64_t deadline_ns) {
    uint64_t now_ns = smb2350_pico_clock_now_ns();

    if (deadline_ns <= now_ns) {
        return;
    }

    sleep_us((uint64_t)((deadline_ns - now_ns + 999ull) / 1000ull));
}
