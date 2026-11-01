<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_low`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008 |
|---|---|---|
| psnr_B_Y | 34.3373 | 34.3334 |
| psnr_P_Y | 35.1899 | 35.1732 |
| psnr_I_Y | 36.4579 | 36.4584 |
| psnr_Y | 34.5649 | 34.5584 |
| ssim_B_Y | 0.871026 | 0.871195 |
| ssim_Y | 0.874086 | 0.874203 |
| lpips_B | 0.131021 | 0.130416 |
| lpips_all | 0.127798 | 0.127256 |
| ib_gap_db_Y | 0.928257 | 0.916614 |
| detail_pulse_Y | 0.22314 | 0.224405 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.036511 | -0.0357073 |
| grain_keep_vs_input_Y | 1 | 1.00642 |
| grain_lag1_vs_input_Y | 0 | -0.000202469 |
| grain_keep_vs_input_band1_Y | 1 | 1.00641 |
| grain_keep_vs_input_band2_Y | 1 | 1.0065 |
| grain_keep_vs_input_band3_Y | 1 | 1.00667 |
| grain_keep_vs_input_band4_Y | 1 | 1.00652 |
| grain_probe_keep_edge_vs_input_Y | 1 | 1.01759 |
| grain_probe_lag1_edge_vs_input_Y | 0 | -0.00019691 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1.00496 |
| grain_probe_lag1_flat_vs_input_Y | 0 | -2.77958e-06 |
| ghost_coeff_Y | 0.102347 | 0.0993283 |
| smear_Y | 0.730705 | 0.747176 |
| halo_overshoot_Y | 0.000426972 | 0.000459978 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 |
| cost_ms_per_frame_pyref | 0.000399998 | 1002.31 |

## Per case

| content_type | clip | case | metric | default | td_b008 | flags |
|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 46.0116 | 46.0082 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.986486 | 0.986481 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00025 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 2.71492e-06 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000201539 | -0.000186946 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00353428 | 0.00341184 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.885163 | 0.888853 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.50286e-05 | 2.67239e-05 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_all | 0.0582708 | 0.0582533 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 46.31 | 46.3079 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 46.5035 | 46.5004 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.8428 | 45.8393 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.986186 | 0.986181 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_B | 0.0592325 | 0.0592113 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.640994 | 0.641374 | td_b008:INFO |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.232474 | 0.237756 | td_b008:INFO |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00025 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00024 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00026 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00025 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.01528 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00028 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 1.30948e-07 | td_b008:PASS |
| film24 | tos | x265_2160p_6k_low | cost_ms_per_frame_pyref | 0.000499997 | 989.084 | td_b008:INFO |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 45.1418 | 45.1325 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.98652 | 0.986512 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00215 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -4.81477e-05 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00165591 | -0.00145819 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00503012 | 0.00391764 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.936867 | 0.949706 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 4.47107e-05 | 5.16353e-05 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | 0.0403076 | 0.0402335 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 43.862 | 43.8598 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.7633 | 45.7482 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 45.0009 | 44.9929 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.98637 | 0.986365 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | 0.0415726 | 0.0414861 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.530196 | 0.525016 | td_b008:INFO |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.069476 | 0.0743398 | td_b008:INFO |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00193 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00216 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00228 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00217 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.01348 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00224 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 0 | -2.29848e-06 | td_b008:PASS |
| film24 | tos | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 0.000799992 | 1072.48 | td_b008:INFO |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 36.3357 | 36.3322 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.934444 | 0.934384 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00574 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 2.99518e-06 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00701674 | -0.00696786 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0209735 | 0.0204267 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.796392 | 0.803625 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000401656 | 0.000413481 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_all | 0.0839357 | 0.0838642 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.5995 | 38.5994 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.8976 | 36.8904 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 36.0924 | 36.0898 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.933618 | 0.933564 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_B | 0.0844818 | 0.0843921 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.948409 | 0.944286 | td_b008:INFO |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.448807 | 0.449504 | td_b008:INFO |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00585 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00577 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00568 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00564 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.0089 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | sol | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00125 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 9.21619e-05 | td_b008:PASS |
| film24 | sol | x265_2160p_6k_low | cost_ms_per_frame_pyref | 0.000399988 | 1337.14 | td_b008:INFO |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.4601 | 33.451 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.893328 | 0.893272 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.0122 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -9.63908e-05 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0129662 | -0.0128975 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0373323 | 0.0353051 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734874 | 0.754239 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000543137 | 0.00057748 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | 0.097648 | 0.097384 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 36.7179 | 36.72 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.1658 | 34.1537 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 33.1573 | 33.1488 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.89 | 0.889956 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | 0.09875 | 0.0984615 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20591 | 1.20314 | td_b008:INFO |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.33281 | 0.335625 | td_b008:INFO |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.0121 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.0122 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.01211 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.01236 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.01681 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00319 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 0 | -0.00129419 | td_b008:PASS |
| film24 | sol | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 0.000300002 | 1200.31 | td_b008:INFO |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 32.962 | 32.9538 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.913936 | 0.914025 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00458 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | -0.000262649 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | td_b008:NA |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00326326 | -0.00263392 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.067087 | 0.0619482 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.796423 | 0.815619 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000331668 | 0.000366101 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_all | 0.0658872 | 0.0656543 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 35.3389 | 35.3462 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.6145 | 33.5923 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.7063 | 32.7014 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.911568 | 0.911695 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_B | 0.067368 | 0.0670937 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.06066 | 1.04551 | td_b008:INFO |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0725941 | 0.0677786 | td_b008:INFO |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00542 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00421 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00439 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00429 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.01933 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| nature30 | boat | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00967 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 0.00046997 | td_b008:PASS |
| nature30 | boat | x265_2160p_6k_low | cost_ms_per_frame_pyref | 0.000300002 | 880.12 | td_b008:INFO |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.7511 | 28.7452 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.848339 | 0.848998 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.02031 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -0.000930478 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0347345 | -0.0338023 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.122664 | 0.114139 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.715071 | 0.745615 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000409907 | 0.000481561 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | 0.0905272 | 0.0899137 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 31.6322 | 31.6319 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.4434 | 29.4183 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.4754 | 28.4742 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.842423 | 0.843193 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | 0.0942817 | 0.0936013 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.15289 | 1.13059 | td_b008:INFO |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.114089 | 0.112659 | td_b008:INFO |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.01979 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.01995 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.0211 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.02034 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.01962 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | 0 | -0.00019691 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.01021 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000579352 | td_b008:PASS |
| nature30 | boat | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 0.000300002 | 792.555 | td_b008:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1053 | 28.0986 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.743033 | 0.74289 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00132 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | -0.000187998 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.10757 | -0.105914 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.252626 | 0.249618 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.502166 | 0.518356 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000838367 | 0.000871421 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | 0.314836 | 0.313469 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.8775 | 28.8538 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.8466 | 27.8442 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.737846 | 0.737795 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | 0.322622 | 0.321094 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.14219 | 1.12178 | td_b008:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.292613 | 0.294239 | td_b008:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00112 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00166 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.0028 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00287 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.02231 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00455 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 5.27283e-05 | td_b008:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | cost_ms_per_frame_pyref | 0.000300002 | 910.943 | td_b008:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.7514 | 25.7455 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.686601 | 0.687061 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00477 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -9.98008e-05 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.12468 | -0.121798 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | td_b008:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.309529 | 0.30586 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.478685 | 0.501396 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000821301 | 0.000891417 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | 0.270973 | 0.269275 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.2534 | 26.2282 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.5768 | 25.5762 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.680199 | 0.680809 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | 0.279859 | 0.277986 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.744804 | 0.721218 | td_b008:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.222255 | 0.223342 | td_b008:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00484 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00584 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00473 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00422 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1.02496 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1.00828 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 0 | 7.99087e-05 | td_b008:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 0.000300002 | 835.864 | td_b008:INFO |
