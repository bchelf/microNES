"""
Gym wrappers for SMBEnv training.

# ============================================================
# DEATH DETECTION AUDIT (conducted 2026-03-19)
# ============================================================
#
# 1. TERMINATION SIGNAL
#    terminated = dead OR complete  (smb_env.py:343)
#    truncated  = max_steps | stagnation | steps_since_progress (smb_env.py:350-354)
#
# 2. DEATH / COMPLETION KEYS IN `info`
#    NONE. The step() info dict contains only: world_x, frame, stagnating.
#    Neither info["death"] nor info["x_pos"] exist.
#    All wrappers below use obs["game_flags"][0] (dead) and
#    obs["game_flags"][1] (complete) directly from the observation.
#
# 3. DEATH DETECTION MECHANISM
#    player_dead    = (RAM[0x000E] == 0x0B)  — death animation state
#    level_complete = (RAM[0x001D] == 0x03)  — end-of-level game mode
#    Both are read in _get_obs() AFTER all action frames have executed.
#    There is no distinction between death-by-enemy and death-by-pit —
#    both result in state 0x0B. The two are treated identically.
#
# 4. OFF-BY-ONE RISK
#    None found. The step() call sequence is:
#      a) execute all NES frames for the action
#      b) _get_obs()              ← reads RAM after action
#      c) _compute_reward(obs)    ← uses obs["game_flags"] from (b)
#      d) dead = obs["game_flags"][0]  ← same obs
#    Reward, death flag, and x_pos are all consistent and from the same state.
#
# 5. x_pos AT DEATH STEP
#    world_x = _read_world_x() is called after all action frames complete.
#    It is the actual position at the moment of death, not the prior frame.
#
# 6. DEATH vs LEVEL-COMPLETE DISTINCTION
#    Distinguished: obs["game_flags"][0] = dead, obs["game_flags"][1] = complete.
#    They are mutually exclusive in practice (can't die and complete simultaneously).
#    No further sub-typing (enemy kill vs pit fall) is available from the RAM state.
#
# 7. BUGFIX: NewMaxXWrapper — wrong info key
#    Previous implementation read info.get("x_pos", 0.0). The env emits
#    info["world_x"], not info["x_pos"]. This caused max_x_seen to stay at
#    0.0 permanently, making the frontier bonus always 0. Fixed below.
#
# 8. FINDING: base env already applies -10.0 death penalty inside _compute_reward()
#    (smb_env.py:1231). DeathPenaltyWrapper replaces the entire reward on death
#    with -death_penalty to avoid double-penalizing and to cleanly remove the
#    x-progress component that is baked into the reward before this wrapper sees it.
#    Default death_penalty=10.0 matches the base env's value so the magnitude is
#    unchanged; the difference is that x-progress is no longer included.
# ============================================================
"""

import numpy as np
import torch
import torch.nn.functional as F
import gymnasium as gym

from running_mean_std import RunningMeanStd
from rnd import flatten_obs_single, flat_obs_dim_from_space


class NewMaxXWrapper(gym.Wrapper):
    """
    Adds a bonus reward whenever the agent surpasses its lifetime-maximum x position
    while Mario is on the ground.

    This creates a frontier signal: the agent is rewarded for reaching any x it has
    never visited before across all episodes. Because max_x_seen is never reset, the
    bonus is zero once the agent revisits old ground — it only fires at the frontier.
    This breaks the deterministic local optimum where reward gradient vanishes at the
    furthest point reached.

    ON-GROUND GATE (BUGFIX 2026-03-20):
    The frontier bonus and max_x_seen update are gated on Mario being on the ground
    (info["on_ground"], sourced from RAM[0x001D] == 0x00).  Without this gate,
    the bonus fired during void jumps where Mario falls rightward into a pit — the
    agent accumulated frontier reward for airborne x positions that were physically
    unreachable at ground level, incentivizing pit jumps rather than navigable
    progress.  With the gate, only ground-level x progress counts toward the frontier.

    Ground state source: info["on_ground"] (bool, True = on ground, False = airborne).
    RAM[0x001D]: 0x00 = on ground, 0x01 = airborne (jump OR pit fall), 0x03 = level
    complete.  Clears immediately on landing — no multi-frame delay.  Defaults to
    True if the key is absent (safe degradation: treats unknown state as grounded).

    Args:
        env:   The wrapped Gymnasium environment.
        scale: Multiplier applied to (current_x - max_x_seen) on a new record.
               Default: 2.0.
    """

    def __init__(self, env: gym.Env, scale: float = 2.0):
        super().__init__(env)
        self.scale = scale
        self.max_x_seen: float = 0.0  # lifetime max — intentionally never reset

    def reset(self, **kwargs):
        # Do NOT reset max_x_seen here. It is a lifetime maximum across all episodes.
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        # BUGFIX (original): env emits info["world_x"], not info["x_pos"].
        current_x = float(info.get("world_x", 0.0))
        # BUGFIX (2026-03-20): gate frontier bonus on Mario being on the ground.
        # info["on_ground"] is set by SMBEnv.step() from RAM[0x001D] == 0x00.
        # Default True so the wrapper degrades gracefully if the key is ever absent.
        on_ground = bool(info.get("on_ground", True))

        bonus = 0.0
        if on_ground and current_x > self.max_x_seen:
            bonus = (current_x - self.max_x_seen) * self.scale
            self.max_x_seen = current_x

        # Always update info["max_x_seen"] so diagnostics remain accurate regardless
        # of the on_ground gate.
        info["max_x_seen"]            = self.max_x_seen
        info["mario_on_ground"]       = on_ground
        info["frontier_bonus_blocked"] = (not on_ground) and (current_x > self.max_x_seen)
        return obs, reward + bonus, terminated, truncated, info


class SurvivalBonusWrapper(gym.Wrapper):
    """
    Adds a small reward every timestep Mario is alive.

    Encourages patience and penalizes the "run right and die far away" strategy.
    Without a per-step survival signal, an agent can greedily maximise x-progress
    by sprinting right and dying at the furthest reachable point — the x-reward on
    the death step is still positive. A survival bonus makes staying alive
    intrinsically valuable and creates a gradient against reckless play.

    The bonus is NOT applied on the terminal death step or the level-complete step:
      - Death step: should be penalised, not rewarded for surviving.
      - Level-complete step: already rewarded by the completion bonus in the base env.

    Detection uses obs["game_flags"][0] (dead) and obs["game_flags"][1] (complete),
    which are always present in SMBEnv observations. No info key is used because
    the audit found no death/complete keys in the step() info dict.

    Args:
        env:            The wrapped Gymnasium environment.
        survival_bonus: Reward added per alive, non-terminal step. Default: 0.02.
    """

    def __init__(self, env: gym.Env, survival_bonus: float = 0.02):
        super().__init__(env)
        self.survival_bonus = survival_bonus
        self._total_survival_bonus: float = 0.0

    def reset(self, **kwargs):
        self._total_survival_bonus = 0.0
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        dead     = obs["game_flags"][0] > 0.5
        complete = obs["game_flags"][1] > 0.5

        if not dead and not complete:
            reward += self.survival_bonus
            self._total_survival_bonus += self.survival_bonus

        info["total_survival_bonus"] = self._total_survival_bonus
        return obs, reward, terminated, truncated, info


class DeathPenaltyWrapper(gym.Wrapper):
    """
    Adds a flat penalty on the death step without touching the x-progress reward.

    The penalty makes death costly, but the x-reward (and all other shaped
    components) is preserved. The new-max-x bonus from NewMaxXWrapper already
    handles the "void-jump" problem more elegantly: it only rewards genuinely
    new ground, so dying far right is only rewarded once per frontier pixel.
    Stripping x-reward on death is therefore unnecessary and was found to
    interact poorly with the survival + frontier signal stack.

    The base env applies its own -10.0 death term inside _compute_reward()
    (smb_env.py:1231). This wrapper's penalty is ADDITIVE with that. With the
    default death_penalty=4.0 the total on-death penalty is roughly -14.0,
    which is bad-but-not-catastrophic relative to the survival bonus scale.

    On level-complete and alive steps the reward is passed through unchanged.
    Detection uses obs["game_flags"][0] (dead) and obs["game_flags"][1] (complete).

    Args:
        env:           The wrapped Gymnasium environment.
        death_penalty: Extra penalty added on the death step (applied as negative).
                       Default: 4.0.
    """

    def __init__(self, env: gym.Env, death_penalty: float = 4.0):
        super().__init__(env)
        self.death_penalty = death_penalty

    def reset(self, **kwargs):
        obs, info = self.env.reset(**kwargs)
        return obs, info

    def step(self, action):
        obs, reward, terminated, truncated, info = self.env.step(action)
        dead     = obs["game_flags"][0] > 0.5
        complete = obs["game_flags"][1] > 0.5

        if dead and not complete:
            reward -= self.death_penalty
            info["death_penalty_applied"] = True
        else:
            info["death_penalty_applied"] = False

        # Expose level_complete so callbacks can distinguish complete from truncation
        # without needing to inspect obs["game_flags"] directly.  No reward impact.
        info["level_complete"] = complete

        return obs, reward, terminated, truncated, info


class RNDWrapper(gym.Wrapper):
    """
    Adds Random Network Distillation (RND) intrinsic rewards on top of
    extrinsic rewards from the underlying environment.

    RND works by training a small *predictor* MLP to match the outputs of
    a frozen *target* MLP on visited observations.  For novel states the
    predictor has high error (high intrinsic reward); for familiar states
    the predictor matches the target well (low intrinsic reward).  This
    pulls the agent toward unexplored territory even when extrinsic reward
    gradients have vanished.

    Non-episodic intrinsic reward (correct RND behaviour):
        The discounted intrinsic return self._ret is NOT reset on episode
        end by default.  This is intentional: curiosity should persist
        across episode boundaries.  Mario dying should not erase the
        novelty signal — the agent still needs to get past that point.
        Set episodic_intrinsic=True only for debugging.

    Observation normalisation:
        A per-instance RunningMeanStd tracks the observation statistics.
        Observations are centred and scaled before being passed to the
        RND networks.  This prevents any single high-magnitude feature
        from dominating the RND signal.

    Subprocess weight synchronisation:
        When used inside SubprocVecEnv, each worker has its own copy of
        the RND networks.  The predictor in each worker starts as a copy
        of the main-process predictor at env-creation time and diverges
        as the main process trains its own predictor.  Call
        sync_rnd_predictor() via env_method() after each predictor
        training round to push updated weights into the workers.

    Info keys added:
        intrinsic_reward  (float) — normalised intrinsic reward this step
        extrinsic_reward  (float) — raw extrinsic reward from inner env
        raw_intrinsic     (float) — unnormalised MSE before scaling

    Args:
        env:               Wrapped Gymnasium environment.
        rnd_target:        Frozen RNDNetwork (requires_grad=False everywhere).
        rnd_predictor:     Trainable RNDNetwork (trained by RNDTrainerCallback).
        intrinsic_scale:   Weight on the normalised intrinsic reward (default 1.0).
        obs_norm_clip:     Clip range for normalised obs (default 5.0).
        reward_norm_gamma: Discount for intrinsic return normaliser (default 0.99).
        episodic_intrinsic: If True, reset the intrinsic return accumulator on
                            episode end (default False = non-episodic).
        device:            torch device string matching the RND networks.
    """

    def __init__(
        self,
        env: gym.Env,
        rnd_target,
        rnd_predictor,
        intrinsic_scale:    float = 1.0,
        obs_norm_clip:      float = 5.0,
        reward_norm_gamma:  float = 0.99,
        episodic_intrinsic: bool  = False,
        device:             str   = "cpu",
    ):
        super().__init__(env)
        self.rnd_target        = rnd_target
        self.rnd_predictor     = rnd_predictor
        self.intrinsic_scale   = intrinsic_scale
        self._obs_norm_clip    = obs_norm_clip
        self._gamma            = reward_norm_gamma
        self._episodic         = episodic_intrinsic
        self._device           = device

        flat_dim = flat_obs_dim_from_space(env.observation_space)
        self.obs_rms    = RunningMeanStd(shape=(flat_dim,))
        self.reward_rms = RunningMeanStd(shape=())
        self._ret: float = 0.0

    # ------------------------------------------------------------------
    def reset(self, *, seed=None, options=None):
        obs, info = self.env.reset(seed=seed, options=options)
        # obs_rms and reward_rms are lifetime statistics — never reset.
        # _ret: non-episodic by default; only reset if explicitly requested.
        if self._episodic:
            self._ret = 0.0
        return obs, info

    def step(self, action):
        obs, extrinsic_reward, terminated, truncated, info = self.env.step(action)

        # Compute intrinsic reward from RND networks.
        raw_intrinsic = self._compute_intrinsic(obs)

        # Normalise intrinsic reward via discounted intrinsic return.
        self._ret = self._ret * self._gamma + raw_intrinsic
        self.reward_rms.update(np.array([self._ret]))
        intrinsic_norm = raw_intrinsic / (np.sqrt(self.reward_rms.var[()]) + 1e-8)

        # Non-episodic: only reset on done if episodic mode is explicitly on.
        if self._episodic and (terminated or truncated):
            self._ret = 0.0

        total_reward = float(extrinsic_reward) + self.intrinsic_scale * float(intrinsic_norm)

        info["intrinsic_reward"] = float(intrinsic_norm)
        info["extrinsic_reward"] = float(extrinsic_reward)
        info["raw_intrinsic"]    = float(raw_intrinsic)

        return obs, total_reward, terminated, truncated, info

    # ------------------------------------------------------------------
    def _compute_intrinsic(self, obs: dict) -> float:
        """
        Compute raw intrinsic reward for one observation.

        The obs dict is flattened to a 1-D vector, normalised via obs_rms,
        and passed through the frozen target and trainable predictor
        networks.  The MSE between their outputs is the intrinsic reward.

        No gradients are computed here — the predictor is trained
        separately in RNDTrainerCallback._on_rollout_end.
        """
        flat = flatten_obs_single(obs).astype(np.float32)

        # Update running observation statistics.
        self.obs_rms.update(flat[np.newaxis])   # shape (1, flat_dim)

        # Normalise and clip.
        obs_norm = np.clip(
            (flat - self.obs_rms.mean.astype(np.float32)) / self.obs_rms.std.astype(np.float32),
            -self._obs_norm_clip,
            self._obs_norm_clip,
        ).astype(np.float32)

        obs_t = torch.from_numpy(obs_norm).unsqueeze(0).to(self._device)   # (1, flat_dim)

        with torch.no_grad():
            feat_target = self.rnd_target(obs_t)
            feat_pred   = self.rnd_predictor(obs_t)

        return float(F.mse_loss(feat_pred, feat_target).item())

    # ------------------------------------------------------------------
    def sync_rnd_predictor(self, state_dict_numpy: dict) -> None:
        """
        Load predictor weights pushed from the main process.

        Called via SubprocVecEnv.env_method("sync_rnd_predictor", weights)
        after RNDTrainerCallback trains the main-process predictor.
        This keeps the subprocess predictors current so intrinsic rewards
        reflect genuine novelty rather than the initial random state.

        Args:
            state_dict_numpy: Dict mapping parameter name → numpy array
                              (serialised from the main-process predictor's
                              state_dict via .cpu().numpy()).
        """
        state_dict = {
            k: torch.from_numpy(v).to(self._device)
            for k, v in state_dict_numpy.items()
        }
        self.rnd_predictor.load_state_dict(state_dict)

    def sync_rnd_target(self, state_dict_numpy: dict) -> None:
        """
        Load target weights pushed from the main process (called once at startup).

        Workers create their own randomly-initialized target; this sync ensures
        all workers use the same frozen target as the main process. Without it,
        the main-process predictor (trained against the main target) would produce
        wrong intrinsic rewards when synced to workers with different targets.

        Args:
            state_dict_numpy: Dict mapping parameter name → numpy array
                              (serialised from the main-process target's
                              state_dict via .cpu().numpy()).
        """
        state_dict = {
            k: torch.from_numpy(v).to(self._device)
            for k, v in state_dict_numpy.items()
        }
        self.rnd_target.load_state_dict(state_dict)
        self.rnd_target.eval()  # target must stay in eval mode
