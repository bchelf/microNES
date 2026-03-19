#ifndef MICRONES_FRAME_PACER_H
#define MICRONES_FRAME_PACER_H

#include <stdbool.h>
#include <stdint.h>

enum {
    MICRONES_NTSC_FRAME_DURATION_NUMERATOR_NS = 357366000000000ull,
    MICRONES_NTSC_FRAME_DURATION_DENOMINATOR = 21477272ull,
};

typedef struct {
    bool throttled;
    uint64_t start_time_ns;
    uint64_t frame_count;
    uint64_t wait_until_ns;
    uint64_t next_deadline_ns;
    uint64_t frame_duration_floor_ns;
    uint64_t frame_duration_remainder_ns;
    uint64_t frame_duration_error_accumulator;
    uint64_t last_frame_done_ns;
    uint64_t last_frame_time_ns;
    uint64_t total_frame_time_ns;
    uint64_t worst_frame_time_ns;
    uint64_t late_frame_count;
    uint64_t last_late_ns;
    uint64_t max_late_ns;
    bool have_last_frame_done;
} MicronesFramePacer;

typedef struct {
    bool throttled;
    uint64_t frame_count;
    uint64_t late_frame_count;
    uint64_t last_frame_time_ns;
    uint64_t worst_frame_time_ns;
    uint64_t last_late_ns;
    uint64_t max_late_ns;
    double target_fps;
    double measured_fps;
    double average_frame_ms;
    double last_frame_ms;
    double worst_frame_ms;
    double last_late_ms;
    double max_late_ms;
} MicronesFramePacerStats;

void micrones_frame_pacer_init(MicronesFramePacer *pacer, bool throttled, uint64_t start_time_ns);
void micrones_frame_pacer_set_throttled(MicronesFramePacer *pacer, bool throttled);
void micrones_frame_pacer_frame_done(MicronesFramePacer *pacer, uint64_t now_ns);
bool micrones_frame_pacer_should_wait(const MicronesFramePacer *pacer, uint64_t now_ns, uint64_t *wait_until_ns);
void micrones_frame_pacer_note_wait_complete(MicronesFramePacer *pacer, uint64_t now_ns);
void micrones_frame_pacer_get_stats(
    const MicronesFramePacer *pacer,
    uint64_t now_ns,
    MicronesFramePacerStats *stats_out
);
double micrones_frame_pacer_target_fps(void);

#endif
