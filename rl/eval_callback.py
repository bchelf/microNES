"""
EvalVideoCallback — periodic checkpoint + per-level deterministic eval for SMBEnv.

Every `eval_interval` training timesteps:
  1. Saves a model checkpoint  →  checkpoints/model_{step:010d}.zip
  2. For each eval level, runs one deterministic episode with render_mode="rgb_array"
  3. Writes collected frames as MP4  →  eval_videos/eval_{level}_{step:010d}.mp4

The eval env uses libmicrones_rl_render.dylib (framebuffer enabled).
Training envs continue using libmicrones_rl.dylib (framebuffer disabled).
"""

from __future__ import annotations

import os
import threading
import warnings
from collections import deque
from pathlib import Path
from typing import Any

import imageio
import numpy as np
import torch as th
import torch.nn.functional as F
from stable_baselines3.common.callbacks import BaseCallback

from rnd import flatten_obs_batch
from smb_env import SMBEnv


class DiagnosticsCallback(BaseCallback):
    """
    Logs custom training diagnostics to TensorBoard under the "diagnostics/" prefix.

    # ================================================================
    # TASK 0 FINDINGS (exploration conducted before writing this code)
    # ================================================================
    #
    # Info keys present at every step (after full wrapper chain):
    #   world_x (int)                — absolute x position in level
    #   frame (int)                  — NES frame count
    #   stagnating (bool)            — stagnation flag from base env
    #   max_x_seen (float)           — lifetime max x (NewMaxXWrapper, never reset)
    #   total_survival_bonus (float) — episode-cumulative survival bonus
    #   death_penalty_applied (bool) — whether DeathPenaltyWrapper fired this step
    #
    # At episode end, VecMonitor additionally injects:
    #   episode["r"] — total episode reward
    #   episode["l"] — episode length in steps
    #   episode["t"] — elapsed wall time
    #
    # max_x_seen access: lives on NewMaxXWrapper as an instance attribute.
    #   gym.Wrapper.__getattr__ delegates unknown attrs down the chain, so
    #   self.training_env.get_attr("max_x_seen") traverses:
    #   VecMonitor → SubprocVecEnv subprocess → DeathPenalty → Survival → NewMaxXWrapper ✓
    #   Returns a list of N floats (one per worker). We use [0] (env 0's lifetime max)
    #   as the representative global frontier per the spec.
    #
    # ent_coef: initialized as float(0.02). EntropySchedulerCallback writes it back
    #   as float after restarts. Guarded with callable() for Schedule safety.
    #
    # policy_entropy: NOT available in self.locals during rollout collection.
    #   Best source is self.model.logger.name_to_value["train/entropy_loss"] (stale
    #   from last training update). entropy ≈ -entropy_loss.
    #
    # max_x_episode: NOT directly in info — info["max_x_seen"] is the LIFETIME max,
    #   not the per-episode max. Tracked manually per env slot in _ep_max_x[].
    #
    # MAX_STEPS: accessed via get_attr in _on_training_start; falls back to 600
    #   if the attribute is not reachable through the VecEnv wrapper chain.
    # ================================================================

    Groups logged:
      A — Spatial Progress     (episode end): max_x_episode, max_x_global
      B — Death & Risk         (episode end): steps_before_death,
                                              death_penalty_applied,
                                              died_before_obstacle
      C — Survival Bonus       (episode end): survival_bonus_total, survival_fraction
      D — Entropy & Policy     (every step):  ent_coef_current, policy_entropy
      E — Value Function       (every check_interval steps): value_at_start_state
      H — 2D Cell Exploration  (episode end): cells_visited_episode,
                                              cells_visited_total, cells_per_step
                               (every check_interval): visited_cells_count

    Args:
        start_obs:       Dict observation from a single-env reset, used for Group E.
                         If None, Group E is skipped with a one-time warning.
        check_interval:  Steps between Group E value-function evaluations (default 10_000).
        verbose:         SB3 verbosity level.
    """

    def __init__(
        self,
        start_obs: dict | None = None,
        check_interval: int = 10_000,
        verbose: int = 0,
    ):
        super().__init__(verbose)
        self._start_obs      = start_obs
        self._check_interval = check_interval

        if start_obs is None:
            warnings.warn(
                "[DiagnosticsCallback] start_obs not provided — "
                "diagnostics/value_at_start_state will not be logged."
            )

        # Per-env episode accumulators; sized in _on_training_start once n_envs is known.
        self._ep_max_x:             list[float] = []
        self._ep_steps:             list[int]   = []
        self._ep_death_flag:        list[bool]  = []
        self._ep_level_complete_flag: list[bool]  = []
        self._ep_intrinsic_sum:     list[float] = []   # cumulative intrinsic per episode
        self._ep_extrinsic_sum:     list[float] = []   # cumulative extrinsic per episode
        self._ep_blocked_steps:     list[int]   = []   # steps where frontier bonus was blocked
        self._ep_max_x_on_ground:   list[float] = []   # highest world_x while on ground
        self._ep_sticky_steps:      list[int]   = []   # steps where action was repeated
        self._max_episode_steps: float = 600.0   # overwritten in _on_training_start
        self._last_cells_check: int = 0          # step count of last visited_cells_count log

    # ------------------------------------------------------------------
    def _on_training_start(self) -> None:
        n = self.training_env.num_envs
        self._ep_max_x               = [0.0]   * n
        self._ep_steps               = [0]     * n
        self._ep_death_flag          = [False] * n
        self._ep_level_complete_flag = [False] * n
        self._ep_intrinsic_sum       = [0.0]   * n
        self._ep_extrinsic_sum       = [0.0]   * n
        self._ep_blocked_steps       = [0]     * n
        self._ep_max_x_on_ground     = [0.0]   * n
        self._ep_sticky_steps        = [0]     * n

        # Try to read MAX_STEPS from the env; fall back to 600.
        # Assumption: if get_attr fails, 600 steps/episode is a reasonable upper bound
        # given STAGNATION_EARLY_STOP=300 and typical SMB episode lengths.
        try:
            self._max_episode_steps = float(
                self.training_env.get_attr("MAX_STEPS")[0]
            )
        except Exception:
            self._max_episode_steps = 600.0

    # ------------------------------------------------------------------
    def _on_step(self) -> bool:
        infos = self.locals.get("infos", [])

        # ---- Groups A / B / C — per-episode metrics ----
        for i, info in enumerate(infos):
            if i >= len(self._ep_max_x):
                break  # guard against unexpected env count change

            # Accumulate episode-max x manually (info["max_x_seen"] is the lifetime
            # max, not the episode max — tracked here instead).
            world_x = float(info.get("world_x", 0.0))
            self._ep_max_x[i] = max(self._ep_max_x[i], world_x)
            self._ep_steps[i] += 1

            # Latch death / level-complete flags for the episode.
            # Both are terminal events so they only fire on the last step, but
            # latching is cleaner than relying on that invariant explicitly.
            if info.get("death_penalty_applied", False):
                self._ep_death_flag[i] = True
            if info.get("level_complete", False):
                self._ep_level_complete_flag[i] = True

            # RND intrinsic / extrinsic accumulators (zero if RND not enabled).
            self._ep_intrinsic_sum[i] += float(info.get("intrinsic_reward", 0.0))
            self._ep_extrinsic_sum[i] += float(info.get("extrinsic_reward", 0.0))

            # Ground-gated frontier metrics.
            # frontier_bonus_blocked comes from NewMaxXWrapper (may be absent if active=False
            # still fires the key — it does, so this is fine either way).
            if info.get("frontier_bonus_blocked", False):
                self._ep_blocked_steps[i] += 1

            # Sticky action tracking (absent when StickyActionWrapper is disabled).
            if info.get("action_was_sticky", False):
                self._ep_sticky_steps[i] += 1
            # max_x_on_ground: read directly from SMBEnv's info["on_ground"] so this
            # metric works regardless of whether NewMaxXWrapper is active or not.
            if info.get("on_ground", True):
                if world_x > self._ep_max_x_on_ground[i]:
                    self._ep_max_x_on_ground[i] = world_x

            if "episode" not in info:
                continue

            # Episode ended — log and reset accumulators for this env slot.
            max_x_ep    = self._ep_max_x[i]
            ep_len      = float(info["episode"].get("l", self._ep_steps[i]))
            death_ep    = self._ep_death_flag[i]
            complete_ep = self._ep_level_complete_flag[i]
            # Truncation: episode ended but Mario neither died nor completed the level.
            # DeathPenaltyWrapper confirmed correct: penalty only fires when dead=True,
            # so truncation episodes are NOT penalised by that wrapper.
            truncated_ep = not death_ep and not complete_ep

            # episode_end_type encoding: death=0, truncation=1, level_complete=2.
            # record_mean() of this gives a weighted centroid of end types per rollout
            # window; more readable as individual rate metrics below.
            end_type = 0.0 if death_ep else (2.0 if complete_ep else 1.0)

            # max_x_global: env 0's lifetime frontier as the representative value.
            # Using [0] rather than max() to stay level-agnostic and avoid the IPC
            # cost of reading all workers; env 0's frontier is stable and sufficient
            # for the died_before_obstacle relative threshold.
            try:
                max_x_global = float(self.training_env.get_attr("max_x_seen")[0])
            except Exception:
                max_x_global = max_x_ep  # fallback if VecEnv attr lookup fails

            # Groups A / B / C / D-episode use record_mean() — SB3's logger only
            # flushes to TensorBoard once per rollout (in _dump_logs). record() would
            # overwrite on every episode end, silently discarding all but the last
            # episode in the window (~25-50 episodes per rollout at n_steps=1024,
            # 8 envs). record_mean() averages all episodes completing in the window.

            # Group A — Spatial Progress
            self.logger.record_mean("diagnostics/max_x_episode", max_x_ep)
            self.logger.record_mean("diagnostics/max_x_global",  max_x_global)

            # Group B — Death & Risk Aversion
            # died_before_obstacle: death episode where max_x < 85% of the frontier.
            # Uses death_ep (not the raw death_penalty_applied latch) so it's
            # consistent with episode_end_type.  Truncations never set this.
            died_short = float(
                death_ep
                and max_x_global > 0
                and max_x_ep < max_x_global * 0.85
            )
            self.logger.record_mean("diagnostics/steps_before_death",   ep_len)
            self.logger.record_mean("diagnostics/death_penalty_applied", float(death_ep))
            self.logger.record_mean("diagnostics/died_before_obstacle",  died_short)

            # Group C — Survival Bonus Health
            surv_bonus = float(info.get("total_survival_bonus", 0.0))
            surv_frac  = ep_len / max(self._max_episode_steps, 1.0)
            self.logger.record_mean("diagnostics/survival_bonus_total", surv_bonus)
            self.logger.record_mean("diagnostics/survival_fraction",    surv_frac)

            # Episode end type breakdown (Task 4)
            self.logger.record_mean("diagnostics/episode_end_type",      end_type)
            self.logger.record_mean("diagnostics/truncation_rate",       float(truncated_ep))
            self.logger.record_mean("diagnostics/level_complete_rate",   float(complete_ep))

            # Group F — RND intrinsic reward health (only logged when RND active).
            ep_intrinsic = self._ep_intrinsic_sum[i]
            ep_extrinsic = self._ep_extrinsic_sum[i]
            if ep_intrinsic != 0.0:
                self.logger.record_mean(
                    "diagnostics/intrinsic_reward_episode_mean",
                    ep_intrinsic / max(ep_len, 1.0),
                )
                # intrinsic_extrinsic_ratio: how large is intrinsic relative to
                # extrinsic for this episode?
                #   > 2.0  → reduce intrinsic_scale
                #   < 0.1  → increase intrinsic_scale
                #   0.3-1.0 → healthy balance
                if abs(ep_extrinsic) > 1e-6:
                    self.logger.record_mean(
                        "diagnostics/intrinsic_extrinsic_ratio",
                        abs(ep_intrinsic / ep_extrinsic),
                    )

            # Group G — Ground-gated frontier health (from NewMaxXWrapper on-ground fix).
            blocked_steps = self._ep_blocked_steps[i]
            ep_steps_nonzero = max(self._ep_steps[i], 1)
            self.logger.record_mean(
                "diagnostics/frontier_bonus_blocked_rate",
                float(blocked_steps) / ep_steps_nonzero,
            )
            self.logger.record_mean(
                "diagnostics/max_x_on_ground",
                self._ep_max_x_on_ground[i],
            )

            # Group H — 2D cell exploration (from VisitedCellsWrapper).
            # episode_new_cells: new cells found this episode (from info dict at episode end).
            # total_cells_visited: lifetime total at this point in time.
            # cells_per_step: exploration efficiency — should be high early, drop as
            #   the agent revisits territory. Healthy range: 0.01-0.05.
            ep_new_cells   = int(info.get("episode_new_cells", 0))
            total_cells    = int(info.get("total_cells_visited", 0))
            cells_per_step = float(ep_new_cells) / ep_steps_nonzero
            self.logger.record_mean("diagnostics/cells_visited_episode", float(ep_new_cells))
            self.logger.record_mean("diagnostics/cells_visited_total",   float(total_cells))
            self.logger.record_mean("diagnostics/cells_per_step",        cells_per_step)

            # Group I — Combat & Action Quality
            # stomps_this_episode: read from StompRewardWrapper's cumulative counter.
            #   Absent (0) when StompRewardWrapper is disabled via --no-stomp.
            stomps = int(info.get("stomps_this_episode", 0))
            self.logger.record_mean("diagnostics/stomps_this_episode", float(stomps))
            climbs = int(info.get("climbs_this_episode", 0))
            self.logger.record_mean("diagnostics/climbs_this_episode", float(climbs))
            # sticky_action_rate: fraction of steps where the previous action was repeated.
            #   Absent (0) when StickyActionWrapper is disabled via --no-sticky.
            #   Healthy range: near sticky_prob (e.g. 0.25). Deviations indicate RNG issues.
            sticky_rate = float(self._ep_sticky_steps[i]) / ep_steps_nonzero
            self.logger.record_mean("diagnostics/sticky_action_rate", sticky_rate)

            # Reset accumulators for this env slot.
            self._ep_max_x[i]               = 0.0
            self._ep_steps[i]               = 0
            self._ep_death_flag[i]          = False
            self._ep_level_complete_flag[i] = False
            self._ep_intrinsic_sum[i]       = 0.0
            self._ep_extrinsic_sum[i]       = 0.0
            self._ep_blocked_steps[i]       = 0
            self._ep_max_x_on_ground[i]     = 0.0
            self._ep_sticky_steps[i]        = 0

        # ---- Group D — Entropy & Policy Health (every step) ----
        coef = self.model.ent_coef
        if callable(coef):
            coef = coef(1.0)
        self.logger.record("diagnostics/ent_coef_current", float(coef))

        # Policy entropy is not available in self.locals during rollout collection.
        # We read it from the SB3 logger as -train/entropy_loss (stale from the
        # last completed training update, not the current rollout step).
        entropy_loss = self.model.logger.name_to_value.get("train/entropy_loss")
        if entropy_loss is not None:
            self.logger.record("diagnostics/policy_entropy", -float(entropy_loss))

        # ---- Group H (periodic) — visited_cells archive size ----
        # Read the actual set length from env 0's VisitedCellsWrapper via get_attr.
        # gym.Wrapper.__getattr__ delegates unknown attrs down the chain, so
        # visited_cells resolves to VisitedCellsWrapper.visited_cells. ✓
        if (
            self.num_timesteps > 0
            and self.num_timesteps % self._check_interval == 0
            and self.num_timesteps != self._last_cells_check
        ):
            self._last_cells_check = self.num_timesteps
            try:
                cells_archive = self.training_env.get_attr("visited_cells")[0]
                self.logger.record(
                    "diagnostics/visited_cells_count", len(cells_archive)
                )
            except Exception:
                pass   # VisitedCellsWrapper may not be in the stack (non-fatal)

        # ---- Group E — Value Function Health (every check_interval steps) ----
        if (
            self._start_obs is not None
            and self.num_timesteps > 0
            and self.num_timesteps % self._check_interval == 0
        ):
            try:
                # Add batch dimension: single-env obs has shape (...,); policy expects (1, ...).
                start_obs_batched = {
                    k: v[np.newaxis] for k, v in self._start_obs.items()
                }
                obs_tensor, _ = self.model.policy.obs_to_tensor(start_obs_batched)
                with th.no_grad():
                    value = self.model.policy.predict_values(obs_tensor)
                self.logger.record(
                    "diagnostics/value_at_start_state",
                    float(value.mean().cpu()),
                )
            except Exception as exc:
                if self.verbose:
                    print(f"[DiagnosticsCallback] value_at_start_state skipped: {exc}")

        return True


class EntropySchedulerCallback(BaseCallback):
    """
    Detects spatial progress plateaus and temporarily boosts entropy to escape
    local optima.

    When max_x_seen (spatial frontier) hasn't changed by more than
    `plateau_threshold` over the last `window_size` measurements, ent_coef is
    spiked to `restart_ent_coef` and then decayed multiplicatively each step
    until it reaches `floor_ent_coef`. This counteracts entropy collapse — the
    failure mode where PPO's conservatism locks the policy into a deterministic
    local optimum and prevents frontier exploration.

    Args:
        check_interval:    Steps between plateau checks (default 15_000).
        plateau_threshold: Max(window) - min(window) below this triggers restart
                           (default 20.0, in world_x units).
        restart_ent_coef:  ent_coef value to set on plateau detection (default 0.05).
        decay_rate:        Multiplicative decay applied to ent_coef each step after
                           restart (default 0.9995).
        floor_ent_coef:    Minimum ent_coef after decay (default 0.001).
        window_size:       Number of recent max_x_seen values to track (default 7).
    """

    def __init__(
        self,
        check_interval: int = 15_000,
        plateau_threshold: float = 20.0,
        restart_ent_coef: float = 0.05,
        decay_rate: float = 0.9995,
        floor_ent_coef: float = 0.001,
        window_size: int = 7,
        verbose: int = 0,
    ):
        super().__init__(verbose)
        self._check_interval    = check_interval
        self._plateau_threshold = plateau_threshold
        self._restart_ent_coef  = restart_ent_coef
        self._decay_rate        = decay_rate
        self._floor_ent_coef    = floor_ent_coef
        self._window_size       = window_size
        self._rew_window: deque[float] = deque(maxlen=window_size)
        self._restarting        = False

    def _on_step(self) -> bool:
        # Read max_x_seen directly from NewMaxXWrapper via VecEnv
        try:
            current_val = float(self.training_env.get_attr("max_x_seen")[0])
        except Exception as e:
            # Fallback: use ep_rew_mean if get_attr fails for any reason
            logger_vals = self.model.logger.name_to_value
            if "rollout/ep_rew_mean" not in logger_vals:
                return True
            current_val = float(logger_vals["rollout/ep_rew_mean"])
            if not hasattr(self, "_get_attr_warned"):
                self._get_attr_warned = True
                import logging as _logging
                _logging.getLogger(__name__).warning(
                    f"[EntropyScheduler] get_attr('max_x_seen') failed: {e}. "
                    f"Falling back to ep_rew_mean."
                )

        self._rew_window.append(current_val)

        # Plateau check every check_interval steps.
        if (
            self.num_timesteps % self._check_interval == 0
            and len(self._rew_window) >= 2
        ):
            spread = max(self._rew_window) - min(self._rew_window)
            if spread < self._plateau_threshold:
                self.model.ent_coef = float(self._restart_ent_coef)
                self._restarting = True
                warnings.warn(
                    f"[EntropyScheduler] step={self.num_timesteps:,}: plateau detected "
                    f"(spread={spread:.4f} < {self._plateau_threshold}). "
                    f"Resetting ent_coef → {self._restart_ent_coef}"
                )

        # Decay ent_coef each step while a restart is active.
        if self._restarting:
            current = float(self.model.ent_coef)
            decayed = max(current * self._decay_rate, self._floor_ent_coef)
            self.model.ent_coef = decayed
            if decayed <= self._floor_ent_coef:
                self._restarting = False

        # Log current ent_coef value (handle Schedule or float).
        coef = self.model.ent_coef
        if callable(coef):
            coef = coef(1.0)
        self.logger.record("train/ent_coef_scheduled", float(coef))
        self.logger.record(
            "diagnostics/entropy_plateau_delta",
            float(max(self._rew_window) - min(self._rew_window))
            if len(self._rew_window) >= 2 else 0.0
        )

        return True


class CurriculumStageCallback(BaseCallback):
    """
    Switches all training envs to a new set of levels at a specified step count.

    Uses SubprocVecEnv.env_method("set_curriculum", ...) so the switch happens
    in-place without restarting workers or losing model state.

    Args:
        stage2_at:      Training step count at which to switch (absolute steps).
        stage2_levels:  Level list for stage 2 (e.g. ["1-1","1-2","1-3","1-4"]).
        stage2_weights: Optional sampling weights (unnormalized). Default: uniform.
        verbose:        Print a message when the switch fires.
    """

    def __init__(
        self,
        stage2_at: int,
        stage2_levels: list[str],
        stage2_weights: list[float] | None = None,
        verbose: int = 1,
    ):
        super().__init__(verbose)
        self._stage2_at      = stage2_at
        self._stage2_levels  = stage2_levels
        self._stage2_weights = stage2_weights
        self._switched       = False

    def _on_step(self) -> bool:
        if not self._switched and self.num_timesteps >= self._stage2_at:
            self.training_env.env_method(
                "set_curriculum",
                self._stage2_levels,
                self._stage2_weights,
            )
            self._switched = True
            if self.verbose:
                wt = self._stage2_weights or ["uniform"] * len(self._stage2_levels)
                pairs = ", ".join(
                    f"{l}({w})" for l, w in zip(self._stage2_levels, wt)
                )
                print(
                    f"\n[curriculum] step={self.num_timesteps:,} → stage 2: {pairs}"
                )
        return True


def _find_render_lib() -> str:
    """Find libmicrones_rl_render.{dylib,so} next to the fast training lib."""
    repo_root = Path(__file__).parent.parent
    for d in [repo_root / "build-host", repo_root / "build"]:
        for suf in [".dylib", ".so"]:
            p = d / f"libmicrones_rl_render{suf}"
            if p.exists():
                return str(p)
    raise FileNotFoundError(
        "libmicrones_rl_render not found. Build with:\n"
        "  cmake --build build-host -j --target micrones_rl_render"
    )


class EvalVideoCallback(BaseCallback):
    """
    Saves a checkpoint and records one deterministic eval episode per level as MP4
    every `eval_interval` training timesteps.

    Args:
        rom_path:        Path to SMB1 .nes ROM.
        checkpoint_dir:  Directory for model checkpoints.
        video_dir:       Directory for eval MP4 files.
        eval_interval:   Timesteps between evaluations (default 1_000_000).
        eval_levels:     List of level strings to evaluate (default ["1-1"]).
        video_fps:       Output video frame rate (default 60 — one frame per NES tick).
        max_eval_steps:  Max env steps per eval episode (default 5_000).
        render_lib_path: Override path to libmicrones_rl_render. Auto-detected if None.
        verbose:         SB3 verbosity level.
    """

    def __init__(
        self,
        rom_path: str,
        checkpoint_dir: str = "checkpoints",
        video_dir: str = "eval_videos",
        eval_interval: int = 1_000_000,
        eval_levels: list[str] | None = None,
        video_fps: int = 60,
        max_eval_steps: int = 5_000,
        render_lib_path: str | None = None,
        verbose: int = 1,
    ):
        super().__init__(verbose)
        self._rom_path       = rom_path
        self._checkpoint_dir = checkpoint_dir
        self._video_dir      = video_dir
        self._eval_interval  = eval_interval
        self._eval_levels    = eval_levels if eval_levels else ["1-1"]
        self._video_fps      = video_fps
        self._max_eval_steps = max_eval_steps
        self._render_lib     = render_lib_path or _find_render_lib()
        self._next_eval_at   = eval_interval

    def _on_training_start(self) -> None:
        os.makedirs(self._checkpoint_dir, exist_ok=True)
        os.makedirs(self._video_dir,      exist_ok=True)
        # On resume num_timesteps > 0; snap to the next eval boundary from here.
        # Pre-fill window with current max_x_seen so we don't false-trigger
        # on the first check when resuming from a checkpoint
        try:
            current = float(self.training_env.get_attr("max_x_seen")[0])
            for _ in range(self._window_size):
                self._rew_window.append(current)
        except Exception as e:
            import logging
            logging.getLogger(__name__).warning(
                f"[EntropyScheduler] could not pre-fill window from max_x_seen: {e}. "
                f"Scheduler may false-trigger on first check."
            )
        n = self.num_timesteps
        self._next_eval_at = (n // self._eval_interval + 1) * self._eval_interval
        # Baseline: record one video per eval level before any training begins.
        if self.verbose:
            print(f"[eval] recording baseline videos (step={n:,}) ...")
        for level in self._eval_levels:
            self._eval_level(n, level)

    def _on_step(self) -> bool:
        if self.num_timesteps >= self._next_eval_at:
            self._run_eval(self.num_timesteps)
            self._next_eval_at += self._eval_interval
        return True

    def _on_training_end(self) -> None:
        # Always save a final checkpoint when training finishes.
        step = self.num_timesteps
        path = os.path.join(self._checkpoint_dir, f"model_{step:010d}_final")
        self.model.save(path)
        if self.verbose:
            print(f"[eval] final model saved → {path}.zip")

    # ------------------------------------------------------------------
    def _run_eval(self, step: int) -> None:
        # 1. Save checkpoint.
        ckpt_path = os.path.join(self._checkpoint_dir, f"model_{step:010d}")
        self.model.save(ckpt_path)
        if self.verbose:
            print(f"\n[eval] step={step:,}  checkpoint → {ckpt_path}.zip")

        # 2. Evaluate each level.
        for level in self._eval_levels:
            self._eval_level(step, level)

    def _eval_level(self, step: int, level: str) -> None:
        """Run one deterministic episode on `level` and write an MP4."""
        env = SMBEnv(
            rom_path    = self._rom_path,
            lib_path    = self._render_lib,
            render_mode = "rgb_array",
        )

        frames: list[np.ndarray] = []
        total_reward = 0.0

        try:
            obs, _ = env.reset(options={"level": level})
            for _ in range(self._max_eval_steps):
                action, _ = self.model.predict(obs, deterministic=True)
                obs, reward, terminated, truncated, _ = env.step(int(action))
                total_reward += float(reward)

                frames.extend(env.pop_step_frames())

                if terminated or truncated:
                    break
        finally:
            env.close()

        # 3. Write MP4.
        level_tag = level.replace("-", "_")
        video_path = os.path.join(self._video_dir, f"eval_{level_tag}_{step:010d}.mp4")
        if frames:
            with imageio.get_writer(
                video_path,
                fps     = self._video_fps,
                codec   = "libx264",
                quality = 8,
            ) as writer:
                for f in frames:
                    writer.append_data(f)
            if self.verbose:
                print(f"[eval] {level}  {len(frames)} frames  "
                      f"reward={total_reward:.1f}  → {video_path}")
        else:
            if self.verbose:
                print(f"[eval] {level}  no frames captured (framebuffer disabled?)")


class BestRunRecorderCallback(BaseCallback):
    """
    Records the best training episode from env-0 as a .npy actions file + MP4.

    Watches env-0's world_x throughout training. When an episode ends with a
    max_x that exceeds the previous best by at least `min_improvement`, the
    episode's action sequence is replayed in a fresh render env and saved as
    an MP4, along with a .npy of the action array (dtype int32).

    Replay and video write run in a background thread so training is not
    blocked. If a new best arrives before the prior save thread finishes, the
    callback joins the thread first (serialised saves, never concurrent).

    Args:
        rom_path:            Path to SMB1 .nes ROM.
        level:               Level string for env-0 (e.g. "1-1"). Used when
                             replaying the episode. Should match the level
                             env-0 trains on; defaults to "1-1".
        video_dir:           Directory for output files (default "best_runs").
        min_improvement:     Min max_x gain over previous best to trigger a
                             save (default 50.0).
        initial_best_max_x:  Pre-seed the best threshold — useful when resuming
                             training so early episodes don't re-trigger for
                             already-known territory (default 0.0).
        video_fps:           Output video frame rate (default 60).
        render_lib_path:     Override path to libmicrones_rl_render.
                             Auto-detected if None.
        verbose:             SB3 verbosity level.
    """

    def __init__(
        self,
        rom_path:            str,
        level:               str   = "1-1",
        video_dir:           str   = "best_runs",
        min_improvement:     float = 50.0,
        initial_best_max_x:  float = 0.0,
        video_fps:           int   = 60,
        render_lib_path:     str | None = None,
        verbose:             int   = 1,
    ):
        super().__init__(verbose)
        self._rom_path           = rom_path
        self._level              = level
        self._video_dir          = video_dir
        self._min_improvement    = min_improvement
        self._video_fps          = video_fps
        self._render_lib         = render_lib_path or _find_render_lib()

        self._best_max_x: float          = initial_best_max_x
        self._episode_actions: list[int] = []
        self._episode_max_x: float       = 0.0
        self._bg_thread: threading.Thread | None = None

    # ------------------------------------------------------------------
    def _on_training_start(self) -> None:
        os.makedirs(self._video_dir, exist_ok=True)

    def _on_step(self) -> bool:
        action = int(self.locals["actions"][0])
        info   = self.locals["infos"][0]
        done   = bool(self.locals["dones"][0])

        self._episode_actions.append(action)
        wx = float(info.get("world_x", 0.0))
        if wx > self._episode_max_x:
            self._episode_max_x = wx

        if done:
            if self._episode_max_x >= self._best_max_x + self._min_improvement:
                actions_snap = list(self._episode_actions)
                max_x_snap   = self._episode_max_x
                step_snap    = self.num_timesteps
                old_best     = self._best_max_x
                self._best_max_x = self._episode_max_x
                self._save_async(actions_snap, max_x_snap, step_snap)
                if self.verbose:
                    print(
                        f"\n[best_run] new best x={max_x_snap:.0f} "
                        f"(prev={old_best:.0f}, +{max_x_snap - old_best:.0f}) "
                        f"step={step_snap:,} — saving ..."
                    )

            self._episode_actions = []
            self._episode_max_x   = 0.0

        return True

    def _on_training_end(self) -> None:
        if self._bg_thread is not None and self._bg_thread.is_alive():
            self._bg_thread.join()

    # ------------------------------------------------------------------
    def _save_async(self, actions: list[int], max_x: float, step: int) -> None:
        if self._bg_thread is not None and self._bg_thread.is_alive():
            self._bg_thread.join()
        self._bg_thread = threading.Thread(
            target=self._save_run,
            args=(actions, max_x, step),
            daemon=True,
        )
        self._bg_thread.start()

    def _save_run(self, actions: list[int], max_x: float, step: int) -> None:
        tag = f"step{step:010d}_x{int(max_x)}"

        # Save action sequence.
        npy_path = os.path.join(self._video_dir, f"best_{tag}.npy")
        np.save(npy_path, np.array(actions, dtype=np.int32))

        # Replay through a fresh render env.
        env = SMBEnv(
            rom_path    = self._rom_path,
            lib_path    = self._render_lib,
            render_mode = "rgb_array",
        )
        frames: list[np.ndarray] = []
        try:
            env.reset(options={"level": self._level})
            for a in actions:
                _, _, terminated, truncated, _ = env.step(a)
                frames.extend(env.pop_step_frames())
                if terminated or truncated:
                    break
        finally:
            env.close()

        if not frames:
            if self.verbose:
                print(f"[best_run] WARNING: no frames captured for {tag} "
                      f"(render lib missing framebuffer?)")
            return

        # Write MP4.
        video_path = os.path.join(self._video_dir, f"best_{tag}.mp4")
        with imageio.get_writer(
            video_path,
            fps     = self._video_fps,
            codec   = "libx264",
            quality = 8,
        ) as writer:
            for f in frames:
                writer.append_data(f)

        if self.verbose:
            print(
                f"[best_run] saved: x={max_x:.0f}  {len(frames)} frames  "
                f"step={step:,}\n"
                f"         npy → {npy_path}\n"
                f"         mp4 → {video_path}"
            )


class RNDTrainerCallback(BaseCallback):
    """
    Trains the RND predictor network after each PPO rollout.

    Fires in _on_rollout_end (after rollout collection and GAE computation,
    before PPO's policy gradient update).  This ordering ensures:
      1. The predictor is trained on the observations just collected.
      2. Intrinsic rewards for the NEXT rollout reflect the current predictor.
      3. The PPO gradient update uses the advantages computed during rollout
         (which include the intrinsic rewards baked in by RNDWrapper).

    After training, the updated predictor weights are synced to all
    SubprocVecEnv worker processes via env_method("sync_rnd_predictor").
    This keeps the per-worker predictors current so intrinsic rewards
    reflect genuine novelty rather than the frozen initial state.

    TensorBoard metrics logged:
      rnd/predictor_loss          — rolling mean over recent minibatches
      rnd/intrinsic_reward_mean   — mean intrinsic across envs this step
      rnd/intrinsic_reward_max    — max intrinsic this step (spikes = novel state)

    Args:
        rnd_target:     Frozen RNDNetwork (never trained here).
        rnd_predictor:  Trainable RNDNetwork (trained here each rollout).
        lr:             Adam learning rate for the predictor (default 1e-4).
        batch_size:     Minibatch size for predictor updates (default 256).
        n_epochs:       Passes over the rollout buffer per rollout (default 4).
        device:         torch device string.
        max_grad_norm:  Gradient clipping norm (default 0.5).
    """

    def __init__(
        self,
        rnd_target,
        rnd_predictor,
        lr:            float = 1e-4,
        batch_size:    int   = 256,
        n_epochs:      int   = 4,
        device:        str   = "cpu",
        max_grad_norm: float = 0.5,
        verbose:       int   = 0,
    ):
        super().__init__(verbose)
        self.rnd_target     = rnd_target
        self.rnd_predictor  = rnd_predictor
        self.batch_size     = batch_size
        self.n_epochs       = n_epochs
        self._device        = device
        self.max_grad_norm  = max_grad_norm

        import torch.optim as optim
        self.optimizer = optim.Adam(rnd_predictor.parameters(), lr=lr)
        self._predictor_losses: list[float] = []

    # ------------------------------------------------------------------
    def _on_rollout_end(self) -> bool:
        """
        Train the predictor on the most recently collected rollout.

        The rollout buffer stores a Dict of obs arrays keyed by observation
        space key.  We flatten all keys (sorted for determinism) into a
        single (N, flat_dim) float32 matrix, then do n_epochs minibatch
        SGD updates of the predictor to minimise MSE with the frozen target.
        """
        raw_buf = self.model.rollout_buffer.observations

        # Flatten Dict obs buffer → (n_steps * n_envs, flat_dim).
        if isinstance(raw_buf, dict):
            obs_array = flatten_obs_batch(raw_buf)          # (N, flat_dim)
        else:
            # Fallback for non-Dict obs (shouldn't happen with this env).
            obs_array = raw_buf.reshape(raw_buf.shape[0] * raw_buf.shape[1], -1).astype(np.float32)

        n_samples = obs_array.shape[0]

        for _ in range(self.n_epochs):
            perm = th.randperm(n_samples)
            for start in range(0, n_samples, self.batch_size):
                idx   = perm[start : start + self.batch_size]
                batch = th.from_numpy(obs_array[idx]).to(self._device)   # (B, flat_dim)

                with th.no_grad():
                    feat_target = self.rnd_target(batch)

                feat_pred = self.rnd_predictor(batch)
                loss = F.mse_loss(feat_pred, feat_target)

                self.optimizer.zero_grad()
                loss.backward()
                th.nn.utils.clip_grad_norm_(
                    self.rnd_predictor.parameters(), self.max_grad_norm
                )
                self.optimizer.step()

                self._predictor_losses.append(float(loss.item()))

        # Log rolling mean of recent losses.
        if self._predictor_losses:
            self.logger.record(
                "rnd/predictor_loss",
                float(np.mean(self._predictor_losses[-100:])),
            )

        # Memory management: keep only recent history.
        if len(self._predictor_losses) > 1000:
            self._predictor_losses = self._predictor_losses[-500:]

        # Sync updated predictor weights to all SubprocVecEnv workers.
        # Convert to numpy for cloudpickle-safe IPC.
        try:
            state_dict_numpy = {
                k: v.detach().cpu().numpy()
                for k, v in self.rnd_predictor.state_dict().items()
            }
            self.training_env.env_method("sync_rnd_predictor", state_dict_numpy)
        except Exception as exc:
            # env_method may fail if the env stack doesn't have sync_rnd_predictor
            # (e.g. during unit tests or when RND is disabled).  Non-fatal.
            if self.verbose:
                print(f"[RNDTrainerCallback] weight sync skipped: {exc}")

        return True

    # ------------------------------------------------------------------
    def _on_step(self) -> bool:
        """Log per-step intrinsic reward statistics."""
        infos = self.locals.get("infos", [])
        intrinsic_vals = [
            float(info.get("intrinsic_reward", 0.0))
            for info in infos
            if "intrinsic_reward" in info
        ]

        if intrinsic_vals:
            self.logger.record("rnd/intrinsic_reward_mean", float(np.mean(intrinsic_vals)))
            self.logger.record("rnd/intrinsic_reward_max",  float(np.max(intrinsic_vals)))

        return True
