<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real_vlow_sweep`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_s0.15_b1_u0.5 | td_s0.02_b3_u1 |
|---|---|---|---|
| psnr_B_Y | 32.1378 | 32.1383 | 32.138 |
| psnr_P_Y | 33.3162 | 33.3161 | 33.3155 |
| psnr_I_Y | 34.1824 | 34.1824 | 34.1824 |
| psnr_Y | 32.4174 | 32.4178 | 32.4174 |
| ssim_B_Y | 0.832992 | 0.833017 | 0.833041 |
| ssim_Y | 0.837102 | 0.837122 | 0.83714 |
| lpips_B | NA | NA | NA |
| lpips_all | NA | NA | NA |
| ib_gap_db_Y | 1.20405 | 1.2035 | 1.20319 |
| detail_pulse_Y | 0.283403 | 0.282902 | 0.282304 |
| gop_ratio_minigop | NA | NA | NA |
| flicker_excess_Y | -0.0691835 | -0.0691658 | -0.0691097 |
| grain_keep_vs_input_Y | 1 | 1.00015 | 1.00055 |
| grain_lag1_vs_input_Y | 0 | -2.38017e-06 | -1.78564e-05 |
| grain_keep_vs_input_band1_Y | 1 | 1.00015 | 1.00056 |
| grain_keep_vs_input_band2_Y | 1 | 1.00022 | 1.00044 |
| grain_keep_vs_input_band3_Y | 1 | 1.00025 | 1.00042 |
| grain_keep_vs_input_band4_Y | 1 | 1.00032 | 1.0004 |
| ghost_coeff_Y | 0.138645 | 0.138535 | 0.138357 |
| smear_Y | 0.65884 | 0.659247 | 0.660233 |
| halo_overshoot_Y | 0.000763976 | 0.000764724 | 0.000766886 |
| lc_boost_fraction_Y | 0 | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 | 0.999999 |
| wtw_kept_vs_input_Y | 1 | 1 | 1 |

## Per case

| content_type | clip | case | metric | default | td_s0.15_b1_u0.5 | td_s0.02_b3_u1 | flags |
|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_4k_vlow | psnr_Y | 43.7214 | 43.7214 | 43.7214 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | ssim_Y | 0.981886 | 0.981886 | 0.981886 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 0 | 1.08266e-07 | 4.99848e-07 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_vs_input_Y | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_2160p_4k_vlow | flicker_excess_Y | -0.00117308 | -0.00117297 | -0.00117254 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_2160p_4k_vlow | ghost_coeff_Y | 0.00572971 | 0.00572943 | 0.00572834 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | smear_Y | 0.872822 | 0.872825 | 0.87284 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | halo_overshoot_Y | 5.7325e-05 | 5.73256e-05 | 5.73324e-05 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_2160p_4k_vlow | psnr_I_Y | 42.93 | 42.93 | 42.93 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | psnr_P_Y | 44.6031 | 44.6031 | 44.6031 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | psnr_B_Y | 43.4795 | 43.4795 | 43.4795 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | ssim_B_Y | 0.981368 | 0.981368 | 0.981368 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_2160p_4k_vlow | ib_gap_db_Y | 0.969389 | 0.969385 | 0.96939 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | tos | x265_2160p_4k_vlow | detail_pulse_Y | 0.176139 | 0.176136 | 0.176126 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Y | 41.5258 | 41.526 | 41.5263 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Y | 0.975926 | 0.975927 | 0.975928 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1 | 1.00001 | 1.00012 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 0 | 1.12415e-06 | -2.77091e-05 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.00330796 | -0.00330591 | -0.00329784 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0107045 | 0.0106989 | 0.0106751 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | smear_Y | 0.894222 | 0.894271 | 0.894474 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000114725 | 0.000114755 | 0.000114881 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_I_Y | 38.7929 | 38.7929 | 38.7929 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_P_Y | 42.4363 | 42.4363 | 42.4362 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_B_Y | 41.3563 | 41.3565 | 41.3569 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_B_Y | 0.975688 | 0.975688 | 0.97569 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | tos | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.661868 | 0.661652 | 0.661184 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | tos | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.124538 | 0.124488 | 0.12447 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00002 | 1.0001 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00001 | 1.00011 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00001 | 1.00012 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00001 | 1.00015 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | psnr_Y | 34.1472 | 34.1472 | 34.147 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | ssim_Y | 0.914251 | 0.91425 | 0.914246 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1.00009 | 1.00046 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 0 | 1.71889e-06 | 1.80013e-05 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | flicker_excess_Y | -0.010129 | -0.010128 | -0.010126 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_2160p_4k_vlow | ghost_coeff_Y | 0.0295286 | 0.0295168 | 0.0294767 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | smear_Y | 0.760447 | 0.760593 | 0.761121 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000804027 | 0.000804323 | 0.000805327 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_2160p_4k_vlow | psnr_I_Y | 37.8151 | 37.8151 | 37.8151 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | psnr_P_Y | 35.1853 | 35.1853 | 35.1851 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | psnr_B_Y | 33.7762 | 33.7761 | 33.776 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | ssim_B_Y | 0.912246 | 0.912244 | 0.912241 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_2160p_4k_vlow | ib_gap_db_Y | 1.5636 | 1.56364 | 1.56354 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | sol | x265_2160p_4k_vlow | detail_pulse_Y | 0.582332 | 0.582249 | 0.582018 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.0001 | 1.00051 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.0001 | 1.00048 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00008 | 1.00043 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00009 | 1.00044 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Y | 31.3693 | 31.3691 | 31.3687 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Y | 0.858715 | 0.858716 | 0.858726 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1 | 1.00024 | 1.00086 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 0 | -8.47162e-06 | -6.29627e-05 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.0238509 | -0.0238514 | -0.0238497 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0547363 | 0.0547023 | 0.0545789 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | smear_Y | 0.671249 | 0.671558 | 0.672662 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00104419 | 0.00104492 | 0.00104738 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_I_Y | 35.581 | 35.581 | 35.581 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_P_Y | 32.3891 | 32.389 | 32.3887 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_B_Y | 30.997 | 30.9969 | 30.9964 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_B_Y | 0.853748 | 0.85375 | 0.853762 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| film24 | sol | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.56953 | 1.56955 | 1.5697 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | sol | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.481418 | 0.481392 | 0.481346 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00025 | 1.00078 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00022 | 1.00085 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00026 | 1.00092 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00026 | 1.00087 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Y | 30.9197 | 30.9204 | 30.921 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Y | 0.879927 | 0.87994 | 0.879958 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1.00011 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 0 | 4.68253e-06 | -1.10815e-06 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_2160p_4k_vlow | flicker_excess_Y | -0.0582624 | -0.0582509 | -0.0581795 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_2160p_4k_vlow | ghost_coeff_Y | 0.112015 | 0.111887 | 0.111718 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | smear_Y | 0.715935 | 0.716243 | 0.716786 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000530408 | 0.000530764 | 0.000531774 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_2160p_4k_vlow | psnr_I_Y | 34.1013 | 34.1013 | 34.1013 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | psnr_P_Y | 32.1529 | 32.1528 | 32.1528 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | psnr_B_Y | 30.5456 | 30.5465 | 30.5473 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | ssim_B_Y | 0.87537 | 0.875387 | 0.875409 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_2160p_4k_vlow | ib_gap_db_Y | 1.74007 | 1.73921 | 1.73838 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| nature30 | boat | x265_2160p_4k_vlow | detail_pulse_Y | 0.108617 | 0.108217 | 0.107905 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00018 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00015 | 0.999997 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00008 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00004 | 1.00002 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Y | 26.6418 | 26.6444 | 26.6421 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Y | 0.77106 | 0.771182 | 0.771257 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1 | 1.00026 | 1.00106 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 0 | -2.1287e-05 | -5.91972e-05 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0.999993 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.115904 | -0.11582 | -0.11565 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.184977 | 0.184316 | 0.183336 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | smear_Y | 0.58304 | 0.584781 | 0.58801 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000511649 | 0.000514137 | 0.000520596 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_I_Y | 29.3204 | 29.3204 | 29.3204 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_P_Y | 27.4861 | 27.4859 | 27.4829 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_B_Y | 26.3611 | 26.3644 | 26.3623 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_B_Y | 0.763001 | 0.763161 | 0.763255 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| nature30 | boat | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.25149 | 1.24807 | 1.24729 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| nature30 | boat | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.1682 | 0.166061 | 0.164503 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00021 | 1.00098 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00031 | 1.001 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00028 | 1.00094 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00024 | 1.00146 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Y | 26.7843 | 26.7843 | 26.7844 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Y | 0.703481 | 0.703484 | 0.703497 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1.00006 | 1.00023 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 0 | -3.45943e-07 | -6.23284e-06 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | flicker_excess_Y | -0.159927 | -0.159901 | -0.159823 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_4k_vlow | ghost_coeff_Y | 0.321029 | 0.321011 | 0.320931 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | smear_Y | 0.409866 | 0.410041 | 0.410728 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | halo_overshoot_Y | 0.00153505 | 0.00153545 | 0.00153676 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_I_Y | 29.6122 | 29.6122 | 29.6122 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_P_Y | 27.6604 | 27.6604 | 27.6601 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_B_Y | 26.4941 | 26.4941 | 26.4943 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_B_Y | 0.696779 | 0.696783 | 0.696802 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_2160p_4k_vlow | ib_gap_db_Y | 1.29935 | 1.29932 | 1.29888 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| sports50_60 | crowd | x265_2160p_4k_vlow | detail_pulse_Y | 0.352532 | 0.352128 | 0.351224 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00005 | 1.0002 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00011 | 1.00041 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00004 | 1.00034 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1 | 1.00011 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Y | 24.2299 | 24.2298 | 24.2286 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Y | 0.611574 | 0.611587 | 0.611624 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1 | 1.00047 | 1.00162 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 0 | 3.42934e-06 | -4.14208e-06 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 1 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.180914 | -0.180896 | -0.180779 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.390438 | 0.39042 | 0.390408 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | smear_Y | 0.363142 | 0.363666 | 0.365246 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00151443 | 0.00151612 | 0.00152104 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_I_Y | 25.3065 | 25.3065 | 25.3065 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_P_Y | 24.6163 | 24.6162 | 24.6149 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_B_Y | 24.0927 | 24.0925 | 24.0913 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_B_Y | 0.605739 | 0.605757 | 0.605803 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | td_s0.15_b1_u0.5:NA td_s0.02_b3_u1:NA |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.577065 | 0.577185 | 0.577122 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.27345 | 0.272542 | 0.270838 | td_s0.15_b1_u0.5:INFO td_s0.02_b3_u1:INFO |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1 | 1.00042 | 1.00191 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1 | 1.00086 | 1.00068 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1 | 1.00124 | 1.00064 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1 | 1.00194 | 1.00014 | td_s0.15_b1_u0.5:PASS td_s0.02_b3_u1:PASS |
