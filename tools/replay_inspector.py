#!/usr/bin/env python3
"""
replay_inspector.py — Web-based replay viewer for SMB1 .npy action sequences.

Replays a saved action sequence through the full training wrapper stack and
renders a step-by-step browser UI with per-wrapper reward breakdown.

Usage:
    python tools/replay_inspector.py --npy path/to/run.npy --rom path/to/smb1.nes
    python tools/replay_inspector.py --npy run.npy --rom smb1.nes --level 1-3 --port 5000
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

# Allow imports from rl/
_RL_DIR = Path(__file__).resolve().parent.parent / "rl"
sys.path.insert(0, str(_RL_DIR))

import imageio.v2 as imageio
import numpy as np

ACTION_NAMES = [
    "WAIT", "STEP_RIGHT", "RUN_RIGHT", "STEP_LEFT", "RUN_LEFT",
    "SHORT_JUMP_R", "SHORT_JUMP_L", "SHORT_JUMP_IP",
    "MED_JUMP_R", "MED_JUMP_L", "MED_JUMP_IP",
    "MAX_JUMP_R", "MAX_JUMP_L", "MAX_JUMP_IP",
]

_STEPS_JSON: str = ""  # populated in main() before server starts


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


def _frame_to_b64_png(frame: np.ndarray) -> str:
    """Encode a (240, 256, 3) uint8 array as a base64-encoded PNG string."""
    buf = io.BytesIO()
    imageio.imwrite(buf, frame, format="png")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def _get_base_smb(env):
    """Walk the wrapper chain to find the underlying SMBEnv instance."""
    from smb_env import SMBEnv
    while not isinstance(env, SMBEnv):
        env = env.env
    return env


def _build_stack(rom_path: str, render_lib: str, level: str):
    """
    Build the full training wrapper stack for replay.

    Matches make_env_fn() exactly, except:
      - sticky_prob=0.0  (replay uses the recorded actions exactly)
      - render_mode="rgb_array"  (need framebuffer for frames)
      - no RND  (intrinsic reward not meaningful for single-run replay)
    """
    from smb_env import SMBEnv
    from wrappers import (
        DeathPenaltyWrapper,
        NewMaxXWrapper,
        PlatformClimbRewardWrapper,
        StompRewardWrapper,
        StickyActionWrapper,
        SurvivalBonusWrapper,
        VisitedCellsWrapper,
    )

    env = SMBEnv(
        rom_path=rom_path,
        lib_path=render_lib,
        render_mode="rgb_array",
        levels=[level],
    )
    env = StickyActionWrapper(env, sticky_prob=0.0)
    env = NewMaxXWrapper(env, scale=2.0, active=False)
    env = SurvivalBonusWrapper(env)
    env = DeathPenaltyWrapper(env)
    env = StompRewardWrapper(env, stomp_bonus=5.0)
    env = PlatformClimbRewardWrapper(env, climb_bonus=2.0)
    env = VisitedCellsWrapper(env, cell_size_x=8, cell_size_y=8, cell_bonus=1.0)
    return env


def run_replay(npy_path: str, rom_path: str, render_lib: str, level: str) -> list[dict]:
    """
    Replay a saved action sequence and collect per-step data.

    Returns a list of dicts, one per step, including frame PNG (base64),
    per-wrapper reward breakdown, and all relevant info keys.
    """
    actions: list[int] = np.load(npy_path).tolist()
    env = _build_stack(rom_path, render_lib, level)
    base_smb = _get_base_smb(env)

    env.reset()
    steps: list[dict] = []
    cumulative_reward = 0.0
    prev_stomps = 0
    prev_climbs = 0
    max_x_on_ground = 0

    print(f"Replaying {len(actions)} actions on level {level}...")

    for step_idx, action in enumerate(actions):
        obs, reward, terminated, truncated, info = env.step(action)

        # Grab current frame from the base SMBEnv (render_mode="rgb_array")
        frame = base_smb.render()  # (240, 256, 3) uint8 or None

        # Per-wrapper reward deltas
        stomps_now = info.get("stomps_this_episode", 0)
        climbs_now = info.get("climbs_this_episode", 0)
        new_cell = info.get("new_cell_found", False)
        death_applied = info.get("death_penalty_applied", False)

        reward_cell     = 1.0  if new_cell                    else 0.0
        reward_climb    = 2.0  if climbs_now > prev_climbs    else 0.0
        reward_stomp    = 5.0  if stomps_now > prev_stomps    else 0.0
        reward_death    = -4.0 if death_applied               else 0.0
        reward_survival = 0.02 if not (terminated or truncated) else 0.0
        reward_base_env = (
            reward
            - reward_cell
            - reward_climb
            - reward_stomp
            - reward_death
            - reward_survival
        )

        cumulative_reward += reward
        prev_stomps = stomps_now
        prev_climbs = climbs_now
        if info.get("on_ground", False):
            max_x_on_ground = max(max_x_on_ground, info.get("world_x", 0))

        player_dead = bool(obs["game_flags"][0] > 0.5)

        steps.append({
            "step":              step_idx,
            "action":            action,
            "action_name":       ACTION_NAMES[action] if action < len(ACTION_NAMES) else f"ACT_{action}",
            # Position / state
            "world_x":           info.get("world_x", 0),
            "mario_y":           info.get("mario_y", 0),
            "on_ground":         bool(info.get("on_ground", False)),
            "lives_remaining":   info.get("lives_remaining", 2),
            "stagnating":        bool(info.get("stagnating", False)),
            # Death / termination
            "player_dead":       player_dead,
            "pit_death":         bool(info.get("pit_death", False)),
            "death_penalty_applied": bool(death_applied),
            "level_complete":    bool(info.get("level_complete", False)),
            "terminated":        bool(terminated),
            "truncated":         bool(truncated),
            # Reward breakdown
            "reward_total":      float(reward),
            "reward_base_env":   float(reward_base_env),
            "reward_survival":   float(reward_survival),
            "reward_death":      float(reward_death),
            "reward_stomp":      float(reward_stomp),
            "reward_climb":      float(reward_climb),
            "reward_cell":       float(reward_cell),
            "cumulative_reward": float(cumulative_reward),
            # Counters
            "stomps_this_episode": int(stomps_now),
            "climbs_this_episode": int(climbs_now),
            "new_cell_found":    bool(new_cell),
            "max_x_on_ground":   max_x_on_ground,
            "max_x_seen":        float(info.get("max_x_seen", 0.0)),
            # RAM diagnostics
            "ram_000E":          int(info.get("ram_000E", 0)),
            "ram_075A":          int(info.get("ram_075A", info.get("lives_remaining", 2))),
            # Frame
            "frame_b64":         _frame_to_b64_png(frame) if frame is not None else "",
        })

        if (step_idx + 1) % 200 == 0:
            print(f"  step {step_idx + 1}/{len(actions)}  "
                  f"x={info.get('world_x', 0)}  cumR={cumulative_reward:.2f}")

        if terminated or truncated:
            end_reason = "terminated" if terminated else "truncated"
            print(f"Episode ended at step {step_idx + 1} ({end_reason})  "
                  f"x={info.get('world_x', 0)}  cumR={cumulative_reward:.2f}")
            break

    env.close()
    print(f"Replay complete: {len(steps)} steps, cumulative reward={cumulative_reward:.2f}")
    return steps


# ---------------------------------------------------------------------------
# Embedded HTML/CSS/JS frontend
# ---------------------------------------------------------------------------
HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>SMB Replay Inspector</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  background: #1a1a2e; color: #e0e0e0;
  font-family: 'Courier New', monospace; font-size: 13px;
  height: 100vh; display: flex; flex-direction: column; overflow: hidden;
}
/* --- top bar --- */
#top-bar {
  display: flex; align-items: center; gap: 6px; flex-shrink: 0;
  padding: 5px 10px; background: #16213e; border-bottom: 1px solid #0f3460;
}
#top-bar button {
  padding: 3px 9px; background: #0f3460; color: #d0d0f0;
  border: 1px solid #3a3a6e; border-radius: 3px; cursor: pointer;
  font-family: inherit; font-size: 13px;
}
#top-bar button:hover { background: #533483; }
button.sp-active { background: #533483 !important; border-color: #8855cc; }
#step-jump {
  width: 64px; padding: 3px 5px; background: #0f3460; color: #d0d0f0;
  border: 1px solid #3a3a6e; font-family: inherit; font-size: 13px;
  text-align: center; border-radius: 3px;
}
#status-text { margin-left: auto; color: #6688aa; font-size: 12px; }
/* --- main area --- */
#main {
  display: flex; gap: 8px; padding: 8px;
  flex: 1; min-height: 0;
}
#canvas-col { flex-shrink: 0; display: flex; align-items: flex-start; }
canvas#game {
  display: block; image-rendering: pixelated;
  width: 512px; height: 480px;
  border: 2px solid #3a3a6e; background: #000;
}
#info-col {
  flex: 1; overflow-y: auto;
  padding-right: 4px;
}
#info-grid {
  display: grid; grid-template-columns: 150px 1fr; gap: 1px 10px;
}
.info-key  { color: #778899; padding: 1px 0; }
.info-val  { color: #ccccee; padding: 1px 0; }
.positive  { color: #44ee88; }
.negative  { color: #ff6655; }
.neutral   { color: #aaaadd; }
.warn      { color: #ffcc44; }
.section-header {
  grid-column: 1 / -1; color: #556677; margin-top: 7px; margin-bottom: 1px;
  border-bottom: 1px solid #2a2a4e; padding-bottom: 2px;
  font-size: 11px; letter-spacing: 1px; text-transform: uppercase;
}
/* --- scrubber --- */
#scrubber-area {
  flex-shrink: 0; padding: 5px 10px 6px;
  background: #16213e; border-top: 1px solid #0f3460;
}
#scrubber-wrap { position: relative; height: 22px; margin-bottom: 3px; }
#scrubber { width: 100%; height: 22px; cursor: pointer; accent-color: #533483; }
canvas#ticks {
  position: absolute; top: 0; left: 0; width: 100%; height: 22px;
  pointer-events: none;
}
#scrubber-legend { font-size: 11px; color: #556677; }
/* --- loading overlay --- */
#loading {
  position: fixed; inset: 0; background: rgba(10,10,20,0.92);
  display: flex; flex-direction: column; align-items: center;
  justify-content: center; z-index: 100; gap: 10px;
}
#loading h2 { color: #aaaadd; font-weight: normal; }
#loading-bar-wrap {
  width: 320px; height: 8px; background: #1e1e3e;
  border-radius: 4px; border: 1px solid #3a3a6e;
}
#loading-bar {
  width: 0%; height: 100%; background: #533483;
  border-radius: 4px; transition: width 0.08s;
}
#loading-status { color: #667788; font-size: 12px; }
</style>
</head>
<body>

<div id="loading">
  <h2>Loading replay data…</h2>
  <div id="loading-bar-wrap"><div id="loading-bar"></div></div>
  <div id="loading-status">Fetching…</div>
</div>

<div id="top-bar">
  <button id="btn-first"  title="First (Home)">&#x23EE;</button>
  <button id="btn-prev"   title="Prev (←)">&#x23F4;</button>
  <button id="btn-play"   title="Play/Pause (Space)">&#x25B6;</button>
  <button id="btn-next"   title="Next (→)">&#x23F5;&#x23F5;</button>
  <button id="btn-last"   title="Last (End)">&#x23ED;</button>
  <span style="color:#556677;margin-left:4px">Speed:</span>
  <button data-sp="0.25">0.25×</button>
  <button data-sp="0.5">0.5×</button>
  <button data-sp="1" class="sp-active">1×</button>
  <button data-sp="2">2×</button>
  <button data-sp="4">4×</button>
  <input id="step-jump" type="number" min="0" placeholder="step #" title="Jump to step">
  <button id="btn-jump">Go</button>
  <span id="status-text">Step 0 / 0</span>
</div>

<div id="main">
  <div id="canvas-col">
    <canvas id="game" width="256" height="240"></canvas>
  </div>
  <div id="info-col">
    <div id="info-grid"></div>
  </div>
</div>

<div id="scrubber-area">
  <div id="scrubber-wrap">
    <input id="scrubber" type="range" min="0" value="0">
    <canvas id="ticks" height="22"></canvas>
  </div>
  <div id="scrubber-legend">
    &#x25A0; <span style="color:#ff4444">red</span> = death / pit &nbsp;
    &#x25A0; <span style="color:#ffdd00">yellow</span> = stomp &nbsp;
    &#x25A0; <span style="color:#44ee88">green</span> = climb &nbsp;
    &#x25A0; <span style="color:#44aaff">blue</span> = new cell
  </div>
</div>

<script>
const BASE_MS = 120;  // ms per step at 1× speed
let steps = [];
let images = [];
let cur = 0;
let playing = false;
let speed = 1.0;
let playTimer = null;

const gameCanvas  = document.getElementById('game');
const gameCtx     = gameCanvas.getContext('2d');
const scrubber    = document.getElementById('scrubber');
const tickCanvas  = document.getElementById('ticks');
const tickCtx     = tickCanvas.getContext('2d');
const infoGrid    = document.getElementById('info-grid');
const statusText  = document.getElementById('status-text');
const loadingEl   = document.getElementById('loading');
const loadingBar  = document.getElementById('loading-bar');
const loadingStatus = document.getElementById('loading-status');

// ---- Reward / bool formatting ----
function fmtR(v) {
  const s = (v >= 0 ? '+' : '') + v.toFixed(3);
  return { text: s, cls: v > 0.0001 ? 'positive' : v < -0.0001 ? 'negative' : 'neutral' };
}
function fmtBool(v) {
  return { text: String(v), cls: v ? 'warn' : 'neutral' };
}
function fmtBoolGood(v) {
  return { text: String(v), cls: v ? 'positive' : 'neutral' };
}

// ---- Info panel ----
function renderInfo(s) {
  const rows = [
    ['Position', null],
    ['step',            { text: s.step + ' / ' + (steps.length - 1), cls: 'neutral' }],
    ['action',          { text: s.action_name + ' (' + s.action + ')', cls: 'neutral' }],
    ['world_x',         { text: s.world_x, cls: 'neutral' }],
    ['mario_y',         { text: s.mario_y, cls: 'neutral' }],
    ['on_ground',       fmtBoolGood(s.on_ground)],
    ['lives',           { text: s.lives_remaining, cls: s.lives_remaining < 2 ? 'negative' : 'neutral' }],
    ['stagnating',      fmtBool(s.stagnating)],

    ['Episode End', null],
    ['terminated',      fmtBool(s.terminated)],
    ['truncated',       fmtBool(s.truncated)],
    ['player_dead',     fmtBool(s.player_dead)],
    ['pit_death',       fmtBool(s.pit_death)],
    ['death_applied',   fmtBool(s.death_penalty_applied)],
    ['level_complete',  fmtBoolGood(s.level_complete)],

    ['Rewards (this step)', null],
    ['total',           fmtR(s.reward_total)],
    ['base_env',        fmtR(s.reward_base_env)],
    ['survival',        fmtR(s.reward_survival)],
    ['death',           fmtR(s.reward_death)],
    ['stomp',           fmtR(s.reward_stomp)],
    ['climb',           fmtR(s.reward_climb)],
    ['cell',            fmtR(s.reward_cell)],
    ['cumulative',      fmtR(s.cumulative_reward)],

    ['Counters', null],
    ['stomps',          { text: s.stomps_this_episode, cls: s.stomps_this_episode > 0 ? 'positive' : 'neutral' }],
    ['climbs',          { text: s.climbs_this_episode, cls: s.climbs_this_episode > 0 ? 'positive' : 'neutral' }],
    ['new_cell',        fmtBoolGood(s.new_cell_found)],
    ['max_x_on_ground', { text: s.max_x_on_ground, cls: 'neutral' }],
    ['max_x_seen',      { text: s.max_x_seen.toFixed(0), cls: 'neutral' }],
  ];

  let html = '';
  for (const [k, v] of rows) {
    if (v === null) {
      html += `<div class="section-header" style="grid-column:1/-1">${k}</div>`;
    } else {
      html += `<div class="info-key">${k}</div><div class="info-val ${v.cls}">${v.text}</div>`;
    }
  }
  infoGrid.innerHTML = html;
}

// ---- Frame display ----
function drawFrame(idx) {
  idx = Math.max(0, Math.min(steps.length - 1, idx));
  cur = idx;
  scrubber.value = idx;
  statusText.textContent = 'Step ' + idx + ' / ' + (steps.length - 1);
  renderInfo(steps[idx]);
  if (images[idx]) {
    gameCtx.drawImage(images[idx], 0, 0);
  } else {
    gameCtx.fillStyle = '#000';
    gameCtx.fillRect(0, 0, 256, 240);
  }
}

function goTo(idx) {
  drawFrame(idx);
}

// ---- Playback ----
function togglePlay() {
  playing = !playing;
  document.getElementById('btn-play').innerHTML = playing ? '&#x23F8;' : '&#x25B6;';
  if (playing) scheduleNext();
  else clearTimeout(playTimer);
}

function scheduleNext() {
  if (!playing) return;
  playTimer = setTimeout(() => {
    if (cur < steps.length - 1) {
      goTo(cur + 1);
      scheduleNext();
    } else {
      playing = false;
      document.getElementById('btn-play').innerHTML = '&#x25B6;';
    }
  }, BASE_MS / speed);
}

function setSpeed(s) {
  speed = s;
  document.querySelectorAll('[data-sp]').forEach(b => {
    b.classList.toggle('sp-active', parseFloat(b.dataset.sp) === s);
  });
  if (playing) { clearTimeout(playTimer); scheduleNext(); }
}

// ---- Tick marks ----
function drawTicks() {
  const w = tickCanvas.offsetWidth || tickCanvas.parentElement.offsetWidth;
  tickCanvas.width = w;
  tickCtx.clearRect(0, 0, w, 22);
  const n = steps.length;
  if (n < 2) return;
  for (let i = 0; i < n; i++) {
    const s = steps[i];
    let color = null;
    if (s.death_penalty_applied || s.pit_death) color = 'rgba(255,68,68,0.9)';
    else if (s.reward_stomp > 0)                color = 'rgba(255,221,0,0.9)';
    else if (s.reward_climb > 0)                color = 'rgba(68,238,136,0.9)';
    else if (s.new_cell_found)                  color = 'rgba(68,170,255,0.35)';
    if (color) {
      const x = Math.round((i / (n - 1)) * (w - 2));
      tickCtx.fillStyle = color;
      tickCtx.fillRect(x, 0, 2, 22);
    }
  }
}

// ---- Controls ----
document.addEventListener('keydown', e => {
  if (document.activeElement === document.getElementById('step-jump')) return;
  if (e.key === 'ArrowRight' || e.key === 'Right') { e.preventDefault(); goTo(cur + 1); }
  else if (e.key === 'ArrowLeft' || e.key === 'Left') { e.preventDefault(); goTo(cur - 1); }
  else if (e.key === ' ') { e.preventDefault(); togglePlay(); }
  else if (e.key === 'Home') goTo(0);
  else if (e.key === 'End')  goTo(steps.length - 1);
});

scrubber.addEventListener('input', () => goTo(parseInt(scrubber.value)));
document.getElementById('btn-first').addEventListener('click', () => goTo(0));
document.getElementById('btn-prev') .addEventListener('click', () => goTo(cur - 1));
document.getElementById('btn-play') .addEventListener('click', togglePlay);
document.getElementById('btn-next') .addEventListener('click', () => goTo(cur + 1));
document.getElementById('btn-last') .addEventListener('click', () => goTo(steps.length - 1));
document.getElementById('btn-jump') .addEventListener('click', () => {
  const v = parseInt(document.getElementById('step-jump').value);
  if (!isNaN(v)) goTo(v);
});
document.getElementById('step-jump').addEventListener('keydown', e => {
  if (e.key === 'Enter') {
    const v = parseInt(e.target.value);
    if (!isNaN(v)) goTo(v);
  }
});
document.querySelectorAll('[data-sp]').forEach(b => {
  b.addEventListener('click', () => setSpeed(parseFloat(b.dataset.sp)));
});
window.addEventListener('resize', drawTicks);

// ---- Init: fetch data + pre-cache frames ----
async function init() {
  loadingStatus.textContent = 'Fetching step data…';
  const resp = await fetch('/data');
  if (!resp.ok) throw new Error('HTTP ' + resp.status);
  const data = await resp.json();
  steps = data;
  scrubber.max = steps.length - 1;

  images = new Array(steps.length);
  let loaded = 0;
  await Promise.all(steps.map((s, i) => new Promise(resolve => {
    if (!s.frame_b64) { loaded++; resolve(); return; }
    const img = new Image();
    img.onload = () => {
      images[i] = img;
      loaded++;
      loadingBar.style.width = (loaded / steps.length * 100).toFixed(1) + '%';
      if (loaded % 50 === 0 || loaded === steps.length) {
        loadingStatus.textContent =
          `Caching frames… ${loaded} / ${steps.length}`;
      }
      resolve();
    };
    img.onerror = resolve;  // skip bad frames rather than stalling
    img.src = 'data:image/png;base64,' + s.frame_b64;
  })));

  drawTicks();
  loadingEl.style.display = 'none';
  goTo(0);
  setSpeed(1.0);
}

init().catch(err => {
  loadingStatus.textContent = 'Error: ' + err.message;
  loadingBar.style.background = '#ff4444';
  console.error(err);
});
</script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------
class _Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = HTML_TEMPLATE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/data":
            body = _STEPS_JSON.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, fmt, *args):
        pass  # suppress per-request access logs


def main():
    parser = argparse.ArgumentParser(
        description="SMB replay inspector — web-based step scrubber with reward breakdown"
    )
    parser.add_argument("--npy",   required=True, help="Path to .npy action sequence")
    parser.add_argument("--rom",   required=True, help="Path to SMB1 .nes ROM")
    parser.add_argument("--level", default="1-1",  help="Level string (default: 1-1)")
    parser.add_argument("--port",  type=int, default=5000, help="HTTP port (default: 5000)")
    parser.add_argument("--lib",   default=None,
                        help="Override render lib path (auto-detected if omitted)")
    args = parser.parse_args()

    render_lib = args.lib or _find_render_lib()
    print(f"Render lib:  {render_lib}")
    print(f"ROM:         {args.rom}")
    print(f"Actions:     {args.npy}")
    print(f"Level:       {args.level}")

    steps = run_replay(args.npy, args.rom, render_lib, args.level)

    global _STEPS_JSON
    _STEPS_JSON = json.dumps(steps)
    kb = len(_STEPS_JSON) // 1024
    print(f"Replay data: {len(steps)} steps, {kb} KB")

    def _open_browser():
        time.sleep(1.0)
        webbrowser.open(f"http://localhost:{args.port}/")

    threading.Thread(target=_open_browser, daemon=True).start()

    print(f"Server:      http://localhost:{args.port}/  (Ctrl-C to quit)")
    server = HTTPServer(("", args.port), _Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
