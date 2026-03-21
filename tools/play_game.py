#!/usr/bin/env python3
"""
play_game.py — Whole-game SMB1 inference harness.

Loads a trained PPO checkpoint and runs it through the full game (1-1 → 8-4),
managing death, lives, game over, and level transitions explicitly.
Records a continuous MP4 per attempt with HUD overlay and writes a JSON summary.

Usage:
    python tools/play_game.py --checkpoint checkpoints/model_0004200000.zip --rom roms/smb1.nes
    python tools/play_game.py --checkpoint ... --rom ... --start-level 1-3 --lives 5 --port 8080

Notes:
    Natural level transitions: after completing a level, the harness runs the NES
    flag-descent / score-tally / castle-entry animation via raw emulator frames
    (bypassing Gymnasium wrappers) and detects the new level from RAM before
    calling env.reset() for the next episode. The animation is captured in the video.

    Title-screen boot: SMBEnv always skips the NES title screen during reset()
    (it presses START and injects level RAM). There is no --faithful-start mode
    without modifying smb_env.py._warm_reset().
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import os
import sys
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

import imageio
import imageio.v2 as imageio_v2
import numpy as np

try:
    from PIL import Image, ImageDraw
    _HAVE_PIL = True
except ImportError:
    _HAVE_PIL = False

# Allow imports from rl/
_RL_DIR = Path(__file__).resolve().parent.parent / "rl"
sys.path.insert(0, str(_RL_DIR))

# ---------------------------------------------------------------------------
# SMB1 canonical level sequence (all 32 levels in game order)
# ---------------------------------------------------------------------------
SMB1_LEVELS: list[str] = [
    "1-1", "1-2", "1-3", "1-4",
    "2-1", "2-2", "2-3", "2-4",
    "3-1", "3-2", "3-3", "3-4",
    "4-1", "4-2", "4-3", "4-4",
    "5-1", "5-2", "5-3", "5-4",
    "6-1", "6-2", "6-3", "6-4",
    "7-1", "7-2", "7-3", "7-4",
    "8-1", "8-2", "8-3", "8-4",
]

# ---------------------------------------------------------------------------
# Button mapping: derived from _ACTION_SEQUENCES in smb_env.py.
# Each entry is the OR of NES button bits for the first (main-hold) segment.
# Bit values from nes_ctypes.py:
#   A=0x01  B=0x02  SELECT=0x04  START=0x08
#   UP=0x10  DOWN=0x20  LEFT=0x40  RIGHT=0x80
# ---------------------------------------------------------------------------
_ACTION_BUTTONS: list[int] = [
    0x00,  # 0  WAIT
    0x80,  # 1  STEP_RIGHT:    RIGHT
    0x82,  # 2  RUN_RIGHT:     RIGHT + B
    0x40,  # 3  STEP_LEFT:     LEFT
    0x42,  # 4  RUN_LEFT:      LEFT + B
    0x81,  # 5  SHORT_JUMP_R:  RIGHT + A
    0x41,  # 6  SHORT_JUMP_L:  LEFT + A
    0x01,  # 7  SHORT_JUMP_IP: A
    0x83,  # 8  MED_JUMP_R:    RIGHT + B + A
    0x43,  # 9  MED_JUMP_L:    LEFT + B + A
    0x01,  # 10 MED_JUMP_IP:   A
    0x83,  # 11 MAX_JUMP_R:    RIGHT + B + A
    0x43,  # 12 MAX_JUMP_L:    LEFT + B + A
    0x01,  # 13 MAX_JUMP_IP:   A
]

# ---------------------------------------------------------------------------
# Reverse of SMBEnv._LEVEL_MAP: (world_0idx, level_0idx) → "W-L"
# RAM[0x075F] = WorldNumber (0-indexed), RAM[0x075C] = LevelNumber (0-indexed)
# ---------------------------------------------------------------------------
_RAM_LEVEL_REVERSE: dict[tuple[int, int], str] = {
    (0, 0): "1-1", (0, 1): "1-2", (0, 2): "1-3", (0, 3): "1-4",
    (1, 0): "2-1", (1, 1): "2-2", (1, 2): "2-3", (1, 3): "2-4",
    (2, 0): "3-1", (2, 1): "3-2", (2, 2): "3-3", (2, 3): "3-4",
    (3, 0): "4-1", (3, 1): "4-2", (3, 2): "4-3", (3, 3): "4-4",
    (4, 0): "5-1", (4, 1): "5-2", (4, 2): "5-3", (4, 3): "5-4",
    (5, 0): "6-1", (5, 1): "6-2", (5, 2): "6-3", (5, 3): "6-4",
    (6, 0): "7-1", (6, 1): "7-2", (6, 2): "7-3", (6, 3): "7-4",
    (7, 0): "8-1", (7, 1): "8-2", (7, 2): "8-3", (7, 3): "8-4",
}

# ---------------------------------------------------------------------------
# Controller overlay constants
# ---------------------------------------------------------------------------
_CTR_W   = 90   # controller graphic width in pixels
_CTR_H   = 28   # controller graphic height in pixels
_CTR_PAD = 2    # pixels of padding from frame edge


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _find_render_lib() -> str:
    repo_root = Path(__file__).resolve().parent.parent
    for d in [repo_root / "build-host", repo_root / "build"]:
        for suf in [".dylib", ".so"]:
            p = d / f"libmicrones_rl_render{suf}"
            if p.exists():
                return str(p)
    raise FileNotFoundError(
        "libmicrones_rl_render not found. Build with:\n"
        "  cmake --build build-host -j --target micrones_rl_render"
    )


def _build_env(rom_path: str, render_lib: str):
    """
    Build the full training wrapper stack for inference.

    Matches make_env_fn() exactly, except:
      - sticky_prob=0.0   (no random action repeats during inference)
      - render_mode='rgb_array'  (framebuffer needed for video capture)
      - NewMaxXWrapper active=False  (diagnostic only; no reward needed)
      - No RNDWrapper  (intrinsic reward weights not available at inference time)
    """
    from smb_env import SMBEnv
    from wrappers import (
        AirborneActionMaskWrapper,
        DeathPenaltyWrapper,
        NewMaxXWrapper,
        PlatformClimbRewardWrapper,
        StickyActionWrapper,
        StompRewardWrapper,
        SurvivalBonusWrapper,
        VisitedCellsWrapper,
    )

    env = SMBEnv(rom_path=rom_path, lib_path=render_lib, render_mode="rgb_array")
    env = AirborneActionMaskWrapper(env)
    env = StickyActionWrapper(env, sticky_prob=0.0)
    env = NewMaxXWrapper(env, scale=2.0, active=False)
    env = SurvivalBonusWrapper(env)
    env = DeathPenaltyWrapper(env)         # adds info["level_complete"]
    env = StompRewardWrapper(env, stomp_bonus=5.0)
    env = PlatformClimbRewardWrapper(env, climb_bonus=2.0)
    env = VisitedCellsWrapper(env, cell_size_x=8, cell_size_y=8, cell_bonus=1.0)
    return env


def _draw_controller(draw: "ImageDraw.ImageDraw", frame_w: int, frame_h: int, buttons: int) -> None:
    """
    Draw a pixel-art NES controller in the bottom-right corner.

    Controller layout (90×28 px):
      D-pad  |  SELECT  START  |  (B)  (A)

    Button bits: A=0x01 B=0x02 SEL=0x04 STA=0x08 UP=0x10 DN=0x20 LF=0x40 RT=0x80
    Active buttons are highlighted; inactive are dark.
    """
    x0 = frame_w - _CTR_W - _CTR_PAD
    y0 = frame_h - _CTR_H - _CTR_PAD

    C_BODY    = (55,  55,  65)    # controller body fill
    C_EDGE    = (100, 100, 115)   # outline
    C_OFF     = (40,  40,  48)    # inactive button
    C_DPAD    = (210, 210, 215)   # active d-pad direction
    C_B       = (255, 200,   0)   # active B (yellow)
    C_A       = (255,  60,  60)   # active A (red)
    C_SS      = (160, 160, 220)   # active SELECT / START
    C_LABEL   = (180, 180, 180)   # button labels

    def r(x1, y1, x2, y2):
        return (x0 + x1, y0 + y1, x0 + x2, y0 + y2)

    # Controller body
    draw.rectangle(r(0, 0, _CTR_W - 1, _CTR_H - 1), fill=C_BODY, outline=C_EDGE)

    # D-pad cross — draw full cross in inactive colour, then highlight active arm
    draw.rectangle(r(11, 2, 17, 26), fill=C_OFF)   # vertical bar
    draw.rectangle(r( 3, 9, 25, 19), fill=C_OFF)   # horizontal bar

    if buttons & 0x10:  # UP
        draw.rectangle(r(11, 2, 17,  9), fill=C_DPAD)
    if buttons & 0x20:  # DOWN
        draw.rectangle(r(11, 19, 17, 26), fill=C_DPAD)
    if buttons & 0x40:  # LEFT
        draw.rectangle(r( 3,  9, 11, 19), fill=C_DPAD)
    if buttons & 0x80:  # RIGHT
        draw.rectangle(r(17,  9, 25, 19), fill=C_DPAD)

    # D-pad centre (always a slightly different shade)
    draw.rectangle(r(11, 9, 17, 19), fill=(62, 62, 74))

    # SELECT
    draw.rectangle(r(33, 11, 42, 17),
                   fill=C_SS if (buttons & 0x04) else C_OFF, outline=C_EDGE)

    # START
    draw.rectangle(r(45, 11, 54, 17),
                   fill=C_SS if (buttons & 0x08) else C_OFF, outline=C_EDGE)

    # B button (circle)
    draw.ellipse(r(60, 4, 73, 24),
                 fill=C_B if (buttons & 0x02) else C_OFF, outline=C_EDGE)
    draw.text((x0 + 64, y0 + 10), "B", fill=C_LABEL)

    # A button (circle)
    draw.ellipse(r(74, 4, 87, 24),
                 fill=C_A if (buttons & 0x01) else C_OFF, outline=C_EDGE)
    draw.text((x0 + 78, y0 + 10), "A", fill=C_LABEL)


def _add_hud(
    frame: np.ndarray,
    level: str,
    lives: int,
    step: int,
    attempt: int,
    action: int = 0,
) -> np.ndarray:
    """
    Overlay HUD onto a frame.

    - Bottom-left: level / lives / step / attempt text
    - Bottom-right: NES controller graphic showing active buttons for `action`

    No-op (returns frame unchanged) if PIL is not installed.
    """
    if not _HAVE_PIL:
        return frame
    out = frame.copy()
    h, w = out.shape[:2]

    # Darken bottom strip for readability
    strip_y = h - _CTR_H - _CTR_PAD - 6
    out[strip_y:, :] = (out[strip_y:, :].astype(np.uint16) * 3 // 8).astype(np.uint8)

    img  = Image.fromarray(out)
    draw = ImageDraw.Draw(img)

    # HUD text — bottom-left
    text = f"Level:{level}  Lives:{lives}  Step:{step}  Att:{attempt}"
    draw.text((2, h - _CTR_H - _CTR_PAD - 4), text, fill=(255, 255, 0))

    # Controller — bottom-right
    buttons = _ACTION_BUTTONS[action] if 0 <= action < len(_ACTION_BUTTONS) else 0
    _draw_controller(draw, w, h, buttons)

    return np.array(img)


def _frame_to_b64(frame: np.ndarray) -> str:
    buf = io.BytesIO()
    imageio_v2.imwrite(buf, frame, format="png")
    return base64.b64encode(buf.getvalue()).decode("ascii")


# ---------------------------------------------------------------------------
# Natural level transition
# ---------------------------------------------------------------------------
def _run_natural_transition(
    base_env,
    current_level: str,
    max_frames: int = 3000,
) -> tuple[list[np.ndarray], str]:
    """
    After a level_complete, run raw NES frames (bypassing Gymnasium wrappers)
    so the flag-descent / score-tally / castle-entry animation plays naturally.

    Watches RAM[0x075F] (WorldNumber) and RAM[0x075C] (LevelNumber) for the
    NES to advance to the next level, confirmed by RAM[0x001D]==0x00 (on ground).

    Returns (frames, detected_level_str).  If the timeout is reached without
    detecting a new level, returns (frames, current_level) and the caller
    falls back to the expected next level in the sequence.

    WHY raw frames instead of env.step(WAIT):
      Calling step() after terminated=True keeps _episode_max_x at the flagpole
      (~2800). When the new level loads, world_x resets to 0. With
      STAGNATION_EARLY_STOP=120, the new episode gets truncated ~67 env steps
      into actual gameplay — far too soon. Bypassing the wrappers avoids all
      episode-state contamination.
    """
    lib = base_env._lib
    h   = base_env._h
    ram = base_env._ram

    old_world     = int(ram[0x075F])
    old_level_idx = int(ram[0x075C])

    frames: list[np.ndarray] = []

    for _ in range(max_frames):
        lib.step(h)

        frame = base_env.render()   # (240, 256, 3) uint8; None if render_mode != rgb_array
        if frame is not None:
            frames.append(frame.copy())

        new_world     = int(ram[0x075F])
        new_level_idx = int(ram[0x075C])

        # Detect new level: world/level registers changed AND Mario is on ground
        if (new_world, new_level_idx) != (old_world, old_level_idx):
            if int(ram[0x001D]) == 0x00:   # 0x00 = on ground
                detected = _RAM_LEVEL_REVERSE.get((new_world, new_level_idx))
                if detected:
                    return frames, detected

    # Timeout — caller will use expected_next from the sequence
    return frames, current_level


# ---------------------------------------------------------------------------
# Live dashboard
# ---------------------------------------------------------------------------
_dash_lock = threading.Lock()
_dash_state: dict = {
    "level": "—",
    "lives": 0,
    "attempt": 1,
    "step": 0,
    "levels_completed": [],
    "deaths": [],
    "frame_b64": "",
    "status": "starting",   # "running" | "game_over" | "game_won"
}

_DASHBOARD_HTML = """\
<!DOCTYPE html>
<html>
<head>
<title>SMB1 — play_game</title>
<meta charset="utf-8"/>
<style>
body { background:#111; color:#eee; font-family:monospace; text-align:center; margin:0; padding:16px; }
h2   { color:#ff0; margin:8px 0; }
.hud { font-size:1.1em; margin:8px 0; letter-spacing:.05em; }
img  { image-rendering:pixelated; width:512px; height:480px; border:2px solid #444; display:block; margin:8px auto; }
.log { text-align:left; display:inline-block; margin:8px; max-height:200px; overflow-y:auto;
       background:#222; padding:8px; border-radius:4px; min-width:400px; }
.done { color:#0f0; font-size:1.3em; font-weight:bold; }
.over { color:#f44; font-size:1.3em; font-weight:bold; }
</style>
</head>
<body>
<h2>SMB1 Inference Harness</h2>
<div class="hud" id="hud">Loading...</div>
<img id="frame" alt="NES frame"/>
<div class="log" id="log">—</div>
<script>
function poll() {
    fetch('/state').then(r => r.json()).then(d => {
        var status = '';
        if (d.status === 'game_won')  status = '<span class="done">GAME WON!</span>';
        if (d.status === 'game_over') status = '<span class="over">GAME OVER</span>';
        document.getElementById('hud').innerHTML =
            'Level: <b>' + d.level + '</b>' +
            '&nbsp;&nbsp;Lives: <b>' + d.lives + '</b>' +
            '&nbsp;&nbsp;Step: <b>' + d.step + '</b>' +
            '&nbsp;&nbsp;Attempt: <b>' + d.attempt + '</b>' +
            (status ? '&nbsp;&nbsp;' + status : '');
        if (d.frame_b64)
            document.getElementById('frame').src = 'data:image/png;base64,' + d.frame_b64;
        var log = '';
        if (d.levels_completed.length)
            log += 'Completed: ' + d.levels_completed.join(', ') + '\\n';
        d.deaths.forEach(function(x) {
            log += 'x ' + x.level + ' step=' + x.step + ' (' + x.cause + ')\\n';
        });
        document.getElementById('log').innerText = log || '(no events yet)';
    });
}
setInterval(poll, 500);
poll();
</script>
</body>
</html>"""


class _DashboardHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = _DASHBOARD_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/state":
            with _dash_lock:
                body = json.dumps(_dash_state).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):
        pass  # suppress per-request noise


def _dash_update(
    level: str,
    lives: int,
    step: int,
    attempt: int,
    levels_completed: list[str],
    deaths: list[dict],
    status: str = "running",
    frame: np.ndarray | None = None,
):
    with _dash_lock:
        _dash_state.update(
            level=level, lives=lives, step=step, attempt=attempt,
            levels_completed=list(levels_completed),
            deaths=list(deaths),
            status=status,
        )
        if frame is not None:
            _dash_state["frame_b64"] = _frame_to_b64(frame)


# ---------------------------------------------------------------------------
# Core attempt loop
# ---------------------------------------------------------------------------
def run_attempt(
    level_models: dict,
    env,
    level_sequence: list[str],
    lives: int,
    attempt: int,
    save_dir: str,
    deterministic: bool,
    use_dashboard: bool,
) -> dict:
    """
    Run one full-game attempt through level_sequence.

    level_models maps every level string in level_sequence to a loaded PPO model.
    Multiple levels may share the same model object (same path loaded once).

    Manages all level transitions and death/game-over logic explicitly.
    On level_complete, runs the NES transition animation via raw emulator
    frames before resetting for the next level.
    Returns a summary dict.
    """
    levels_completed: list[str] = []
    level_completions: list[dict] = []
    deaths: list[dict] = []
    all_frames: list[np.ndarray] = []
    total_steps = 0
    current_idx = 0
    current_level = level_sequence[0]
    model = level_models[current_level]

    ts_tag  = datetime.now().strftime("%Y%m%d_%H%M%S")
    base_env = env.unwrapped   # SMBEnv — for pop_step_frames() and raw lib access

    _ts = lambda: datetime.now().strftime("%H:%M:%S")

    print(f"\n{'='*60}")
    print(f"Attempt {attempt} | start={current_level} | lives={lives}")
    print(f"Sequence: {', '.join(level_sequence)}")
    print(f"{'='*60}")

    obs, _ = env.reset(options={"level": current_level})

    game_won = False

    while True:
        # --- Inference step ---
        action, _ = model.predict(obs, deterministic=deterministic)
        obs, _reward, terminated, truncated, info = env.step(int(action))
        total_steps += 1

        # Actual executed action (AirborneActionMaskWrapper may replace jumps with WAIT)
        executed_action = 0 if info.get("action_masked", False) else int(action)

        # --- Collect and annotate frames ---
        raw_frames = base_env.pop_step_frames()
        hud_frames = [
            _add_hud(f, current_level, lives, total_steps, attempt, executed_action)
            for f in raw_frames
        ]
        all_frames.extend(hud_frames)

        # --- Update live dashboard (every 8 steps) ---
        if use_dashboard and total_steps % 8 == 0 and hud_frames:
            _dash_update(
                current_level, lives, total_steps, attempt,
                levels_completed, deaths, "running", hud_frames[-1],
            )

        if not (terminated or truncated):
            continue

        # --- Handle termination ---
        level_complete = info.get("level_complete", False)

        if level_complete:
            # ---- Level completed ----
            print(f"[{_ts()}] ✓ {current_level} completed  step={total_steps}")
            levels_completed.append(current_level)
            level_completions.append({"level": current_level, "step": total_steps})

            current_idx += 1
            if current_idx >= len(level_sequence):
                game_won = True
                print(f"\n  GAME COMPLETE! All {len(level_sequence)} levels cleared!")
                if use_dashboard:
                    _dash_update(
                        current_level, lives, total_steps, attempt,
                        levels_completed, deaths, "game_won",
                        hud_frames[-1] if hud_frames else None,
                    )
                break

            expected_next = level_sequence[current_idx]

            # Run the NES flag-descent / score-tally / castle-entry animation
            # via raw emulator frames so the transition is captured in the video.
            print(f"       → playing level-transition animation...")
            trans_frames, detected = _run_natural_transition(base_env, current_level)

            # Annotate transition frames (action=0/WAIT while animation plays)
            for f in trans_frames:
                all_frames.append(_add_hud(f, current_level, lives, total_steps, attempt, 0))

            print(
                f"       → {len(trans_frames)} transition frames | "
                f"NES→{detected!r}  (expected {expected_next!r})"
            )

            # Use detected level when it matches sequence; otherwise expected_next
            current_level = detected if detected != current_level else expected_next
            model = level_models[current_level]
            obs, _ = env.reset(options={"level": current_level})

        else:
            # ---- Death or stagnation truncation ----
            cause = (
                "pit"        if info.get("pit_death") else
                "stagnation" if truncated else
                "enemy"
            )
            lives -= 1
            deaths.append({"level": current_level, "step": total_steps, "cause": cause})
            print(
                f"[{_ts()}] x {cause.upper()} at {current_level}  "
                f"step={total_steps}  lives_left={lives}"
            )

            if lives <= 0:
                print(f"\n  GAME OVER — furthest: {current_level}")
                if use_dashboard:
                    _dash_update(
                        current_level, 0, total_steps, attempt,
                        levels_completed, deaths, "game_over",
                        hud_frames[-1] if hud_frames else None,
                    )
                break

            # Retry same level
            obs, _ = env.reset(options={"level": current_level})

    # --- Save video ---
    os.makedirs(save_dir, exist_ok=True)
    video_path = os.path.join(save_dir, f"game_run_attempt{attempt}_{ts_tag}.mp4")
    if all_frames:
        with imageio.get_writer(video_path, fps=60, codec="libx264", quality=8) as writer:
            for f in all_frames:
                writer.append_data(f)
        print(f"\nVideo → {video_path}  ({len(all_frames)} frames)")
    else:
        print("\nWARNING: no frames captured — is the render lib built with framebuffer enabled?")

    # --- Save JSON summary ---
    summary = {
        "attempt":           attempt,
        "levels_completed":  levels_completed,
        "furthest_level":    current_level,
        "total_steps":       total_steps,
        "deaths":            deaths,
        "level_completions": level_completions,
        "game_won":          game_won,
        "video":             video_path,
    }
    json_path = os.path.join(save_dir, f"game_run_attempt{attempt}_{ts_tag}.json")
    with open(json_path, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"Summary → {json_path}")

    return summary


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="SMB1 whole-game inference harness",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--checkpoint",   default=None,
                        help="Default PPO .zip checkpoint (used for any level not in "
                             "--level-checkpoints)")
    parser.add_argument("--level-checkpoints", nargs="+", default=[], metavar="LEVEL:PATH",
                        help="Per-level checkpoint overrides, e.g. "
                             "1-1:model_a.zip 1-3:model_b.zip. "
                             "Any level not listed falls back to --checkpoint.")
    parser.add_argument("--rom",          required=True,  help="Path to SMB1 .nes ROM")
    parser.add_argument("--start-level",  default="1-1",  help="First level to play")
    parser.add_argument("--lives",        type=int, default=3,  help="Lives per attempt")
    parser.add_argument("--max-attempts", type=int, default=3,  help="Total attempts before exit")
    parser.add_argument("--deterministic", action="store_true", default=True,
                        help="Use deterministic policy (argmax, no sampling)")
    parser.add_argument("--save-video",   default="game_runs", help="Output directory for MP4 + JSON")
    parser.add_argument("--port",         type=int, default=None,
                        help="Port for live dashboard (omit to disable)")
    parser.add_argument("--lib",          default=None,    help="Override path to libmicrones_rl_render")
    args = parser.parse_args()

    render_lib = args.lib or _find_render_lib()

    # --- Validate level sequence against SMBEnv's _LEVEL_MAP ---
    from smb_env import SMBEnv
    supported = set(SMBEnv._LEVEL_MAP.keys())

    sequence: list[str] = []
    for lvl in SMB1_LEVELS:
        if lvl not in supported:
            print(f"WARNING: {lvl} not in SMBEnv._LEVEL_MAP — skipping")
        else:
            sequence.append(lvl)

    if args.start_level not in sequence:
        print(f"ERROR: --start-level {args.start_level!r} is not a supported level")
        sys.exit(1)

    # Trim to start level
    sequence = sequence[sequence.index(args.start_level):]

    # --- Resolve per-level checkpoint paths ---
    # Parse LEVEL:PATH pairs from --level-checkpoints
    level_ckpt_paths: dict[str, str] = {}
    for entry in args.level_checkpoints:
        if ":" not in entry:
            parser.error(
                f"--level-checkpoints entry {entry!r} must be LEVEL:PATH "
                f"(e.g. 1-1:checkpoints/model.zip)"
            )
        lvl, path = entry.split(":", 1)
        if lvl not in supported:
            parser.error(f"--level-checkpoints: {lvl!r} is not a supported level")
        level_ckpt_paths[lvl] = path

    # Ensure every level in the sequence has a checkpoint
    for lvl in sequence:
        if lvl not in level_ckpt_paths:
            if args.checkpoint is None:
                parser.error(
                    f"No checkpoint for level {lvl!r}. "
                    f"Provide --checkpoint as a default or add {lvl}:PATH to --level-checkpoints."
                )
            level_ckpt_paths[lvl] = args.checkpoint

    print(f"ROM:          {args.rom}")
    print(f"Start level:  {args.start_level}")
    print(f"Sequence:     {len(sequence)} levels  ({sequence[0]} → {sequence[-1]})")
    print(f"Lives/attempt:{args.lives}")
    print(f"Max attempts: {args.max_attempts}")
    print(f"Output dir:   {args.save_video}")
    print(f"PIL HUD:      {'yes' if _HAVE_PIL else 'no (Pillow not installed — HUD disabled)'}")

    # --- Build env ---
    env = _build_env(args.rom, render_lib)

    # --- Load models (deduplicated: same path → same object) ---
    from stable_baselines3 import PPO

    unique_paths = sorted(set(level_ckpt_paths[lvl] for lvl in sequence))
    path_to_model: dict[str, object] = {}
    for path in unique_paths:
        print(f"Loading checkpoint: {path}")
        m = PPO.load(path, env=env)
        m.policy.set_training_mode(False)
        path_to_model[path] = m

    level_models: dict[str, object] = {
        lvl: path_to_model[level_ckpt_paths[lvl]] for lvl in sequence
    }

    # Print model assignment summary (group levels that share a checkpoint)
    from collections import defaultdict
    by_path: dict[str, list[str]] = defaultdict(list)
    for lvl in sequence:
        by_path[level_ckpt_paths[lvl]].append(lvl)
    print("Model assignments:")
    for path, lvls in by_path.items():
        print(f"  {path}  →  {', '.join(lvls)}")

    # --- Start live dashboard if requested ---
    use_dashboard = args.port is not None
    server: HTTPServer | None = None
    if use_dashboard:
        server = HTTPServer(("", args.port), _DashboardHandler)
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()
        print(f"Live dashboard: http://localhost:{args.port}/")

    # --- Run attempts ---
    try:
        for attempt in range(1, args.max_attempts + 1):
            summary = run_attempt(
                level_models=level_models,
                env=env,
                level_sequence=sequence,
                lives=args.lives,
                attempt=attempt,
                save_dir=args.save_video,
                deterministic=args.deterministic,
                use_dashboard=use_dashboard,
            )
            n_done = len(summary["levels_completed"])
            print(
                f"\nAttempt {attempt} result: "
                f"{n_done}/{len(sequence)} levels  "
                f"furthest={summary['furthest_level']}  "
                f"deaths={len(summary['deaths'])}"
            )
            if summary["game_won"]:
                print("Game complete — stopping.")
                break

        print("\nAll attempts exhausted.")

    finally:
        env.close()
        if server is not None:
            server.server_close()


if __name__ == "__main__":
    main()
