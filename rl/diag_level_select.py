"""
Diagnostic: compare full RAM snapshots between a natural 1-1 boot and an
injected target-level boot to find every address that differs.

Run from repo root:
    python rl/diag_level_select.py --rom roms/smb1.nes --level 1-4
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

import numpy as np
from nes_ctypes import NES_BUTTON_START, NesLib

A_OPER_MODE = 0x0770
A_OPER_TASK = 0x0772
A_WORLD     = 0x075F
A_LEVEL     = 0x075C
A_AREA_TYPE = 0x0760
A_AREA_VAL  = 0x0750   # packed area identifier: bits 6-5=type, 4-0=area_num
A_AREA_760  = 0x0760   # relative area offset within world's $9CBC section

LEVEL_MAP = {
    "1-1": (0, 0), "1-2": (0, 1), "1-3": (0, 2), "1-4": (0, 3),
    "2-1": (1, 0), "2-2": (1, 1), "2-3": (1, 2), "2-4": (1, 3),
    "3-1": (2, 0), "3-2": (2, 1), "3-3": (2, 2), "3-4": (2, 3),
    "4-1": (3, 0), "4-2": (3, 1), "4-3": (3, 2), "4-4": (3, 3),
    "5-1": (4, 0), "5-2": (4, 1), "5-3": (4, 2), "5-4": (4, 3),
    "6-1": (5, 0), "6-2": (5, 1), "6-3": (5, 2), "6-4": (5, 3),
    "7-1": (6, 0), "7-2": (6, 1), "7-3": (6, 2), "7-4": (6, 3),
    "8-1": (7, 0), "8-2": (7, 1), "8-3": (7, 2), "8-4": (7, 3),
}

# ($0750, $0760) derived from SMB1 ROM:
#   abs_idx = WorldAddrOffsets[$CCC7][world*4+level]
#   $0750   = $9CBC[abs_idx]
#   $0760   = (abs_idx - $9CB4[WorldNumber]) & 0xFF
AREA_REGS_MAP = {
    "1-1": (0x25, 0x00), "1-2": (0x29, 0x01), "1-3": (0x26, 0x03), "1-4": (0x60, 0x04),
    "2-1": (0x28, 0x00), "2-2": (0x29, 0x01), "2-3": (0x27, 0x03), "2-4": (0x62, 0x04),
    "3-1": (0x27, 0xFE), "3-2": (0x25, 0xF6), "3-3": (0x26, 0xF9), "3-4": (0x29, 0xFC),
    "4-1": (0x62, 0xFB), "4-2": (0x35, 0xFD), "4-3": (0x63, 0xFF), "4-4": (0x22, 0x00),
    "5-1": (0x29, 0xFC), "5-2": (0x41, 0xFD), "5-3": (0x25, 0xED), "5-4": (0x60, 0xF1),
    "6-1": (0x62, 0xF2), "6-2": (0x63, 0xF6), "6-3": (0x41, 0xF9), "6-4": (0x2A, 0xFC),
    "7-1": (0x62, 0xFB), "7-2": (0x2E, 0xFC), "7-3": (0x23, 0xFD), "7-4": (0x25, 0xE5),
    "8-1": (0x29, 0xE6), "8-2": (0x20, 0xEC), "8-3": (0x61, 0xF2), "8-4": (0x62, 0xF6),
}


def boot_to_gameplay(lib, rom_path, world_idx=0, level_idx=0, area_val=0x25, area_760=0x00, inject=True):
    """
    Boot to gameplay and return a copy of the full 2048-byte RAM.
    If inject=True, writes $0750/$075F/$075C after OperMode==1 and forces a
    task reset so LoadAreaPointer re-runs with the injected values.
    """
    h   = lib.create()
    lib.load_rom(h, rom_path)
    lib.reset(h)
    ram = lib.ram_view(h)

    # Boot 80 frames
    for _ in range(80):
        lib.step(h)

    # Press START 20 frames
    for _ in range(20):
        lib.set_buttons(h, NES_BUTTON_START)
        lib.step(h)
    lib.set_buttons(h, 0)

    # Wait for OperMode==1
    for _ in range(300):
        lib.step(h)
        if int(ram[A_OPER_MODE]) == 1:
            break

    if inject:
        # $0750 = packed area identifier (read by LoadAreaPointer)
        # $0760 = relative area offset within world's $9CBC section
        # $075F/$075C = HUD display only
        lib.write_ram(h, A_AREA_VAL,  area_val)
        lib.write_ram(h, A_AREA_760,  area_760)
        lib.write_ram(h, A_WORLD,     world_idx)
        lib.write_ram(h, A_LEVEL,     level_idx)
        lib.write_ram(h, A_OPER_TASK, 0x00)

    # Settle 60 frames
    for _ in range(60):
        if inject:
            lib.write_ram(h, A_AREA_VAL, area_val)
            lib.write_ram(h, A_AREA_760, area_760)
            lib.write_ram(h, A_WORLD,    world_idx)
            lib.write_ram(h, A_LEVEL,    level_idx)
        lib.step(h)

    snap = np.array(ram, dtype=np.uint8).copy()
    mode = int(ram[A_OPER_MODE])
    task = int(ram[A_OPER_TASK])
    e7 = int(snap[0xE7])
    e8 = int(snap[0xE8])
    lib.destroy(h)
    return snap, mode, task, e8 * 256 + e7


def run(rom_path, target_level, lib_path=None):
    world_idx, level_idx = LEVEL_MAP[target_level]
    area_val, area_760   = AREA_REGS_MAP.get(target_level, (0x25, 0x00))
    print(f"\n=== Level-select RAM diff: 1-1 vs {target_level} "
          f"(world={world_idx} level={level_idx} $0750=0x{area_val:02X} $0760=0x{area_760:02X}) ===\n")

    lib = NesLib(lib_path)

    print("Booting 1-1 (no injection)...")
    snap_11, mode_11, task_11, ptr_11 = boot_to_gameplay(lib, rom_path, inject=False)
    print(f"  mode={mode_11} task={task_11}  "
          f"world={snap_11[A_WORLD]:02x} level={snap_11[A_LEVEL]:02x} "
          f"$0750={snap_11[A_AREA_VAL]:02x} $0760={snap_11[A_AREA_760]:02x} area_ptr=${ptr_11:04X}\n")

    print(f"Booting {target_level} (with injection)...")
    snap_tgt, mode_tgt, task_tgt, ptr_tgt = boot_to_gameplay(
        lib, rom_path, world_idx, level_idx, area_val, area_760, inject=True)
    print(f"  mode={mode_tgt} task={task_tgt}  "
          f"world={snap_tgt[A_WORLD]:02x} level={snap_tgt[A_LEVEL]:02x} "
          f"$0750={snap_tgt[A_AREA_VAL]:02x} $0760={snap_tgt[A_AREA_760]:02x} area_ptr=${ptr_tgt:04X}\n")

    # Verify area_ptr changed
    if ptr_11 != ptr_tgt:
        print(f"  ✓ Area data pointer changed: ${ptr_11:04X} → ${ptr_tgt:04X}")
    else:
        print(f"  ✗ Area data pointer UNCHANGED at ${ptr_11:04X} — injection FAILED")

    # Full RAM diff
    diffs = np.where(snap_11 != snap_tgt)[0]
    if len(diffs) == 0:
        print("  RAM is IDENTICAL — injection had no effect at all.")
    else:
        print(f"\n  {len(diffs)} addresses differ between 1-1 and {target_level}:")
        print(f"  {'addr':>6}  {'1-1':>4}  {target_level:>4}  note")
        print(f"  {'------':>6}  {'----':>4}  {'----':>4}")
        for addr in diffs:
            v11  = snap_11[addr]
            vtgt = snap_tgt[addr]
            note = ""
            if addr == A_AREA_VAL:  note = "← $0750 PackedAreaVal"
            elif addr == A_AREA_760: note = "← $0760 AreaOffset"
            elif addr == A_WORLD:   note = "← WorldNumber"
            elif addr == A_LEVEL:   note = "← LevelNumber"
            elif addr == A_AREA_TYPE: note = "← AreaType"
            elif addr == A_OPER_MODE: note = "← OperMode"
            elif addr == A_OPER_TASK: note = "← OperMode_Task"
            elif addr == 0x00E7:    note = "← area_ptr lo"
            elif addr == 0x00E8:    note = "← area_ptr hi"
            elif addr == 0x074E:    note = "← $074E (area_type)"
            elif addr == 0x074F:    note = "← $074F (area_num)"
            print(f"  ${addr:04x}  {v11:4d}  {vtgt:4d}  {note}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom",   required=True)
    parser.add_argument("--level", default="1-4")
    parser.add_argument("--lib",   default=None)
    args = parser.parse_args()
    run(args.rom, args.level, args.lib)
