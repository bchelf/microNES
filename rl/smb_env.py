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
# Action space
# ---------------------------------------------------------------------------
# 12 physically meaningful button combinations for SMB1.
ACTION_BUTTONS: list[int] = [
    0,                                               # 0  NOOP
    NES_BUTTON_RIGHT,                                # 1  walk right
    NES_BUTTON_RIGHT | NES_BUTTON_B,                 # 2  run right
    NES_BUTTON_RIGHT | NES_BUTTON_A,                 # 3  jump right
    NES_BUTTON_RIGHT | NES_BUTTON_B | NES_BUTTON_A,  # 4  run+jump right
    NES_BUTTON_LEFT,                                 # 5  walk left
    NES_BUTTON_LEFT  | NES_BUTTON_B,                 # 6  run left
    NES_BUTTON_LEFT  | NES_BUTTON_A,                 # 7  jump left
    NES_BUTTON_LEFT  | NES_BUTTON_B | NES_BUTTON_A,  # 8  run+jump left
    NES_BUTTON_A,                                    # 9  jump in place
    NES_BUTTON_DOWN,                                 # 10 crouch / enter pipe
    NES_BUTTON_B,                                    # 11 fireball (fire Mario)
]
N_ACTIONS = len(ACTION_BUTTONS)

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
# SMBEnv
# ---------------------------------------------------------------------------
class SMBEnv(gym.Env):
    """
    Gymnasium environment for SMB1 using structured RAM/PPU observations.
    Requires libmicrones_rl.{so,dylib} from the build-host target.
    """

    metadata = {"render_modes": ["rgb_array"], "render_fps": 20}

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
        self._action_history: deque[int] = deque([0, 0, 0, 0], maxlen=4)
        self._prev_reward_x: int = 0

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

        obs = self._get_obs()
        return obs, {}

    def step(self, action: int) -> tuple[dict, float, bool, bool, dict]:
        buttons = ACTION_BUTTONS[action]
        self._lib.set_buttons(self._h, buttons)
        for _ in range(self._frame_skip):
            self._lib.step(self._h)
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
        truncated = (self._step_count >= self.MAX_STEPS) or stagnating

        info: dict[str, Any] = {
            "world_x":    world_x,
            "frame":      self._lib.frame_count(self._h),
            "stagnating": stagnating,
        }
        return obs, float(reward), terminated, truncated, info

    def render(self):
        if self._render_mode != "rgb_array":
            return None
        # framebuffer: (240, 256) palette indices in [0, 63]
        fb = self._lib.framebuffer_view(self._h)
        return NES_PALETTE_RGB[fb & 0x3F]  # (240, 256, 3) uint8

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
            # 5 enemy slots × 7 features each
            "enemies":    spaces.Box(-1.0, 1.0, (5, 7), f32),
            # Interactive objects: powerup (4) + fireball×2 (3 each) = 10
            "objects":    spaces.Box(-1.0, 1.0, (10,), f32),
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
        enemies_arr, enemy_mask, enemy_sx = self._build_enemies(ram, mario_sx, mario_sy)
        # Update prev enemy X and compute velocity
        for i in range(5):
            if enemy_mask[i]:
                dx = int(enemy_sx[i]) - int(self._prev_enemy_screen_x[i])
                if abs(dx) < 100:  # ignore screen-wrap glitches
                    enemies_arr[i, 3] = float(np.clip(dx / 8.0, -1.0, 1.0))
        self._prev_enemy_screen_x[:] = enemy_sx

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
    # Enemy extraction
    # ------------------------------------------------------------------
    def _build_enemies(self, ram, mario_sx: int, mario_sy: int):
        """
        Returns:
            arr  : (5, 7) float32 — [type/15, rel_x, rel_y, vx(init 0), dir, state/5, alive]
            mask : list[bool] — True if slot is active
            sx   : (5,) int32 — raw screen X per slot for velocity tracking
        """
        arr  = np.zeros((5, 7), dtype=np.float32)
        mask = [False] * 5
        sx   = np.zeros(5, dtype=np.int32)

        for i in range(5):
            etype = int(ram[0x000F + i])
            if etype == 0:
                continue
            mask[i] = True
            ex    = int(ram[0x0087 + i])  # screen X
            ey    = int(ram[0x00CF + i])  # screen Y
            state = int(ram[0x001E + i])
            # direction: bit 1 of state is commonly the direction flag in SMB1
            direction = float((state >> 1) & 1)
            sx[i] = ex
            arr[i] = [
                float(etype) / 15.0,
                float(np.clip((ex - mario_sx) / 128.0, -1, 1)),
                float(np.clip((ey - mario_sy) / 120.0, -1, 1)),
                0.0,   # vx filled in by caller via frame-delta
                direction,
                float(state & 0x07) / 5.0,
                1.0,
            ]
        return arr, mask, sx

    # ------------------------------------------------------------------
    # Reward
    # ------------------------------------------------------------------
    def _compute_reward(self, obs: dict, action: int, world_x: int) -> float:
        r = 0.0

        # Dense forward progress
        dx = world_x - self._prev_reward_x
        if dx > 0:
            r += float(np.clip(dx / 40.0, 0, 1.5))    # ~1.0 at full run speed
        elif dx < 0:
            r += float(np.clip(dx / 40.0, -0.5, 0))   # mild backtrack penalty

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
        if action in (3, 4, 7, 8, 9):
            on_ground      = obs["player_state"][5] > 0.5
            gap_any        = float(obs["gap_ahead"].max())    > 0.1
            obstacle_any   = float(obs["obstacle_ahead"].max()) > 0.1
            if on_ground and not gap_any and not obstacle_any:
                r -= 0.005

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
