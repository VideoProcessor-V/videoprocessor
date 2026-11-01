<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TemporalDetail: regional adaptation (transfer ceiling, oracle, fitted local rule)

Per-block strength a_b on the model's transfer-ceiling output (forceWeight 1, boost off). Train cases: boat_1080p_low, boat_2160p_low, tos_1080p_low, tos_2160p_low; held-out: crowd_1080p_low, crowd_2160p_low, sol_1080p_low, sol_2160p_low. PSNR change on B-frames (and all frames) vs the decoded input; MSE in block-mean form over the 960x544 crops (frames from the 3rd on).

Fitted rule a_b = clamp(c . f, 0, 1): bias -0.7797, deficit +0.3326, signif -0.0041, trust +1.1822, grain +0.0418, dark -0.3051, phase +1.4856

| case | set | ceiling a=1 (B) | global best const a (B) | oracle a* (B) | rule (B) | rule (all) | rule mean a |
|---|---|---|---|---|---|---|---|
| boat_1080p_low | train | -0.001 | +0.011 (a=0.50) | +0.051 | +0.014 | +0.010 | 0.401 |
| boat_2160p_low | train | +0.002 | +0.009 (a=0.50) | +0.044 | +0.011 | +0.007 | 0.456 |
| tos_1080p_low | train | -0.024 | +0.004 (a=0.30) | +0.041 | +0.004 | +0.002 | 0.376 |
| tos_2160p_low | train | -0.029 | +0.000 (a=0.05) | +0.026 | -0.010 | -0.007 | 0.502 |
| crowd_1080p_low | test | -0.039 | +0.000 (a=0.00) | +0.039 | -0.016 | -0.014 | 0.520 |
| crowd_2160p_low | test | -0.003 | +0.005 (a=0.45) | +0.054 | +0.005 | +0.003 | 0.564 |
| sol_1080p_low | test | -0.033 | +0.000 (a=0.05) | +0.032 | -0.017 | -0.015 | 0.649 |
| sol_2160p_low | test | -0.009 | +0.002 (a=0.30) | +0.039 | -0.004 | -0.004 | 0.753 |
| **mean train** | | -0.013 | +0.006 | +0.041 | +0.005 | +0.003 | 0.434 |
| **mean test** | | -0.021 | +0.002 | +0.041 | -0.008 | -0.007 | 0.621 |

