<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_rerun_crowd_1080p`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008_trust1 |
|---|---|---|
| psnr_B_Y | 28.2453 | 28.2533 |
| psnr_P_Y | 29.263 | 29.2352 |
| psnr_I_Y | 31.9908 | 31.9847 |
| psnr_Y | 28.5169 | 28.5172 |
| ssim_B_Y | 0.800445 | 0.801459 |
| ssim_Y | 0.806901 | 0.807729 |
| lpips_B | 0.147005 | 0.145435 |
| lpips_all | 0.139355 | 0.13796 |
| ib_gap_db_Y | 1.12018 | 1.08501 |
| detail_pulse_Y | 0.172233 | 0.170972 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.0617728 | -0.0585519 |
| grain_keep_vs_input_Y | 1 | 1.02628 |
| grain_lag1_vs_input_Y | 0 | -0.00125674 |
| grain_keep_vs_input_band1_Y | 1 | 1.02492 |
| grain_keep_vs_input_band2_Y | 1 | 1.03508 |
| grain_keep_vs_input_band3_Y | 1 | 1.03498 |
| grain_keep_vs_input_band4_Y | 1 | 1.02699 |
| grain_probe_keep_edge_vs_input_Y | 1 | 1.02837 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA |
| grain_probe_keep_flat_vs_input_Y | 1 | 1.01416 |
| grain_probe_lag1_flat_vs_input_Y | 0 | 0.00015777 |
| ghost_coeff_Y | 0.186549 | 0.179541 |
| smear_Y | 0.657254 | 0.69203 |
| halo_overshoot_Y | 0.000366931 | 0.000441706 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 |
| cost_ms_per_frame_pyref | 0.000200002 | 517.087 |

## Per case

| content_type | clip | case | metric | default | td_b008_trust1 | flags |
|---|---|---|---|---|---|---|
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | 28.5169 | 28.5172 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | 0.806901 | 0.807729 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | 1 | 1.02628 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | 0 | -0.00125674 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | -0.0617728 | -0.0585519 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | 0.186549 | 0.179541 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | 0.657254 | 0.69203 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | 0.000366931 | 0.000441706 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | 0.139355 | 0.13796 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | 31.9908 | 31.9847 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | 29.263 | 29.2352 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | 28.2453 | 28.2533 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | 0.800445 | 0.801459 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | 0.147005 | 0.145435 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | 1.12018 | 1.08501 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | 0.172233 | 0.170972 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | 1 | 1.02492 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | 1 | 1.03508 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | 1 | 1.03498 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | 1 | 1.02699 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_edge_vs_input_Y | 1 | 1.02837 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_flat_vs_input_Y | 1 | 1.01416 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.00015777 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | 0.000200002 | 517.087 | td_b008_trust1:INFO |
