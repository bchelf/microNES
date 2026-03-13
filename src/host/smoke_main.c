#include "nes.h"

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
    Cpu6502 cpu;
    NesStopInfo stop_info;
    NesExecutionStats stats;
} SmokeRunResult;

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

static bool run_smoke_pass(const char *rom_path, uint64_t step_limit, SmokeRunResult *result, bool verbose) {
    Nes nes;
    bool ok = true;

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
    }

    while (nes_execution_stats(&nes)->instruction_count < step_limit) {
        if (!nes_step_instruction(&nes)) {
            ok = false;
            break;
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
    result->cpu = nes.cpu;
    result->stop_info = nes.stop_info;
    result->stats = nes.stats;

    if (verbose) {
        printf("Instructions executed: %" PRIu64 "\n", nes.stats.instruction_count);
        printf("Unique opcodes executed: %" PRIu32 "\n", nes.stats.unique_opcodes);
        printf("NMI count: %" PRIu64 "\n", nes.stats.nmi_count);
        printf("Final CPU state:\n");
        print_cpu_state(&nes.cpu);
        printf(
            "PPU frame=%" PRIu64 " scanline=%d scanline_ready=%d\n",
            nes_frame_count(&nes),
            nes_scanline(&nes),
            nes_scanline_buffer(&nes)->ready ? 1 : 0
        );
        print_stop_summary(&nes);
        print_top_opcodes(&nes.stats, 10);
        print_unsupported_hits(&nes.stats);
        print_trace_window(&nes);
        printf("State hash: 0x%016" PRIx64 "\n", nes_state_hash(&nes));
        printf("Coverage hash: 0x%016" PRIx64 "\n", hash_opcode_coverage(&nes.stats));
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
    if (a->state_hash != b->state_hash) return false;
    if (a->coverage_hash != b->coverage_hash) return false;
    if (memcmp(&a->stats.opcode_counts, &b->stats.opcode_counts, sizeof(a->stats.opcode_counts)) != 0) return false;
    return true;
}

int main(int argc, char **argv) {
    const char *rom_path = (argc > 1) ? argv[1] : "roms/smb1.nes";
    uint64_t step_limit = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1000000ull;
    SmokeRunResult run1;
    SmokeRunResult run2;

    memset(&run1, 0, sizeof(run1));
    memset(&run2, 0, sizeof(run2));

    if (!run_smoke_pass(rom_path, step_limit, &run1, true)) {
        return 1;
    }

    if (!run_smoke_pass(rom_path, step_limit, &run2, false)) {
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
