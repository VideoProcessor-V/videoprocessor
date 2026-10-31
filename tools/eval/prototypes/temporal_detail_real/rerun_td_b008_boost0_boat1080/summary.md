<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_rerun_boat_1080p`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008_boost0 |
|---|---|---|
| psnr_B_Y | 31.5496 | 31.5505 |
| psnr_P_Y | 32.5329 | 32.5327 |
| psnr_I_Y | 35.6116 | 35.6122 |
| psnr_Y | 31.8428 | 31.8435 |
| ssim_B_Y | 0.916379 | 0.916393 |
| ssim_Y | 0.920042 | 0.920053 |
| lpips_B | NA | NA |
| lpips_all | NA | NA |
| ib_gap_db_Y | 1.16339 | 1.16238 |
| detail_pulse_Y | 0.0589656 | 0.0587428 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.00688464 | -0.00687959 |
| grain_keep_vs_input_Y | 1 | 1.0001 |
| grain_lag1_vs_input_Y | 0 | -4.1381e-06 |
| grain_keep_vs_input_band1_Y | 1 | 1.0001 |
| grain_keep_vs_input_band2_Y | 1 | 1.0001 |
| grain_keep_vs_input_band3_Y | 1 | 1.00011 |
| grain_keep_vs_input_band4_Y | 1 | 1.0001 |
| ghost_coeff_Y | 0.0668913 | 0.0668233 |
| smear_Y | 0.836716 | 0.837013 |
| halo_overshoot_Y | 0.000299464 | 0.000299999 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 |
| cost_ms_per_frame_pyref | 0.000100001 | 1191.71 |

## Per case

| content_type | clip | case | metric | default | td_b008_boost0 | flags |
|---|---|---|---|---|---|---|
| nature30 | boat | x265_1080p_5k | psnr_Y | 31.8428 | 31.8435 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | ssim_Y | 0.920042 | 0.920053 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Y | 1 | 1.0001 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Y | 0 | -4.1381e-06 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_Y | 1 | 1 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | flicker_excess_Y | -0.00688464 | -0.00687959 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | gop_ratio_minigop | NA | NA | td_b008_boost0:NA |
| nature30 | boat | x265_1080p_5k | ghost_coeff_Y | 0.0668913 | 0.0668233 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | smear_Y | 0.836716 | 0.837013 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | halo_overshoot_Y | 0.000299464 | 0.000299999 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | lc_boost_fraction_Y | 0 | 0 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | lpips_all | NA | NA | td_b008_boost0:NA |
| nature30 | boat | x265_1080p_5k | psnr_I_Y | 35.6116 | 35.6122 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | psnr_P_Y | 32.5329 | 32.5327 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | psnr_B_Y | 31.5496 | 31.5505 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | ssim_B_Y | 0.916379 | 0.916393 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | lpips_B | NA | NA | td_b008_boost0:NA |
| nature30 | boat | x265_1080p_5k | ib_gap_db_Y | 1.16339 | 1.16238 | td_b008_boost0:INFO |
| nature30 | boat | x265_1080p_5k | detail_pulse_Y | 0.0589656 | 0.0587428 | td_b008_boost0:INFO |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band1_Y | 1 | 1.0001 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band2_Y | 1 | 1.0001 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band3_Y | 1 | 1.00011 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band4_Y | 1 | 1.0001 | td_b008_boost0:PASS |
| nature30 | boat | x265_1080p_5k | cost_ms_per_frame_pyref | 0.000100001 | 1191.71 | td_b008_boost0:INFO |
