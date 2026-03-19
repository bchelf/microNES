#include "frame_pacer.h"

#include <string.h>

static void micrones_frame_pacer_advance_deadline(MicronesFramePacer *pacer) {
    pacer->next_deadline_ns += pacer->frame_duration_floor_ns;
    pacer->frame_duration_error_accumulator += pacer->frame_duration_remainder_ns;
    if (pacer->frame_duration_error_accumulator >= MICRONES_NTSC_FRAME_DURATION_DENOMINATOR) {
        ++pacer->next_deadline_ns;
        pacer->frame_duration_error_accumulator -= MICRONES_NTSC_FRAME_DURATION_DENOMINATOR;
    }
}

void micrones_frame_pacer_init(MicronesFramePacer *pacer, bool throttled, uint64_t start_time_ns) {
    memset(pacer, 0, sizeof(*pacer));
    pacer->throttled = throttled;
    pacer->start_time_ns = start_time_ns;
    pacer->wait_until_ns = start_time_ns;
    pacer->next_deadline_ns = start_time_ns;
    pacer->frame_duration_floor_ns =
        MICRONES_NTSC_FRAME_DURATION_NUMERATOR_NS / MICRONES_NTSC_FRAME_DURATION_DENOMINATOR;
    pacer->frame_duration_remainder_ns =
        MICRONES_NTSC_FRAME_DURATION_NUMERATOR_NS % MICRONES_NTSC_FRAME_DURATION_DENOMINATOR;
    micrones_frame_pacer_advance_deadline(pacer);
}

void micrones_frame_pacer_set_throttled(MicronesFramePacer *pacer, bool throttled) {
    pacer->throttled = throttled;
}

void micrones_frame_pacer_frame_done(MicronesFramePacer *pacer, uint64_t now_ns) {
    ++pacer->frame_count;

    if (pacer->have_last_frame_done) {
        pacer->last_frame_time_ns = now_ns - pacer->last_frame_done_ns;
    } else {
        pacer->last_frame_time_ns = now_ns - pacer->start_time_ns;
        pacer->have_last_frame_done = true;
    }

    pacer->last_frame_done_ns = now_ns;
    pacer->total_frame_time_ns += pacer->last_frame_time_ns;
    if (pacer->last_frame_time_ns > pacer->worst_frame_time_ns) {
        pacer->worst_frame_time_ns = pacer->last_frame_time_ns;
    }

    if (!pacer->throttled) {
        pacer->wait_until_ns = now_ns;
        pacer->last_late_ns = 0;
        return;
    }

    pacer->wait_until_ns = pacer->next_deadline_ns;
    if (now_ns > pacer->wait_until_ns) {
        pacer->last_late_ns = now_ns - pacer->wait_until_ns;
        ++pacer->late_frame_count;
        if (pacer->last_late_ns > pacer->max_late_ns) {
            pacer->max_late_ns = pacer->last_late_ns;
        }
    } else {
        pacer->last_late_ns = 0;
    }

    micrones_frame_pacer_advance_deadline(pacer);
}

bool micrones_frame_pacer_should_wait(const MicronesFramePacer *pacer, uint64_t now_ns, uint64_t *wait_until_ns) {
    if (!pacer->throttled || now_ns >= pacer->wait_until_ns) {
        return false;
    }

    if (wait_until_ns != NULL) {
        *wait_until_ns = pacer->wait_until_ns;
    }
    return true;
}

void micrones_frame_pacer_note_wait_complete(MicronesFramePacer *pacer, uint64_t now_ns) {
    (void)pacer;
    (void)now_ns;
}

void micrones_frame_pacer_get_stats(
    const MicronesFramePacer *pacer,
    uint64_t now_ns,
    MicronesFramePacerStats *stats_out
) {
    double elapsed_seconds = 0.0;

    memset(stats_out, 0, sizeof(*stats_out));
    stats_out->throttled = pacer->throttled;
    stats_out->frame_count = pacer->frame_count;
    stats_out->late_frame_count = pacer->late_frame_count;
    stats_out->last_frame_time_ns = pacer->last_frame_time_ns;
    stats_out->worst_frame_time_ns = pacer->worst_frame_time_ns;
    stats_out->last_late_ns = pacer->last_late_ns;
    stats_out->max_late_ns = pacer->max_late_ns;
    stats_out->target_fps = micrones_frame_pacer_target_fps();
    stats_out->last_frame_ms = (double)pacer->last_frame_time_ns / 1000000.0;
    stats_out->worst_frame_ms = (double)pacer->worst_frame_time_ns / 1000000.0;
    stats_out->last_late_ms = (double)pacer->last_late_ns / 1000000.0;
    stats_out->max_late_ms = (double)pacer->max_late_ns / 1000000.0;

    if (pacer->frame_count != 0) {
        stats_out->average_frame_ms =
            ((double)pacer->total_frame_time_ns / (double)pacer->frame_count) / 1000000.0;
    }

    if (now_ns > pacer->start_time_ns) {
        elapsed_seconds = (double)(now_ns - pacer->start_time_ns) / 1000000000.0;
        if (elapsed_seconds > 0.0) {
            stats_out->measured_fps = (double)pacer->frame_count / elapsed_seconds;
        }
    }
}

double micrones_frame_pacer_target_fps(void) {
    return (double)MICRONES_NTSC_FRAME_DURATION_DENOMINATOR /
           ((double)MICRONES_NTSC_FRAME_DURATION_NUMERATOR_NS / 1000000000.0);
}
