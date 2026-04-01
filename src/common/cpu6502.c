#include "cpu6502.h"

#include "nes.h"

#include <stdio.h>

static void cpu_record_trace(Nes *nes, const Cpu6502 *cpu, uint16_t pc, uint8_t opcode) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    Cpu6502TraceEntry *entry = &nes->trace[nes->trace_head];

    entry->instruction_index = nes->stats.instruction_count + 1;
    entry->pc = pc;
    entry->opcode = opcode;
    entry->a = cpu->a;
    entry->x = cpu->x;
    entry->y = cpu->y;
    entry->sp = cpu->sp;
    entry->p = cpu->p;
    entry->cpu_cycles = cpu->cycles;

    nes->trace_head = (uint8_t)((nes->trace_head + 1u) % NES_TRACE_CAPACITY);
    if (nes->trace_count < NES_TRACE_CAPACITY) {
        ++nes->trace_count;
    }
#else
    (void)nes;
    (void)cpu;
    (void)pc;
    (void)opcode;
#endif
}

static void cpu_note_opcode(Nes *nes, uint16_t pc, uint8_t opcode) {
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
    const Cpu6502OpcodeInfo *info = cpu6502_opcode_info(opcode);

    if (nes->stats.opcode_counts[opcode]++ == 0) {
        ++nes->stats.unique_opcodes;
    }
    if (nes->stop_info.reason == NES_STOP_NONE) {
        nes->stop_info.pc = pc;
        nes->stop_info.opcode = opcode;
        nes->stop_info.opcode_is_official = info->official;
        nes->stop_info.opcode_is_supported = info->supported;
        nes->stop_info.instruction_index = nes->stats.instruction_count + 1;
    }
#else
    (void)nes;
    (void)pc;
    (void)opcode;
#endif
}

static inline uint8_t cpu_read(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    (void)cpu;
    return nes_cpu_bus_read_fast(nes, addr);
}

static inline void cpu_write(Cpu6502 *cpu, Nes *nes, uint16_t addr, uint8_t value) {
    (void)cpu;
    nes_cpu_bus_write_fast(nes, addr, value);
}

static inline void cpu_set_zn(Cpu6502 *cpu, uint8_t value) {
    if (value == 0) {
        cpu->p |= CPU6502_FLAG_Z;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_Z;
    }

    if (value & 0x80u) {
        cpu->p |= CPU6502_FLAG_N;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_N;
    }
}

static inline uint8_t cpu_fetch8(Cpu6502 *cpu, Nes *nes) {
    uint16_t pc = cpu->pc;
    uint32_t off;

    cpu->pc = (uint16_t)(pc + 1u);
    if (pc >= 0x8000u) {
        off = (uint32_t)(pc - 0x8000u);
        if (nes->cartridge.mapper == 4) {
            return nes->cartridge.prg_banks_8k[off >> 13][off & 0x1fffu];
        }
        if (off < 0x4000u) {
            return nes->cartridge.prg_bank_lo[off];
        }
        return nes->cartridge.prg_bank_hi[off - 0x4000u];
    }
    return cpu_read(cpu, nes, pc);
}

static inline uint8_t cpu_read_ram_fast(const Nes *nes, uint16_t addr) {
    return nes->cpu_ram[addr & 0x07ffu];
}

static inline void cpu_write_ram_fast(Nes *nes, uint16_t addr, uint8_t value) {
    nes->cpu_ram[addr & 0x07ffu] = value;
}

static inline uint16_t cpu_fetch16(Cpu6502 *cpu, Nes *nes) {
    uint16_t pc = cpu->pc;
    uint8_t lo;
    uint8_t hi;
    uint32_t lo_off;
    uint32_t hi_off;

    if (pc >= 0x8000u && (uint16_t)(pc + 1u) >= 0x8000u) {
        lo_off = (uint32_t)(pc - 0x8000u);
        hi_off = (uint32_t)((uint16_t)(pc + 1u) - 0x8000u);
        if (nes->cartridge.mapper == 4) {
            lo = nes->cartridge.prg_banks_8k[lo_off >> 13][lo_off & 0x1fffu];
            hi = nes->cartridge.prg_banks_8k[hi_off >> 13][hi_off & 0x1fffu];
        } else {
            lo = (lo_off < 0x4000u)
                ? nes->cartridge.prg_bank_lo[lo_off]
                : nes->cartridge.prg_bank_hi[lo_off - 0x4000u];
            hi = (hi_off < 0x4000u)
                ? nes->cartridge.prg_bank_lo[hi_off]
                : nes->cartridge.prg_bank_hi[hi_off - 0x4000u];
        }
        cpu->pc = (uint16_t)(pc + 2u);
    } else {
        lo = cpu_fetch8(cpu, nes);
        hi = cpu_fetch8(cpu, nes);
    }
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline void cpu_push(Cpu6502 *cpu, Nes *nes, uint8_t value) {
    cpu_write_ram_fast(nes, (uint16_t)(0x0100u | cpu->sp), value);
    --cpu->sp;
}

static inline uint8_t cpu_pop(Cpu6502 *cpu, Nes *nes) {
    ++cpu->sp;
    return cpu_read_ram_fast(nes, (uint16_t)(0x0100u | cpu->sp));
}

static inline uint16_t cpu_addr_zp(Cpu6502 *cpu, Nes *nes) {
    return cpu_fetch8(cpu, nes);
}

static inline uint16_t cpu_addr_zpx(Cpu6502 *cpu, Nes *nes) {
    return (uint8_t)(cpu_fetch8(cpu, nes) + cpu->x);
}

static inline uint16_t cpu_addr_zpy(Cpu6502 *cpu, Nes *nes) {
    return (uint8_t)(cpu_fetch8(cpu, nes) + cpu->y);
}

static inline uint16_t cpu_addr_abs(Cpu6502 *cpu, Nes *nes) {
    return cpu_fetch16(cpu, nes);
}

static inline uint16_t cpu_addr_absx(Cpu6502 *cpu, Nes *nes, bool *page_crossed) {
    uint16_t base = cpu_fetch16(cpu, nes);
    uint16_t addr = (uint16_t)(base + cpu->x);
    *page_crossed = (base & 0xff00u) != (addr & 0xff00u);
    return addr;
}

static inline uint16_t cpu_addr_absy(Cpu6502 *cpu, Nes *nes, bool *page_crossed) {
    uint16_t base = cpu_fetch16(cpu, nes);
    uint16_t addr = (uint16_t)(base + cpu->y);
    *page_crossed = (base & 0xff00u) != (addr & 0xff00u);
    return addr;
}

static inline uint16_t cpu_addr_indx(Cpu6502 *cpu, Nes *nes) {
    uint8_t zp = (uint8_t)(cpu_fetch8(cpu, nes) + cpu->x);
    uint8_t lo = cpu_read_ram_fast(nes, zp);
    uint8_t hi = cpu_read_ram_fast(nes, (uint8_t)(zp + 1u));
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline uint16_t cpu_addr_indy(Cpu6502 *cpu, Nes *nes, bool *page_crossed) {
    uint8_t zp = cpu_fetch8(cpu, nes);
    uint16_t base = (uint16_t)(cpu_read_ram_fast(nes, zp) |
                     ((uint16_t)cpu_read_ram_fast(nes, (uint8_t)(zp + 1u)) << 8));
    uint16_t addr = (uint16_t)(base + cpu->y);
    *page_crossed = (base & 0xff00u) != (addr & 0xff00u);
    return addr;
}

static inline uint16_t cpu_addr_indirect(Cpu6502 *cpu, Nes *nes) {
    uint16_t ptr = cpu_fetch16(cpu, nes);
    uint8_t lo = cpu_read(cpu, nes, ptr);
    uint8_t hi = cpu_read(cpu, nes, (uint16_t)((ptr & 0xff00u) | ((ptr + 1u) & 0x00ffu)));
    return (uint16_t)(lo | ((uint16_t)hi << 8));
}

static inline void cpu_branch(Cpu6502 *cpu, int8_t offset, bool condition, uint32_t *cycles) {
    if (condition) {
        uint16_t old_pc = cpu->pc;
        cpu->pc = (uint16_t)(cpu->pc + offset);
        *cycles += 1;
        if ((old_pc & 0xff00u) != (cpu->pc & 0xff00u)) {
            *cycles += 1;
        }
    }
}

static inline void cpu_cmp(Cpu6502 *cpu, uint8_t reg, uint8_t value) {
    uint8_t result = (uint8_t)(reg - value);
    if (reg >= value) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }
    cpu_set_zn(cpu, result);
}

static inline void cpu_adc(Cpu6502 *cpu, uint8_t value) {
    uint16_t carry = (cpu->p & CPU6502_FLAG_C) ? 1u : 0u;
    uint16_t sum = (uint16_t)cpu->a + value + carry;
    uint8_t result = (uint8_t)sum;

    if (sum > 0xffu) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }

    if (((cpu->a ^ result) & (value ^ result) & 0x80u) != 0) {
        cpu->p |= CPU6502_FLAG_V;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_V;
    }

    cpu->a = result;
    cpu_set_zn(cpu, cpu->a);
}

static inline void cpu_sbc(Cpu6502 *cpu, uint8_t value) {
    cpu_adc(cpu, (uint8_t)(value ^ 0xffu));
}

static inline uint8_t cpu_asl_value(Cpu6502 *cpu, uint8_t value) {
    if (value & 0x80u) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }
    value <<= 1;
    cpu_set_zn(cpu, value);
    return value;
}

static inline uint8_t cpu_lsr_value(Cpu6502 *cpu, uint8_t value) {
    if (value & 0x01u) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }
    value >>= 1;
    cpu_set_zn(cpu, value);
    return value;
}

static inline uint8_t cpu_rol_value(Cpu6502 *cpu, uint8_t value) {
    uint8_t carry = (cpu->p & CPU6502_FLAG_C) ? 1u : 0u;
    if (value & 0x80u) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }
    value = (uint8_t)((value << 1) | carry);
    cpu_set_zn(cpu, value);
    return value;
}

static inline uint8_t cpu_ror_value(Cpu6502 *cpu, uint8_t value) {
    uint8_t carry = (cpu->p & CPU6502_FLAG_C) ? 0x80u : 0u;
    if (value & 0x01u) {
        cpu->p |= CPU6502_FLAG_C;
    } else {
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
    }
    value = (uint8_t)((value >> 1) | carry);
    cpu_set_zn(cpu, value);
    return value;
}

/* --- Illegal (composite) opcodes required by Zelda and similar games --- */

/* DCP: M -= 1, then CMP(A, M).  Flags set as CMP; does not update A. */
static inline void cpu_dcp(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = (uint8_t)(cpu_read(cpu, nes, addr) - 1u);
    cpu_write(cpu, nes, addr, m);
    cpu_cmp(cpu, cpu->a, m);
}

/* ISB (also called ISC): M += 1, then SBC(A, M). */
static inline void cpu_isb(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = (uint8_t)(cpu_read(cpu, nes, addr) + 1u);
    cpu_write(cpu, nes, addr, m);
    cpu_sbc(cpu, m);
}

/* LAX: A = X = M.  Sets N,Z flags. */
static inline void cpu_lax(Cpu6502 *cpu, uint8_t value) {
    cpu->a = value;
    cpu->x = value;
    cpu_set_zn(cpu, value);
}

/* SAX: store A & X.  No flag effects. */
static inline void cpu_sax(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    cpu_write(cpu, nes, addr, (uint8_t)(cpu->a & cpu->x));
}

/* SLO: M = ASL(M), then A |= M.  C flag from ASL; N,Z from ORA result. */
static inline void cpu_slo(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = cpu_asl_value(cpu, cpu_read(cpu, nes, addr));
    cpu_write(cpu, nes, addr, m);
    cpu->a |= m;
    cpu_set_zn(cpu, cpu->a);
}

/* RLA: M = ROL(M), then A &= M. */
static inline void cpu_rla(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = cpu_rol_value(cpu, cpu_read(cpu, nes, addr));
    cpu_write(cpu, nes, addr, m);
    cpu->a &= m;
    cpu_set_zn(cpu, cpu->a);
}

/* SRE: M = LSR(M), then A ^= M. */
static inline void cpu_sre(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = cpu_lsr_value(cpu, cpu_read(cpu, nes, addr));
    cpu_write(cpu, nes, addr, m);
    cpu->a ^= m;
    cpu_set_zn(cpu, cpu->a);
}

/* RRA: M = ROR(M), then ADC(A, M). */
static inline void cpu_rra(Cpu6502 *cpu, Nes *nes, uint16_t addr) {
    uint8_t m = cpu_ror_value(cpu, cpu_read(cpu, nes, addr));
    cpu_write(cpu, nes, addr, m);
    cpu_adc(cpu, m);
}

static void cpu_service_interrupt(Cpu6502 *cpu, Nes *nes, uint16_t vector, bool set_break_flag) {
#if MICRONES_ENABLE_STEP_PROFILING
    uint64_t cpu_started_us = nes_profile_now_us(nes);
#endif
    uint32_t cpu_started_cycles = micrones_profile_now_cycles();
    uint8_t flags = (uint8_t)(cpu->p | CPU6502_FLAG_U);
    uint16_t pc = cpu->pc;

    if (set_break_flag) {
        flags |= CPU6502_FLAG_B;
    } else {
        flags &= (uint8_t)~CPU6502_FLAG_B;
    }

    cpu_push(cpu, nes, (uint8_t)(pc >> 8));
    cpu_push(cpu, nes, (uint8_t)(pc & 0xffu));
    cpu_push(cpu, nes, flags);
    cpu->p |= CPU6502_FLAG_I;
    cpu->pc = (uint16_t)(cpu_read(cpu, nes, vector) | ((uint16_t)cpu_read(cpu, nes, (uint16_t)(vector + 1u)) << 8));
    cpu->cycles += 7;
#if MICRONES_ENABLE_STEP_PROFILING
    nes->step_profile.cpu_exec_us_total += nes_profile_now_us(nes) - cpu_started_us;
#endif
    if (cpu_started_cycles != 0) {
        nes->step_profile.cpu_exec_cycles_total +=
            (uint32_t)(micrones_profile_now_cycles() - cpu_started_cycles);
    }
    nes->pending_apu_cycles += 7;
#if MICRONES_ENABLE_STEP_PROFILING
    cpu_started_us = nes_profile_now_us(nes);
#endif
    cpu_started_cycles = micrones_profile_now_cycles();
    ppu_step_cycles(&nes->ppu, &nes->cartridge, 21);
#if MICRONES_ENABLE_STEP_PROFILING
    nes->step_profile.ppu_step_us_total += nes_profile_now_us(nes) - cpu_started_us;
#endif
    if (cpu_started_cycles != 0) {
        nes->step_profile.ppu_step_cycles_total +=
            (uint32_t)(micrones_profile_now_cycles() - cpu_started_cycles);
    }
}

void cpu6502_init(Cpu6502 *cpu) {
    cpu->a = 0;
    cpu->x = 0;
    cpu->y = 0;
    cpu->sp = 0xfd;
    cpu->p = CPU6502_FLAG_I | CPU6502_FLAG_U;
    cpu->pc = 0;
    cpu->cycles = 0;
    cpu->last_opcode = 0;
    cpu->jammed = false;
}

void cpu6502_reset(Cpu6502 *cpu, Nes *nes) {
    cpu->a = 0;
    cpu->x = 0;
    cpu->y = 0;
    cpu->sp = 0xfd;
    cpu->p = CPU6502_FLAG_I | CPU6502_FLAG_U;
    cpu->pc = (uint16_t)(nes_cpu_bus_read_fast(nes, 0xfffcu) | ((uint16_t)nes_cpu_bus_read_fast(nes, 0xfffdu) << 8));
    cpu->cycles = 7;
    cpu->last_opcode = 0;
    cpu->jammed = false;
}

bool MICRONES_HOT_FUNC(cpu6502_step)(Cpu6502 *cpu, Nes *nes) {
    uint32_t cycles = 0;
    bool page_crossed = false;
    uint16_t addr = 0;
    uint8_t value = 0;
    int8_t rel = 0;
    uint16_t pc_before = cpu->pc;
#if MICRONES_ENABLE_STEP_PROFILING
    uint64_t cpu_started_us = 0;
#endif
    uint32_t cpu_started_cycles = 0;

    if (cpu->jammed) {
        if (nes->stop_info.reason == NES_STOP_NONE) {
            nes->stop_info.reason = NES_STOP_CPU_JAMMED;
            nes->stop_info.pc = cpu->pc;
            nes->stop_info.opcode = cpu->last_opcode;
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
            nes->stop_info.opcode_is_official = cpu6502_opcode_info(cpu->last_opcode)->official;
            nes->stop_info.opcode_is_supported = cpu6502_opcode_info(cpu->last_opcode)->supported;
#endif
            nes->stop_info.instruction_index = nes->stats.instruction_count;
        }
        snprintf(nes->last_error, sizeof(nes->last_error), "CPU is jammed");
        return false;
    }

    if (nes->ppu.nmi_pending) {
        nes->ppu.nmi_pending = false;
        ++nes->stats.nmi_count;
        cpu_service_interrupt(cpu, nes, 0xfffau, false);
        return true;
    }
    if ((cpu->p & CPU6502_FLAG_I) == 0) {
        if (nes->cartridge.irq_pending) {
            nes->cartridge.irq_pending = false;
            cpu_service_interrupt(cpu, nes, 0xfffeu, false);
            return true;
        }
        if (apu_has_irq(&nes->apu)) {
            /* APU IRQ (frame counter or DMC): flag is level-triggered,
             * the interrupt handler must clear it via $4015/$4017 reads/writes. */
            cpu_service_interrupt(cpu, nes, 0xfffeu, false);
            return true;
        }
    }

#if MICRONES_ENABLE_STEP_PROFILING
    cpu_started_us = nes_profile_now_us(nes);
#endif
    cpu_started_cycles = micrones_profile_now_cycles();
    cpu->last_opcode = cpu_fetch8(cpu, nes);
    cpu_record_trace(nes, cpu, pc_before, cpu->last_opcode);
    cpu_note_opcode(nes, pc_before, cpu->last_opcode);

    switch (cpu->last_opcode) {
    case 0x00:
        cpu->pc++;
        cpu_service_interrupt(cpu, nes, 0xfffeu, true);
        ++nes->stats.instruction_count;
        return true;

    case 0x01:
        cpu->a |= cpu_read(cpu, nes, cpu_addr_indx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x05:
        cpu->a |= cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x06:
        addr = cpu_addr_zp(cpu, nes);
        value = cpu_asl_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 5;
        break;
    case 0x08:
        cpu_push(cpu, nes, (uint8_t)(cpu->p | CPU6502_FLAG_B | CPU6502_FLAG_U));
        cycles = 3;
        break;
    case 0x09:
        cpu->a |= cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x0a:
        cpu->a = cpu_asl_value(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x0d:
        cpu->a |= cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x0e:
        addr = cpu_addr_abs(cpu, nes);
        value = cpu_asl_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x10:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_N) == 0, &cycles);
        break;
    case 0x11:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu->a |= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 6 : 5;
        break;
    case 0x15:
        cpu->a |= cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x16:
        addr = cpu_addr_zpx(cpu, nes);
        value = cpu_asl_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x18:
        cpu->p &= (uint8_t)~CPU6502_FLAG_C;
        cycles = 2;
        break;
    case 0x19:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu->a |= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x1d:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu->a |= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x1e:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = cpu_asl_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 7;
        break;
    case 0x20:
        addr = cpu_addr_abs(cpu, nes);
        cpu_push(cpu, nes, (uint8_t)((cpu->pc - 1u) >> 8));
        cpu_push(cpu, nes, (uint8_t)((cpu->pc - 1u) & 0xffu));
        cpu->pc = addr;
        cycles = 6;
        break;
    case 0x21:
        cpu->a &= cpu_read(cpu, nes, cpu_addr_indx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x24:
        value = cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        if ((cpu->a & value) == 0) cpu->p |= CPU6502_FLAG_Z; else cpu->p &= (uint8_t)~CPU6502_FLAG_Z;
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(CPU6502_FLAG_N | CPU6502_FLAG_V)) | (value & (CPU6502_FLAG_N | CPU6502_FLAG_V)));
        cycles = 3;
        break;
    case 0x25:
        cpu->a &= cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x26:
        addr = cpu_addr_zp(cpu, nes);
        value = cpu_rol_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 5;
        break;
    case 0x28:
        cpu->p = (uint8_t)((cpu_pop(cpu, nes) & (uint8_t)~CPU6502_FLAG_B) | CPU6502_FLAG_U);
        cycles = 4;
        break;
    case 0x29:
        cpu->a &= cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x2a:
        cpu->a = cpu_rol_value(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x2c:
        value = cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        if ((cpu->a & value) == 0) cpu->p |= CPU6502_FLAG_Z; else cpu->p &= (uint8_t)~CPU6502_FLAG_Z;
        cpu->p = (uint8_t)((cpu->p & (uint8_t)~(CPU6502_FLAG_N | CPU6502_FLAG_V)) | (value & (CPU6502_FLAG_N | CPU6502_FLAG_V)));
        cycles = 4;
        break;
    case 0x2d:
        cpu->a &= cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x2e:
        addr = cpu_addr_abs(cpu, nes);
        value = cpu_rol_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x31:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu->a &= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 6 : 5;
        break;
    case 0x30:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_N) != 0, &cycles);
        break;
    case 0x35:
        cpu->a &= cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x36:
        addr = cpu_addr_zpx(cpu, nes);
        value = cpu_rol_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x38:
        cpu->p |= CPU6502_FLAG_C;
        cycles = 2;
        break;
    case 0x39:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu->a &= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x3d:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu->a &= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x3e:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = cpu_rol_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 7;
        break;
    case 0x40:
        cpu->p = (uint8_t)((cpu_pop(cpu, nes) & (uint8_t)~CPU6502_FLAG_B) | CPU6502_FLAG_U);
        cpu->pc = (uint16_t)(cpu_pop(cpu, nes) | ((uint16_t)cpu_pop(cpu, nes) << 8));
        cycles = 6;
        break;
    case 0x41:
        cpu->a ^= cpu_read(cpu, nes, cpu_addr_indx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 6;
        break;
    case 0x45:
        cpu->a ^= cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 3;
        break;
    case 0x46:
        addr = cpu_addr_zp(cpu, nes);
        value = cpu_lsr_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 5;
        break;
    case 0x48:
        cpu_push(cpu, nes, cpu->a);
        cycles = 3;
        break;
    case 0x49:
        cpu->a ^= cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x4a:
        cpu->a = cpu_lsr_value(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x4c:
        cpu->pc = cpu_addr_abs(cpu, nes);
        cycles = 3;
        break;
    case 0x4d:
        cpu->a ^= cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x4e:
        addr = cpu_addr_abs(cpu, nes);
        value = cpu_lsr_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x50:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_V) == 0, &cycles);
        break;
    case 0x51:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu->a ^= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 6 : 5;
        break;
    case 0x55:
        cpu->a ^= cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x56:
        addr = cpu_addr_zpx(cpu, nes);
        value = cpu_lsr_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x58:
        cpu->p &= (uint8_t)~CPU6502_FLAG_I;
        cycles = 2;
        break;
    case 0x59:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu->a ^= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x5d:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu->a ^= cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x5e:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = cpu_lsr_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 7;
        break;
    case 0x60:
        cpu->pc = (uint16_t)(cpu_pop(cpu, nes) | ((uint16_t)cpu_pop(cpu, nes) << 8));
        cpu->pc++;
        cycles = 6;
        break;
    case 0x61:
        cpu_adc(cpu, cpu_read(cpu, nes, cpu_addr_indx(cpu, nes)));
        cycles = 6;
        break;
    case 0x65:
        cpu_adc(cpu, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));
        cycles = 3;
        break;
    case 0x66:
        addr = cpu_addr_zp(cpu, nes);
        value = cpu_ror_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 5;
        break;
    case 0x68:
        cpu->a = cpu_pop(cpu, nes);
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0x69:
        cpu_adc(cpu, cpu_fetch8(cpu, nes));
        cycles = 2;
        break;
    case 0x6a:
        cpu->a = cpu_ror_value(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x6c:
        cpu->pc = cpu_addr_indirect(cpu, nes);
        cycles = 5;
        break;
    case 0x6d:
        cpu_adc(cpu, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));
        cycles = 4;
        break;
    case 0x6e:
        addr = cpu_addr_abs(cpu, nes);
        value = cpu_ror_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x70:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_V) != 0, &cycles);
        break;
    case 0x71:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 6 : 5;
        break;
    case 0x75:
        cpu_adc(cpu, cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes)));
        cycles = 4;
        break;
    case 0x76:
        addr = cpu_addr_zpx(cpu, nes);
        value = cpu_ror_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 6;
        break;
    case 0x78:
        cpu->p |= CPU6502_FLAG_I;
        cycles = 2;
        break;
    case 0x79:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x7d:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu_adc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0x7e:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = cpu_ror_value(cpu, cpu_read(cpu, nes, addr));
        cpu_write(cpu, nes, addr, value);
        cycles = 7;
        break;
    case 0x81:
        cpu_write(cpu, nes, cpu_addr_indx(cpu, nes), cpu->a);
        cycles = 6;
        break;
    case 0x84:
        cpu_write(cpu, nes, cpu_addr_zp(cpu, nes), cpu->y);
        cycles = 3;
        break;
    case 0x85:
        cpu_write(cpu, nes, cpu_addr_zp(cpu, nes), cpu->a);
        cycles = 3;
        break;
    case 0x86:
        cpu_write(cpu, nes, cpu_addr_zp(cpu, nes), cpu->x);
        cycles = 3;
        break;
    case 0x88:
        cpu->y--;
        cpu_set_zn(cpu, cpu->y);
        cycles = 2;
        break;
    case 0x8a:
        cpu->a = cpu->x;
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x8c:
        cpu_write(cpu, nes, cpu_addr_abs(cpu, nes), cpu->y);
        cycles = 4;
        break;
    case 0x8d:
        cpu_write(cpu, nes, cpu_addr_abs(cpu, nes), cpu->a);
        cycles = 4;
        break;
    case 0x8e:
        cpu_write(cpu, nes, cpu_addr_abs(cpu, nes), cpu->x);
        cycles = 4;
        break;
    case 0x90:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_C) == 0, &cycles);
        break;
    case 0x91:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu_write(cpu, nes, addr, cpu->a);
        cycles = 6;
        break;
    case 0x94:
        cpu_write(cpu, nes, cpu_addr_zpx(cpu, nes), cpu->y);
        cycles = 4;
        break;
    case 0x95:
        cpu_write(cpu, nes, cpu_addr_zpx(cpu, nes), cpu->a);
        cycles = 4;
        break;
    case 0x96:
        cpu_write(cpu, nes, cpu_addr_zpy(cpu, nes), cpu->x);
        cycles = 4;
        break;
    case 0x98:
        cpu->a = cpu->y;
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0x99:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu_write(cpu, nes, addr, cpu->a);
        cycles = 5;
        break;
    case 0x9a:
        cpu->sp = cpu->x;
        cycles = 2;
        break;
    case 0x9d:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu_write(cpu, nes, addr, cpu->a);
        cycles = 5;
        break;
    case 0xa0:
        cpu->y = cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xa1:
        cpu->a = cpu_read(cpu, nes, cpu_addr_indx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 6;
        break;
    case 0xa2:
        cpu->x = cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xa4:
        cpu->y = cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->y);
        cycles = 3;
        break;
    case 0xa5:
        cpu->a = cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 3;
        break;
    case 0xa6:
        cpu->x = cpu_read(cpu, nes, cpu_addr_zp(cpu, nes));
        cpu_set_zn(cpu, cpu->x);
        cycles = 3;
        break;
    case 0xa8:
        cpu->y = cpu->a;
        cpu_set_zn(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xa9:
        cpu->a = cpu_fetch8(cpu, nes);
        cpu_set_zn(cpu, cpu->a);
        cycles = 2;
        break;
    case 0xaa:
        cpu->x = cpu->a;
        cpu_set_zn(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xac:
        cpu->y = cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->y);
        cycles = 4;
        break;
    case 0xad:
        cpu->a = cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0xae:
        cpu->x = cpu_read(cpu, nes, cpu_addr_abs(cpu, nes));
        cpu_set_zn(cpu, cpu->x);
        cycles = 4;
        break;
    case 0xb0:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_C) != 0, &cycles);
        break;
    case 0xb1:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu->a = cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 6 : 5;
        break;
    case 0xb4:
        cpu->y = cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes));
        cpu_set_zn(cpu, cpu->y);
        cycles = 4;
        break;
    case 0xb5:
        cpu->a = cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes));
        cpu_set_zn(cpu, cpu->a);
        cycles = 4;
        break;
    case 0xb6:
        cpu->x = cpu_read(cpu, nes, cpu_addr_zpy(cpu, nes));
        cpu_set_zn(cpu, cpu->x);
        cycles = 4;
        break;
    case 0xb8:
        cpu->p &= (uint8_t)~CPU6502_FLAG_V;
        cycles = 2;
        break;
    case 0xb9:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu->a = cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xba:
        cpu->x = cpu->sp;
        cpu_set_zn(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xbc:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu->y = cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->y);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xbd:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu->a = cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->a);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xbe:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu->x = cpu_read(cpu, nes, addr);
        cpu_set_zn(cpu, cpu->x);
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xc0:
        cpu_cmp(cpu, cpu->y, cpu_fetch8(cpu, nes));
        cycles = 2;
        break;
    case 0xc1:
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, cpu_addr_indx(cpu, nes)));
        cycles = 6;
        break;
    case 0xc4:
        cpu_cmp(cpu, cpu->y, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));
        cycles = 3;
        break;
    case 0xc5:
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));
        cycles = 3;
        break;
    case 0xc6:
        addr = cpu_addr_zp(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) - 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 5;
        break;
    case 0xc8:
        cpu->y++;
        cpu_set_zn(cpu, cpu->y);
        cycles = 2;
        break;
    case 0xc9:
        cpu_cmp(cpu, cpu->a, cpu_fetch8(cpu, nes));
        cycles = 2;
        break;
    case 0xca:
        cpu->x--;
        cpu_set_zn(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xcc:
        cpu_cmp(cpu, cpu->y, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));
        cycles = 4;
        break;
    case 0xcd:
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));
        cycles = 4;
        break;
    case 0xce:
        addr = cpu_addr_abs(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) - 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 6;
        break;
    case 0xd0:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_Z) == 0, &cycles);
        break;
    case 0xd1:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 6 : 5;
        break;
    case 0xd5:
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes)));
        cycles = 4;
        break;
    case 0xd6:
        addr = cpu_addr_zpx(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) - 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 6;
        break;
    case 0xd8:
        cpu->p &= (uint8_t)~CPU6502_FLAG_D;
        cycles = 2;
        break;
    case 0xd9:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xdd:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu_cmp(cpu, cpu->a, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xde:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = (uint8_t)(cpu_read(cpu, nes, addr) - 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 7;
        break;
    case 0xe0:
        cpu_cmp(cpu, cpu->x, cpu_fetch8(cpu, nes));
        cycles = 2;
        break;
    case 0xe1:
        cpu_sbc(cpu, cpu_read(cpu, nes, cpu_addr_indx(cpu, nes)));
        cycles = 6;
        break;
    case 0xe4:
        cpu_cmp(cpu, cpu->x, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));
        cycles = 3;
        break;
    case 0xe5:
        cpu_sbc(cpu, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));
        cycles = 3;
        break;
    case 0xe6:
        addr = cpu_addr_zp(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) + 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 5;
        break;
    case 0xe8:
        cpu->x++;
        cpu_set_zn(cpu, cpu->x);
        cycles = 2;
        break;
    case 0xe9:
        cpu_sbc(cpu, cpu_fetch8(cpu, nes));
        cycles = 2;
        break;
    case 0xea:
        cycles = 2;
        break;
    case 0xec:
        cpu_cmp(cpu, cpu->x, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));
        cycles = 4;
        break;
    case 0xed:
        cpu_sbc(cpu, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));
        cycles = 4;
        break;
    case 0xee:
        addr = cpu_addr_abs(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) + 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 6;
        break;
    case 0xf0:
        rel = (int8_t)cpu_fetch8(cpu, nes);
        cycles = 2;
        cpu_branch(cpu, rel, (cpu->p & CPU6502_FLAG_Z) != 0, &cycles);
        break;
    case 0xf1:
        addr = cpu_addr_indy(cpu, nes, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 6 : 5;
        break;
    case 0xf5:
        cpu_sbc(cpu, cpu_read(cpu, nes, cpu_addr_zpx(cpu, nes)));
        cycles = 4;
        break;
    case 0xf6:
        addr = cpu_addr_zpx(cpu, nes);
        value = (uint8_t)(cpu_read(cpu, nes, addr) + 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 6;
        break;
    case 0xf8:
        cpu->p |= CPU6502_FLAG_D;
        cycles = 2;
        break;
    case 0xf9:
        addr = cpu_addr_absy(cpu, nes, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xfd:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        cpu_sbc(cpu, cpu_read(cpu, nes, addr));
        cycles = page_crossed ? 5 : 4;
        break;
    case 0xfe:
        addr = cpu_addr_absx(cpu, nes, &page_crossed);
        value = (uint8_t)(cpu_read(cpu, nes, addr) + 1u);
        cpu_write(cpu, nes, addr, value);
        cpu_set_zn(cpu, value);
        cycles = 7;
        break;

    /* ------------------------------------------------------------------ */
    /* Illegal opcodes used by The Legend of Zelda and similar games       */
    /* ------------------------------------------------------------------ */

    /* NOP variants: implied (1 byte, 2 cycles) */
    case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa:
        cycles = 2;
        break;
    /* NOP immediate (2 bytes, 2 cycles) */
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2:
        cpu_fetch8(cpu, nes);
        cycles = 2;
        break;
    /* NOP zero page (2 bytes, 3 cycles) */
    case 0x04: case 0x44: case 0x64:
        cpu_fetch8(cpu, nes);
        cycles = 3;
        break;
    /* NOP zero page,X (2 bytes, 4 cycles) */
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4:
        cpu_fetch8(cpu, nes);
        cycles = 4;
        break;
    /* NOP absolute (3 bytes, 4 cycles) */
    case 0x0c:
        cpu_fetch16(cpu, nes);
        cycles = 4;
        break;
    /* NOP absolute,X (3 bytes, 4+1 cycles) */
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc:
        cpu_addr_absx(cpu, nes, &page_crossed);
        cycles = page_crossed ? 5 : 4;
        break;

    /* SLO: ASL mem, ORA A */
    case 0x03: cpu_slo(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0x07: cpu_slo(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0x0f: cpu_slo(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0x13: cpu_slo(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0x17: cpu_slo(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0x1b: cpu_slo(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0x1f: cpu_slo(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* RLA: ROL mem, AND A */
    case 0x23: cpu_rla(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0x27: cpu_rla(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0x2f: cpu_rla(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0x33: cpu_rla(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0x37: cpu_rla(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0x3b: cpu_rla(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0x3f: cpu_rla(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* SRE: LSR mem, EOR A */
    case 0x43: cpu_sre(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0x47: cpu_sre(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0x4f: cpu_sre(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0x53: cpu_sre(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0x57: cpu_sre(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0x5b: cpu_sre(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0x5f: cpu_sre(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* RRA: ROR mem, ADC A */
    case 0x63: cpu_rra(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0x67: cpu_rra(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0x6f: cpu_rra(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0x73: cpu_rra(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0x77: cpu_rra(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0x7b: cpu_rra(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0x7f: cpu_rra(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* SAX: store A & X (no flags) */
    case 0x83: cpu_sax(cpu, nes, cpu_addr_indx(cpu, nes)); cycles = 6; break;
    case 0x87: cpu_sax(cpu, nes, cpu_addr_zp(cpu, nes));   cycles = 3; break;
    case 0x8f: cpu_sax(cpu, nes, cpu_addr_abs(cpu, nes));  cycles = 4; break;
    case 0x97: cpu_sax(cpu, nes, cpu_addr_zpy(cpu, nes));  cycles = 4; break;

    /* LAX: A = X = M */
    case 0xa3: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_indx(cpu, nes)));           cycles = 6; break;
    case 0xa7: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_zp(cpu, nes)));             cycles = 3; break;
    case 0xaf: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_abs(cpu, nes)));            cycles = 4; break;
    case 0xb3: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed))); cycles = page_crossed ? 6 : 5; break;
    case 0xb7: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_zpy(cpu, nes)));            cycles = 4; break;
    case 0xbf: cpu_lax(cpu, cpu_read(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed))); cycles = page_crossed ? 5 : 4; break;

    /* DCP: DEC mem, CMP A */
    case 0xc3: cpu_dcp(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0xc7: cpu_dcp(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0xcf: cpu_dcp(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0xd3: cpu_dcp(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0xd7: cpu_dcp(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0xdb: cpu_dcp(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0xdf: cpu_dcp(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* ISB: INC mem, SBC A */
    case 0xe3: cpu_isb(cpu, nes, cpu_addr_indx(cpu, nes));           cycles = 8; break;
    case 0xe7: cpu_isb(cpu, nes, cpu_addr_zp(cpu, nes));             cycles = 5; break;
    case 0xef: cpu_isb(cpu, nes, cpu_addr_abs(cpu, nes));            cycles = 6; break;
    case 0xf3: cpu_isb(cpu, nes, cpu_addr_indy(cpu, nes, &page_crossed)); cycles = 8; break;
    case 0xf7: cpu_isb(cpu, nes, cpu_addr_zpx(cpu, nes));            cycles = 6; break;
    case 0xfb: cpu_isb(cpu, nes, cpu_addr_absy(cpu, nes, &page_crossed)); cycles = 7; break;
    case 0xff: cpu_isb(cpu, nes, cpu_addr_absx(cpu, nes, &page_crossed)); cycles = 7; break;

    /* SHA/TAS/SHY/SHX/LAS: unstable illegals — treat as 1-cycle NOP to avoid halting */
    case 0x93: cycles = 1; break;
    case 0x9b: cycles = 1; break;
    case 0x9c: cycles = 1; break;
    case 0x9e: cycles = 1; break;
    case 0x9f: cycles = 1; break;
    case 0xbb: cycles = 1; break;
    case 0x0b: cycles = 1; break;
    case 0x2b: cycles = 1; break;
    case 0x4b: cycles = 1; break;
    case 0x6b: cycles = 1; break;
    case 0x8b: cycles = 1; break;
    case 0xab: cycles = 1; break;
    case 0xcb: cycles = 1; break;
    case 0xeb: cycles = 1; break;

    default: {
        const Cpu6502OpcodeInfo *opcode_info = cpu6502_opcode_info(cpu->last_opcode);
        if (nes->stop_info.reason == NES_STOP_NONE) {
            nes->stop_info.reason = opcode_info->official ? NES_STOP_UNSUPPORTED_OPCODE : NES_STOP_ILLEGAL_OPCODE;
            nes->stop_info.pc = pc_before;
            nes->stop_info.opcode = cpu->last_opcode;
#if MICRONES_ENABLE_RUNTIME_DIAGNOSTICS
            nes->stop_info.opcode_is_official = opcode_info->official;
            nes->stop_info.opcode_is_supported = opcode_info->supported;
#endif
            nes->stop_info.instruction_index = nes->stats.instruction_count + 1;
        }
        snprintf(
            nes->last_error,
            sizeof(nes->last_error),
            "%s opcode $%02x (%s %s) at PC=$%04x A=%02x X=%02x Y=%02x SP=%02x P=%02x instr=%llu",
            opcode_info->official ? "unsupported" : "illegal",
            cpu->last_opcode,
            opcode_info->mnemonic,
            opcode_info->addressing_mode,
            pc_before,
            cpu->a,
            cpu->x,
            cpu->y,
            cpu->sp,
            cpu->p,
            (unsigned long long)(nes->stats.instruction_count + 1)
        );
        cpu->jammed = true;
        return false;
    } /* default */
    } /* switch */

    cpu->cycles += cycles;
    ++nes->stats.instruction_count;
#if MICRONES_ENABLE_STEP_PROFILING
    nes->step_profile.cpu_exec_us_total += nes_profile_now_us(nes) - cpu_started_us;
#endif
    if (cpu_started_cycles != 0) {
        nes->step_profile.cpu_exec_cycles_total +=
            (uint32_t)(micrones_profile_now_cycles() - cpu_started_cycles);
    }
    nes->pending_apu_cycles += cycles;
#if MICRONES_ENABLE_STEP_PROFILING
    cpu_started_us = nes_profile_now_us(nes);
#endif
    cpu_started_cycles = micrones_profile_now_cycles();
    ppu_step_cycles(&nes->ppu, &nes->cartridge, cycles * 3u);
#if MICRONES_ENABLE_STEP_PROFILING
    nes->step_profile.ppu_step_us_total += nes_profile_now_us(nes) - cpu_started_us;
#endif
    if (cpu_started_cycles != 0) {
        nes->step_profile.ppu_step_cycles_total +=
            (uint32_t)(micrones_profile_now_cycles() - cpu_started_cycles);
    }
    return true;
}
