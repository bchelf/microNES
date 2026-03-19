"""
Tile mapping diagnostic: boot to each level, log raw nametable tile IDs below
Mario's feet and compare against what smb_env._TILE_SEMANTICS considers solid.

Run:
    python rl/diag_tile_audit.py --rom roms/smb1.nes
"""
from __future__ import annotations
import argparse, os, sys
sys.path.insert(0, os.path.dirname(__file__))

import numpy as np
from nes_ctypes import NES_BUTTON_START, NesLib
from smb_env import _TILE_SEMANTICS, _SEMANTIC_IS_SOLID, MARIO_ROW, MARIO_COL, GRID_ROWS, GRID_COLS

# Same area-injection tables as SMBEnv
_LEVEL_MAP = {
    "1-1": (0,0), "1-2": (0,1), "1-3": (0,2), "1-4": (0,3),
    "2-1": (1,0), "2-2": (1,1), "2-3": (1,2), "2-4": (1,3),
}
_AREA_REGS = {
    "1-1": (0x25, 0x00), "1-2": (0x29, 0x01), "1-3": (0x26, 0x03), "1-4": (0x60, 0x04),
    "2-1": (0x28, 0x00), "2-2": (0x29, 0x01), "2-3": (0x01, 0x02), "2-4": (0x01, 0x02),
}


def boot_to_level(lib, rom_path: str, level: str, settle_extra: int = 0):
    world_idx, level_idx = _LEVEL_MAP[level]
    area_val,  area_760  = _AREA_REGS[level]

    h = lib.create()
    lib.load_rom(h, rom_path)
    lib.reset(h)
    ram = lib.ram_view(h)
    nt  = lib.nametables_view(h)

    for _ in range(80):
        lib.step(h)
    for _ in range(20):
        lib.set_buttons(h, NES_BUTTON_START)
        lib.step(h)
    lib.set_buttons(h, 0)
    for _ in range(300):
        lib.step(h)
        if int(ram[0x0770]) == 1:
            break

    lib.write_ram(h, 0x0750, area_val)
    lib.write_ram(h, 0x0760, area_760)
    lib.write_ram(h, 0x075F, world_idx)
    lib.write_ram(h, 0x075C, level_idx)
    lib.write_ram(h, 0x0772, 0x00)

    for _ in range(60 + settle_extra):
        lib.write_ram(h, 0x0750, area_val)
        lib.write_ram(h, 0x0760, area_760)
        lib.write_ram(h, 0x075F, world_idx)
        lib.write_ram(h, 0x075C, level_idx)
        lib.step(h)

    return h, ram, nt


def get_mario_tile_pos(ram):
    mario_sx  = int(ram[0x0086])
    mario_sy  = int(ram[0x00CE])
    scroll_lo = int(ram[0x071A])
    scroll_hi = int(ram[0x071B])
    scroll_px = scroll_hi * 256 + scroll_lo
    mario_tx  = (scroll_px + mario_sx) // 8
    mario_ty  = mario_sy // 8
    return mario_sx, mario_sy, mario_tx, mario_ty, scroll_px


def read_raw_tile(nt, world_tx: int, world_ty: int) -> int:
    if world_ty < 0 or world_ty >= 30:
        return -1
    nt_x     = world_tx % 64
    nt_index = nt_x // 32
    local_x  = nt_x % 32
    offset   = nt_index * 1024 + world_ty * 32 + local_x
    return int(nt[offset])


def audit_level(lib, rom_path: str, level: str, n_steps: int = 25) -> dict:
    """Boot level, run n_steps with no input, collect tile+feature data."""
    h, ram, nt = boot_to_level(lib, rom_path, level)

    records = []
    unmapped_ids: set[int] = set()
    void_grounded_mismatches = 0

    for step_i in range(n_steps):
        lib.step(h)

        on_ground_ram = int(ram[0x001C] != 0)
        mario_sx, mario_sy, mario_tx, mario_ty, scroll_px = get_mario_tile_pos(ram)
        world_x = int(ram[0x006D]) * 256 + mario_sx
        vx = int(np.int8(ram[0x0057]))

        # Raw tile IDs in 4 rows below Mario (feet+3)
        raw_below = []
        for dy in range(1, 5):
            row = []
            for dx in range(-2, 6):
                tid = read_raw_tile(nt, mario_tx + dx, mario_ty + dy)
                row.append(tid)
            raw_below.append(row)

        # Semantic tile below Mario's feet column
        foot_col_tiles = [read_raw_tile(nt, mario_tx, mario_ty + dy) for dy in range(1, 5)]
        foot_semantics = [int(_TILE_SEMANTICS[t]) if t >= 0 else -1 for t in foot_col_tiles]
        foot_solid     = [bool(_SEMANTIC_IS_SOLID[s]) if s >= 0 else False for s in foot_semantics]

        # Compute support_below as SMBEnv does (any solid in foot column, ground_start=MARIO_ROW+2)
        # We simulate has_support for the Mario column using the ground rows only
        ground_row_start = MARIO_ROW + 2

        # Build a tiny semantic grid for just the columns around Mario
        def has_support_col(col_offset):
            for dy in range(ground_row_start - MARIO_ROW, GRID_ROWS - MARIO_ROW):
                tid = read_raw_tile(nt, mario_tx + col_offset, mario_ty + dy)
                if tid < 0:
                    return True  # out of bounds treated solid
                sem = int(_TILE_SEMANTICS[tid])
                if _SEMANTIC_IS_SOLID[sem]:
                    return True
            return False

        support_below_feat = has_support_col(0)
        over_void_feat     = not support_below_feat

        if on_ground_ram and over_void_feat:
            void_grounded_mismatches += 1

        # Collect unmapped IDs when floor appears as void
        if over_void_feat:
            for row in raw_below:
                for tid in row:
                    if 0 <= tid <= 255 and not _SEMANTIC_IS_SOLID[int(_TILE_SEMANTICS[tid])]:
                        unmapped_ids.add(tid)

        records.append({
            "step":          step_i,
            "on_ground_ram": on_ground_ram,
            "world_x":       world_x,
            "vx":            vx,
            "support_below": support_below_feat,
            "over_void":     over_void_feat,
            "foot_raw_ids":  foot_col_tiles,
            "foot_semantics":foot_semantics,
            "foot_solid":    foot_solid,
            "raw_below":     raw_below,
        })

    lib.destroy(h)
    return {
        "records":       records,
        "unmapped_ids":  sorted(unmapped_ids),
        "void_grounded_mismatches": void_grounded_mismatches,
    }


def print_audit(level: str, result: dict):
    records = result["records"]
    mismatches = result["void_grounded_mismatches"]
    unmapped   = result["unmapped_ids"]

    print(f"\n{'='*60}")
    print(f"  LEVEL {level}  ({len(records)} steps)")
    print(f"  void_grounded_mismatches: {mismatches}/{len(records)}")
    if unmapped:
        print(f"  unmapped tile IDs seen during void: {[hex(x) for x in unmapped]}")
    print(f"{'='*60}")
    print(f"  {'step':>4}  {'gnd':>3}  {'supp':>4}  {'void':>4}  {'world_x':>7}  {'vx':>3}  foot_raw_ids")
    print(f"  {'-'*60}")

    prev_over_void = None
    for r in records:
        flag = " *** MISMATCH ***" if r["on_ground_ram"] and r["over_void"] else ""
        changed = "" if prev_over_void is None or r["over_void"] == prev_over_void else " <-- void changed"
        print(
            f"  {r['step']:>4}  "
            f"{r['on_ground_ram']:>3}  "
            f"{'T' if r['support_below'] else 'F':>4}  "
            f"{'T' if r['over_void'] else 'F':>4}  "
            f"{r['world_x']:>7}  "
            f"{r['vx']:>3}  "
            f"{[hex(x) for x in r['foot_raw_ids']]}"
            f"{flag}{changed}"
        )
        prev_over_void = r["over_void"]

    # Print full tile grid for first 3 steps
    print(f"\n  Raw tile ID grid below feet (rows 1-4 below Mario, cols -2..+5), first 3 steps:")
    for r in records[:3]:
        print(f"  step={r['step']}:")
        for row_i, row in enumerate(r["raw_below"]):
            solid_flags = ["S" if _SEMANTIC_IS_SOLID[int(_TILE_SEMANTICS[t])] else "." for t in row if t >= 0]
            print(f"    dy=+{row_i+1}: {[hex(t) for t in row]}  solid={solid_flags}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom",    required=True)
    parser.add_argument("--lib",    default=None)
    parser.add_argument("--levels", nargs="+", default=["1-1", "1-3", "1-4"])
    parser.add_argument("--steps",  type=int, default=25)
    args = parser.parse_args()

    lib = NesLib(args.lib)

    all_results = {}
    for level in args.levels:
        print(f"Auditing {level}...", flush=True)
        result = audit_level(lib, args.rom, level, n_steps=args.steps)
        all_results[level] = result
        print_audit(level, result)

    # Cross-level comparison
    print(f"\n{'='*60}")
    print("  CROSS-LEVEL SUMMARY")
    print(f"  {'level':>5}  {'void_gnd_mismatch':>18}  {'unmapped_ids'}")
    print(f"  {'-'*60}")
    for level, result in all_results.items():
        print(
            f"  {level:>5}  "
            f"{result['void_grounded_mismatches']:>18}  "
            f"{[hex(x) for x in result['unmapped_ids']]}"
        )

    # Collect all unique floor tile IDs across levels for comparison
    print(f"\n  Foot column tile IDs at step 0 per level:")
    for level, result in all_results.items():
        r0 = result["records"][0]
        print(f"  {level}: {[hex(x) for x in r0['foot_raw_ids']]}  "
              f"solid={r0['foot_solid']}  support_below={r0['support_below']}")


if __name__ == "__main__":
    main()
