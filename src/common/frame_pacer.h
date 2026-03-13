#ifndef SMB2350_FRAME_PACER_H
#define SMB2350_FRAME_PACER_H

#include <stdbool.h>
#include <stdint.h>

enum {
    SMB2350_NTSC_FRAME_DURATION_NUMERATOR_NS = 357366000000000ull,
    SMB2350_NTSC_FRAME_DURATION_DENOMINATOR = 21477272ull,
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
} Smb2350FramePacer;

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
} Smb2350FramePacerStats;

void smb2350_frame_pacer_init(Smb2350FramePacer *pacer, bool throttled, uint64_t start_time_ns);
void smb2350_frame_pacer_set_throttled(Smb2350FramePacer *pacer, bool throttled);
void smb2350_frame_pacer_frame_done(Smb2350FramePacer *pacer, uint64_t now_ns);
bool smb2350_frame_pacer_should_wait(const Smb2350FramePacer *pacer, uint64_t now_ns, uint64_t *wait_until_ns);
void smb2350_frame_pacer_note_wait_complete(Smb2350FramePacer *pacer, uint64_t now_ns);
void smb2350_frame_pacer_get_stats(
    const Smb2350FramePacer *pacer,
    uint64_t now_ns,
    Smb2350FramePacerStats *stats_out
);
double smb2350_frame_pacer_target_fps(void);

#endif
