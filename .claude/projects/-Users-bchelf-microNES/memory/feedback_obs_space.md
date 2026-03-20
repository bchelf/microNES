---
name: Obs space is Dict structured features, not pixels
description: SMBEnv uses a 20-key Dict observation space with RAM/PPU features. Any model or wrapper expecting pixel input (CNN, /255 normalisation) needs to be adapted to MLP + flat feature vector.
type: feedback
---

The SMBEnv observation space is `gymnasium.spaces.Dict` with 20 keys totalling ≈ 371 float32 features. It is NOT a pixel buffer.

**Why:** The emulator provides structured RAM features (player state, tile grid, enemies, etc.) rather than raw pixels. The policy uses MultiInputPolicy (SB3), which handles Dict obs via separate MLP encoders.

**How to apply:** Any time a new network or wrapper is added that processes observations:
1. Do NOT use CNN layers or divide by 255
2. DO flatten the Dict to a 1-D vector using `flatten_obs_single(obs_dict)` from `rnd.py` (keys sorted for determinism)
3. Compute flat dim dynamically via `flat_obs_dim_from_space(env.observation_space)` — never hardcode 371
4. For rollout buffer access: use `flatten_obs_batch(obs_dict)` which handles the (n_steps, n_envs, *shape) layout
