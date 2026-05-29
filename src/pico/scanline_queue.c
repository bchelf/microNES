#include "scanline_queue.h"

#include "pico/time.h"
#include "pico/stdlib.h"

// ARM Cortex-M33 inner-shareable data memory barrier.
// Ensures all memory accesses before the barrier are visible to other cores
// before any accesses after the barrier. Required around head/tail updates
// in a SPSC queue shared across cores.
#define MICRONES_DMB() __asm volatile ("dmb ish" ::: "memory")

// Send event to wake a core sleeping in WFE.
#define MICRONES_SEV() __asm volatile ("sev" ::: "memory")

// Wait for event (low-power sleep until SEV or IRQ fires).
#define MICRONES_WFE() __asm volatile ("wfe" ::: "memory")

void scanline_queue_init(ScanlineQueue *q) {
    q->head = 0;
    q->tail = 0;
    q->producer_stall_count = 0;
    q->producer_stall_us_total = 0;
}

void scanline_queue_push(ScanlineQueue *q, const uint8_t *pixels, uint16_t y) {
    uint32_t head = q->head;
    uint32_t next_head = head + 1u;
    uint64_t stall_start_us = 0;

    // Spin while the queue is full.
    while ((next_head - q->tail) > SCANLINE_QUEUE_CAPACITY) {
        if (stall_start_us == 0u) {
            stall_start_us = time_us_64();
            ++q->producer_stall_count;
        }
        tight_loop_contents();
    }
    if (stall_start_us != 0u) {
        q->producer_stall_us_total += time_us_64() - stall_start_us;
    }

    ScanlineQueueSlot *slot = &q->slots[head % SCANLINE_QUEUE_CAPACITY];
    __builtin_memcpy(slot->pixels, pixels, 256);
    slot->y = y;

    // Barrier: ensure slot data is written before head is incremented.
    MICRONES_DMB();
    q->head = next_head;

    // Wake consumer if it is sleeping in WFE.
    MICRONES_SEV();
}

void scanline_queue_pop_blocking(ScanlineQueue *q, ScanlineQueueSlot *out_slot) {
    uint32_t tail = q->tail;

    // Wait until at least one slot is available.
    while (q->head == tail) {
        MICRONES_WFE();
    }

    // Barrier: ensure head read happened before we read the slot data.
    MICRONES_DMB();

    *out_slot = q->slots[tail % SCANLINE_QUEUE_CAPACITY];
    q->tail = tail + 1u;
}

bool scanline_queue_try_pop(ScanlineQueue *q, ScanlineQueueSlot *out_slot) {
    uint32_t tail = q->tail;

    if (q->head == tail) {
        return false;
    }

    MICRONES_DMB();

    *out_slot = q->slots[tail % SCANLINE_QUEUE_CAPACITY];
    q->tail = tail + 1u;
    return true;
}
