<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_letterbox`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008 |
|---|---|---|
| psnr_B_Y | 48.8612 | 48.8465 |
| psnr_P_Y | 49.8744 | 49.8603 |
| psnr_I_Y | 48.6617 | 48.6534 |
| psnr_Y | 49.0863 | 49.0719 |
| ssim_B_Y | 0.992524 | 0.992518 |
| ssim_Y | 0.992765 | 0.992759 |
| lpips_B | 0.0277534 | 0.0276911 |
| lpips_all | 0.0265906 | 0.0265299 |
| ib_gap_db_Y | 0.874889 | 0.876154 |
| detail_pulse_Y | 0.0549107 | 0.0584324 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.000848546 | -0.000764205 |
| grain_keep_vs_input_Y | 1 | 1.00143 |
| grain_lag1_vs_input_Y | 0 | 7.44523e-06 |
| grain_keep_vs_input_band1_Y | 1 | 1.00128 |
| grain_keep_vs_input_band2_Y | 1 | 1.00134 |
| grain_keep_vs_input_band3_Y | 1 | 1.00144 |
| grain_keep_vs_input_band4_Y | 1 | 1.0016 |
| grain_probe_keep_edge_vs_input_Y | 1 | 1.01476 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA |
| grain_probe_keep_flat_vs_input_Y | 1 | 1.00082 |
| grain_probe_lag1_flat_vs_input_Y | 0 | -1.53683e-06 |
| ghost_coeff_Y | 0.00353418 | 0.00295616 |
| smear_Y | 0.937493 | 0.951235 |
| halo_overshoot_Y | 5.39679e-05 | 6.6508e-05 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 |
| wtw_kept_vs_input_Y | NA | NA |
| cost_ms_per_frame_pyref | 0.0004 | 1109.44 |

## Per case

| content_type | clip | case | metric | default | td_b008 | flags |
|---|---|---|---|---|---|---|
| film24 | toslb | x265_2160p_12k | psnr_Y | 50.2948 | 50.2847 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | ssim_Y | 0.993876 | 0.993873 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1.00031 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_lag1_vs_input_Y | 0 | 4.75956e-06 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | wtw_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | toslb | x265_2160p_12k | flicker_excess_Y | -0.000496782 | -0.000492361 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | toslb | x265_2160p_12k | ghost_coeff_Y | 0.00298686 | 0.00284335 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | smear_Y | 0.927541 | 0.939612 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | halo_overshoot_Y | 7.21136e-05 | 8.87703e-05 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | lpips_all | 0.0283512 | 0.0283188 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | psnr_I_Y | 49.8237 | 49.8162 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | psnr_P_Y | 51.2609 | 51.2503 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | psnr_B_Y | 50.0228 | 50.0127 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | ssim_B_Y | 0.993596 | 0.993592 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | lpips_B | 0.0298103 | 0.029778 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | ib_gap_db_Y | 1.07104 | 1.0709 | td_b008:INFO |
| film24 | toslb | x265_2160p_12k | detail_pulse_Y | 0.0723116 | 0.0732897 | td_b008:INFO |
| film24 | toslb | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1.00028 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1.00033 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1.0003 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1.00034 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01477 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | toslb | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00023 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 0 | -5.61141e-07 | td_b008:PASS |
| film24 | toslb | x265_2160p_12k | cost_ms_per_frame_pyref | 0.000499997 | 1140.31 | td_b008:INFO |
| film24 | toslb | x265_1080p_4k | psnr_Y | 47.8778 | 47.8591 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | ssim_Y | 0.991653 | 0.991645 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1.00254 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_lag1_vs_input_Y | 0 | 1.01309e-05 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | wtw_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | toslb | x265_1080p_4k | flicker_excess_Y | -0.00120031 | -0.00103605 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | toslb | x265_1080p_4k | ghost_coeff_Y | 0.00408149 | 0.00306897 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | smear_Y | 0.947446 | 0.962858 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | halo_overshoot_Y | 3.58222e-05 | 4.42457e-05 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | lpips_all | 0.02483 | 0.0247409 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | psnr_I_Y | 47.4996 | 47.4907 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | psnr_P_Y | 48.488 | 48.4702 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | psnr_B_Y | 47.6997 | 47.6802 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | ssim_B_Y | 0.991452 | 0.991444 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | lpips_B | 0.0256965 | 0.0256042 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | ib_gap_db_Y | 0.678738 | 0.681407 | td_b008:INFO |
| film24 | toslb | x265_1080p_4k | detail_pulse_Y | 0.0375097 | 0.043575 | td_b008:INFO |
| film24 | toslb | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1.00227 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1.00234 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1.00259 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1 | 1.00286 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01474 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | toslb | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00142 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 0 | -2.51251e-06 | td_b008:PASS |
| film24 | toslb | x265_1080p_4k | cost_ms_per_frame_pyref | 0.000300002 | 1078.58 | td_b008:INFO |
