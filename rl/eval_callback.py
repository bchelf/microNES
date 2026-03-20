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
import warnings
from collections import deque
from pathlib import Path
from typing import Any

import imageio
import numpy as np
import torch as th
from stable_baselines3.common.callbacks import BaseCallback

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
        self._max_episode_steps: float = 600.0   # overwritten in _on_training_start

    # ------------------------------------------------------------------
    def _on_training_start(self) -> None:
        n = self.training_env.num_envs
        self._ep_max_x               = [0.0]   * n
        self._ep_steps               = [0]     * n
        self._ep_death_flag          = [False] * n
        self._ep_level_complete_flag = [False] * n

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

            # Reset accumulators for this env slot.
            self._ep_max_x[i]               = 0.0
            self._ep_steps[i]               = 0
            self._ep_death_flag[i]          = False
            self._ep_level_complete_flag[i] = False

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
