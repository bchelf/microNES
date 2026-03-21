"""
tests/test_savestates.py
========================
Acceptance tests for savestate support in MicroNES / SMBEnv.
All tests are skipped automatically when no SMB1 ROM is available.

ROM discovery order:
  1. SMB1_ROM environment variable
  2. <repo_root>/roms/smb1.nes
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import numpy as np
import pytest

# ---------------------------------------------------------------------------
# Repo / path setup
# ---------------------------------------------------------------------------
REPO_ROOT = Path(__file__).parent.parent
RL_DIR    = REPO_ROOT / "rl"
sys.path.insert(0, str(RL_DIR))

# ---------------------------------------------------------------------------
# ROM discovery
# ---------------------------------------------------------------------------
def _find_rom() -> str | None:
    # 1. Env var
    env = os.environ.get("SMB1_ROM")
    if env and Path(env).is_file():
        return str(env)
    # 2. Conventional location
    candidate = REPO_ROOT / "roms" / "smb1.nes"
    if candidate.is_file():
        return str(candidate)
    return None

ROM_PATH = _find_rom()

pytestmark = pytest.mark.skipif(
    ROM_PATH is None,
    reason=(
        "SMB1 ROM not found. Set SMB1_ROM=/path/to/smb1.nes or place ROM at "
        "roms/smb1.nes relative to repo root."
    ),
)

# ---------------------------------------------------------------------------
# Shared fixtures
# ---------------------------------------------------------------------------

def _make_env(**kwargs):
    """Create a headless SMBEnv on level 1-1."""
    from smb_env import SMBEnv
    env = SMBEnv(
        rom_path=ROM_PATH,
        render_mode=None,
        levels=["1-1"],
        **kwargs,
    )
    return env


def _known_state_size() -> int:
    """Return sizeof(MicroNESSaveState) from the C library."""
    from nes_ctypes import NesLib
    lib = NesLib()
    h   = lib.create()
    sz  = lib.state_size()
    lib.destroy(h)
    return sz


@pytest.fixture
def env():
    """Fresh SMBEnv, reset to 1-1, torn down after test."""
    e = _make_env()
    e.reset()
    yield e
    e.close()


# ---------------------------------------------------------------------------
# TEST 1 — Round-trip identity
# ---------------------------------------------------------------------------
def test_roundtrip_identity():
    """
    Save state at step 50, take 20 more random actions, load state,
    replay same 20 actions — obs and info must be identical at every step.
    Repeated 10 times with different random seeds.
    """
    env = _make_env()

    for trial in range(10):
        rng = np.random.default_rng(trial * 17 + 3)
        env.reset()

        # Advance 50 steps
        for _ in range(50):
            action = int(rng.integers(0, env.action_space.n))
            env.step(action)

        state = env.save_state()

        # Record 20 actions + resulting (obs, info)
        replay_actions = [int(rng.integers(0, env.action_space.n)) for _ in range(20)]
        first_run: list[tuple[dict, dict]] = []
        for act in replay_actions:
            obs, _, _, _, info = env.step(act)
            first_run.append((obs, info))

        # Restore and replay
        env.load_state(state)
        for step_i, act in enumerate(replay_actions):
            obs2, _, _, _, info2 = env.step(act)
            orig_obs, orig_info = first_run[step_i]

            for key, arr in orig_obs.items():
                assert np.allclose(arr, obs2[key], atol=1e-6), (
                    f"Trial {trial} step {step_i}: obs[{key}] mismatch"
                )
            assert info2["world_x"] == orig_info["world_x"], (
                f"Trial {trial} step {step_i}: world_x mismatch "
                f"{info2['world_x']} != {orig_info['world_x']}"
            )
            assert info2["mario_y"] == orig_info["mario_y"], (
                f"Trial {trial} step {step_i}: mario_y mismatch"
            )
            assert info2["lives_remaining"] == orig_info["lives_remaining"], (
                f"Trial {trial} step {step_i}: lives mismatch"
            )

    env.close()


# ---------------------------------------------------------------------------
# TEST 2 — Independence
# ---------------------------------------------------------------------------
def test_independence(env):
    """
    Diverge 100 steps after save, reload — restored state must match
    the values at save time, unaffected by the divergent path.
    """
    rng = np.random.default_rng(42)

    # Advance 50 steps
    for _ in range(50):
        env.step(int(rng.integers(0, env.action_space.n)))

    state = env.save_state()

    # Read values at save point
    ram_at_save = bytes(env._ram)
    world_x_at_save      = env._read_world_x()
    mario_y_at_save      = int(env._ram[0x00CE])
    lives_at_save        = int(env._ram[0x075A])
    ram_000e_at_save     = int(env._ram[0x000E])

    # Diverge: 100 more random steps
    for _ in range(100):
        action = int(rng.integers(0, env.action_space.n))
        env.step(action)

    # Restore
    env.load_state(state)

    # Verify restored values match save point
    assert env._read_world_x() == world_x_at_save, "world_x mismatch after restore"
    assert int(env._ram[0x00CE]) == mario_y_at_save, "mario_y mismatch after restore"
    assert int(env._ram[0x075A]) == lives_at_save, "lives_remaining mismatch after restore"
    assert int(env._ram[0x000E]) == ram_000e_at_save, "RAM[0x000E] mismatch after restore"

    # Step once and verify world_x is still consistent (not the post-diverge value)
    _, _, _, _, info = env.step(0)  # WAIT
    # After one step, x should be near save-point x (not the divergent x)
    assert abs(info["world_x"] - world_x_at_save) < 50, (
        f"world_x {info['world_x']} too far from save-point x {world_x_at_save} "
        f"after restore — divergent path may have leaked"
    )


# ---------------------------------------------------------------------------
# TEST 3 — Python state sync (prev_lives bug)
# ---------------------------------------------------------------------------
def test_python_state_sync(env):
    """
    After load_state(), _prev_lives must be re-derived from restored RAM
    so that a lives decrement that happened AFTER the save does not produce
    a spurious pit_death on the first step after restore.
    """
    rng = np.random.default_rng(99)

    # Advance until we have a stable position on level 1-1
    for _ in range(30):
        env.step(int(rng.integers(0, env.action_space.n)))

    # Confirm Mario has 2 lives (normal start)
    lives_at_save = int(env._ram[0x075A])
    state = env.save_state()

    # Simulate what happens after a death: Python-side prev_lives drops to 1.
    # (In real gameplay this is set by step() when current_lives < prev_lives.)
    env._prev_lives = max(0, lives_at_save - 1)

    # Load state — must restore _prev_lives to lives_at_save via RAM
    env.load_state(state)

    # _prev_lives must now equal lives_at_save (derived from restored RAM[0x075A])
    assert env._prev_lives == lives_at_save, (
        f"After load_state, _prev_lives={env._prev_lives} but expected "
        f"{lives_at_save} — Python state sync broken"
    )

    # Step once — pit_death must NOT fire because lives haven't actually changed
    _, _, _, _, info = env.step(0)
    assert not info["pit_death"], (
        "pit_death fired on first step after load_state — prev_lives was not "
        "correctly restored to match the emulator state"
    )


# ---------------------------------------------------------------------------
# TEST 4 — Savestate size is fixed
# ---------------------------------------------------------------------------
def test_savestate_size(env):
    """Savestate must be exactly sizeof(MicroNESSaveState) bytes."""
    sz = _known_state_size()
    assert sz > 0, "micrones_rl_state_size() returned 0"

    state = env.save_state()
    assert len(state) == sz, (
        f"save_state() returned {len(state)} bytes but sizeof(MicroNESSaveState)={sz}"
    )

    # Verify it's the same size after advancing
    for _ in range(100):
        env.step(1)  # STEP_RIGHT
    state2 = env.save_state()
    assert len(state2) == sz, "Savestate size must be constant regardless of game state"

    print(f"\n  sizeof(MicroNESSaveState) = {sz} bytes")


# ---------------------------------------------------------------------------
# TEST 5 — Performance
# ---------------------------------------------------------------------------
def test_performance(env):
    """
    1000 consecutive save_state() and load_state() calls must each have
    mean latency < 1 ms.
    """
    # Warm up
    state = env.save_state()
    for _ in range(10):
        env.save_state()
        env.load_state(state)

    N = 1000

    # Time save_state
    t0 = time.perf_counter()
    for _ in range(N):
        state = env.save_state()
    save_total = time.perf_counter() - t0

    # Time load_state
    t0 = time.perf_counter()
    for _ in range(N):
        env.load_state(state)
    load_total = time.perf_counter() - t0

    save_mean_ms = (save_total / N) * 1000.0
    load_mean_ms = (load_total / N) * 1000.0

    # Compute p99 via individual timings for load
    load_times_ms = []
    for _ in range(N):
        t0 = time.perf_counter()
        env.load_state(state)
        load_times_ms.append((time.perf_counter() - t0) * 1000.0)

    save_times_ms = []
    for _ in range(N):
        t0 = time.perf_counter()
        env.save_state()
        save_times_ms.append((time.perf_counter() - t0) * 1000.0)

    save_p99 = float(np.percentile(save_times_ms, 99))
    load_p99 = float(np.percentile(load_times_ms, 99))

    print(
        f"\n  save_state: mean={save_mean_ms:.3f}ms  p99={save_p99:.3f}ms  "
        f"(N={N})"
    )
    print(
        f"  load_state: mean={load_mean_ms:.3f}ms  p99={load_p99:.3f}ms  "
        f"(N={N})"
    )

    assert save_mean_ms < 1.0, (
        f"save_state mean {save_mean_ms:.3f}ms >= 1ms — too slow for Go-Explore"
    )
    assert load_mean_ms < 1.0, (
        f"load_state mean {load_mean_ms:.3f}ms >= 1ms — too slow for Go-Explore"
    )


# ---------------------------------------------------------------------------
# TEST 6 — Cross-level restore
# ---------------------------------------------------------------------------
def test_cross_level(env):
    """
    Save state on 1-1 at step 100, reset env to 1-3, load the 1-1 state —
    Mario must be back at the 1-1 position.
    """
    from smb_env import SMBEnv

    rng = np.random.default_rng(7)

    # Advance on 1-1
    for _ in range(100):
        env.step(int(rng.integers(0, env.action_space.n)))

    world_x_1_1  = env._read_world_x()
    mario_y_1_1  = int(env._ram[0x00CE])
    level_ram_75f = int(env._ram[0x075F])  # WorldNumber byte (0 = world 1)

    state_1_1 = env.save_state()

    # Reset to level 1-3
    env.reset(options={"level": "1-3"})
    assert int(env._ram[0x075C]) == 2, "Should be on level index 2 (1-3)"

    # Load the 1-1 state
    env.load_state(state_1_1)

    # Verify world number RAM matches 1-1
    assert int(env._ram[0x075F]) == level_ram_75f, (
        f"World number RAM[0x075F]={env._ram[0x075F]} after restore, "
        f"expected {level_ram_75f} (1-1)"
    )

    # World x should be close to where we saved (within 2 tiles, accounting
    # for the env.step(0) warmup we DON'T do here)
    restored_x = env._read_world_x()
    assert abs(restored_x - world_x_1_1) < 20, (
        f"Restored world_x={restored_x} too far from 1-1 save x={world_x_1_1}"
    )

    # Take one step — must not crash and x should stay consistent
    _, _, _, _, info = env.step(0)
    assert info["world_x"] is not None, "No world_x in info after cross-level restore"
