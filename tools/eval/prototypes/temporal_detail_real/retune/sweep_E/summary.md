<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_low_sweep`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008 | td_s0.02_b3_u1_boost |
|---|---|---|---|
| psnr_B_Y | 34.1215 | 34.1172 | 34.117 |
| psnr_P_Y | 35.0294 | 35.0134 | 35.0129 |
| psnr_I_Y | 36.3262 | 36.3262 | 36.3262 |
| psnr_Y | 34.3493 | 34.3428 | 34.3426 |
| ssim_B_Y | 0.870983 | 0.87112 | 0.871134 |
| ssim_Y | 0.874117 | 0.874207 | 0.874218 |
| lpips_B | NA | NA | NA |
| lpips_all | NA | NA | NA |
| ib_gap_db_Y | 0.963872 | 0.953121 | 0.952828 |
| detail_pulse_Y | 0.239524 | 0.24064 | 0.24019 |
| gop_ratio_minigop | NA | NA | NA |
| flicker_excess_Y | -0.0381958 | -0.0374129 | -0.037393 |
| grain_keep_vs_input_Y | 1 | 1.00612 | 1.00619 |
| grain_lag1_vs_input_Y | 0 | -0.00011823 | -0.000112741 |
| grain_keep_vs_input_band1_Y | 1 | 1.00574 | 1.00581 |
| grain_keep_vs_input_band2_Y | 1 | 1.007 | 1.00686 |
| grain_keep_vs_input_band3_Y | 1 | 1.00682 | 1.0068 |
| grain_keep_vs_input_band4_Y | 1 | 1.00683 | 1.00687 |
| ghost_coeff_Y | 0.103284 | 0.100449 | 0.100353 |
| smear_Y | 0.73211 | 0.747398 | 0.748002 |
| halo_overshoot_Y | 0.000439903 | 0.000470949 | 0.000472157 |
| lc_boost_fraction_Y | 0 | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 | 1 |

## Per case

| content_type | clip | case | metric | default | td_b008 | td_s0.02_b3_u1_boost | flags |
|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3243 | 45.3243 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984695 | 0.984695 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00082 | 1.00082 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 2.04849e-05 | 2.04951e-05 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269955 | -0.000258988 | -0.000258988 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355461 | 0.00346947 | 0.00346862 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886196 | 0.888338 | 0.888347 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.76493e-05 | 2.85786e-05 | 2.85818e-05 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 44.8223 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.8961 | 45.8961 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.159 | 45.159 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984312 | 0.984312 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.644637 | 0.644412 | 0.644407 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164206 | 0.164999 | 0.164992 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00078 | 1.00078 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00082 | 1.00082 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00076 | 1.00076 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.0009 | 1.0009 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3917 | 44.3845 | 44.3843 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.98466 | 0.98466 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00456 | 1.0046 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -8.68253e-05 | -8.74514e-05 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200683 | -0.0018066 | -0.00179907 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00576678 | 0.00474805 | 0.00473187 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.93148 | 0.942882 | 0.943022 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.15697e-05 | 5.73569e-05 | 5.744e-05 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 41.6599 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0903 | 45.0763 | 45.0759 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2805 | 44.2749 | 44.2748 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984576 | 0.984576 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425336 | 0.419193 | 0.418884 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.073986 | 0.0783581 | 0.078248 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00428 | 1.00432 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00453 | 1.00456 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00454 | 1.00458 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00483 | 1.00488 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9508 | 35.9509 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930211 | 0.930212 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00516 | 1.00535 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 0.000162555 | 0.000192308 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599649 | -0.00594719 | -0.00594649 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205843 | 0.0199792 | 0.0199616 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797125 | 0.805175 | 0.805402 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.00043796 | 0.000452397 | 0.00045269 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 38.8405 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5225 | 36.5226 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7134 | 35.7135 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929559 | 0.92956 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.95446 | 0.949544 | 0.94947 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562107 | 0.562534 | 0.562497 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00534 | 1.00554 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00513 | 1.00535 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.0051 | 1.00531 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00503 | 1.00518 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2855 | 33.2756 | 33.2754 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.890966 | 0.890876 | 0.890885 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.01106 | 1.01106 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -6.77433e-05 | -9.18262e-05 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130409 | -0.0129513 | -0.0129525 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0375779 | 0.0355176 | 0.0354633 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734601 | 0.754157 | 0.754668 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000585954 | 0.000624009 | 0.000624979 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 37.4255 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0091 | 33.9953 | 33.9957 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.9909 | 32.982 | 32.9817 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887697 | 0.887621 | 0.88763 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20411 | 1.19974 | 1.2004 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414546 | 0.418585 | 0.418649 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.01106 | 1.01097 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.0112 | 1.01116 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.01094 | 1.01097 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.01108 | 1.01118 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1646 | 33.1548 | 33.1554 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918374 | 0.918421 | 0.91843 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00042 | 1.00042 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 2.94716e-06 | 3.22557e-06 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00781908 | -0.00733701 | -0.0072935 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0654834 | 0.0611764 | 0.0610684 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.806187 | 0.821736 | 0.822047 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332055 | 0.000359583 | 0.000360041 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 36.3165 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9524 | 33.934 | 33.9339 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.8911 | 32.8832 | 32.884 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916007 | 0.916077 | 0.916088 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21593 | 1.20635 | 1.20554 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0618573 | 0.0584516 | 0.0580554 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00035 | 1.00035 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00046 | 1.00046 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.0004 | 1.0004 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00049 | 1.00049 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8149 | 28.8093 | 28.8087 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851496 | 0.852095 | 0.85212 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.01458 | 1.01448 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -0.000451622 | -0.000464603 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0462079 | -0.0452979 | -0.0453452 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.120286 | 0.112115 | 0.1117 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.716427 | 0.744565 | 0.745786 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000396541 | 0.000459669 | 0.000462194 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 32.3426 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.658 | 29.6361 | 29.6339 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5237 | 28.5219 | 28.5216 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.84545 | 0.846147 | 0.846184 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30451 | 1.28541 | 1.28355 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.114455 | 0.112809 | 0.111863 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.01238 | 1.0122 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.01386 | 1.01375 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.01643 | 1.01633 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.01531 | 1.01529 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1923 | 28.1847 | 28.1847 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.747524 | 0.747354 | 0.74737 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00139 | 1.00141 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | -0.000161484 | -0.000159338 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101988 | -0.100326 | -0.100232 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254786 | 0.251841 | 0.251777 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.509077 | 0.525131 | 0.525761 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837101 | 0.000870098 | 0.000870959 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 31.5195 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9645 | 28.9394 | 28.9392 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9203 | 27.9167 | 27.9168 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.74198 | 0.741898 | 0.741918 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20829 | 1.18793 | 1.1876 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.296664 | 0.299268 | 0.298841 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00103 | 1.00106 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00294 | 1.00297 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00367 | 1.00371 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00268 | 1.00269 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6645 | 25.6587 | 25.6569 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.684926 | 0.685341 | 0.685374 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.011 | 1.0114 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -0.000364155 | -0.000314742 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128237 | -0.125378 | -0.125316 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318236 | 0.314749 | 0.314651 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.47579 | 0.497201 | 0.498985 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000850393 | 0.000915903 | 0.000920369 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 27.6828 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1316 | 26.1077 | 26.1061 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.488 | 25.4868 | 25.4849 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678209 | 0.678767 | 0.678807 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | td_b008:NA td_s0.02_b3_u1_boost:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753705 | 0.732385 | 0.732777 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.228373 | 0.230118 | 0.228378 | td_b008:INFO td_s0.02_b3_u1_boost:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.01069 | 1.01125 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.01707 | 1.01583 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.01268 | 1.01234 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.01431 | 1.01437 | td_b008:PASS td_s0.02_b3_u1_boost:PASS |
