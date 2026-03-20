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

import gymnasium as gym


class NewMaxXWrapper(gym.Wrapper):
    """
    Adds a bonus reward whenever the agent surpasses its lifetime-maximum x position.

    This creates a frontier signal: the agent is rewarded for reaching any x it has
    never visited before across all episodes. Because max_x_seen is never reset, the
    bonus is zero once the agent revisits old ground — it only fires at the frontier.
    This breaks the deterministic local optimum where reward gradient vanishes at the
    furthest point reached.

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
        # BUGFIX: env emits info["world_x"], not info["x_pos"].
        # Previous version read info.get("x_pos", 0.0) and always got 0.0.
        current_x = float(info.get("world_x", 0.0))
        bonus = 0.0
        if current_x > self.max_x_seen:
            bonus = (current_x - self.max_x_seen) * self.scale
            self.max_x_seen = current_x
        info["max_x_seen"] = self.max_x_seen
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
