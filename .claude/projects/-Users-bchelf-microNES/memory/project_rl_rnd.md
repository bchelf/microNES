---
name: RND implementation added to SMB RL stack
description: Random Network Distillation (MLP-based) added as intrinsic curiosity reward. Files created: rnd.py, running_mean_std.py. Files modified: wrappers.py, eval_callback.py, train_smb_rl.py, CLAUDE.md.
type: project
---

RND (Random Network Distillation) intrinsic motivation was added to the SMB PPO training stack (2026-03-19).

**Why:** Agent was stuck at a hard obstacle requiring precise timing. RND provides novelty rewards to pull it toward unexplored territory.

**How to apply:** RND is enabled by default. Use `--no-rnd` to disable, `--rnd-scale` to tune. Monitor `diagnostics/intrinsic_extrinsic_ratio` — target 0.3–1.0.

Key facts:
- RND uses MLP (not CNN) because obs are structured RAM/PPU features, flat dim ≈ 371
- RNDWrapper is outermost single-env wrapper (inside SubprocVecEnv)
- RNDTrainerCallback must be FIRST in callback list
- After each rollout, predictor weights are synced to subprocesses via env_method("sync_rnd_predictor")
- max_x_seen initialized to 905 (known frontier from prior runs) via vec_env.env_method("__setattr__", "max_x_seen", 905) — remove for fresh starts
- intrinsic reward is NON-episodic (_ret not reset on episode end) — this is intentional
