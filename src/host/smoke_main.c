#include "nes.h"
#include "nrom.h"
#include "png_write.h"
#include "video_capture.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *rom_path;
    uint64_t step_limit;
    const char *dump_frame_path;
    const char *video_out_path;
} SmokeOptions;

enum {
    HOST_VIDEO_FPS = 60,
    HOST_SPRITE0_DIAG_FRAME_START = 760,
    HOST_SPRITE0_DIAG_FRAME_END = 805,
    HOST_PC_RING_CAPACITY = 256,
    HOST_TOP_PC_COUNT = 4,
    HOST_STALL_NO_EVENT_INSTRUCTIONS = 200000,
    HOST_STALL_STATIC_FRAME_THRESHOLD = 480,
    HOST_STALL_PC_DOMINANCE_THRESHOLD = 224,
    HOST_CODE_REGION_BYTES = 12,
};

typedef struct {
    uint16_t pc;
    uint32_t count;
} HostPcCount;

typedef struct {
    uint16_t pcs[HOST_PC_RING_CAPACITY];
    size_t count;
    size_t head;
    uint16_t previous_pc;
    uint64_t same_pc_streak;
} HostPcWindow;

typedef struct {
    bool triggered;
    uint64_t instruction_index;
    uint64_t frame_index;
    uint64_t video_frame_index;
    uint64_t nmi_count;
    uint64_t sprite0_hit_count;
    uint16_t pc;
    uint8_t opcode;
    uint64_t same_pc_streak;
    uint64_t instructions_since_last_completed_frame;
    uint64_t instructions_since_last_frame_hash_change;
    uint64_t instructions_since_last_nmi;
    uint64_t instructions_since_last_sprite0_hit;
    uint64_t repeated_frame_hash_frames;
    uint64_t current_frame_hash;
    uint64_t last_changed_frame_hash;
    uint64_t last_changed_frame_index;
    uint64_t last_changed_frame_instruction;
    uint16_t dominant_pcs[HOST_TOP_PC_COUNT];
    uint32_t dominant_pc_counts[HOST_TOP_PC_COUNT];
    uint16_t code_region_start;
    uint8_t code_region_bytes[HOST_CODE_REGION_BYTES];
    bool code_region_valid;
    char classification[96];
    PpuSprite0FrameDiag sprite0_frame_diag;
    bool sprite0_first_hit_diag_valid;
    PpuSprite0FrameDiag sprite0_first_hit_diag;
} StallDiagnosis;

typedef struct {
    HostPcWindow pc_window;
    uint64_t last_completed_frame_instruction;
    uint64_t last_nmi_instruction;
    uint64_t last_sprite0_hit_instruction;
    uint64_t last_frame_hash_change_instruction;
    uint64_t last_frame_hash_change_frame;
    uint64_t last_changed_frame_hash;
    uint64_t repeated_frame_hash_frames;
    uint64_t observed_completed_frames;
    uint64_t observed_nmi_count;
    uint64_t observed_sprite0_hit_count;
    StallDiagnosis stall;
} ProgressTracker;

typedef struct {
    bool completed;
    NesStopReason stop_reason;
    uint64_t state_hash;
    uint64_t coverage_hash;
    uint64_t instructions;
    uint64_t unique_opcodes;
    uint64_t nmi_count;
    uint64_t frames;
    int scanline;
    uint64_t completed_frames;
    uint64_t last_completed_frame_hash;
    uint32_t last_completed_nonzero_pixels;
    uint64_t first_nonblank_frame_index;
    uint64_t first_nonblank_frame_hash;
    bool sprite0_hit_occurred;
    uint64_t sprite0_hit_count;
    uint64_t first_sprite0_hit_frame;
    int first_sprite0_hit_scanline;
    int first_sprite0_hit_x;
    uint64_t frames_with_sprite_pixels;
    uint32_t last_completed_sprite_pixels;
    uint64_t first_frame_with_sprite_pixels;
    uint8_t max_scanline_sprite_count;
    bool entered_old_wait_loop;
    bool exited_old_wait_loop_after_sprite0_hit;
    uint16_t first_pc_after_wait_loop;
    Cpu6502 cpu;
    NesStopInfo stop_info;
    NesExecutionStats stats;
    StallDiagnosis stall;
} SmokeRunResult;

static const char *sprite0_reject_reason_name(uint8_t reason) {
    switch (reason) {
    case PPU_SPRITE0_REJECT_RENDER_DISABLED:
        return "render-disabled";
    case PPU_SPRITE0_REJECT_LEFT_MASK:
        return "left-mask";
    case PPU_SPRITE0_REJECT_SPRITE_TRANSPARENT:
        return "sprite-transparent";
    case PPU_SPRITE0_REJECT_BG_TRANSPARENT:
        return "bg-transparent";
    case PPU_SPRITE0_REJECT_X255:
        return "x255";
    case PPU_SPRITE0_REJECT_NOT_IN_SCANLINE_SELECTION:
        return "sprite0-not-selected";
    case PPU_SPRITE0_REJECT_OCCLUDED_BY_EARLIER_SPRITE:
        return "occluded-by-earlier-sprite";
    default:
        return "unknown";
    }
}

static const char *oam_update_source_name(uint8_t source) {
    switch (source) {
    case PPU_OAM_UPDATE_OAMDATA:
        return "oamdata";
    case PPU_OAM_UPDATE_DMA:
        return "dma";
    default:
        return "none";
    }
}

static void print_sprite0_frame_diag(const char *label, const PpuSprite0FrameDiag *frame);
static void print_render_artifact_diag(const PpuRenderArtifactDiag *diag);
static void print_visible_write_diag(const Ppu *ppu);

static void print_usage(const char *argv0) {
    printf(
        "Usage: %s [rom_path] [step_limit] [frame_dump.png] [--video-out output.mp4]\n",
        argv0
    );
    printf("       %s roms/smb1.nes --video-out smb.mp4\n", argv0);
}

static bool parse_u64_arg(const char *text, uint64_t *value_out) {
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0') {
        return false;
    }

    *value_out = (uint64_t)parsed;
    return true;
}

static bool parse_args(int argc, char **argv, SmokeOptions *options) {
    int positional_count = 0;

    options->rom_path = "roms/smb1.nes";
    options->step_limit = 1000000ull;
    options->dump_frame_path = NULL;
    options->video_out_path = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return false;
        }
        if (strcmp(arg, "--video-out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--video-out requires a path\n");
                return false;
            }
            options->video_out_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--frame-out") == 0 || strcmp(arg, "--png-out") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a path\n", arg);
                return false;
            }
            options->dump_frame_path = argv[++i];
            continue;
        }
        if (strcmp(arg, "--steps") == 0) {
            if (i + 1 >= argc || !parse_u64_arg(argv[i + 1], &options->step_limit)) {
                fprintf(stderr, "--steps requires an integer value\n");
                return false;
            }
            ++i;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }

        ++positional_count;
        if (positional_count == 1) {
            options->rom_path = arg;
        } else if (positional_count == 2) {
            if (!parse_u64_arg(arg, &options->step_limit)) {
                fprintf(stderr, "Invalid step limit: %s\n", arg);
                return false;
            }
        } else if (positional_count == 3) {
            options->dump_frame_path = arg;
        } else {
            fprintf(stderr, "Unexpected positional argument: %s\n", arg);
            return false;
        }
    }

    return true;
}

static void host_pc_window_record(HostPcWindow *window, uint16_t pc) {
    if (window->count != 0 && window->previous_pc == pc) {
        ++window->same_pc_streak;
    } else {
        window->same_pc_streak = 1;
    }
    window->previous_pc = pc;

    window->pcs[window->head] = pc;
    window->head = (window->head + 1u) % HOST_PC_RING_CAPACITY;
    if (window->count < HOST_PC_RING_CAPACITY) {
        ++window->count;
    }
}

static void host_record_top_pc(HostPcCount *top, uint16_t pc, uint32_t count) {
    for (size_t slot = 0; slot < HOST_TOP_PC_COUNT; ++slot) {
        if (count > top[slot].count) {
            for (size_t move = HOST_TOP_PC_COUNT - 1; move > slot; --move) {
                top[move] = top[move - 1];
            }
            top[slot].pc = pc;
            top[slot].count = count;
            return;
        }
    }
}

static void host_pc_window_analyze(const HostPcWindow *window, HostPcCount *top) {
    HostPcCount counts[HOST_PC_RING_CAPACITY];
    size_t unique = 0;

    memset(top, 0, sizeof(HostPcCount) * HOST_TOP_PC_COUNT);
    memset(counts, 0, sizeof(counts));

    for (size_t i = 0; i < window->count; ++i) {
        uint16_t pc = window->pcs[i];
        bool found = false;

        for (size_t j = 0; j < unique; ++j) {
            if (counts[j].pc == pc) {
                ++counts[j].count;
                found = true;
                break;
            }
        }
        if (!found && unique < HOST_PC_RING_CAPACITY) {
            counts[unique].pc = pc;
            counts[unique].count = 1;
            ++unique;
        }
    }

    for (size_t i = 0; i < unique; ++i) {
        host_record_top_pc(top, counts[i].pc, counts[i].count);
    }
}

static bool host_safe_code_peek(const Nes *nes, uint16_t addr, uint8_t *value_out) {
    if (addr < 0x2000u) {
        *value_out = nes->cpu_ram[addr & 0x07ffu];
        return true;
    }
    if (addr >= 0x8000u) {
        *value_out = nrom_cpu_read(&nes->cartridge, addr);
        return true;
    }
    return false;
}

static void host_capture_code_region(const Nes *nes, uint16_t pc, StallDiagnosis *stall) {
    stall->code_region_valid = false;
    stall->code_region_start = pc;
    memset(stall->code_region_bytes, 0, sizeof(stall->code_region_bytes));

    if (pc < 4u) {
        return;
    }

    stall->code_region_start = (uint16_t)(pc - 4u);
    for (size_t i = 0; i < HOST_CODE_REGION_BYTES; ++i) {
        if (!host_safe_code_peek(nes, (uint16_t)(stall->code_region_start + i), &stall->code_region_bytes[i])) {
            return;
        }
    }
    stall->code_region_valid = true;
}

static const char *host_classify_stall(const Nes *nes, const StallDiagnosis *stall) {
    uint8_t op0 = 0;
    uint8_t op1 = 0;
    uint8_t op2 = 0;
    uint8_t op3 = 0;
    uint8_t op4 = 0;
    uint8_t op5 = 0;
    uint16_t jmp_target = 0;

    if (host_safe_code_peek(nes, stall->pc, &op0) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 1u), &op1) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 2u), &op2) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 3u), &op3) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 4u), &op4) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 5u), &op5)) {
        if (op0 == 0xadu && op1 == 0x02u && op2 == 0x20u && op3 == 0x29u && op4 == 0x40u && op5 == 0xf0u) {
            return "waiting for sprite-0 hit";
        }
        if (op0 == 0xadu && op1 == 0x02u && op2 == 0x20u && op3 == 0x29u && op4 == 0x80u &&
            (op5 == 0xf0u || op5 == 0xd0u)) {
            return "waiting on PPUSTATUS vblank";
        }
    }

    if (op0 == 0x4cu &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 1u), &op1) &&
        host_safe_code_peek(nes, (uint16_t)(stall->pc + 2u), &op2)) {
        jmp_target = (uint16_t)(op1 | ((uint16_t)op2 << 8));
        if (jmp_target == stall->pc) {
            if (stall->instructions_since_last_nmi < HOST_STALL_NO_EVENT_INSTRUCTIONS / 4 &&
                stall->instructions_since_last_completed_frame < HOST_STALL_NO_EVENT_INSTRUCTIONS / 4) {
                return "main-thread idle spin; NMI/frame cadence still alive";
            }
            return "tight self-jump loop";
        }
    }

    if (stall->instructions_since_last_completed_frame >= HOST_STALL_NO_EVENT_INSTRUCTIONS) {
        return "PPU/frame progress stalled";
    }
    if (stall->instructions_since_last_nmi >= HOST_STALL_NO_EVENT_INSTRUCTIONS) {
        return "NMI generation stalled";
    }
    if (stall->repeated_frame_hash_frames >= HOST_STALL_STATIC_FRAME_THRESHOLD) {
        return "rendering continues but framebuffer is frozen";
    }

    return "unknown tight loop";
}

static bool host_should_report_stall(const ProgressTracker *tracker) {
    HostPcCount top[HOST_TOP_PC_COUNT];
    bool tight_pc_window;
    bool repeated_static_frame;
    bool no_frame_progress;
    bool no_nmi_progress;

    if (tracker->stall.triggered || tracker->pc_window.count < HOST_PC_RING_CAPACITY) {
        return false;
    }

    host_pc_window_analyze(&tracker->pc_window, top);
    tight_pc_window = top[0].count >= HOST_STALL_PC_DOMINANCE_THRESHOLD;
    repeated_static_frame = tracker->repeated_frame_hash_frames >= HOST_STALL_STATIC_FRAME_THRESHOLD;
    no_frame_progress =
        tracker->stall.instructions_since_last_completed_frame >= HOST_STALL_NO_EVENT_INSTRUCTIONS;
    no_nmi_progress =
        tracker->stall.instructions_since_last_nmi >= HOST_STALL_NO_EVENT_INSTRUCTIONS;

    return no_frame_progress || no_nmi_progress || (tight_pc_window && repeated_static_frame);
}

static void host_capture_stall(
    ProgressTracker *tracker,
    const Nes *nes,
    uint64_t video_frames_written
) {
    HostPcCount top[HOST_TOP_PC_COUNT];

    memset(&tracker->stall, 0, sizeof(tracker->stall));
    tracker->stall.triggered = true;
    tracker->stall.instruction_index = nes->stats.instruction_count;
    tracker->stall.frame_index = nes->ppu.completed_frame_count;
    tracker->stall.video_frame_index = video_frames_written;
    tracker->stall.nmi_count = nes->stats.nmi_count;
    tracker->stall.sprite0_hit_count = nes->ppu.sprite0_hit_count;
    tracker->stall.pc = nes->cpu.pc;
    tracker->stall.opcode = nes->cpu.last_opcode;
    tracker->stall.same_pc_streak = tracker->pc_window.same_pc_streak;
    tracker->stall.instructions_since_last_completed_frame =
        nes->stats.instruction_count - tracker->last_completed_frame_instruction;
    tracker->stall.instructions_since_last_frame_hash_change =
        nes->stats.instruction_count - tracker->last_frame_hash_change_instruction;
    tracker->stall.instructions_since_last_nmi =
        nes->stats.instruction_count - tracker->last_nmi_instruction;
    tracker->stall.instructions_since_last_sprite0_hit =
        nes->stats.instruction_count - tracker->last_sprite0_hit_instruction;
    tracker->stall.repeated_frame_hash_frames = tracker->repeated_frame_hash_frames;
    tracker->stall.current_frame_hash = nes->ppu.last_completed_frame_hash;
    tracker->stall.last_changed_frame_hash = tracker->last_changed_frame_hash;
    tracker->stall.last_changed_frame_index = tracker->last_frame_hash_change_frame;
    tracker->stall.last_changed_frame_instruction = tracker->last_frame_hash_change_instruction;

    host_pc_window_analyze(&tracker->pc_window, top);
    for (size_t i = 0; i < HOST_TOP_PC_COUNT; ++i) {
        tracker->stall.dominant_pcs[i] = top[i].pc;
        tracker->stall.dominant_pc_counts[i] = top[i].count;
    }

    host_capture_code_region(nes, nes->cpu.pc, &tracker->stall);
    tracker->stall.sprite0_frame_diag = nes->ppu.sprite0_diag.current_frame;
    tracker->stall.sprite0_first_hit_diag_valid = nes->ppu.sprite0_diag.first_hit_frame_valid;
    tracker->stall.sprite0_first_hit_diag = nes->ppu.sprite0_diag.first_hit_frame;
    snprintf(
        tracker->stall.classification,
        sizeof(tracker->stall.classification),
        "%s",
        host_classify_stall(nes, &tracker->stall)
    );
}

static void print_code_region(const StallDiagnosis *stall) {
    if (!stall->code_region_valid) {
        printf("Code region: unavailable for PC=%04X\n", stall->pc);
        return;
    }

    printf("Code region around PC=%04X:\n", stall->pc);
    printf("  %04X:", stall->code_region_start);
    for (size_t i = 0; i < HOST_CODE_REGION_BYTES; ++i) {
        printf(" %02X", stall->code_region_bytes[i]);
    }
    printf("\n");
}

static void print_stall_summary(const Nes *nes, const StallDiagnosis *stall) {
    const Cpu6502OpcodeInfo *info = cpu6502_opcode_info(stall->opcode);

    if (!stall->triggered) {
        return;
    }

    printf("Suspected stalled progress detected:\n");
    printf("  classification: %s\n", stall->classification);
    printf(
        "  instruction=%" PRIu64 " frame=%" PRIu64 " video_frame=%" PRIu64 " pc=%04X opcode=%02X %-3s %-7s\n",
        stall->instruction_index,
        stall->frame_index,
        stall->video_frame_index,
        stall->pc,
        stall->opcode,
        info->mnemonic,
        info->addressing_mode
    );
    printf(
        "  NMI=%" PRIu64 " sprite0_hits=%" PRIu64 " same_pc_streak=%" PRIu64 "\n",
        stall->nmi_count,
        stall->sprite0_hit_count,
        stall->same_pc_streak
    );
    printf(
        "  since_frame=%" PRIu64 " since_frame_hash_change=%" PRIu64 " since_nmi=%" PRIu64 " since_sprite0=%" PRIu64 "\n",
        stall->instructions_since_last_completed_frame,
        stall->instructions_since_last_frame_hash_change,
        stall->instructions_since_last_nmi,
        stall->instructions_since_last_sprite0_hit
    );
    printf(
        "  frame_hash=0x%016" PRIx64 " last_changed_hash=0x%016" PRIx64 " last_changed_frame=%" PRIu64 " repeated_same_hash_frames=%" PRIu64 "\n",
        stall->current_frame_hash,
        stall->last_changed_frame_hash,
        stall->last_changed_frame_index,
        stall->repeated_frame_hash_frames
    );
    printf("  dominant PCs in recent window:\n");
    for (size_t i = 0; i < HOST_TOP_PC_COUNT && stall->dominant_pc_counts[i] != 0; ++i) {
        printf("    %zu. %04X x%u\n", i + 1, stall->dominant_pcs[i], stall->dominant_pc_counts[i]);
    }
    printf(
        "  PPU ctrl=%02X mask=%02X status=%02X frame=%" PRIu64 " scanline=%d cycle=%d\n",
        nes->ppu.ctrl,
        nes->ppu.mask,
        nes->ppu.status,
        nes->ppu.completed_frame_count,
        nes->ppu.scanline,
        nes->ppu.cycle
    );
    print_code_region(stall);
    print_sprite0_frame_diag("Sprite0 failing-frame snapshot", &stall->sprite0_frame_diag);
    if (stall->sprite0_first_hit_diag_valid) {
        print_sprite0_frame_diag("Sprite0 known-good first-hit frame", &stall->sprite0_first_hit_diag);
    }
}

static void print_sprite0_diag_window_summary(const PpuSprite0Diag *diag) {
    uint64_t first_raw_without_effective = 0;
    uint64_t first_no_raw_overlap = 0;
    uint64_t first_post_779_raw_overlap = 0;
    uint64_t first_post_779_effective_overlap = 0;
    uint64_t last_frame_with_hit = 0;
    uint64_t last_frame_with_raw_overlap = 0;
    uint64_t last_frame_with_effective_overlap = 0;

    if (!diag->enabled || diag->frame_count == 0) {
        return;
    }

    for (uint8_t i = 0; i < diag->frame_count; ++i) {
        const PpuSprite0FrameDiag *frame = &diag->frames[i];

        if (frame->raw_overlap_pixels != 0) {
            last_frame_with_raw_overlap = frame->render_frame_index;
            if (frame->render_frame_index > 779 && first_post_779_raw_overlap == 0) {
                first_post_779_raw_overlap = frame->render_frame_index;
            }
        } else if (first_no_raw_overlap == 0) {
            first_no_raw_overlap = frame->render_frame_index;
        }
        if (frame->effective_overlap_pixels != 0) {
            last_frame_with_effective_overlap = frame->render_frame_index;
            if (frame->render_frame_index > 779 && first_post_779_effective_overlap == 0) {
                first_post_779_effective_overlap = frame->render_frame_index;
            }
        } else if (frame->raw_overlap_pixels != 0 && first_raw_without_effective == 0) {
            first_raw_without_effective = frame->render_frame_index;
        }
        if (frame->sprite0_hit_set_this_frame) {
            last_frame_with_hit = frame->render_frame_index;
        }
    }

    printf(
        "Sprite0 diag window %" PRIu64 "..%" PRIu64 ": stored=%u last_hit_frame=%" PRIu64 " last_raw_overlap_frame=%" PRIu64 " last_effective_overlap_frame=%" PRIu64,
        diag->frame_start,
        diag->frame_end,
        (unsigned)diag->frame_count,
        last_frame_with_hit,
        last_frame_with_raw_overlap,
        last_frame_with_effective_overlap
    );
    if (first_raw_without_effective != 0) {
        printf(" first_raw_without_effective=%" PRIu64, first_raw_without_effective);
    }
    if (first_no_raw_overlap != 0) {
        printf(" first_no_raw_overlap=%" PRIu64, first_no_raw_overlap);
    }
    if (first_post_779_raw_overlap != 0) {
        printf(" first_post_779_raw_overlap=%" PRIu64, first_post_779_raw_overlap);
    }
    if (first_post_779_effective_overlap != 0) {
        printf(" first_post_779_effective_overlap=%" PRIu64, first_post_779_effective_overlap);
    }
    printf("\n");
    printf(
        "Sprite0 status lifecycle: total_sets=%" PRIu64 " total_clears=%" PRIu64 " suspicious_clears=%" PRIu64 " last_set=(frame=%" PRIu64 " scanline=%d cycle=%d) last_clear=(frame=%" PRIu64 " scanline=%d cycle=%d)\n",
        diag->total_status_set_count,
        diag->total_status_clear_count,
        diag->suspicious_status_clear_count,
        diag->last_status_set_render_frame,
        diag->last_status_set_scanline,
        diag->last_status_set_cycle,
        diag->last_status_clear_render_frame,
        diag->last_status_clear_scanline,
        diag->last_status_clear_cycle
    );
}

static void print_cpu_state(const Cpu6502 *cpu) {
    printf(
        "PC=%04X A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu OPC=%02X\n",
        cpu->pc,
        cpu->a,
        cpu->x,
        cpu->y,
        cpu->p,
        cpu->sp,
        (unsigned long long)cpu->cycles,
        cpu->last_opcode
    );
}

static const PpuSprite0FrameDiag *find_sprite0_diag_frame(const PpuSprite0Diag *diag, uint64_t render_frame_index) {
    for (uint8_t i = 0; i < diag->frame_count; ++i) {
        if (diag->frames[i].valid && diag->frames[i].render_frame_index == render_frame_index) {
            return &diag->frames[i];
        }
    }
    return NULL;
}

static void print_sprite0_frame_diag(const char *label, const PpuSprite0FrameDiag *frame) {
    if (frame == NULL || !frame->valid) {
        printf("%s: unavailable\n", label);
        return;
    }

    printf(
        "%s: render_frame=%" PRIu64 " completed_frame=%" PRIu64 " sprite0=[y=%u tile=%u attr=%02X x=%u]\n",
        label,
        frame->render_frame_index,
        frame->completed_frame_index,
        frame->sprite_y,
        frame->sprite_tile,
        frame->sprite_attributes,
        frame->sprite_x
    );
    printf(
        "  regs: ctrl=%02X mask=%02X status=%02X scroll=(%u,%u) render_scroll=(%u,%u) nt=%u v=%04X t=%04X fine_x=%u\n",
        frame->ctrl,
        frame->mask,
        frame->status,
        frame->scroll_x,
        frame->scroll_y,
        frame->render_scroll_x,
        frame->render_scroll_y,
        frame->render_base_nametable,
        frame->vram_addr,
        frame->temp_addr,
        frame->fine_x
    );
    printf(
        "  mask bits: bg=%d sprites=%d bg_left=%d sprites_left=%d intersects_visible=%d visible_scanlines=%u selected_scanlines=%u\n",
        frame->show_bg ? 1 : 0,
        frame->show_sprites ? 1 : 0,
        frame->show_bg_left ? 1 : 0,
        frame->show_sprites_left ? 1 : 0,
        frame->sprite0_intersects_visible ? 1 : 0,
        frame->sprite0_visible_scanline_count,
        frame->sprite0_selected_scanline_count
    );
    printf(
        "  pixels: candidates=%" PRIu32 " sprite0_raw=%" PRIu32 " bg_in_bounds=%" PRIu32 " raw_overlap=%" PRIu32 " effective_overlap=%" PRIu32 "\n",
        frame->visible_candidate_pixels,
        frame->sprite0_opaque_pixels_raw,
        frame->bg_opaque_pixels_in_sprite_bounds,
        frame->raw_overlap_pixels,
        frame->effective_overlap_pixels
    );
    printf(
        "  rejects: render_disabled=%" PRIu32 " left_mask=%" PRIu32 " sprite_transparent=%" PRIu32 " bg_transparent=%" PRIu32 " x255=%" PRIu32 " not_selected=%" PRIu32 " occluded=%" PRIu32 "\n",
        frame->reject_render_disabled,
        frame->reject_left_mask,
        frame->reject_sprite_transparent,
        frame->reject_bg_transparent,
        frame->reject_x255,
        frame->reject_not_in_scanline_selection,
        frame->reject_occluded_by_earlier_sprite
    );
    if (frame->first_raw_overlap_valid) {
        printf("  first_raw_overlap=(%d,%d)\n", frame->first_raw_overlap_x, frame->first_raw_overlap_y);
    }
    if (frame->first_effective_overlap_valid) {
        printf("  first_effective_overlap=(%d,%d)\n", frame->first_effective_overlap_x, frame->first_effective_overlap_y);
    }
    printf(
        "  hit lifecycle: set_this_frame=%d set_count=%u first_hit=(%d,%d)\n",
        frame->sprite0_hit_set_this_frame ? 1 : 0,
        frame->sprite0_hit_set_count,
        frame->first_hit_x,
        frame->first_hit_y
    );
    printf(
        "  sprite0 oam updates: mask=%02X last_update_frame=%" PRIu64 " last_update_scanline=%d cycle=%d source=%s\n",
        frame->sprite0_oam_changed_mask,
        frame->last_sprite0_oam_update_render_frame,
        frame->last_sprite0_oam_update_scanline,
        frame->last_sprite0_oam_update_cycle,
        oam_update_source_name(frame->last_sprite0_oam_update_source)
    );
    if (frame->example_count != 0) {
        printf("  example rejections:\n");
        for (uint8_t i = 0; i < frame->example_count; ++i) {
            printf(
                "    %u. (%u,%u) %s\n",
                (unsigned)(i + 1),
                frame->examples[i].x,
                frame->examples[i].y,
                sprite0_reject_reason_name(frame->examples[i].reason)
            );
        }
    }
}

static void print_render_artifact_diag(const PpuRenderArtifactDiag *diag) {
    if (diag == NULL || !diag->valid) {
        return;
    }

    printf(
        "Render artifact suspect: frame=%" PRIu64 " scanline=%u equal_prev=%u repeated_prev_chunks=%u transitions=%u longest_run=%u focus_x=%u\n",
        diag->frame_index,
        diag->scanline,
        diag->equal_prev_pixels,
        diag->repeated_prev_chunks,
        diag->transitions,
        diag->longest_run,
        diag->focus_x
    );
    for (uint8_t i = 0; i < diag->tile_count; ++i) {
        const PpuRenderTileDiag *tile = &diag->tiles[i];
        printf(
            "  tile_x=%u coarse=(%u,%u) fine_y=%u nt=%u nt_addr=%04X attr_addr=%04X pattern_base=%04X tile=%02X pattern_addr=%04X row=%02X/%02X\n",
            tile->tile_x,
            tile->coarse_x,
            tile->coarse_y,
            tile->fine_y,
            tile->nametable,
            tile->nametable_addr,
            tile->attribute_addr,
            tile->pattern_base,
            tile->tile_index,
            tile->pattern_addr,
            tile->pattern_low,
            tile->pattern_high
        );
    }
}

static const char *ppu_reg_name(uint8_t reg) {
    switch (reg) {
    case 0: return "PPUCTRL";
    case 1: return "PPUMASK";
    case 5: return "PPUSCROLL";
    case 6: return "PPUADDR";
    default: return "PPU?";
    }
}

static void print_visible_write_diag(const Ppu *ppu) {
    if (ppu->last_completed_visible_write_diag_count == 0) {
        return;
    }

    printf("Visible PPU writes for completed frame %" PRIu64 ":\n", ppu->completed_frame_count);
    for (uint8_t i = 0; i < ppu->last_completed_visible_write_diag_count; ++i) {
        const PpuVisibleWriteDiag *diag = &ppu->last_completed_visible_write_diag[i];
        printf(
            "  scanline=%u cycle=%u reg=%s value=%02X v=%04X t=%04X fx=%u ctrl=%02X mask=%02X\n",
            diag->scanline,
            diag->cycle,
            ppu_reg_name(diag->reg),
            diag->value,
            diag->vram_addr,
            diag->temp_addr,
            diag->fine_x,
            diag->ctrl,
            diag->mask
        );
    }
}

static uint64_t hash_opcode_coverage(const NesExecutionStats *stats) {
    uint64_t hash = 1469598103934665603ull;

    for (int i = 0; i < 256; ++i) {
        uint64_t value = stats->opcode_counts[i];
        hash ^= (uint8_t)i;
        hash *= 1099511628211ull;
        for (int j = 0; j < 8; ++j) {
            hash ^= (uint8_t)((value >> (j * 8)) & 0xffu);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static void print_trace_window(const Nes *nes) {
    Cpu6502TraceEntry trace[NES_TRACE_CAPACITY];
    size_t count = nes_trace_copy(nes, trace, NES_TRACE_CAPACITY);

    if (count == 0) {
        return;
    }

    printf("Trace window (%zu instructions):\n", count);
    for (size_t i = 0; i < count; ++i) {
        const Cpu6502OpcodeInfo *info = cpu6502_opcode_info(trace[i].opcode);
        printf(
            "  #%06llu PC=%04X OPC=%02X %-3s %-7s A=%02X X=%02X Y=%02X P=%02X SP=%02X CYC=%llu\n",
            (unsigned long long)trace[i].instruction_index,
            trace[i].pc,
            trace[i].opcode,
            info->mnemonic,
            info->addressing_mode,
            trace[i].a,
            trace[i].x,
            trace[i].y,
            trace[i].p,
            trace[i].sp,
            (unsigned long long)trace[i].cpu_cycles
        );
    }
}

static void print_top_opcodes(const NesExecutionStats *stats, size_t top_n) {
    struct OpcodeCount {
        uint8_t opcode;
        uint64_t count;
    } top[16];

    if (top_n > 16) {
        top_n = 16;
    }
    memset(top, 0, sizeof(top));

    for (int opcode = 0; opcode < 256; ++opcode) {
        uint64_t count = stats->opcode_counts[opcode];
        if (count == 0) {
            continue;
        }

        for (size_t slot = 0; slot < top_n; ++slot) {
            if (count > top[slot].count) {
                for (size_t move = top_n - 1; move > slot; --move) {
                    top[move] = top[move - 1];
                }
                top[slot].opcode = (uint8_t)opcode;
                top[slot].count = count;
                break;
            }
        }
    }

    printf("Top opcodes:\n");
    for (size_t i = 0; i < top_n && top[i].count != 0; ++i) {
        const Cpu6502OpcodeInfo *info = cpu6502_opcode_info(top[i].opcode);
        printf(
            "  %2zu. %02X %-3s %-7s %" PRIu64 "\n",
            i + 1,
            top[i].opcode,
            info->mnemonic,
            info->addressing_mode,
            top[i].count
        );
    }
}

static void print_unsupported_hits(const NesExecutionStats *stats) {
    bool any = false;

    for (int opcode = 0; opcode < 256; ++opcode) {
        if (stats->opcode_counts[opcode] == 0) {
            continue;
        }
        if (!cpu6502_opcode_info((uint8_t)opcode)->supported) {
            if (!any) {
                printf("Unsupported/illegal opcodes hit:\n");
                any = true;
            }
            printf(
                "  %02X %-3s %-7s %" PRIu64 "\n",
                opcode,
                cpu6502_opcode_info((uint8_t)opcode)->mnemonic,
                cpu6502_opcode_info((uint8_t)opcode)->addressing_mode,
                stats->opcode_counts[opcode]
            );
        }
    }

    if (!any) {
        printf("Unsupported/illegal opcodes hit: none\n");
    }
}

static void print_stop_summary(const Nes *nes) {
    const NesStopInfo *stop = nes_stop_info(nes);
    const Cpu6502OpcodeInfo *info = cpu6502_opcode_info(stop->opcode);

    printf("Stop reason: %s\n", nes_stop_reason_name(stop->reason));
    if (stop->reason == NES_STOP_ILLEGAL_OPCODE || stop->reason == NES_STOP_UNSUPPORTED_OPCODE) {
        printf(
            "Stop opcode: %02X %-3s %-7s at PC=%04X official=%d supported=%d instruction=%" PRIu64 "\n",
            stop->opcode,
            info->mnemonic,
            info->addressing_mode,
            stop->pc,
            stop->opcode_is_official ? 1 : 0,
            stop->opcode_is_supported ? 1 : 0,
            stop->instruction_index
        );
    }
    if (nes_last_error(nes)[0] != '\0') {
        printf("Detail: %s\n", nes_last_error(nes));
    }
}

static bool run_smoke_pass(
    const char *rom_path,
    uint64_t step_limit,
    const char *dump_frame_path,
    const char *video_out_path,
    SmokeRunResult *result,
    bool verbose
) {
    Nes nes;
    ProgressTracker tracker;
    bool ok = true;
    bool entered_old_wait_loop = false;
    bool exited_old_wait_loop_after_sprite0_hit = false;
    uint16_t first_pc_after_wait_loop = 0;
    host_video_capture_t *video_capture = NULL;
    uint64_t video_frames_written = 0;

    nes_init(&nes);
    memset(&tracker, 0, sizeof(tracker));

    if (!nes_load_cartridge_file(&nes, rom_path)) {
        fprintf(stderr, "ROM load failed: %s\n", nes_last_error(&nes));
        nes_destroy(&nes);
        return false;
    }

    nes_reset(&nes);
    nes_set_sprite0_diag_window(&nes, HOST_SPRITE0_DIAG_FRAME_START, HOST_SPRITE0_DIAG_FRAME_END);
    tracker.pc_window.previous_pc = nes.cpu.pc;

    if (verbose) {
        printf("ROM: %s\n", rom_path);
        printf(
            "PRG=%zu CHR=%zu mapper=%u mirror=%s\n",
            nes.cartridge.prg_rom_size,
            nes.cartridge.chr_size,
            nes.cartridge.mapper,
            nes.cartridge.mirror_mode == NES_MIRROR_VERTICAL ? "vertical" : "horizontal"
        );
        printf("Reset vector: %04X\n", nes.cpu.pc);
        print_cpu_state(nes_cpu_state(&nes));
        if (video_out_path != NULL) {
            video_capture = host_video_start(video_out_path, NES_FRAME_WIDTH, NES_FRAME_HEIGHT, HOST_VIDEO_FPS);
            if (video_capture != NULL) {
                printf("video capture enabled\n");
                printf("output file: %s\n", video_out_path);
                printf("resolution: %dx%d\n", NES_FRAME_WIDTH, NES_FRAME_HEIGHT);
                printf("fps: %d\n", HOST_VIDEO_FPS);
            } else {
                printf("Warning: video capture disabled: %s\n", host_video_last_error());
            }
        }
    }

    while (nes_execution_stats(&nes)->instruction_count < step_limit) {
        bool check_for_stall = false;

        if (nes.cpu.pc >= 0x8150u && nes.cpu.pc <= 0x8155u) {
            entered_old_wait_loop = true;
        } else if (entered_old_wait_loop && nes.ppu.sprite0_hit_ever && !exited_old_wait_loop_after_sprite0_hit) {
            exited_old_wait_loop_after_sprite0_hit = true;
            first_pc_after_wait_loop = nes.cpu.pc;
        }

        if (!nes_step_instruction(&nes)) {
            ok = false;
            break;
        }

        host_pc_window_record(&tracker.pc_window, nes.cpu.pc);

        if (nes.ppu.completed_frame_count != tracker.observed_completed_frames) {
            check_for_stall = true;
            tracker.last_completed_frame_instruction = nes.stats.instruction_count;
            if (nes.ppu.last_completed_frame_hash != tracker.last_changed_frame_hash) {
                tracker.last_changed_frame_hash = nes.ppu.last_completed_frame_hash;
                tracker.last_frame_hash_change_frame = nes.ppu.completed_frame_count;
                tracker.last_frame_hash_change_instruction = nes.stats.instruction_count;
                tracker.repeated_frame_hash_frames = 0;
            } else {
                ++tracker.repeated_frame_hash_frames;
            }
            if (verbose && nes.ppu.render_artifact_diag.valid) {
                print_render_artifact_diag(&nes.ppu.render_artifact_diag);
            }
            tracker.observed_completed_frames = nes.ppu.completed_frame_count;
        }
        if (nes.stats.nmi_count != tracker.observed_nmi_count) {
            tracker.observed_nmi_count = nes.stats.nmi_count;
            tracker.last_nmi_instruction = nes.stats.instruction_count;
        }
        if (nes.ppu.sprite0_hit_count != tracker.observed_sprite0_hit_count) {
            tracker.observed_sprite0_hit_count = nes.ppu.sprite0_hit_count;
            tracker.last_sprite0_hit_instruction = nes.stats.instruction_count;
        }

        tracker.stall.instructions_since_last_completed_frame =
            nes.stats.instruction_count - tracker.last_completed_frame_instruction;
        tracker.stall.instructions_since_last_nmi =
            nes.stats.instruction_count - tracker.last_nmi_instruction;
        tracker.stall.instructions_since_last_sprite0_hit =
            nes.stats.instruction_count - tracker.last_sprite0_hit_instruction;

        if ((nes.stats.instruction_count & 0x0fffu) == 0u) {
            check_for_stall = true;
        }

        if (check_for_stall && host_should_report_stall(&tracker)) {
            host_capture_stall(&tracker, &nes, video_frames_written);
            if (verbose) {
                print_stall_summary(&nes, &tracker.stall);
                print_trace_window(&nes);
            }
        }

        if (video_capture != NULL && nes.ppu.completed_frame_count > video_frames_written) {
            if (host_video_write_frame(video_capture, nes.ppu.frame_buffer.pixels)) {
                video_frames_written = nes.ppu.completed_frame_count;
            } else {
                printf("Warning: video capture write failed: %s\n", host_video_last_error());
                host_video_close(video_capture);
                video_capture = NULL;
            }
        }
    }

    if (ok && nes_stop_info(&nes)->reason == NES_STOP_NONE) {
        nes.stop_info.reason = NES_STOP_STEP_LIMIT;
        nes.stop_info.pc = nes.cpu.pc;
        nes.stop_info.opcode = nes.cpu.last_opcode;
        nes.stop_info.opcode_is_official = cpu6502_opcode_info(nes.cpu.last_opcode)->official;
        nes.stop_info.opcode_is_supported = cpu6502_opcode_info(nes.cpu.last_opcode)->supported;
        nes.stop_info.instruction_index = nes.stats.instruction_count;
    }

    result->completed = ok;
    result->stop_reason = nes.stop_info.reason;
    result->state_hash = nes_state_hash(&nes);
    result->coverage_hash = hash_opcode_coverage(&nes.stats);
    result->instructions = nes.stats.instruction_count;
    result->unique_opcodes = nes.stats.unique_opcodes;
    result->nmi_count = nes.stats.nmi_count;
    result->frames = nes_frame_count(&nes);
    result->scanline = nes_scanline(&nes);
    result->completed_frames = nes.ppu.completed_frame_count;
    result->last_completed_frame_hash = nes.ppu.last_completed_frame_hash;
    result->last_completed_nonzero_pixels = nes.ppu.last_completed_nonzero_pixels;
    result->first_nonblank_frame_index = nes.ppu.first_nonblank_frame_index;
    result->first_nonblank_frame_hash = nes.ppu.first_nonblank_frame_hash;
    result->sprite0_hit_occurred = nes.ppu.sprite0_hit_ever;
    result->sprite0_hit_count = nes.ppu.sprite0_hit_count;
    result->first_sprite0_hit_frame = nes.ppu.first_sprite0_hit_frame;
    result->first_sprite0_hit_scanline = nes.ppu.first_sprite0_hit_scanline;
    result->first_sprite0_hit_x = nes.ppu.first_sprite0_hit_x;
    result->frames_with_sprite_pixels = nes.ppu.frames_with_sprite_pixels;
    result->last_completed_sprite_pixels = nes.ppu.last_completed_sprite_pixels;
    result->first_frame_with_sprite_pixels = nes.ppu.first_frame_with_sprite_pixels;
    result->max_scanline_sprite_count = nes.ppu.max_scanline_sprite_count;
    result->entered_old_wait_loop = entered_old_wait_loop;
    result->exited_old_wait_loop_after_sprite0_hit = exited_old_wait_loop_after_sprite0_hit;
    result->first_pc_after_wait_loop = first_pc_after_wait_loop;
    result->cpu = nes.cpu;
    result->stop_info = nes.stop_info;
    result->stats = nes.stats;
    result->stall = tracker.stall;

    if (verbose) {
        printf("Instructions executed: %" PRIu64 "\n", nes.stats.instruction_count);
        printf("Unique opcodes executed: %" PRIu32 "\n", nes.stats.unique_opcodes);
        printf("NMI count: %" PRIu64 "\n", nes.stats.nmi_count);
        printf(
            "Completed frames=%" PRIu64 " last_frame_hash=0x%016" PRIx64 " last_nonzero_pixels=%" PRIu32,
            nes.ppu.completed_frame_count,
            nes.ppu.last_completed_frame_hash,
            nes.ppu.last_completed_nonzero_pixels
        );
        if (nes.ppu.first_nonblank_frame_index != 0) {
            printf(
                " first_nonblank_frame=%" PRIu64 " first_nonblank_hash=0x%016" PRIx64,
                nes.ppu.first_nonblank_frame_index,
                nes.ppu.first_nonblank_frame_hash
            );
        }
        printf("\n");
        printf(
            "Sprite-0 hit: %s count=%" PRIu64,
            nes.ppu.sprite0_hit_ever ? "yes" : "no",
            nes.ppu.sprite0_hit_count
        );
        if (nes.ppu.sprite0_hit_ever) {
            printf(
                " first_frame=%" PRIu64 " first_scanline=%d first_x=%d",
                nes.ppu.first_sprite0_hit_frame,
                nes.ppu.first_sprite0_hit_scanline,
                nes.ppu.first_sprite0_hit_x
            );
        }
        printf("\n");
        printf(
            "Sprite-0 opaque pixels=%" PRIu64 " bg-overlap-pixels=%" PRIu64,
            nes.ppu.sprite0_opaque_pixel_count,
            nes.ppu.sprite0_background_overlap_count
        );
        if (nes.ppu.first_sprite0_opaque_scanline >= 0) {
            printf(
                " first_opaque_frame=%" PRIu64 " first_opaque_scanline=%d first_opaque_x=%d",
                nes.ppu.first_sprite0_opaque_frame,
                nes.ppu.first_sprite0_opaque_scanline,
                nes.ppu.first_sprite0_opaque_x
            );
        }
        printf("\n");
        printf(
            "Sprite framebuffer pixels: last_completed=%" PRIu32 " frames_with_sprites=%" PRIu64 " max_scanline_sprites=%u",
            nes.ppu.last_completed_sprite_pixels,
            nes.ppu.frames_with_sprite_pixels,
            (unsigned)nes.ppu.max_scanline_sprite_count
        );
        if (nes.ppu.first_frame_with_sprite_pixels != 0) {
            printf(
                " first_sprite_frame=%" PRIu64,
                nes.ppu.first_frame_with_sprite_pixels
            );
        }
        printf("\n");
        printf(
            "Old wait loop entered=%d exited_after_sprite0_hit=%d",
            entered_old_wait_loop ? 1 : 0,
            exited_old_wait_loop_after_sprite0_hit ? 1 : 0
        );
        if (exited_old_wait_loop_after_sprite0_hit) {
            printf(" first_pc_after_exit=%04X", first_pc_after_wait_loop);
        }
        printf("\n");
        printf("Final CPU state:\n");
        print_cpu_state(&nes.cpu);
        printf(
            "PPU frame=%" PRIu64 " scanline=%d scanline_ready=%d\n",
            nes_frame_count(&nes),
            nes_scanline(&nes),
            nes_scanline_buffer(&nes)->ready ? 1 : 0
        );
        printf(
            "PPU ctrl=%02X mask=%02X status=%02X scroll=(%u,%u) sprite0=[y=%u tile=%u attr=%02X x=%u]\n",
            nes.ppu.ctrl,
            nes.ppu.mask,
            nes.ppu.status,
            nes.ppu.scroll_x,
            nes.ppu.scroll_y,
            nes.ppu.oam[0],
            nes.ppu.oam[1],
            nes.ppu.oam[2],
            nes.ppu.oam[3]
        );
        print_visible_write_diag(&nes.ppu);
        print_stop_summary(&nes);
        print_top_opcodes(&nes.stats, 10);
        print_unsupported_hits(&nes.stats);
        print_trace_window(&nes);
        printf("State hash: 0x%016" PRIx64 "\n", nes_state_hash(&nes));
        printf("Coverage hash: 0x%016" PRIx64 "\n", hash_opcode_coverage(&nes.stats));
        printf(
            "Frame progress: last_changed_frame=%" PRIu64 " last_changed_hash=0x%016" PRIx64 " repeated_same_hash_frames=%" PRIu64 "\n",
            tracker.last_frame_hash_change_frame,
            tracker.last_changed_frame_hash,
            tracker.repeated_frame_hash_frames
        );
        print_sprite0_diag_window_summary(&nes.ppu.sprite0_diag);
        if (tracker.stall.triggered) {
            const PpuSprite0FrameDiag *last_changed =
                find_sprite0_diag_frame(&nes.ppu.sprite0_diag, tracker.last_frame_hash_change_frame);
            const PpuSprite0FrameDiag *failed_completed =
                find_sprite0_diag_frame(&nes.ppu.sprite0_diag, tracker.stall.frame_index);
            print_sprite0_frame_diag("Sprite0 last frame with changed framebuffer", last_changed);
            print_sprite0_frame_diag("Sprite0 last completed frame before stall", failed_completed);
        }
        if (video_out_path != NULL) {
            printf(
                "Video frames written: %" PRIu64 " (~%.2f seconds at %d fps)\n",
                video_frames_written,
                (double)video_frames_written / (double)HOST_VIDEO_FPS,
                HOST_VIDEO_FPS
            );
        }
        if (dump_frame_path != NULL) {
            if (host_write_png_gray8(
                    dump_frame_path,
                    nes.ppu.frame_buffer.pixels,
                    NES_FRAME_WIDTH,
                    NES_FRAME_HEIGHT,
                    NES_FRAME_WIDTH
                )) {
                printf("Frame dump: %s (PNG)\n", dump_frame_path);
            } else {
                printf("Frame dump failed: %s\n", dump_frame_path);
            }
        }
    }

    if (video_capture != NULL && !host_video_close(video_capture)) {
        printf("Warning: video capture close reported: %s\n", host_video_last_error());
    }

    nes_destroy(&nes);
    return true;
}

static bool compare_runs(const SmokeRunResult *a, const SmokeRunResult *b) {
    if (a->completed != b->completed) return false;
    if (a->stop_reason != b->stop_reason) return false;
    if (memcmp(&a->cpu, &b->cpu, sizeof(a->cpu)) != 0) return false;
    if (a->instructions != b->instructions) return false;
    if (a->unique_opcodes != b->unique_opcodes) return false;
    if (a->nmi_count != b->nmi_count) return false;
    if (a->frames != b->frames || a->scanline != b->scanline) return false;
    if (a->completed_frames != b->completed_frames) return false;
    if (a->last_completed_frame_hash != b->last_completed_frame_hash) return false;
    if (a->last_completed_nonzero_pixels != b->last_completed_nonzero_pixels) return false;
    if (a->first_nonblank_frame_index != b->first_nonblank_frame_index) return false;
    if (a->first_nonblank_frame_hash != b->first_nonblank_frame_hash) return false;
    if (a->sprite0_hit_occurred != b->sprite0_hit_occurred) return false;
    if (a->sprite0_hit_count != b->sprite0_hit_count) return false;
    if (a->first_sprite0_hit_frame != b->first_sprite0_hit_frame) return false;
    if (a->first_sprite0_hit_scanline != b->first_sprite0_hit_scanline) return false;
    if (a->first_sprite0_hit_x != b->first_sprite0_hit_x) return false;
    if (a->frames_with_sprite_pixels != b->frames_with_sprite_pixels) return false;
    if (a->last_completed_sprite_pixels != b->last_completed_sprite_pixels) return false;
    if (a->first_frame_with_sprite_pixels != b->first_frame_with_sprite_pixels) return false;
    if (a->max_scanline_sprite_count != b->max_scanline_sprite_count) return false;
    if (a->entered_old_wait_loop != b->entered_old_wait_loop) return false;
    if (a->exited_old_wait_loop_after_sprite0_hit != b->exited_old_wait_loop_after_sprite0_hit) return false;
    if (a->first_pc_after_wait_loop != b->first_pc_after_wait_loop) return false;
    if (a->stall.triggered != b->stall.triggered) return false;
    if (a->stall.instruction_index != b->stall.instruction_index) return false;
    if (a->stall.frame_index != b->stall.frame_index) return false;
    if (a->stall.pc != b->stall.pc) return false;
    if (a->stall.opcode != b->stall.opcode) return false;
    if (a->stall.nmi_count != b->stall.nmi_count) return false;
    if (a->stall.sprite0_hit_count != b->stall.sprite0_hit_count) return false;
    if (a->stall.same_pc_streak != b->stall.same_pc_streak) return false;
    if (a->stall.instructions_since_last_completed_frame != b->stall.instructions_since_last_completed_frame) return false;
    if (a->stall.instructions_since_last_frame_hash_change != b->stall.instructions_since_last_frame_hash_change) return false;
    if (a->stall.instructions_since_last_nmi != b->stall.instructions_since_last_nmi) return false;
    if (a->stall.instructions_since_last_sprite0_hit != b->stall.instructions_since_last_sprite0_hit) return false;
    if (a->stall.repeated_frame_hash_frames != b->stall.repeated_frame_hash_frames) return false;
    if (a->stall.current_frame_hash != b->stall.current_frame_hash) return false;
    if (a->stall.last_changed_frame_hash != b->stall.last_changed_frame_hash) return false;
    if (a->stall.last_changed_frame_index != b->stall.last_changed_frame_index) return false;
    if (a->stall.last_changed_frame_instruction != b->stall.last_changed_frame_instruction) return false;
    if (memcmp(a->stall.dominant_pcs, b->stall.dominant_pcs, sizeof(a->stall.dominant_pcs)) != 0) return false;
    if (memcmp(a->stall.dominant_pc_counts, b->stall.dominant_pc_counts, sizeof(a->stall.dominant_pc_counts)) != 0) return false;
    if (strcmp(a->stall.classification, b->stall.classification) != 0) return false;
    if (memcmp(&a->stall.sprite0_frame_diag, &b->stall.sprite0_frame_diag, sizeof(a->stall.sprite0_frame_diag)) != 0) return false;
    if (a->stall.sprite0_first_hit_diag_valid != b->stall.sprite0_first_hit_diag_valid) return false;
    if (memcmp(&a->stall.sprite0_first_hit_diag, &b->stall.sprite0_first_hit_diag, sizeof(a->stall.sprite0_first_hit_diag)) != 0) return false;
    if (a->state_hash != b->state_hash) return false;
    if (a->coverage_hash != b->coverage_hash) return false;
    if (memcmp(&a->stats.opcode_counts, &b->stats.opcode_counts, sizeof(a->stats.opcode_counts)) != 0) return false;
    return true;
}

int main(int argc, char **argv) {
    SmokeOptions options;
    SmokeRunResult run1;
    SmokeRunResult run2;

    if (!parse_args(argc, argv, &options)) {
        return 1;
    }

    memset(&run1, 0, sizeof(run1));
    memset(&run2, 0, sizeof(run2));

    if (!run_smoke_pass(options.rom_path, options.step_limit, options.dump_frame_path, options.video_out_path, &run1, true)) {
        return 1;
    }

    if (!run_smoke_pass(options.rom_path, options.step_limit, NULL, NULL, &run2, false)) {
        fprintf(stderr, "Determinism rerun failed unexpectedly\n");
        return 2;
    }

    printf(
        "Determinism: %s\n",
        compare_runs(&run1, &run2) ? "PASS" : "FAIL"
    );
    if (!compare_runs(&run1, &run2)) {
        printf(
            "Run1 state=0x%016" PRIx64 " coverage=0x%016" PRIx64 " stop=%s\n",
            run1.state_hash,
            run1.coverage_hash,
            nes_stop_reason_name(run1.stop_reason)
        );
        printf(
            "Run2 state=0x%016" PRIx64 " coverage=0x%016" PRIx64 " stop=%s\n",
            run2.state_hash,
            run2.coverage_hash,
            nes_stop_reason_name(run2.stop_reason)
        );
        return 3;
    }

    return 0;
}
