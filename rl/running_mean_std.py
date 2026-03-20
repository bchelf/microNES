"""
RunningMeanStd — Welford's online algorithm for tracking mean and variance.

Used in RNDWrapper for:
  - Observation normalisation before passing to the RND networks
    (shape = (flat_obs_dim,), updated every env step)
  - Intrinsic reward normalisation via discounted intrinsic return
    (shape = (), updated every env step)

The parallel (batch) variant of Welford's update is used so that both
single-sample and multi-sample updates produce identical results.
"""

import numpy as np


class RunningMeanStd:
    """
    Incremental mean and variance tracker using Welford's parallel algorithm.

    Args:
        shape: Shape of the quantity being tracked.  Use () for scalars,
               (D,) for vectors, (C, H, W) for feature maps, etc.
               Defaults to ().
    """

    def __init__(self, shape: tuple = ()):
        self.mean:  np.ndarray = np.zeros(shape, dtype=np.float64)
        self.var:   np.ndarray = np.ones(shape,  dtype=np.float64)
        self.count: float      = 1e-4   # small non-zero start avoids /0 on first call

    # ------------------------------------------------------------------
    def update(self, x: np.ndarray) -> None:
        """
        Update statistics with a batch of samples.

        Args:
            x: Array whose first axis is the batch dimension.
               For shape=() pass x with shape (N,).
               For shape=(D,) pass x with shape (N, D).
               Single samples are fine: shape (1,) or (1, D).
        """
        x = np.asarray(x, dtype=np.float64)

        # Reshape to (batch_size, *self.mean.shape) so axis-0 is always batch.
        if self.mean.shape:
            batch = x.reshape(-1, *self.mean.shape)
        else:
            batch = x.reshape(-1)

        batch_count = float(batch.shape[0])
        batch_mean  = batch.mean(axis=0)
        batch_var   = batch.var(axis=0, ddof=0)

        # Parallel Welford update.
        delta     = batch_mean - self.mean
        new_count = self.count + batch_count

        self.mean = self.mean + delta * (batch_count / new_count)

        # Combine M2 (sum of squared deviations) from both sides:
        #   M2 = M2_a + M2_b + delta^2 * n_a * n_b / (n_a + n_b)
        m2_a = self.var   * self.count
        m2_b = batch_var  * batch_count
        self.var = (m2_a + m2_b + delta ** 2 * self.count * batch_count / new_count) / new_count

        self.count = new_count

    # ------------------------------------------------------------------
    @property
    def std(self) -> np.ndarray:
        """Standard deviation with a small epsilon for numerical stability."""
        return np.sqrt(self.var + 1e-8)
