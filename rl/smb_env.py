"""
SMBEnv — Gymnasium environment for Super Mario Bros 1 (NROM / mapper-0).

Structured observations from emulator RAM and PPU nametables.
No pixels used. Frame skip = 3.

Observation space: Dict of grouped Box spaces (all float32, range [-1,1] or [0,1]).
Action space: Discrete(12) — meaningful NES button combinations.
"""

from __future__ import annotations

import os
from collections import deque
from typing import Any

import gymnasium as gym
import numpy as np
from gymnasium import spaces

# ---------------------------------------------------------------------------
# Standard NTSC NES palette (64 entries, RGB uint8)
# ---------------------------------------------------------------------------
NES_PALETTE_RGB = np.array([
    [124,124,124],[  0,  0,252],[  0,  0,188],[ 68, 40,188],
    [148,  0,132],[168,  0, 32],[168, 16,  0],[136, 20,  0],
    [ 80, 48,  0],[  0,120,  0],[  0,104,  0],[  0, 88,  0],
    [  0, 64, 88],[  0,  0,  0],[  0,  0,  0],[  0,  0,  0],
    [188,188,188],[  0,120,248],[  0, 88,248],[104, 68,252],
    [216,  0,204],[228,  0, 88],[248, 56,  0],[228, 92, 16],
    [172,124,  0],[  0,184,  0],[  0,168,  0],[  0,168, 68],
    [  0,136,136],[  0,  0,  0],[  0,  0,  0],[  0,  0,  0],
    [248,248,248],[ 60,188,252],[104,136,252],[152,120,248],
    [248,120,248],[248, 88,152],[248,120, 88],[252,160, 68],
    [248,184,  0],[184,248, 24],[ 88,216, 84],[ 88,248,152],
    [  0,232,216],[120,120,120],[  0,  0,  0],[  0,  0,  0],
    [252,252,252],[164,228,252],[184,184,248],[216,184,248],
    [248,184,248],[248,164,192],[240,208,176],[252,224,168],
    [248,216,120],[216,248,120],[184,248,184],[184,248,216],
    [  0,252,252],[248,216,248],[  0,  0,  0],[  0,  0,  0],
], dtype=np.uint8)

from nes_ctypes import (
    NES_BUTTON_A, NES_BUTTON_B, NES_BUTTON_DOWN,
    NES_BUTTON_LEFT, NES_BUTTON_RIGHT, NES_BUTTON_START,
    NesLib,
)

# ---------------------------------------------------------------------------
# Action space — higher-level motor primitives
# ---------------------------------------------------------------------------
# Each action is a list of (buttons, n_frames) segments executed in sequence
# within a single env step.  PPO still sees Discrete(N_ACTIONS).
#
# Jump timing: SMB1 jump height is controlled by how long A is held.
#   _SHORT_JUMP_HOLD frames ≈ 50-60% max height  (clear 1-tile gaps / low platforms)
#   _FULL_JUMP_HOLD  frames ≈ near-maximum height (clear tall obstacles / wide gaps)
# ---------------------------------------------------------------------------
_R   = NES_BUTTON_RIGHT
_L   = NES_BUTTON_LEFT
_A   = NES_BUTTON_A
_B   = NES_BUTTON_B
_D   = NES_BUTTON_DOWN

_BASE_FRAMES     = 5   # NES frames for all non-jump actions
_SHORT_JUMP_HOLD = 5   # NES frames to hold A → short arc
_FULL_JUMP_HOLD  = 9   # NES frames to hold A → full arc

# Action durations: non-jump = 5 frames, short jump = 6, full jump = 10.
# Index  Name              Segments: [(buttons, n_frames), ...]
_ACTION_SEQUENCES: list[list[tuple[int, int]]] = [
    [(0,              _BASE_FRAMES)],                              # 0  WAIT
    [(_R,             _BASE_FRAMES)],                              # 1  STEP_RIGHT
    [(_R | _B,        _BASE_FRAMES)],                              # 2  RUN_RIGHT
    [(_L,             _BASE_FRAMES)],                              # 3  STEP_LEFT
    [(_L | _B,        _BASE_FRAMES)],                              # 4  RUN_LEFT
    [(_R | _A,        _SHORT_JUMP_HOLD), (_R,       1)],           # 5  SHORT_JUMP_R   (6)
    [(_R | _B | _A,   _FULL_JUMP_HOLD),  (_R | _B,  1)],           # 6  FULL_JUMP_R   (10)
    [(_L | _A,        _SHORT_JUMP_HOLD), (_L,       1)],           # 7  SHORT_JUMP_L   (6)
    [(_L | _B | _A,   _FULL_JUMP_HOLD),  (_L | _B,  1)],           # 8  FULL_JUMP_L   (10)
    [(_A,             _SHORT_JUMP_HOLD), (0,         1)],           # 9  SHORT_JUMP_IP  (6)
    [(_A,             _FULL_JUMP_HOLD),  (0,         1)],           # 10 FULL_JUMP_IP  (10)
    [(0,              _BASE_FRAMES)],                              # 11 BRAKE
    [(_D,             _BASE_FRAMES)],                              # 12 CROUCH / pipe
    [(_B,             _BASE_FRAMES)],                              # 13 FIREBALL
]
N_ACTIONS = len(_ACTION_SEQUENCES)

# ---------------------------------------------------------------------------
# Tile semantics — maps NES tile ID (0-255) → category uint8
# ---------------------------------------------------------------------------
# 0=empty  1=solid  2=breakable  3=question  4=pipe  5=platform  6=hazard  7=coin
TILE_EMPTY    = 0
TILE_SOLID    = 1
TILE_BREAK    = 2
TILE_QUESTION = 3
TILE_PIPE     = 4
TILE_PLATFORM = 5
TILE_HAZARD   = 6
TILE_COIN     = 7

_TILE_SEMANTICS = np.zeros(256, dtype=np.uint8)  # default = EMPTY

def _mark(ids, cat):
    for i in ids:
        _TILE_SEMANTICS[i] = cat

# Solid ground and hard blocks
_mark(range(0x52, 0x58), TILE_SOLID)   # underground brick tops/sides
_mark(range(0x5A, 0x60), TILE_SOLID)   # more block variants
_mark([0xC1], TILE_SOLID)              # used ? block
_mark(range(0xC2, 0xCA), TILE_SOLID)   # hard blocks (ground/walls)
_mark(range(0xD0, 0xD8), TILE_SOLID)   # staircase tiles
_mark([0xE0, 0xE1, 0xE2, 0xE3], TILE_SOLID)  # flagpole base

# Breakable bricks
_mark(range(0x85, 0x8D), TILE_BREAK)

# Question blocks
_mark([0xC0], TILE_QUESTION)

# Pipes  ($00-$03 top pieces, $10-$13 and $60-$63 body)
_mark(range(0x00, 0x04), TILE_PIPE)
_mark(range(0x10, 0x14), TILE_PIPE)
_mark(range(0x60, 0x64), TILE_PIPE)

# Platforms (mushroom platforms, cloud platforms)
_mark(range(0xA4, 0xA8), TILE_PLATFORM)
_mark(range(0xB4, 0xB8), TILE_PLATFORM)

# Hazards (lava, cannon barrel)
_mark(range(0xF0, 0xF8), TILE_HAZARD)

# Coins
_mark([0xA0, 0xA1, 0xA2, 0xA3], TILE_COIN)

# Which SEMANTIC categories are physically solid (block movement / support Mario).
# Indexed by semantic value (0-7), NOT by tile ID.
_SEMANTIC_IS_SOLID = np.zeros(8, dtype=bool)
_SEMANTIC_IS_SOLID[TILE_SOLID]    = True
_SEMANTIC_IS_SOLID[TILE_BREAK]    = True
_SEMANTIC_IS_SOLID[TILE_QUESTION] = True
_SEMANTIC_IS_SOLID[TILE_PIPE]     = True
_SEMANTIC_IS_SOLID[TILE_PLATFORM] = True

# ---------------------------------------------------------------------------
# Grid layout
# ---------------------------------------------------------------------------
GRID_ROWS   = 13
GRID_COLS   = 17
MARIO_ROW   = 8   # 8 rows above (overhead), 4 rows below
MARIO_COL   = 4   # 4 cols behind, 12 cols ahead (lookahead)
LOOKAHEAD_N = 5   # columns ahead to check for gap/obstacle/enemy signals

# Max tile search distance for progress-to-next-object signals (in tiles)
_DIST_MAX_TILES = 16

# ---------------------------------------------------------------------------
# Platform topology scan parameters
# ---------------------------------------------------------------------------
SCAN_AHEAD_COLS  = 24   # tile columns to scan ahead for topology (192px at 8px/tile)
MAX_JUMP_HEIGHT  = 5    # tiles above foot level reachable by jumping
MAX_FALL_HEIGHT  = 8    # tiles below foot level scanned for surfaces
MAX_SPAN_WIDTH   = 12   # normalization ceiling for surface span width
TRAJ_HIST_LEN    = 8    # steps of trajectory history

_JUMP_ACTIONS = frozenset([5, 6, 7, 8, 9, 10])  # action indices that press A

# ---------------------------------------------------------------------------
# Route viability reward shaping parameters
# ---------------------------------------------------------------------------
VIABILITY_SCALE   = 0.5   # Ng-style potential shaping weight
VIABILITY_FLOOR   = 0.2   # minimum dx multiplier even on a doomed route
DOOM_THRESHOLD    = 0.15  # viability below this = "doomed"
DOOM_DROP_MIN     = 0.25  # delta-V drop magnitude that triggers doomed-state penalty
DOOM_PENALTY      = 0.3   # one-shot doomed-state entry penalty
LANDING_BONUS     = 0.2   # bonus when landing on a viability-improving surface
VIABILITY_IMPROVE = 0.10  # min V improvement on landing to grant landing bonus

# ---------------------------------------------------------------------------
# Exploration / breakthrough reward parameters
# ---------------------------------------------------------------------------
NEW_MAX_X_COEF    = 0.05  # bonus per pixel of new episode-max progress
STAGNATION_WINDOW = 120   # steps without new max-x before penalty kicks in
STAGNATION_PENALTY= 0.01  # reward penalty per step while stagnating
STAGNATION_EARLY_STOP = 300  # truncate if stuck for this many steps


def _nt_semantic(nametables, world_tx: int, world_ty: int) -> int:
    """Tile semantic at nametable position. Returns TILE_SOLID for out-of-bounds."""
    if world_tx < 0 or not (0 <= world_ty <= 29):
        return TILE_SOLID
    nt_x    = world_tx % 64
    nt_idx  = nt_x // 32
    local_x = nt_x % 32
    return int(_TILE_SEMANTICS[nametables[nt_idx * 1024 + world_ty * 32 + local_x]])


# ---------------------------------------------------------------------------
# SMBEnv
# ---------------------------------------------------------------------------
class SMBEnv(gym.Env):
    """
    Gymnasium environment for SMB1 using structured RAM/PPU observations.
    Requires libmicrones_rl.{so,dylib} from the build-host target.
    """

    metadata = {"render_modes": ["rgb_array"], "render_fps": 60}

    FRAME_SKIP = 3
    MAX_STEPS  = 20_000

    # Stagnation: if max progress over this many steps < threshold, truncate.
    STAGNATION_WINDOW = 100   # steps (~5 seconds at frame_skip=3)
    STAGNATION_PIXELS = 64    # must advance 64px within the window

    def __init__(
        self,
        rom_path: str,
        lib_path: str | None = None,
        render_mode: str | None = None,
        frame_skip: int = FRAME_SKIP,
        levels: list[str] | None = None,
        level_weights: list[float] | None = None,
    ):
        super().__init__()

        self._rom_path    = rom_path
        self._frame_skip  = frame_skip
        self._render_mode = render_mode

        # Curriculum level list: ["1-1", "1-2", ...]; defaults to 1-1 only.
        self._levels = levels if levels else ["1-1"]
        if level_weights is not None:
            w = np.array(level_weights, dtype=np.float64)
            self._level_weights = w / w.sum()
        else:
            self._level_weights = None  # uniform

        # Load shared library and create emulator instance.
        self._lib = NesLib(lib_path)
        self._h   = self._lib.create()
        self._lib.load_rom(self._h, rom_path)

        # Zero-copy views — valid until next step/reset call.
        self._ram        = self._lib.ram_view(self._h)
        self._nametables = self._lib.nametables_view(self._h)
        self._oam        = self._lib.oam_view(self._h)

        # Runtime state
        self._step_count:   int   = 0
        self._prev_world_x: int   = 0
        self._world_x_history: deque[int] = deque(maxlen=self.STAGNATION_WINDOW)
        self._prev_enemy_screen_x = np.zeros(5, dtype=np.int32)
        self._prev_enemy_screen_y = np.zeros(5, dtype=np.int32)
        self._step_frames: list[np.ndarray] = []   # per-emulator-frame capture
        self._action_history: deque[int] = deque([0, 0, 0, 0], maxlen=4)
        self._prev_reward_x: int = 0

        # Trajectory memory (rolling history for recent-motion features)
        self._traj_x:      deque[int]  = deque([0] * TRAJ_HIST_LEN, maxlen=TRAJ_HIST_LEN)
        self._traj_sy:     deque[int]  = deque([128] * TRAJ_HIST_LEN, maxlen=TRAJ_HIST_LEN)
        self._traj_ground: deque[bool] = deque([True] * TRAJ_HIST_LEN, maxlen=TRAJ_HIST_LEN)
        self._time_since_ground:  int  = 0
        self._time_since_jump:    int  = 30
        self._time_since_dir_chg: int  = 30
        self._prev_facing:        int  = 1   # 1 = facing right
        self._prev_viability_score: float = 0.5   # neutral start
        self._episode_max_x:   int = 0
        self._last_progress_step: int = 0

        self.action_space = spaces.Discrete(N_ACTIONS)
        self.observation_space = self._build_obs_space()

    # ------------------------------------------------------------------
    # Gymnasium API
    # ------------------------------------------------------------------
    def reset(self, *, seed=None, options=None) -> tuple[dict, dict]:
        super().reset(seed=seed)

        # options["level"] overrides curriculum sampling.
        if options and "level" in options:
            level = options["level"]
        else:
            rng = self.np_random if self.np_random is not None else np.random.default_rng()
            level = rng.choice(self._levels, p=self._level_weights)

        self._warm_reset(level)

        self._step_count   = 0
        self._prev_world_x = self._read_world_x()
        self._prev_reward_x = self._prev_world_x
        self._world_x_history.clear()
        self._world_x_history.append(self._prev_world_x)
        self._prev_enemy_screen_x[:] = 0
        self._action_history.extend([0, 0, 0, 0])

        # Initialise trajectory history to current position so first-step deltas are zero.
        wx0 = self._read_world_x()
        sy0 = int(self._ram[0x00CE])
        for _ in range(TRAJ_HIST_LEN):
            self._traj_x.append(wx0)
            self._traj_sy.append(sy0)
            self._traj_ground.append(True)
        self._time_since_ground  = 0
        self._time_since_jump    = 30
        self._time_since_dir_chg = 30
        self._prev_facing        = 1
        self._prev_viability_score = 0.5
        self._episode_max_x    = self._prev_world_x
        self._last_progress_step = 0

        obs = self._get_obs()
        return obs, {}

    def step(self, action: int) -> tuple[dict, float, bool, bool, dict]:
        # Execute the multi-segment motor primitive for this action.
        # When recording, capture one frame per emulator frame (not per step).
        self._step_frames.clear()
        _recording = self._render_mode == "rgb_array"
        for buttons, n_frames in _ACTION_SEQUENCES[action]:
            self._lib.set_buttons(self._h, buttons)
            for _ in range(n_frames):
                self._lib.step(self._h)
                if _recording:
                    fb = self._lib.framebuffer_view(self._h)
                    self._step_frames.append(NES_PALETTE_RGB[fb & 0x3F].copy())
        self._lib.set_buttons(self._h, 0)

        self._action_history.append(int(action))
        self._step_count += 1

        obs = self._get_obs()

        world_x = self._read_world_x()
        self._world_x_history.append(world_x)

        reward = self._compute_reward(obs, action, world_x)
        self._prev_world_x = world_x

        dead     = bool(obs["game_flags"][0])
        complete = bool(obs["game_flags"][1])
        terminated = dead or complete

        stagnating = (
            len(self._world_x_history) >= self.STAGNATION_WINDOW
            and (max(self._world_x_history) - min(self._world_x_history)) < self.STAGNATION_PIXELS
        )
        steps_since_progress = self._step_count - self._last_progress_step
        truncated = (
            (self._step_count >= self.MAX_STEPS)
            or stagnating
            or (steps_since_progress >= STAGNATION_EARLY_STOP)
        )

        info: dict[str, Any] = {
            "world_x":    world_x,
            "frame":      self._lib.frame_count(self._h),
            "stagnating": stagnating,
        }
        return obs, float(reward), terminated, truncated, info

    def render(self):
        if self._render_mode != "rgb_array":
            return None
        fb = self._lib.framebuffer_view(self._h)
        return NES_PALETTE_RGB[fb & 0x3F]  # (240, 256, 3) uint8

    def pop_step_frames(self) -> list[np.ndarray]:
        """Return and clear all emulator frames captured during the last step().
        Only populated when render_mode='rgb_array'. Use this instead of render()
        when recording video so macro-action steps produce the correct frame count.
        """
        frames = self._step_frames.copy()
        self._step_frames.clear()
        return frames

    def close(self):
        if self._h:
            self._lib.destroy(self._h)
            self._h = 0

    # ------------------------------------------------------------------
    # Observation space
    # ------------------------------------------------------------------
    @staticmethod
    def _build_obs_space() -> spaces.Dict:
        f32 = np.float32
        return spaces.Dict({
            # Continuous player physics + power state (12 values)
            "player_state":   spaces.Box(-1.0, 1.0, (12,), f32),
            # World/level context (6 values)
            "level_context":  spaces.Box( 0.0, 1.0,  (6,), f32),
            # Binary game state flags: [dead, complete, in_pipe, in_transition]
            "game_flags":     spaces.Box( 0.0, 1.0,  (4,), f32),
            # Scalar derived signals
            "player_speed":       spaces.Box(0.0, 1.0, (1,), f32),
            "jump_phase":         spaces.Box(0.0, 1.0, (1,), f32),
            "distance_to_ground": spaces.Box(0.0, 1.0, (1,), f32),
            # Lookahead: 5 columns ahead, one value per column
            "gap_ahead":      spaces.Box(0.0, 1.0, (LOOKAHEAD_N,), f32),
            "obstacle_ahead": spaces.Box(0.0, 1.0, (LOOKAHEAD_N,), f32),
            "enemy_threat":   spaces.Box(0.0, 1.0, (LOOKAHEAD_N,), f32),
            # Distance-to-next signals (normalized to [0,1])
            "dist_to_next_enemy": spaces.Box(0.0, 1.0, (1,), f32),
            "dist_to_next_gap":   spaces.Box(0.0, 1.0, (1,), f32),
            "dist_to_next_pipe":  spaces.Box(0.0, 1.0, (1,), f32),
            # Last 4 actions (normalized)
            "action_history": spaces.Box(0.0, 1.0, (4,), f32),
            # Tile semantic grid (13×17), each value in [0,1]
            "tile_grid":  spaces.Box(0.0, 1.0, (GRID_ROWS, GRID_COLS), f32),
            # 5 enemy slots × 8 features each (added vy column)
            "enemies":    spaces.Box(-1.0, 1.0, (5, 8), f32),
            # Compact timing + dynamic safety context (10 values, [-1,1]).
            # Nearest forward enemy dynamics + landing zone + collision heuristics.
            # See _compute_dynamic_context() for feature index map.
            "dynamic_context": spaces.Box(-1.0, 1.0, (10,), f32),
            # Interactive objects: powerup (4) + fireball×2 (3 each) = 10
            "objects":    spaces.Box(-1.0, 1.0, (10,), f32),
            # Generic platform topology (18 values, all [0,1]).
            # Encodes reachable landings above/below, runway, route branching.
            # See _compute_platform_topology() for feature index map.
            "platform_topology":  spaces.Box(0.0, 1.0, (18,), f32),
            # Recent trajectory memory (14 values, dx/dy in [-1,1], rest [0,1]).
            # Encodes recent motion, time-since events, support state changes.
            # See _compute_trajectory_memory() for feature index map.
            "trajectory_memory":  spaces.Box(-1.0, 1.0, (14,), f32),
            # Route viability summary (11 values, all [0,1]).
            # Viability score + dead-end signals + surface options + doomed flag.
            # See _compute_route_viability_obs() for feature index map.
            "route_viability":    spaces.Box(0.0, 1.0, (11,), f32),
        })

    # ------------------------------------------------------------------
    # Observation extraction
    # ------------------------------------------------------------------
    def _get_obs(self) -> dict:
        ram = self._ram
        nt  = self._nametables

        # --- Player position ---
        mario_sx   = int(ram[0x0086])  # screen X pixel
        mario_sy   = int(ram[0x00CE])  # screen Y pixel
        page       = int(ram[0x006D])
        world_x    = page * 256 + mario_sx

        scroll_lo  = int(ram[0x071A])
        scroll_hi  = int(ram[0x071B])
        scroll_px  = scroll_hi * 256 + scroll_lo

        # --- Velocity (signed bytes) ---
        vx_raw = np.int8(ram[0x0057])
        vy_raw = np.int8(ram[0x009F])

        speed       = abs(float(vx_raw)) / 40.0

        # Jump phase: 0=grounded, 1=rising, 2=apex, 3=falling
        on_ground = int(ram[0x001C] != 0)
        if on_ground:
            jump_phase = 0
        elif vy_raw < -2:
            jump_phase = 1
        elif abs(vy_raw) <= 2:
            jump_phase = 2
        else:
            jump_phase = 3

        # --- Tile grid ---
        mario_tx = (scroll_px + mario_sx) // 8
        mario_ty = mario_sy // 8
        grid = self._build_tile_grid(mario_tx, mario_ty)

        # --- Distance to ground (from Mario's feet downward) ---
        dist_to_ground = self._scan_down_for_solid(grid, MARIO_ROW + 1)

        # --- Lookahead signals ---
        gap_ahead, obstacle_ahead = self._compute_gap_obstacle(grid)

        # --- Enemies ---
        enemies_arr, enemy_mask, enemy_sx, enemy_sy = self._build_enemies(ram, mario_sx, mario_sy)
        # Frame-delta vx and vy per slot; ignore wrap glitches (|delta| >= 100).
        for i in range(5):
            if enemy_mask[i]:
                dx = int(enemy_sx[i]) - int(self._prev_enemy_screen_x[i])
                if abs(dx) < 100:
                    enemies_arr[i, 3] = float(np.clip(dx / 8.0, -1.0, 1.0))
                dy = int(enemy_sy[i]) - int(self._prev_enemy_screen_y[i])
                if abs(dy) < 100:
                    enemies_arr[i, 4] = float(np.clip(dy / 8.0, -1.0, 1.0))
        self._prev_enemy_screen_x[:] = enemy_sx
        self._prev_enemy_screen_y[:] = enemy_sy

        # --- Enemy threat per lookahead column ---
        enemy_threat = self._compute_enemy_threat(enemies_arr, enemy_mask, mario_sx)

        # --- Distance-to-next signals (tile grid scan) ---
        dist_enemy  = self._dist_to_next_enemy(enemies_arr, enemy_mask, mario_sx)
        dist_gap    = self._dist_ahead_for_category(grid, TILE_EMPTY, check_below=True)
        dist_pipe   = self._dist_ahead_for_category_tile(grid, TILE_PIPE)

        # --- Power state ---
        power_state         = int(ram[0x0756]) & 0x03   # 0=small,1=super,2=fire
        invincibility_timer = int(ram[0x079F])
        in_transition       = int(ram[0x0773] != 0)

        # --- Time ---
        time_val = int(ram[0x07F8]) * 100 + int(ram[0x07F9]) * 10 + int(ram[0x07FA])

        # --- Game flags ---
        player_dead    = int(int(ram[0x000E]) == 0x0B)
        level_complete = int(int(ram[0x001D]) == 0x03)
        in_pipe        = int(ram[0x0001] != 0)

        # --- Objects (powerup + 2 fireballs) ---
        pu_type  = int(ram[0x0014])
        pu_rx    = float(np.clip((int(ram[0x0092]) - mario_sx) / 128.0, -1, 1))
        pu_ry    = float(np.clip((int(ram[0x00D4]) - mario_sy) / 120.0, -1, 1))
        fb0_act  = float(ram[0x001A] != 0)
        fb0_rx   = float(np.clip((int(ram[0x008B]) - mario_sx) / 128.0, -1, 1))
        fb0_ry   = float(np.clip((int(ram[0x00D5]) - mario_sy) / 120.0, -1, 1))
        fb1_act  = float(ram[0x001B] != 0)
        fb1_rx   = float(np.clip((int(ram[0x008C]) - mario_sx) / 128.0, -1, 1))
        fb1_ry   = float(np.clip((int(ram[0x00D6]) - mario_sy) / 120.0, -1, 1))

        # --- Pack observation ---
        player_state = np.array([
            float(np.clip(world_x  / 8192.0, 0, 1)),
            float(np.clip(mario_sx / 255.0,  0, 1)),
            float(np.clip(mario_sy / 255.0,  0, 1)),
            float(np.clip(vx_raw   / 40.0,  -1, 1)),
            float(np.clip(vy_raw   / 80.0,  -1, 1)),
            float(on_ground),
            float(int(ram[0x0033]) == 0),   # facing right: 0x0033=0 → right
            float(ram[0x00F1] != 0),         # crouching
            float((int(ram[0x0756]) & 0x03) / 2.0),  # animation cycle proxy
            float(power_state / 2.0),
            float(invincibility_timer / 255.0),
            float(in_transition),
        ], dtype=np.float32)

        level_context = np.array([
            float(int(ram[0x075F]) / 7.0),
            float(int(ram[0x075C]) / 3.0),
            float(int(ram[0x0760]) / 3.0),
            float(np.clip(scroll_px / 8192.0, 0, 1)),
            float(np.clip(time_val  / 400.0,  0, 1)),
            float(int(ram[0x071B])  / 31.0),
        ], dtype=np.float32)

        game_flags = np.array([
            float(player_dead),
            float(level_complete),
            float(in_pipe),
            float(in_transition),
        ], dtype=np.float32)

        action_history = np.array(
            [a / float(N_ACTIONS - 1) for a in self._action_history],
            dtype=np.float32,
        )

        objects = np.array([
            float(pu_type != 0),
            float(pu_type / 3.0),
            pu_rx, pu_ry,
            fb0_act, fb0_rx, fb0_ry,
            fb1_act, fb1_rx, fb1_ry,
        ], dtype=np.float32)

        # --- Update trajectory state (must happen before computing traj features) ---
        facing_right = int(int(ram[0x0033]) == 0)
        if on_ground:
            self._time_since_ground = 0
        else:
            self._time_since_ground = min(self._time_since_ground + 1, 60)
        last_action = self._action_history[-1]
        if last_action in _JUMP_ACTIONS:
            self._time_since_jump = 0
        else:
            self._time_since_jump = min(self._time_since_jump + 1, 60)
        if facing_right != self._prev_facing:
            self._time_since_dir_chg = 0
        else:
            self._time_since_dir_chg = min(self._time_since_dir_chg + 1, 60)
        self._prev_facing = facing_right
        self._traj_x.append(world_x)
        self._traj_sy.append(mario_sy)
        self._traj_ground.append(bool(on_ground))

        # --- New derived features ---
        plat_topo    = self._compute_platform_topology(mario_tx, mario_ty)
        traj_mem     = self._compute_trajectory_memory()
        route_viab   = self._compute_route_viability_obs(plat_topo)
        dyn_ctx      = self._compute_dynamic_context(
            enemies_arr, enemy_mask, mario_sx, mario_sy, int(vx_raw), plat_topo
        )

        return {
            "player_state":       player_state,
            "level_context":      level_context,
            "game_flags":         game_flags,
            "player_speed":       np.array([float(np.clip(speed, 0, 1))],         dtype=np.float32),
            "jump_phase":         np.array([float(jump_phase / 3.0)],             dtype=np.float32),
            "distance_to_ground": np.array([float(dist_to_ground)],               dtype=np.float32),
            "gap_ahead":          gap_ahead,
            "obstacle_ahead":     obstacle_ahead,
            "enemy_threat":       enemy_threat,
            "dist_to_next_enemy": np.array([float(dist_enemy)],                   dtype=np.float32),
            "dist_to_next_gap":   np.array([float(dist_gap)],                     dtype=np.float32),
            "dist_to_next_pipe":  np.array([float(dist_pipe)],                    dtype=np.float32),
            "action_history":     action_history,
            "tile_grid":          grid.astype(np.float32) / 7.0,
            "enemies":            enemies_arr,
            "objects":            objects,
            "platform_topology":  plat_topo,
            "trajectory_memory":  traj_mem,
            "route_viability":    route_viab,
            "dynamic_context":    dyn_ctx,
        }

    # ------------------------------------------------------------------
    # Tile grid construction
    # ------------------------------------------------------------------
    def _build_tile_grid(self, mario_tx: int, mario_ty: int) -> np.ndarray:
        """Return (GRID_ROWS, GRID_COLS) uint8 grid of tile semantics."""
        grid = np.zeros((GRID_ROWS, GRID_COLS), dtype=np.uint8)
        nt   = self._nametables
        for row in range(GRID_ROWS):
            world_ty = mario_ty - MARIO_ROW + row
            if world_ty < 0 or world_ty >= 30:
                grid[row, :] = TILE_SOLID  # treat out-of-bounds as solid
                continue
            for col in range(GRID_COLS):
                world_tx = mario_tx - MARIO_COL + col
                if world_tx < 0:
                    grid[row, col] = TILE_SOLID
                    continue
                nt_x      = world_tx % 64
                nt_index  = nt_x // 32
                local_x   = nt_x % 32
                offset    = nt_index * 1024 + world_ty * 32 + local_x
                tile_id   = int(nt[offset])
                grid[row, col] = _TILE_SEMANTICS[tile_id]
        return grid

    # ------------------------------------------------------------------
    # Derived signals
    # ------------------------------------------------------------------
    def _scan_down_for_solid(self, grid: np.ndarray, start_row: int) -> float:
        """Fraction of rows below start_row before hitting solid tile, [0,1]."""
        max_dist = GRID_ROWS - start_row
        if max_dist <= 0:
            return 0.0
        mario_col = MARIO_COL
        for r in range(start_row, GRID_ROWS):
            if _SEMANTIC_IS_SOLID[int(grid[r, mario_col])]:
                return float(r - start_row) / float(max_dist)
        return 1.0

    def _compute_gap_obstacle(self, grid: np.ndarray):
        """
        gap_ahead[i]: no solid tile in the column MARIO_COL+1+i at or below ground.
        obstacle_ahead[i]: solid tile at Mario's body rows in column MARIO_COL+1+i.
        """
        gap      = np.zeros(LOOKAHEAD_N, dtype=np.float32)
        obstacle = np.zeros(LOOKAHEAD_N, dtype=np.float32)
        ground_row = MARIO_ROW + 2  # row just below Mario's feet
        body_rows  = [MARIO_ROW - 1, MARIO_ROW]  # Mario occupies these rows

        for i in range(LOOKAHEAD_N):
            col = MARIO_COL + 1 + i
            if col >= GRID_COLS:
                break
            # Gap: no solid tile from ground_row to bottom of grid
            has_ground = any(
                _SEMANTIC_IS_SOLID[int(grid[r, col])]
                for r in range(ground_row, GRID_ROWS)
            )
            gap[i] = 0.0 if has_ground else 1.0
            # Obstacle at body height
            for r in body_rows:
                if 0 <= r < GRID_ROWS and _SEMANTIC_IS_SOLID[int(grid[r, col])]:
                    obstacle[i] = 1.0
                    break
        return gap, obstacle

    def _compute_enemy_threat(
        self, enemies: np.ndarray, mask: list[bool], mario_sx: int
    ) -> np.ndarray:
        """enemy_threat[i]: is an enemy within column MARIO_COL+1+i?"""
        threat = np.zeros(LOOKAHEAD_N, dtype=np.float32)
        # enemy rel_x is normalised to screen width; convert to tiles
        for slot in range(5):
            if not mask[slot]:
                continue
            rel_x_pixels = enemies[slot, 1] * 128.0  # undo /128 normalisation
            tile_offset  = rel_x_pixels / 8.0
            for i in range(LOOKAHEAD_N):
                lo, hi = float(i), float(i + 2)  # 2-tile window per slot
                if lo <= tile_offset < hi:
                    threat[i] = 1.0
        return threat

    def _dist_ahead_for_category(
        self, grid: np.ndarray, category: int, check_below: bool = False
    ) -> float:
        """Normalised tile distance ahead until hitting a column with given category."""
        for i in range(1, _DIST_MAX_TILES + 1):
            col = MARIO_COL + i
            if col >= GRID_COLS:
                break
            if check_below:
                # "gap": entire column from ground down is empty
                ground_row = MARIO_ROW + 2
                col_has_solid = any(
                    _SEMANTIC_IS_SOLID[int(grid[r, col])]
                    for r in range(ground_row, GRID_ROWS)
                )
                if not col_has_solid:
                    return float(i) / float(_DIST_MAX_TILES)
            else:
                for r in range(GRID_ROWS):
                    if _TILE_SEMANTICS[int(grid[r, col])] == category:
                        return float(i) / float(_DIST_MAX_TILES)
        return 1.0

    def _dist_ahead_for_category_tile(self, grid: np.ndarray, category: int) -> float:
        """Normalised tile distance to next tile of given semantic category."""
        for i in range(1, _DIST_MAX_TILES + 1):
            col = MARIO_COL + i
            if col >= GRID_COLS:
                break
            for r in range(GRID_ROWS):
                if int(grid[r, col]) == category:
                    return float(i) / float(_DIST_MAX_TILES)
        return 1.0

    def _dist_to_next_enemy(
        self, enemies: np.ndarray, mask: list[bool], mario_sx: int
    ) -> float:
        """Normalised pixel distance to closest enemy ahead of Mario."""
        min_dist = float(_DIST_MAX_TILES)
        for slot in range(5):
            if not mask[slot]:
                continue
            rel_x_pixels = enemies[slot, 1] * 128.0
            if 0 < rel_x_pixels < _DIST_MAX_TILES * 8:
                tile_dist = rel_x_pixels / 8.0
                min_dist  = min(min_dist, tile_dist)
        return float(min_dist) / float(_DIST_MAX_TILES)

    # ------------------------------------------------------------------
    # Platform topology helpers
    # ------------------------------------------------------------------
    def _find_col_surface_row(self, world_tx: int, mario_ty: int) -> int | None:
        """
        Find the world_ty of the topmost solid surface in column world_tx.

        A "surface" is a solid tile whose tile above is non-solid (or at scan top).
        Scans from (mario_ty - MAX_JUMP_HEIGHT) to (mario_ty + MAX_FALL_HEIGHT + 2).
        Returns None if no surface found in the window.
        """
        nt       = self._nametables
        scan_min = max(0, mario_ty - MAX_JUMP_HEIGHT)
        scan_max = min(29, mario_ty + MAX_FALL_HEIGHT + 2)
        for ty in range(scan_min, scan_max + 1):
            if _SEMANTIC_IS_SOLID[_nt_semantic(nt, world_tx, ty)]:
                above = _nt_semantic(nt, world_tx, ty - 1) if ty > 0 else TILE_EMPTY
                if not _SEMANTIC_IS_SOLID[above]:
                    return ty
        return None

    def _scan_surfaces_ahead(self, mario_tx: int, mario_ty: int) -> list[dict]:
        """
        Scan SCAN_AHEAD_COLS columns ahead and return distinct support surface spans.

        Each span dict:
            dx    — col offset from mario_tx where span begins (1-based)
            dy    — world_ty_of_surface - (mario_ty + 2); negative = above foot level
            width — number of consecutive columns at the same surface row
        """
        ground_ty = mario_ty + 2
        surfaces: list[dict] = []
        cur: dict | None     = None

        for k in range(1, SCAN_AHEAD_COLS + 1):
            surf_ty = self._find_col_surface_row(mario_tx + k, mario_ty)
            if surf_ty is None:
                if cur is not None:
                    surfaces.append(cur)
                    cur = None
            else:
                if cur is None or surf_ty != cur['_ty']:
                    if cur is not None:
                        surfaces.append(cur)
                    cur = {'dx': k, '_ty': surf_ty, 'width': 1}
                else:
                    cur['width'] += 1
            if len(surfaces) >= 8:      # cap to avoid degenerate levels
                break

        if cur is not None and len(surfaces) < 8:
            surfaces.append(cur)

        for s in surfaces:
            s['dy'] = s['_ty'] - ground_ty
        return surfaces

    def _compute_platform_topology(self, mario_tx: int, mario_ty: int) -> np.ndarray:
        """
        18-element float32 generic platform topology vector. All values in [0,1].

        Index  Feature
          0    reachable_lower_exists         1 if any walkable/fallable surface ahead
          1    nearest_lower_dx               tiles ahead / SCAN_AHEAD_COLS
          2    nearest_lower_dy               tiles below foot / MAX_FALL_HEIGHT
          3    nearest_lower_width            span tiles / MAX_SPAN_WIDTH
          4    reachable_upper_exists         1 if any jumpable surface above ahead
          5    nearest_upper_dx               tiles ahead / SCAN_AHEAD_COLS
          6    nearest_upper_dy               tiles above foot / MAX_JUMP_HEIGHT
          7    nearest_upper_width            span tiles / MAX_SPAN_WIDTH
          8    current_runway                 continuous same-level support / SCAN_AHEAD_COLS
          9    support_rise_dist              tiles to first upper surface / SCAN_AHEAD_COLS
         10    upper_route_available          1 if upper surface exists
         11    must_jump_soon                 1 if runway < 8 AND upper route exists
         12    jump_window_dist               tiles until latest viable jump / SCAN_AHEAD_COLS
         13    dead_end_dist                  tiles to dead end (no path) / SCAN_AHEAD_COLS
         14    n_surfaces_lower               count of distinct lower spans / 4
         15    n_surfaces_upper               count of distinct upper spans / 4
         16    lowest_surface_dy              deepest lower surface / MAX_FALL_HEIGHT
         17    highest_surface_dy             highest upper surface / MAX_JUMP_HEIGHT
        """
        ground_ty = mario_ty + 2
        surfaces  = self._scan_surfaces_ahead(mario_tx, mario_ty)

        # dy >= -1: walkable same-level or below (small rise is still walkable)
        # -MAX_JUMP_HEIGHT <= dy <= -2: above foot level, reachable by jump
        lower = [s for s in surfaces if s['dy'] >= -1]
        upper = [s for s in surfaces if -MAX_JUMP_HEIGHT <= s['dy'] <= -2]

        # --- Nearest lower landing ---
        if lower:
            nl = lower[0]
            lower_exists = 1.0
            lower_dx     = float(nl['dx'])                           / SCAN_AHEAD_COLS
            lower_dy     = float(max(nl['dy'], 0))                   / MAX_FALL_HEIGHT
            lower_width  = float(min(nl['width'], MAX_SPAN_WIDTH))   / MAX_SPAN_WIDTH
        else:
            lower_exists, lower_dx, lower_dy, lower_width = 0.0, 1.0, 1.0, 0.0

        # --- Nearest upper landing ---
        if upper:
            nu = upper[0]
            upper_exists = 1.0
            upper_dx     = float(nu['dx'])                           / SCAN_AHEAD_COLS
            upper_dy     = float(min(-nu['dy'], MAX_JUMP_HEIGHT))    / MAX_JUMP_HEIGHT
            upper_width  = float(min(nu['width'], MAX_SPAN_WIDTH))   / MAX_SPAN_WIDTH
        else:
            upper_exists, upper_dx, upper_dy, upper_width = 0.0, 1.0, 1.0, 0.0

        # --- Current support runway at same level (dy in [-1, 1]) ---
        runway = 0
        for k in range(1, SCAN_AHEAD_COLS + 1):
            surf_ty = self._find_col_surface_row(mario_tx + k, mario_ty)
            if surf_ty is None:
                break
            if -1 <= (surf_ty - ground_ty) <= 1:
                runway = k
            else:
                break
        runway_norm = float(runway) / SCAN_AHEAD_COLS

        # --- Rise distance: tiles to first upper surface ---
        rise_norm = float(upper[0]['dx']) / SCAN_AHEAD_COLS if upper else 1.0

        # --- Upper route signals ---
        upper_available = float(bool(upper))
        if upper:
            # Last column where initiating a jump can still reach the upper surface.
            # Conservative: must jump before current support ends OR before upper surface.
            jump_col      = min(runway if runway > 0 else SCAN_AHEAD_COLS, upper[0]['dx'])
            jump_win_norm = float(jump_col) / SCAN_AHEAD_COLS
            must_jump     = 1.0 if runway < 8 else 0.0
        else:
            jump_win_norm = 1.0
            must_jump     = 0.0

        # --- Dead end: current support ends with no alternate (upper or lower) ---
        if runway < SCAN_AHEAD_COLS and not upper and not lower:
            dead_end_norm = float(max(runway, 1)) / SCAN_AHEAD_COLS
        elif runway < SCAN_AHEAD_COLS and not upper:
            # Lower surface exists but no upper: mild dead-end signal
            dead_end_norm = float(max(runway, 1)) / SCAN_AHEAD_COLS
        else:
            dead_end_norm = 1.0

        # --- Counts and extremes ---
        n_lower = float(min(len(lower), 4)) / 4.0
        n_upper = float(min(len(upper), 4)) / 4.0
        lowest_dy_norm  = float(np.clip(
            max((s['dy'] for s in lower), default=0), 0, MAX_FALL_HEIGHT
        )) / MAX_FALL_HEIGHT
        highest_dy_norm = float(np.clip(
            max((-s['dy'] for s in upper), default=0), 0, MAX_JUMP_HEIGHT
        )) / MAX_JUMP_HEIGHT

        return np.array([
            lower_exists, lower_dx, lower_dy, lower_width,   # 0-3
            upper_exists, upper_dx, upper_dy, upper_width,   # 4-7
            runway_norm,  rise_norm,                          # 8-9
            upper_available, must_jump,                       # 10-11
            jump_win_norm,   dead_end_norm,                   # 12-13
            n_lower,         n_upper,                         # 14-15
            lowest_dy_norm,  highest_dy_norm,                 # 16-17
        ], dtype=np.float32)

    # ------------------------------------------------------------------
    # Route viability helpers
    # ------------------------------------------------------------------
    def _viability_from_topo(self, topo: np.ndarray) -> float:
        """
        Scalar route viability in [0, 1] derived from a platform_topology vector.

        Components:
          runway_norm  (topo[8])  — continuous support ahead (up to 0.40)
          upper route  (topo[10]) — jump option available    (0.35)
          lower option (topo[0])  — fall landing available   (0.15)
          dead_end_d   (topo[13]) — distance from dead end   (0.10)

        Returns ~0 when a very close dead-end exists with no alternatives.
        """
        runway  = float(topo[8])   # runway_norm
        upper   = float(topo[10])  # upper_route_available (0 or 1)
        lower   = float(topo[0])   # reachable_lower_exists (0 or 1)
        dead_d  = float(topo[13])  # dead_end_dist (1 = far, 0 = right here)

        # Immediately doomed: dead end within 2 tiles and no jump/fall option
        if dead_d < (2.0 / SCAN_AHEAD_COLS) and upper < 0.5 and lower < 0.5:
            return 0.0

        score  = 0.40 * min(runway * 2.0, 1.0)  # runway up to 0.40
        score += 0.35 * upper                     # jump route available
        score += 0.15 * lower                     # fall landing available
        score += 0.10 * dead_d                    # distance from dead end
        return float(np.clip(score, 0.0, 1.0))

    def _compute_route_viability_obs(self, topo: np.ndarray) -> np.ndarray:
        """
        11-element route viability observation vector. All values in [0, 1].

        Index  Feature
          0    viability_score         scalar V derived from local geometry
          1    current_route_viable    1 if V > 0.4
          2    dead_end_distance       tiles to dead end / SCAN_AHEAD_COLS
          3    lower_reachable_exists  any walkable/fallable surface ahead
          4    lower_nearest_dx        nearest lower surface dx / SCAN_AHEAD_COLS
          5    lower_nearest_dy        nearest lower surface depth / MAX_FALL_HEIGHT
          6    upper_reachable_exists  any jumpable surface above ahead
          7    upper_nearest_dx        nearest upper surface dx / SCAN_AHEAD_COLS
          8    upper_nearest_dy        nearest upper surface height / MAX_JUMP_HEIGHT
          9    doomed                  1 if V < DOOM_THRESHOLD
         10    runway_norm             continuous same-level support ahead
        """
        v = self._viability_from_topo(topo)
        return np.array([
            v,
            float(v > 0.4),
            float(topo[13]),   # dead_end_dist
            float(topo[0]),    # lower_exists
            float(topo[1]),    # lower_dx
            float(topo[2]),    # lower_dy
            float(topo[4]),    # upper_exists
            float(topo[5]),    # upper_dx
            float(topo[6]),    # upper_dy
            float(v < DOOM_THRESHOLD),
            float(topo[8]),    # runway_norm
        ], dtype=np.float32)

    # ------------------------------------------------------------------
    # Trajectory memory helper
    # ------------------------------------------------------------------
    def _compute_trajectory_memory(self) -> np.ndarray:
        """
        14-element float32 recent-trajectory feature vector.

        Index  Feature                     Range
          0    recent_dx_1                 [-1,1]  1-step x delta / 8
          1    recent_dx_2                 [-1,1]  2-step x delta / 16
          2    recent_dx_4                 [-1,1]  4-step x delta / 32
          3    recent_dy_1                 [-1,1]  1-step y delta / 8  (pos=down)
          4    recent_dy_2                 [-1,1]  2-step y delta / 16
          5    recent_dy_4                 [-1,1]  4-step y delta / 32
          6    time_since_on_ground        [0,1]   steps / 30
          7    time_since_jump_start       [0,1]   steps / 30
          8    time_since_dir_change       [0,1]   steps / 30
          9    recently_moved_backward     [0,1]   bool: net-backward over last 4 steps
         10    recent_backward_distance    [0,1]   total backward px in last 8 / 64
         11    recent_forward_distance     [0,1]   total forward px in last 8 / 64
         12    recent_support_loss         [0,1]   bool: ground→air in last 4 steps
         13    recent_landing_event        [0,1]   bool: air→ground in last 2 steps
        """
        tx = list(self._traj_x)       # world_x history (up to TRAJ_HIST_LEN)
        ty = list(self._traj_sy)      # screen_y history
        tg = list(self._traj_ground)  # on_ground history
        n  = len(tx)

        def _dx(k: int) -> float:
            if n >= k + 1:
                return float(np.clip((tx[-1] - tx[-1 - k]) / (8.0 * k), -1.0, 1.0))
            return 0.0

        def _dy(k: int) -> float:
            if n >= k + 1:
                return float(np.clip((ty[-1] - ty[-1 - k]) / (8.0 * k), -1.0, 1.0))
            return 0.0

        # Time-since features
        t_ground = float(np.clip(self._time_since_ground  / 30.0, 0.0, 1.0))
        t_jump   = float(np.clip(self._time_since_jump    / 30.0, 0.0, 1.0))
        t_dir    = float(np.clip(self._time_since_dir_chg / 30.0, 0.0, 1.0))

        # Net movement over last 4 steps
        moved_back = 0.0
        if n >= 5 and (tx[-1] - tx[-5]) < -2:
            moved_back = 1.0

        # Sum of backward / forward pixel movement over last 8 steps
        back_total = 0.0
        fwd_total  = 0.0
        for i in range(max(0, n - 8), n - 1):
            d = tx[i + 1] - tx[i]
            if d < 0:
                back_total += -d
            else:
                fwd_total  += d
        back_norm = float(np.clip(back_total / 64.0, 0.0, 1.0))
        fwd_norm  = float(np.clip(fwd_total  / 64.0, 0.0, 1.0))

        # Support loss: ground → not-ground in last 4 steps
        support_loss = 0.0
        for i in range(max(0, n - 4), n - 1):
            if tg[i] and not tg[i + 1]:
                support_loss = 1.0
                break

        # Landing event: not-ground → ground in last 2 steps
        landing = 0.0
        for i in range(max(0, n - 2), n - 1):
            if not tg[i] and tg[i + 1]:
                landing = 1.0
                break

        return np.array([
            _dx(1), _dx(2), _dx(4),
            _dy(1), _dy(2), _dy(4),
            t_ground, t_jump, t_dir,
            moved_back, back_norm, fwd_norm,
            support_loss, landing,
        ], dtype=np.float32)

    # ------------------------------------------------------------------
    # Enemy extraction
    # ------------------------------------------------------------------
    def _build_enemies(self, ram, mario_sx: int, mario_sy: int):
        """
        Returns:
            arr  : (5, 8) float32 — [type/15, rel_x, rel_y, vx(0), vy(0), dir, state/5, alive]
            mask : list[bool] — True if slot is active
            sx   : (5,) int32 — raw screen X per slot (for vx frame-delta)
            sy   : (5,) int32 — raw screen Y per slot (for vy frame-delta)
        """
        arr  = np.zeros((5, 8), dtype=np.float32)
        mask = [False] * 5
        sx   = np.zeros(5, dtype=np.int32)
        sy   = np.zeros(5, dtype=np.int32)

        for i in range(5):
            etype = int(ram[0x000F + i])
            if etype == 0:
                continue
            mask[i] = True
            ex    = int(ram[0x0087 + i])  # screen X
            ey    = int(ram[0x00CF + i])  # screen Y
            state = int(ram[0x001E + i])
            direction = float((state >> 1) & 1)
            sx[i] = ex
            sy[i] = ey
            arr[i] = [
                float(etype) / 15.0,
                float(np.clip((ex - mario_sx) / 128.0, -1, 1)),
                float(np.clip((ey - mario_sy) / 120.0, -1, 1)),
                0.0,   # vx — filled by caller via frame-delta
                0.0,   # vy — filled by caller via frame-delta
                direction,
                float(state & 0x07) / 5.0,
                1.0,
            ]
        return arr, mask, sx, sy

    # ------------------------------------------------------------------
    # Dynamic context (enemy motion + timing heuristics)
    # ------------------------------------------------------------------
    def _compute_dynamic_context(
        self,
        enemies: np.ndarray,   # (5, 8): [..., rel_x, rel_y, vx, vy, ...]
        mask: list[bool],
        mario_sx: int,
        mario_sy: int,
        vx_raw: int,           # Mario pixel velocity per NES frame (signed)
        topo: np.ndarray,      # platform_topology (18,)
    ) -> np.ndarray:
        """
        10-element compact timing + dynamic safety context. Range [-1, 1].

        Index  Feature
          0    nearest_fwd_enemy_rel_x     nearest ahead enemy rel x (normalised)
          1    nearest_fwd_enemy_rel_y     nearest ahead enemy rel y
          2    nearest_fwd_enemy_vx        enemy x velocity
          3    nearest_fwd_enemy_vy        enemy y velocity
          4    nearest_fwd_enemy_ttx       steps to x-intersection / 30  [0,1]
          5    landing_zone_occupied       enemy on nearest lower landing [0,1]
          6    landing_zone_clear_soon     occupied enemy is moving away  [0,1]
          7    predicted_collision_if_jump enemy in short right-jump path [0,1]
          8    safe_to_wait               no near threats + runway > 0   [0,1]
          9    safe_to_proceed            no near threats + runway > 2t  [0,1]
        """
        # -- Nearest enemy ahead --
        best_rx   = 2.0   # > 1 sentinel = "no enemy found"
        best_slot = -1
        for i in range(5):
            if not mask[i]:
                continue
            rx = float(enemies[i, 1])   # normalised, positive = ahead
            if 0.0 < rx < best_rx:
                best_rx   = rx
                best_slot = i

        if best_slot >= 0:
            e_rx = float(enemies[best_slot, 1])
            e_ry = float(enemies[best_slot, 2])
            e_vx = float(enemies[best_slot, 3])
            e_vy = float(enemies[best_slot, 4])
            # ttx in env-steps: rel_x_pixels / (mario_approach - enemy_vx)
            e_rx_px     = e_rx * 128.0
            # vx_raw is px/frame; enemy vx tracked as px/3-frames (one env step)
            mario_step  = float(vx_raw) * 3.0
            enemy_step  = e_vx * 8.0           # undo /8 normalisation
            approach    = mario_step - enemy_step
            ttx = float(np.clip(e_rx_px / approach, 0.0, 30.0)) / 30.0 if approach > 0.5 else 1.0
        else:
            e_rx, e_ry, e_vx, e_vy, ttx = 0.0, 0.0, 0.0, 0.0, 1.0

        # -- Landing zone occupancy --
        landing_occupied = 0.0
        landing_clear    = 0.0
        if float(topo[0]) > 0.5:   # lower landing exists
            lz_dx_px = float(topo[1]) * SCAN_AHEAD_COLS * 8.0
            lz_dy_px = float(topo[2]) * MAX_FALL_HEIGHT  * 8.0
            lz_sx    = mario_sx + lz_dx_px
            lz_sy    = mario_sy + lz_dy_px
            for i in range(5):
                if not mask[i]:
                    continue
                ex_abs = mario_sx + enemies[i, 1] * 128.0
                ey_abs = mario_sy + enemies[i, 2] * 120.0
                if abs(ex_abs - lz_sx) < 20 and abs(ey_abs - lz_sy) < 20:
                    landing_occupied = 1.0
                    # Moving enemy will likely clear soon
                    if abs(enemies[i, 3] * 8.0) > 0.3:
                        landing_clear = 1.0
                    break

        # -- Collision if jumping right: enemy in short right-jump box --
        jump_collision = 0.0
        for i in range(5):
            if not mask[i]:
                continue
            rx_px = enemies[i, 1] * 128.0
            ry_px = enemies[i, 2] * 120.0
            if 0 < rx_px < 32 and -48 < ry_px < 16:  # 4 tiles wide, 6 tiles tall
                jump_collision = 1.0
                break

        # -- Safety heuristics --
        # "near threat": any enemy within 2 tiles ahead (16px / 128 ≈ 0.125)
        near_threat = any(
            mask[i] and 0.0 < enemies[i, 1] < 0.125
            for i in range(5)
        )
        runway = float(topo[8])
        safe_wait    = float(not near_threat and runway > 0.0)
        safe_proceed = float(not near_threat and runway > 2.0 / SCAN_AHEAD_COLS)

        return np.array([
            e_rx, e_ry, e_vx, e_vy,
            ttx,
            landing_occupied,
            landing_clear,
            jump_collision,
            safe_wait,
            safe_proceed,
        ], dtype=np.float32)

    # ------------------------------------------------------------------
    # Reward
    # ------------------------------------------------------------------
    def _compute_reward(self, obs: dict, action: int, world_x: int) -> float:
        r = 0.0

        # --- Route viability potential (Ng-style shaping, zero-sum over time) ---
        viability_now = self._viability_from_topo(obs["platform_topology"])
        delta_V       = viability_now - self._prev_viability_score
        r += VIABILITY_SCALE * delta_V
        self._prev_viability_score = viability_now

        # --- Dense forward progress, attenuated by route viability ---
        dx = world_x - self._prev_reward_x
        viability_factor = VIABILITY_FLOOR + (1.0 - VIABILITY_FLOOR) * viability_now
        if dx > 0:
            r += float(np.clip(dx / 40.0, 0, 1.5)) * viability_factor
        elif dx < 0:
            r += float(np.clip(dx / 40.0, -0.5, 0))   # backtrack penalty unchanged

        self._prev_reward_x = world_x

        # Alive bonus (tiny, encourages staying alive over stagnating)
        r += 0.002

        # Death penalty
        if obs["game_flags"][0] > 0.5:
            r -= 10.0

        # Level completion bonus + time bonus
        if obs["game_flags"][1] > 0.5:
            time_bonus = float(obs["level_context"][4]) * 5.0  # up to 5.0
            r += 10.0 + time_bonus

        # Unnecessary jump penalty: jumping while on solid ground with no
        # obstacle/gap ahead wastes time (small discouragement)
        if action in _JUMP_ACTIONS:
            on_ground    = obs["player_state"][5] > 0.5
            gap_any      = float(obs["gap_ahead"].max())      > 0.1
            obstacle_any = float(obs["obstacle_ahead"].max()) > 0.1
            if on_ground and not gap_any and not obstacle_any:
                r -= 0.005

        # --- Landing bonus when landing on a viability-improving surface ---
        landing_event = obs["trajectory_memory"][13] > 0.5
        if landing_event and delta_V > VIABILITY_IMPROVE:
            r += LANDING_BONUS

        # --- Doomed-state entry penalty: sharp viability drop into doomed zone ---
        if viability_now < DOOM_THRESHOLD and delta_V < -DOOM_DROP_MIN:
            r -= DOOM_PENALTY

        # --- New-episode-max-x bonus ---
        new_max_delta = world_x - self._episode_max_x
        if new_max_delta > 0:
            r += NEW_MAX_X_COEF * new_max_delta
            self._episode_max_x    = world_x
            self._last_progress_step = self._step_count

        # --- Stagnation penalty ---
        if (self._step_count - self._last_progress_step) > STAGNATION_WINDOW:
            r -= STAGNATION_PENALTY

        return float(np.clip(r, -15.0, 15.0))

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------
    def _read_world_x(self) -> int:
        return int(self._ram[0x006D]) * 256 + int(self._ram[0x0086])

    def set_curriculum(
        self,
        levels: list[str],
        level_weights: list[float] | None = None,
    ) -> None:
        """Hot-swap the curriculum without resetting the env. Called by CurriculumStageCallback."""
        self._levels = levels
        if level_weights is not None:
            w = np.array(level_weights, dtype=np.float64)
            self._level_weights = w / w.sum()
        else:
            self._level_weights = None

    # Level string → (WorldNumber, LevelNumber) — both 0-indexed as stored in RAM.
    # $075F = WorldNumber (0-7), $075C = LevelNumber (0-3)
    _LEVEL_MAP: dict[str, tuple[int, int]] = {
        "1-1": (0, 0), "1-2": (0, 1), "1-3": (0, 2), "1-4": (0, 3),
        "2-1": (1, 0), "2-2": (1, 1), "2-3": (1, 2), "2-4": (1, 3),
        "3-1": (2, 0), "3-2": (2, 1), "3-3": (2, 2), "3-4": (2, 3),
        "4-1": (3, 0), "4-2": (3, 1), "4-3": (3, 2), "4-4": (3, 3),
        "5-1": (4, 0), "5-2": (4, 1), "5-3": (4, 2), "5-4": (4, 3),
        "6-1": (5, 0), "6-2": (5, 1), "6-3": (5, 2), "6-4": (5, 3),
        "7-1": (6, 0), "7-2": (6, 1), "7-3": (6, 2), "7-4": (6, 3),
        "8-1": (7, 0), "8-2": (7, 1), "8-3": (7, 2), "8-4": (7, 3),
    }

    # $0750 = packed area identifier used by LoadAreaPointer to compute the area
    # data pointer.  $0760 = relative area offset within the world's $9CBC section
    # (may wrap as uint8 for worlds 3+).
    #
    # Derived from SMB1 ROM:
    #   abs_idx = WorldAddrOffsets[$CCC7][world*4+level]  (absolute $9CBC index)
    #   $0750   = $9CBC[abs_idx]
    #   $0760   = (abs_idx - $9CB4[WorldNumber]) & 0xFF
    _AREA_REGS: dict[str, tuple[int, int]] = {
        # level : ($0750, $0760)
        "1-1": (0x25, 0x00), "1-2": (0x29, 0x01), "1-3": (0x26, 0x03), "1-4": (0x60, 0x04),
        "2-1": (0x28, 0x00), "2-2": (0x29, 0x01), "2-3": (0x01, 0x02), "2-4": (0x01, 0x02),
        "3-1": (0x27, 0xFE), "3-2": (0x25, 0xF6), "3-3": (0x26, 0xF9), "3-4": (0x29, 0xFC),
        "4-1": (0x62, 0xFB), "4-2": (0x35, 0xFD), "4-3": (0x63, 0xFF), "4-4": (0x22, 0x00),
        "5-1": (0x29, 0xFC), "5-2": (0x41, 0xFD), "5-3": (0x25, 0xED), "5-4": (0x60, 0xF1),
        "6-1": (0x62, 0xF2), "6-2": (0x63, 0xF6), "6-3": (0x41, 0xF9), "6-4": (0x2A, 0xFC),
        "7-1": (0x62, 0xFB), "7-2": (0x2E, 0xFC), "7-3": (0x23, 0xFD), "7-4": (0x25, 0xE5),
        "8-1": (0x29, 0xE6), "8-2": (0x20, 0xEC), "8-3": (0x61, 0xF2), "8-4": (0x62, 0xF6),
    }

    def _warm_reset(self, level: str = "1-1"):
        """
        Reset the NES and advance to active gameplay on the requested level.

        $0750 (packed area identifier) is the key RAM address: LoadAreaPointer
        reads it to compute the area data pointer.  $075F/$075C (world/level)
        control the HUD display.  Both must be set correctly before the area
        reload is triggered.
        """
        self._lib.reset(self._h)

        # Refresh memory views after reset (pointers remain valid but contents change)
        self._ram        = self._lib.ram_view(self._h)
        self._nametables = self._lib.nametables_view(self._h)
        self._oam        = self._lib.oam_view(self._h)

        # Advance through NES power-on boot (~80 frames blank screen)
        for _ in range(80):
            self._lib.step(self._h)

        world_idx, level_idx = self._LEVEL_MAP.get(level, (0, 0))
        area_val, area_760   = self._AREA_REGS.get(level, (0x25, 0x00))

        # Press START to kick off InitGame (20 frames)
        for _ in range(20):
            self._lib.set_buttons(self._h, NES_BUTTON_START)
            self._lib.step(self._h)
        self._lib.set_buttons(self._h, 0)

        # Wait until OperMode ($0770) == 1 (game active), max 300 frames.
        for _ in range(300):
            self._lib.step(self._h)
            if int(self._ram[0x0770]) == 1:
                break

        # Inject the target level.
        #   $0750 = packed area identifier (read by LoadAreaPointer)
        #   $0760 = relative area offset within world's $9CBC section
        #   $075F/$075C = WorldNumber/LevelNumber (HUD display)
        # Reset OperMode_Task ($0772) to 0 to force LoadAreaPointer to re-run.
        self._lib.write_ram(self._h, 0x0750, area_val)
        self._lib.write_ram(self._h, 0x0760, area_760)
        self._lib.write_ram(self._h, 0x075F, world_idx)
        self._lib.write_ram(self._h, 0x075C, level_idx)
        self._lib.write_ram(self._h, 0x0772, 0x00)

        # Settle 60 frames; keep re-asserting injected values so incidental
        # task-0 writes don't clobber them before LoadAreaPointer completes.
        for _ in range(60):
            self._lib.write_ram(self._h, 0x0750, area_val)
            self._lib.write_ram(self._h, 0x0760, area_760)
            self._lib.write_ram(self._h, 0x075F, world_idx)
            self._lib.write_ram(self._h, 0x075C, level_idx)
            self._lib.step(self._h)
