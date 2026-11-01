<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_low_sweep`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_s0.02_b3_u1 | td_s0.02_b2_u0.75 | td_s0.08_b1.5_u0.75 | td_s0.05_b3_u1 |
|---|---|---|---|---|---|
| psnr_B_Y | 34.1215 | 34.1219 | 34.1222 | 34.1221 | 34.1219 |
| psnr_P_Y | 35.0294 | 35.0287 | 35.0291 | 35.0292 | 35.0289 |
| psnr_I_Y | 36.3262 | 36.3262 | 36.3262 | 36.3262 | 36.3262 |
| psnr_Y | 34.3493 | 34.3496 | 34.3499 | 34.3498 | 34.3496 |
| ssim_B_Y | 0.870983 | 0.871022 | 0.871016 | 0.871008 | 0.871017 |
| ssim_Y | 0.874117 | 0.874148 | 0.874143 | 0.874137 | 0.874144 |
| lpips_B | NA | NA | NA | NA | NA |
| lpips_all | NA | NA | NA | NA | NA |
| ib_gap_db_Y | 0.963872 | 0.962743 | 0.962842 | 0.963076 | 0.962947 |
| detail_pulse_Y | 0.239524 | 0.238703 | 0.238836 | 0.238993 | 0.238798 |
| gop_ratio_minigop | NA | NA | NA | NA | NA |
| flicker_excess_Y | -0.0381958 | -0.0381493 | -0.0381689 | -0.0381836 | -0.0381578 |
| grain_keep_vs_input_Y | 1 | 1.00032 | 1.00023 | 1.00019 | 1.00031 |
| grain_lag1_vs_input_Y | 0 | -1.01948e-06 | -4.34067e-06 | -3.40286e-06 | -3.12015e-06 |
| grain_keep_vs_input_band1_Y | 1 | 1.00032 | 1.00022 | 1.00018 | 1.00029 |
| grain_keep_vs_input_band2_Y | 1 | 1.00023 | 1.00023 | 1.00019 | 1.0003 |
| grain_keep_vs_input_band3_Y | 1 | 1.0002 | 1.00019 | 1.00016 | 1.00022 |
| grain_keep_vs_input_band4_Y | 1 | 1.00019 | 1.00014 | 1.0001 | 1.0002 |
| ghost_coeff_Y | 0.103284 | 0.103054 | 0.103108 | 0.103164 | 0.103113 |
| smear_Y | 0.73211 | 0.733376 | 0.733003 | 0.732712 | 0.733167 |
| halo_overshoot_Y | 0.000439903 | 0.000442092 | 0.000441338 | 0.000440814 | 0.000441677 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 | 0.999997 | 1 | 0.999999 |
| wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 |

## Per case

| content_type | clip | case | metric | default | td_s0.02_b3_u1 | td_s0.02_b2_u0.75 | td_s0.08_b1.5_u0.75 | td_s0.05_b3_u1 | flags |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | 45.3258 | 45.3258 | 45.3258 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | 0.984698 | 0.984698 | 0.984698 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 1.01813e-08 | 3.17633e-09 | 3.17633e-09 | 3.17633e-09 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269955 | -0.000269908 | -0.000269769 | -0.000269877 | -0.000269949 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355461 | 0.00355361 | 0.00355394 | 0.00355427 | 0.00355381 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886196 | 0.886205 | 0.886202 | 0.886199 | 0.886204 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.76493e-05 | 2.7653e-05 | 2.76507e-05 | 2.76504e-05 | 2.76523e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 44.8223 | 44.8223 | 44.8223 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | 45.898 | 45.898 | 45.898 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | 45.1605 | 45.1605 | 45.1605 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | 0.984316 | 0.984316 | 0.984316 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.644637 | 0.644632 | 0.644626 | 0.644646 | 0.644633 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164206 | 0.1642 | 0.164202 | 0.164203 | 0.164201 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3917 | 44.3917 | 44.3919 | 44.3919 | 44.3919 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 0.984669 | 0.984669 | 0.984669 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00005 | 1.00004 | 1.00003 | 1.00002 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -4.44302e-07 | -4.26986e-06 | -8.5744e-07 | -1.64048e-06 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200683 | -0.00199855 | -0.00200202 | -0.00200195 | -0.00200062 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00576678 | 0.00574039 | 0.00574229 | 0.00575321 | 0.0057372 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.93148 | 0.931715 | 0.931682 | 0.931589 | 0.931716 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.15697e-05 | 5.16871e-05 | 5.16547e-05 | 5.16202e-05 | 5.16716e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 41.6599 | 41.6599 | 41.6599 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0903 | 45.0898 | 45.0901 | 45.0902 | 45.0901 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2805 | 44.2807 | 44.2809 | 44.2809 | 44.2809 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984582 | 0.984582 | 0.984582 | 0.984582 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425336 | 0.424714 | 0.42484 | 0.424881 | 0.424846 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.073986 | 0.0738734 | 0.073919 | 0.0739289 | 0.0738752 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00005 | 1.00004 | 1.00003 | 1.00002 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00004 | 1.00004 | 1.00004 | 1.00002 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00004 | 1.00004 | 1.00004 | 1.00002 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00005 | 1.00004 | 1.00003 | 1.00003 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9553 | 35.9552 | 35.9552 | 35.9553 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | 0.930287 | 0.930287 | 0.930288 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00029 | 1.00022 | 1.00021 | 1.00027 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 2.92631e-05 | 1.04898e-05 | 9.3223e-06 | 1.92189e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599649 | -0.00599395 | -0.00599562 | -0.00599592 | -0.00599324 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205843 | 0.0205535 | 0.0205626 | 0.0205668 | 0.0205499 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797125 | 0.797525 | 0.797409 | 0.797357 | 0.79756 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.00043796 | 0.000438523 | 0.000438397 | 0.000438296 | 0.000438594 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 38.8405 | 38.8405 | 38.8405 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | 36.531 | 36.531 | 36.531 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7168 | 35.7168 | 35.7168 | 35.7169 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | 0.929626 | 0.929626 | 0.929627 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.95446 | 0.954233 | 0.954248 | 0.954293 | 0.954116 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562107 | 0.562039 | 0.562056 | 0.562032 | 0.562018 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.0003 | 1.00024 | 1.00022 | 1.00029 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00032 | 1.00022 | 1.00021 | 1.00028 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.0003 | 1.00022 | 1.00021 | 1.00028 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00024 | 1.0002 | 1.00021 | 1.00024 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2855 | 33.2853 | 33.2856 | 33.2855 | 33.2853 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.890966 | 0.89098 | 0.890979 | 0.890975 | 0.890979 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00065 | 1.00055 | 1.00038 | 1.00062 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -5.0823e-05 | -3.82005e-05 | -3.08339e-05 | -4.13817e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130409 | -0.0130401 | -0.0130398 | -0.0130409 | -0.0130401 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0375779 | 0.0374336 | 0.037477 | 0.0375049 | 0.0374492 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734601 | 0.735935 | 0.735558 | 0.735296 | 0.735798 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000585954 | 0.000588234 | 0.000587518 | 0.000587089 | 0.000587992 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 37.4255 | 37.4255 | 37.4255 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0091 | 34.0094 | 34.0094 | 34.0092 | 34.0094 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.9909 | 32.9906 | 32.991 | 32.9909 | 32.9906 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887697 | 0.887712 | 0.887711 | 0.887707 | 0.88771 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20411 | 1.20465 | 1.20423 | 1.20415 | 1.20465 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414546 | 0.414785 | 0.414646 | 0.414609 | 0.414755 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00057 | 1.00051 | 1.00037 | 1.00055 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00067 | 1.00058 | 1.00036 | 1.00063 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00064 | 1.00055 | 1.00037 | 1.0006 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00075 | 1.00059 | 1.00042 | 1.0007 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1646 | 33.1661 | 33.1661 | 33.1655 | 33.1658 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918374 | 0.918393 | 0.91839 | 0.918385 | 0.918391 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | -1.00655e-07 | -7.21446e-08 | -1.2812e-07 | -7.25454e-08 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 0.999993 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00781908 | -0.00777139 | -0.00777457 | -0.00780151 | -0.00774169 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0654834 | 0.0651768 | 0.0652283 | 0.0653377 | 0.0652937 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.806187 | 0.8071 | 0.806943 | 0.806628 | 0.806845 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332055 | 0.000333303 | 0.000333028 | 0.000332603 | 0.000332972 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 36.3165 | 36.3165 | 36.3165 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9524 | 33.952 | 33.9521 | 33.9523 | 33.9521 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.8911 | 32.893 | 32.893 | 32.8922 | 32.8926 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916007 | 0.916031 | 0.916028 | 0.916021 | 0.916028 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21593 | 1.21372 | 1.21379 | 1.21479 | 1.21413 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0618573 | 0.061059 | 0.0611075 | 0.0613808 | 0.0611764 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8149 | 28.8169 | 28.8177 | 28.8174 | 28.8169 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851496 | 0.851601 | 0.851591 | 0.851571 | 0.85158 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.00058 | 1.00046 | 1.0004 | 1.00053 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | -2.62385e-05 | -1.48366e-05 | -1.41405e-05 | -2.52887e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0.99998 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0462079 | -0.0461546 | -0.0461764 | -0.0462215 | -0.0462065 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.120286 | 0.119248 | 0.119477 | 0.119704 | 0.119569 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.716427 | 0.719728 | 0.718849 | 0.71809 | 0.718581 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000396541 | 0.000402246 | 0.000400375 | 0.000398847 | 0.000400146 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 32.3426 | 32.3426 | 32.3426 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.658 | 29.6557 | 29.6572 | 29.6575 | 29.6568 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5237 | 28.5267 | 28.5274 | 28.5269 | 28.5265 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.84545 | 0.845586 | 0.845572 | 0.845547 | 0.845558 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30451 | 1.29924 | 1.30001 | 1.30078 | 1.3005 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.114455 | 0.112507 | 0.112701 | 0.112798 | 0.113022 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00045 | 1.00038 | 1.00032 | 1.0004 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00074 | 1.0006 | 1.00052 | 1.00067 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1.00068 | 1.00052 | 1.00048 | 1.00063 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00035 | 1.00026 | 1.00023 | 1.00035 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1923 | 28.1925 | 28.1926 | 28.1925 | 28.1924 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.747524 | 0.747547 | 0.747542 | 0.747535 | 0.747546 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1.00003 | 1.00003 | 1.00005 | 1.00011 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 1.0344e-06 | -7.73374e-06 | -6.01137e-06 | -9.19449e-06 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101988 | -0.101863 | -0.101945 | -0.101953 | -0.101894 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254786 | 0.254659 | 0.25469 | 0.254728 | 0.254659 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.509077 | 0.510182 | 0.509766 | 0.509516 | 0.510258 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837101 | 0.000838641 | 0.000838044 | 0.000837697 | 0.000838737 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 31.5195 | 31.5195 | 31.5195 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9645 | 28.964 | 28.9642 | 28.9643 | 28.964 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9203 | 27.9206 | 27.9207 | 27.9205 | 27.9205 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.74198 | 0.742012 | 0.742005 | 0.741995 | 0.74201 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20829 | 1.20739 | 1.2076 | 1.20782 | 1.20752 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.296664 | 0.295811 | 0.295786 | 0.296036 | 0.295327 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1.00003 | 1.00003 | 1.00004 | 1.00009 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1.00005 | 1.00005 | 1.00008 | 1.00017 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1.00005 | 1.00007 | 1.00007 | 1.00015 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1.00002 | 1.00005 | 1.00002 | 1.00003 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6645 | 25.6631 | 25.6639 | 25.6643 | 25.6633 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.684926 | 0.685012 | 0.684987 | 0.684977 | 0.685003 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1.001 | 1.00054 | 1.00044 | 1.00093 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 0 | 3.91429e-05 | 1.98945e-05 | 1.5423e-05 | 3.33946e-05 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 1 | 1 | 1 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128237 | -0.128103 | -0.128148 | -0.128184 | -0.128116 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318236 | 0.31807 | 0.318129 | 0.318166 | 0.318094 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.47579 | 0.478616 | 0.477612 | 0.477025 | 0.478376 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000850393 | 0.000856447 | 0.000854034 | 0.000852709 | 0.000855654 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | 0 | 0 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 27.6828 | 27.6828 | 27.6828 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1316 | 26.1299 | 26.1307 | 26.131 | 26.1299 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.488 | 25.4867 | 25.4874 | 25.4879 | 25.4869 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678209 | 0.678313 | 0.678286 | 0.678273 | 0.678305 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | td_s0.02_b3_u1:NA td_s0.02_b2_u0.75:NA td_s0.08_b1.5_u0.75:NA td_s0.05_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753705 | 0.753364 | 0.753394 | 0.75325 | 0.753184 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.228373 | 0.225348 | 0.22627 | 0.226953 | 0.226013 | td_s0.02_b3_u1:INFO td_s0.02_b2_u0.75:INFO td_s0.08_b1.5_u0.75:INFO td_s0.05_b3_u1:INFO |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1.00112 | 1.00057 | 1.00046 | 1.00096 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1.00004 | 1.00034 | 1.00034 | 1.00067 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 0.999899 | 1.00013 | 1.00013 | 1.00012 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1.00009 | 0.999992 | 0.999858 | 1.00028 | td_s0.02_b3_u1:PASS td_s0.02_b2_u0.75:PASS td_s0.08_b1.5_u0.75:PASS td_s0.05_b3_u1:PASS |
