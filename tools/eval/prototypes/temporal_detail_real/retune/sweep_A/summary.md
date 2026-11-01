<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_low_sweep`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_s0.15_b1_u0.5 | td_s0.15_b1_u1 | td_s0.05_b1_u0.5 | td_s0.02_b1_u0.5 |
|---|---|---|---|---|---|
| psnr_B_Y | 34.1215 | 34.1219 | 34.122 | 34.122 | 34.122 |
| psnr_P_Y | 35.0294 | 35.0293 | 35.0293 | 35.0293 | 35.0293 |
| psnr_I_Y | 36.3262 | 36.3262 | 36.3262 | 36.3262 | 36.3262 |
| psnr_Y | 34.3493 | 34.3496 | 34.3497 | 34.3497 | 34.3497 |
| ssim_B_Y | 0.870983 | 0.870998 | 0.871005 | 0.871003 | 0.871006 |
| ssim_Y | 0.874117 | 0.874129 | 0.874135 | 0.874133 | 0.874135 |
| lpips_B | NA | NA | NA | NA | NA |
| lpips_all | NA | NA | NA | NA | NA |
| ib_gap_db_Y | 0.963872 | 0.963403 | 0.963259 | 0.963274 | 0.963214 |
| detail_pulse_Y | 0.239524 | 0.239222 | 0.239138 | 0.239097 | 0.239057 |
| gop_ratio_minigop | NA | NA | NA | NA | NA |
| flicker_excess_Y | -0.0381958 | -0.0381853 | -0.0381916 | -0.0381888 | -0.0381898 |
| grain_keep_vs_input_Y | 1 | 1.00014 | 1.00021 | 1.00013 | 1.00015 |
| grain_lag1_vs_input_Y | 0 | -1.44836e-06 | 2.31471e-06 | -3.70713e-08 | 5.85654e-06 |
| grain_keep_vs_input_band1_Y | 1 | 1.00014 | 1.00021 | 1.00012 | 1.00015 |
| grain_keep_vs_input_band2_Y | 1 | 1.00018 | 1.00022 | 1.00015 | 1.00015 |
| grain_keep_vs_input_band3_Y | 1 | 1.0001 | 1.00015 | 1.00012 | 1.00013 |
| grain_keep_vs_input_band4_Y | 1 | 1.00007 | 1.00011 | 1.00011 | 1.00008 |
| ghost_coeff_Y | 0.103284 | 0.103228 | 0.103205 | 0.10319 | 0.103172 |
| smear_Y | 0.73211 | 0.732416 | 0.732544 | 0.732553 | 0.732648 |
| halo_overshoot_Y | 0.000439903 | 0.000440373 | 0.000440619 | 0.000440576 | 0.000440727 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 | 0.999999 | 0.999999 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 |

## Per case

| content_type | clip | case | metric | default | td_s0.15_b1_u0.5 | td_s0.15_b1_u1 | td_s0.05_b1_u0.5 | td_s0.02_b1_u0.5 | flags |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | 45.3258 | 45.3258 | 45.3258 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | 0.984698 | 0.984698 | 0.984698 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 0 | 2.42431e-09 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269955 | -0.000269913 | -0.000269879 | -0.000269907 | -0.000269886 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355461 | 0.00355448 | 0.00355437 | 0.0035544 | 0.00355436 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886196 | 0.886197 | 0.886198 | 0.886198 | 0.886198 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.76493e-05 | 2.76499e-05 | 2.76507e-05 | 2.765e-05 | 2.76501e-05 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 44.8223 | 44.8223 | 44.8223 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | 45.898 | 45.898 | 45.898 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | 45.1605 | 45.1605 | 45.1605 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | 0.984316 | 0.984316 | 0.984316 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.644637 | 0.644641 | 0.644643 | 0.64464 | 0.64464 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164206 | 0.164207 | 0.164205 | 0.164206 | 0.164206 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3917 | 44.3918 | 44.3919 | 44.3918 | 44.3918 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 0.984669 | 0.984669 | 0.984669 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00001 | 1.00002 | 1.00004 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | 2.66471e-09 | 4.35657e-06 | -1.41526e-06 | -3.71005e-07 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200683 | -0.00200624 | -0.00200574 | -0.00200556 | -0.00200603 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00576678 | 0.00576415 | 0.00575931 | 0.00575791 | 0.00575919 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.93148 | 0.931503 | 0.931546 | 0.931555 | 0.931552 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.15697e-05 | 5.15806e-05 | 5.15969e-05 | 5.16022e-05 | 5.16072e-05 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 41.6599 | 41.6599 | 41.6599 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0903 | 45.0902 | 45.0902 | 45.0902 | 45.0902 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2805 | 44.2806 | 44.2808 | 44.2808 | 44.2807 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984581 | 0.984581 | 0.984581 | 0.984581 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425336 | 0.425199 | 0.42505 | 0.425031 | 0.425135 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.073986 | 0.07396 | 0.0739404 | 0.0739306 | 0.0739448 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00001 | 1.00003 | 1.00003 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00001 | 1.00002 | 1.00003 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1.00002 | 1.00004 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00001 | 1.00002 | 1.00004 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9552 | 35.9552 | 35.9552 | 35.9552 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | 0.930288 | 0.930287 | 0.930287 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.0001 | 1.00015 | 1.00013 | 1.00013 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 4.45026e-07 | 4.98507e-06 | 3.28168e-06 | 2.63031e-06 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599649 | -0.00599574 | -0.00599608 | -0.0059953 | -0.00599546 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205843 | 0.0205771 | 0.0205714 | 0.0205739 | 0.0205728 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797125 | 0.797218 | 0.797293 | 0.797256 | 0.79727 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.00043796 | 0.000438101 | 0.000438209 | 0.000438154 | 0.000438178 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 38.8405 | 38.8405 | 38.8405 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | 36.5311 | 36.5311 | 36.5311 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7167 | 35.7168 | 35.7167 | 35.7167 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | 0.929627 | 0.929626 | 0.929626 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.95446 | 0.954471 | 0.954358 | 0.954458 | 0.954464 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562107 | 0.562066 | 0.562056 | 0.562052 | 0.562048 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00011 | 1.00016 | 1.00014 | 1.00014 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00009 | 1.00015 | 1.00012 | 1.00012 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.0001 | 1.00014 | 1.00012 | 1.00012 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.0001 | 1.00014 | 1.00012 | 1.00013 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2855 | 33.2855 | 33.2856 | 33.2856 | 33.2855 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.890966 | 0.89097 | 0.890975 | 0.890971 | 0.890972 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00044 | 1.00056 | 1.00022 | 1.00022 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -6.67027e-06 | -1.694e-05 | -1.5437e-05 | -1.02008e-05 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130409 | -0.0130408 | -0.0130411 | -0.0130415 | -0.013041 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0375779 | 0.0375384 | 0.0375232 | 0.0375396 | 0.037526 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734601 | 0.734976 | 0.735117 | 0.734958 | 0.73508 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000585954 | 0.000586564 | 0.000586818 | 0.000586531 | 0.000586693 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 37.4255 | 37.4255 | 37.4255 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0091 | 34.0091 | 34.0093 | 34.0093 | 34.0092 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.9909 | 32.9909 | 32.9911 | 32.991 | 32.991 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887697 | 0.887702 | 0.887708 | 0.887702 | 0.887704 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20411 | 1.20406 | 1.20406 | 1.20412 | 1.20404 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414546 | 0.414576 | 0.414602 | 0.414585 | 0.414576 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00044 | 1.00054 | 1.0002 | 1.0002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00047 | 1.00061 | 1.00021 | 1.00022 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00041 | 1.00053 | 1.00021 | 1.0002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00044 | 1.00057 | 1.00024 | 1.00026 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1646 | 33.1652 | 33.1652 | 33.1654 | 33.1654 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918374 | 0.91838 | 0.918382 | 0.918384 | 0.918385 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | -3.82499e-07 | -3.82499e-07 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00781908 | -0.00781416 | -0.00781188 | -0.00781259 | -0.00780528 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0654834 | 0.0654231 | 0.0654293 | 0.0653802 | 0.0653644 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.806187 | 0.806378 | 0.806389 | 0.806534 | 0.80659 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332055 | 0.000332284 | 0.000332335 | 0.000332441 | 0.000332499 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 36.3165 | 36.3165 | 36.3165 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9524 | 33.9523 | 33.9523 | 33.9523 | 33.9523 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.8911 | 32.8918 | 32.8918 | 32.892 | 32.8921 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916007 | 0.916015 | 0.916016 | 0.916019 | 0.91602 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21593 | 1.21519 | 1.2152 | 1.21496 | 1.2149 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0618573 | 0.0616137 | 0.0616202 | 0.0614587 | 0.0614185 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8149 | 28.8167 | 28.817 | 28.8172 | 28.8173 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851496 | 0.851543 | 0.851564 | 0.851565 | 0.851572 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00028 | 1.0004 | 1.0002 | 1.00022 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -8.63783e-06 | -5.69928e-06 | -3.02616e-06 | -4.39972e-06 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0.999993 | 0.999993 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0462079 | -0.0462019 | -0.046235 | -0.0462168 | -0.046193 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.120286 | 0.120014 | 0.119876 | 0.119767 | 0.119651 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.716427 | 0.717294 | 0.717629 | 0.71793 | 0.718298 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000396541 | 0.000397674 | 0.000398352 | 0.000398709 | 0.000399307 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 32.3426 | 32.3426 | 32.3426 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.658 | 29.658 | 29.6577 | 29.6577 | 29.6576 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5237 | 28.5259 | 28.5264 | 28.5266 | 28.5268 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.84545 | 0.845511 | 0.845539 | 0.84554 | 0.845549 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30451 | 1.30219 | 1.30145 | 1.30129 | 1.30104 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.114455 | 0.113523 | 0.113211 | 0.112867 | 0.112583 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.0003 | 1.00039 | 1.00018 | 1.0002 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00037 | 1.00052 | 1.00025 | 1.00027 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00031 | 1.00043 | 1.00023 | 1.00025 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00011 | 1.00019 | 1.00011 | 1.00012 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1923 | 28.1924 | 28.1924 | 28.1923 | 28.1923 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.747524 | 0.74753 | 0.747533 | 0.74753 | 0.74753 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1.00006 | 1.00001 | 1.00001 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 1.78608e-07 | 1.56338e-05 | -1.76513e-06 | -1.76657e-06 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101988 | -0.101973 | -0.101954 | -0.101975 | -0.101974 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254786 | 0.254754 | 0.25475 | 0.254762 | 0.25476 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.509077 | 0.50934 | 0.509428 | 0.509343 | 0.509363 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837101 | 0.000837474 | 0.000837633 | 0.000837482 | 0.000837513 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 31.5195 | 31.5195 | 31.5195 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9645 | 28.9645 | 28.9644 | 28.9644 | 28.9644 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9203 | 27.9204 | 27.9204 | 27.9203 | 27.9203 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.74198 | 0.741989 | 0.741993 | 0.741988 | 0.741989 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20829 | 1.20811 | 1.20798 | 1.20813 | 1.20812 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.296664 | 0.296272 | 0.296269 | 0.296407 | 0.29638 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1.00007 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00001 | 0.99996 | 1.00001 | 1.00001 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1.00003 | 1.00002 | 1.00003 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 0.999997 | 0.999986 | 0.999989 | 0.999989 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6645 | 25.6646 | 25.6644 | 25.6644 | 25.6644 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.684926 | 0.684955 | 0.684969 | 0.68496 | 0.684966 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00028 | 1.00047 | 1.00041 | 1.00063 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | 3.47742e-06 | 1.65616e-05 | 1.80653e-05 | 6.09601e-05 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128237 | -0.128181 | -0.128219 | -0.128194 | -0.128234 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318236 | 0.318197 | 0.318174 | 0.318188 | 0.318185 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.47579 | 0.476424 | 0.476753 | 0.476648 | 0.47683 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000850393 | 0.000851659 | 0.000852354 | 0.000852041 | 0.000852372 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 27.6828 | 27.6828 | 27.6828 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1316 | 26.1314 | 26.1312 | 26.1314 | 26.1313 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.488 | 25.4882 | 25.4879 | 25.488 | 25.488 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678209 | 0.678247 | 0.678262 | 0.678253 | 0.678261 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.15_b1_u1:NA td_s0.05_b1_u0.5:NA td_s0.02_b1_u0.5:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753705 | 0.753365 | 0.75333 | 0.753559 | 0.753376 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.228373 | 0.22756 | 0.227201 | 0.22727 | 0.227301 | td_s0.15_b1_u0.5:INFO td_s0.15_b1_u1:INFO td_s0.05_b1_u0.5:INFO td_s0.02_b1_u0.5:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00028 | 1.00049 | 1.0004 | 1.00065 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00047 | 1.00048 | 1.0006 | 1.00053 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00001 | 1.00002 | 1.00037 | 1.00039 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 0.999892 | 0.999991 | 1.00039 | 1.00016 | td_s0.15_b1_u0.5:PASS td_s0.15_b1_u1:PASS td_s0.05_b1_u0.5:PASS td_s0.02_b1_u0.5:PASS |
