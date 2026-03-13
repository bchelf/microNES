#include "nes.h"
#include "png_write.h"
#include "video_capture.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
} SmokeRunResult;

typedef struct {
    const char *rom_path;
    uint64_t step_limit;
    const char *dump_frame_path;
    const char *video_out_path;
} SmokeOptions;

enum {
    HOST_VIDEO_FPS = 60,
};

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
    bool ok = true;
    bool entered_old_wait_loop = false;
    bool exited_old_wait_loop_after_sprite0_hit = false;
    uint16_t first_pc_after_wait_loop = 0;
    host_video_capture_t *video_capture = NULL;
    uint64_t video_frames_written = 0;

    nes_init(&nes);

    if (!nes_load_cartridge_file(&nes, rom_path)) {
        fprintf(stderr, "ROM load failed: %s\n", nes_last_error(&nes));
        nes_destroy(&nes);
        return false;
    }

    nes_reset(&nes);

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
        print_stop_summary(&nes);
        print_top_opcodes(&nes.stats, 10);
        print_unsupported_hits(&nes.stats);
        print_trace_window(&nes);
        printf("State hash: 0x%016" PRIx64 "\n", nes_state_hash(&nes));
        printf("Coverage hash: 0x%016" PRIx64 "\n", hash_opcode_coverage(&nes.stats));
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
