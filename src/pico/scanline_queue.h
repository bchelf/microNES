#ifndef MICRONES_SCANLINE_QUEUE_H
#define MICRONES_SCANLINE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

// SPSC lock-free ring buffer for NES scanline pixel data.
// Producer = core 0 (NES step), consumer = core 1 (composite convert).
//
// Capacity is sized to absorb the maximum burst core 0 can produce while
// core 1 is blocked waiting for a DMA frame swap (~77 scanlines at current
// step rate). 128 slots gives comfortable headroom.
#define SCANLINE_QUEUE_CAPACITY 128u

typedef struct {
    uint8_t pixels[256];
    uint16_t y;
} ScanlineQueueSlot;

typedef struct {
    ScanlineQueueSlot slots[SCANLINE_QUEUE_CAPACITY];
    volatile uint32_t head;               // incremented by producer (core 0)
    volatile uint32_t tail;               // incremented by consumer (core 1)
    uint32_t producer_stall_count;        // times core 0 blocked on full queue
    uint64_t producer_stall_us_total;     // total us core 0 spent stalled
} ScanlineQueue;

void scanline_queue_init(ScanlineQueue *q);

// Push one scanline. Blocks (spinning) if the queue is full.
// Called from core 0 only. Updates stall counters on the queue.
void scanline_queue_push(ScanlineQueue *q, const uint8_t *pixels, uint16_t y);

// Pop one scanline. Blocks using WFE until a slot is available.
// Called from core 1 only.
void scanline_queue_pop_blocking(ScanlineQueue *q, ScanlineQueueSlot *out_slot);

// Try to pop one scanline without blocking. Returns false if empty.
// Called from core 1 only.
bool scanline_queue_try_pop(ScanlineQueue *q, ScanlineQueueSlot *out_slot);

#endif
