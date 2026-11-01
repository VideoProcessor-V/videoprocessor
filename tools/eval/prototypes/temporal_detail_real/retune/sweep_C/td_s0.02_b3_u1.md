<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.02_b3_u1 vs td_off (default)

Suite: `td_real_low_sweep` (8 cases). Candidate `td_s0.02_b3_u1` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

## Per-content-type summary

### film24  (INFO 16, LIMIT 12, NA 46, PASS 134)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | 0 | 0 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_C | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| detail_pulse_Y | 0.303724 | 0.303711 | 1.32035e-05 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00532564 | -0.00532854 | 2.90001e-06 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0168203 | 0.0168709 | -5.06176e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.00376 | 1.00376 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.01471 | 1.0147 | 1.0147e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00025 | 1 | 0.000248169 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00023 | 1 | 0.000231463 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00026 | 1 | 0.000255652 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00025 | 1 | 0.000246307 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00026 | 1 | 0.000262952 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00332618 | -0.00332618 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00684421 | -0.00683871 | -5.49851e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -5.49851e-06 | 0 | -5.49851e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.598868 | 0.598868 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.829801 | 0.829599 | 0.000202673 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000276524 | 0.000275783 | 7.41115e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.807058 | 0.807136 | -7.8157e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.856011 | 0.855784 | 0.000226945 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.906359 | 0.906148 | 0.000210819 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.940966 | 0.940795 | 0.000170384 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 39.5372 | 39.5372 | 1.82167e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 43.2064 | 43.2064 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.8076 | 42.8076 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 40.687 | 40.687 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 40.3821 | 40.3821 | -7.79084e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 39.7395 | 39.7395 | -4.56813e-06 | 4 | 0 | PASS:4 |
| smear_Y | 0.837845 | 0.83735 | 0.000494739 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.946559 | 0.946555 | 3.5876e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.951896 | 0.951896 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.950357 | 0.950357 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.947659 | 0.947655 | 3.43883e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0641056 | 0.0641056 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 9, NA 23, PASS 64)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.291714 | 0.288601 | 0.00311326 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.0867832 | 0.088156 | -0.0013728 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.026963 | -0.0270135 | 5.04842e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0922125 | 0.0928845 | -0.000672039 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.696661 | 0.696661 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.01115 | 1.01119 | -3.825e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00029 | 1 | 0.000290417 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00023 | 1 | 0.00022735 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00037 | 1 | 0.000367998 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00034 | 1 | 0.000340045 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00018 | 1 | 0.000177451 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.278869 | 0.278869 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0365888 | 0.036602 | -1.31696e-05 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -1.31696e-05 | 0 | -1.31696e-05 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.349037 | 0.349037 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.838617 | 0.838393 | 0.00022437 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000367774 | 0.000364298 | 3.47667e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.25648 | 1.26022 | -0.00374334 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.85274 | 0.851863 | 0.000876363 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.897352 | 0.896538 | 0.000813958 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.932767 | 0.932113 | 0.000653594 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 30.7098 | 30.7074 | 0.00246235 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.7776 | 37.7776 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 40.1287 | 40.1287 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 34.3295 | 34.3295 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 31.8038 | 31.8052 | -0.00134433 | 2 | 0 | PASS:2 |
| psnr_Y | 30.9915 | 30.9898 | 0.00171735 | 2 | 0 | PASS:2 |
| smear_Y | 0.763414 | 0.761307 | 0.00210738 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.880809 | 0.880728 | 8.05372e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.9067 | 0.9067 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.952778 | 0.952778 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.884997 | 0.884935 | 6.2228e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |

### sports50_60  (INFO 8, LIMIT 10, NA 23, PASS 63)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| detail_pulse_Y | 0.26058 | 0.262519 | -0.00193877 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.114983 | -0.115113 | 0.000129466 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.286365 | 0.286511 | -0.000146075 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.153179 | 0.153179 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.243316 | 0.243587 | -0.00027077 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00051 | 1 | 0.000513234 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00058 | 1 | 0.000576116 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00004 | 1 | 4.45744e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 0.999972 | 1 | -2.75933e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00006 | 1 | 5.53102e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.531016 | 0.531016 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.362312 | 0.362292 | 2.00886e-05 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 2.00886e-05 | 0 | 2.00886e-05 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.093969 | 0.093969 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.334133 | 0.333956 | 0.0001774 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000847544 | 0.000843747 | 3.79727e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.980377 | 0.980999 | -0.000621966 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.655988 | 0.655002 | 0.000986538 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.738837 | 0.737882 | 0.000954803 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.809711 | 0.808892 | 0.000819471 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 26.7036 | 26.7041 | -0.00047295 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.9624 | 33.9624 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 34.5098 | 34.5098 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 29.6011 | 29.6011 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 27.5469 | 27.5481 | -0.00116094 | 2 | 0 | PASS:2 |
| psnr_Y | 26.9278 | 26.9284 | -0.000601258 | 2 | 0 | PASS:2 |
| smear_Y | 0.494399 | 0.492434 | 0.00196529 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.710162 | 0.710095 | 6.77368e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.834848 | 0.834848 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.8561 | 0.8561 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.716279 | 0.716225 | 5.43599e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | -5.39144e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cb | 49.1802 | 49.1802 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cr | 48.5561 | 48.5561 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | -3.69228e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cb | 0.992429 | 0.992429 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cr | 0.991153 | 0.991153 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Y | 0.751795 | 0.751795 | 1.21072e-08 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Y | 1.03677 | 1.03677 | 7.4759e-09 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Y | -0.0215625 | -0.0215625 | 1.01813e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.50032 | 0.50032 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.978132 | 0.978132 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.00953641 | 0.00953641 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1.61043e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 1.01813e-08 | 0 | 1.01813e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269908 | -0.000269955 | 4.68847e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355361 | 0.00355461 | -9.95588e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886205 | 0.886196 | 9.48857e-06 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.7653e-05 | 2.76493e-05 | 3.67537e-09 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s1_Y | 0.855862 | 0.855858 | 3.20745e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s2_Y | 0.925529 | 0.925526 | 3.23092e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s3_Y | 0.964093 | 0.96409 | 2.71631e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | -1.00388e-05 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | -4.21343e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | -3.97708e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.644632 | 0.644637 | -4.85705e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.1642 | 0.164206 | -6.40943e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 9.14794e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | -4.4831e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | -2.37216e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 7.06194e-08 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3917 | 44.3917 | 4.39545e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cb | 49.3269 | 49.3269 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cr | 48.9454 | 48.9454 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 2.51912e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cb | 0.991923 | 0.991923 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cr | 0.990929 | 0.990929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.939286 | 0.939242 | 4.36549e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.0068 | 1.0068 | 4.39508e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.00570111 | -0.00570067 | -4.44302e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.486201 | 0.486201 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.00659 | 1.00659 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.0124173 | -0.0124173 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00005 | 1 | 4.64788e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -4.44302e-07 | 0 | -4.44302e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00199855 | -0.00200683 | 8.28111e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00574039 | 0.00576678 | -2.63894e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.931715 | 0.93148 | 0.000235281 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.16871e-05 | 5.15697e-05 | 1.17406e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.916436 | 0.91634 | 9.51032e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.950709 | 0.950619 | 8.94007e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.972543 | 0.972471 | 7.1599e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0898 | 45.0903 | -0.000498631 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2807 | 44.2805 | 0.000200649 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984582 | 0.984581 | 4.65104e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.424714 | 0.425336 | -0.000621939 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.0738734 | 0.073986 | -0.000112648 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00005 | 1 | 4.90384e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00004 | 1 | 4.06934e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00004 | 1 | 4.42854e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00005 | 1 | 5.41107e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9553 | 35.9552 | 0.000102438 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cb | 38.1562 | 38.1562 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cr | 37.9064 | 37.9064 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | -1.60975e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cb | 0.929937 | 0.929937 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cr | 0.92961 | 0.92961 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Y | 0.821875 | 0.821634 | 0.000240828 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00625 | 1.00629 | -3.64336e-05 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Y | -0.00421732 | -0.00424658 | 2.92631e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.7274 | 0.7274 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.01428 | 1.01428 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Cb | -0.00766725 | -0.00766725 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00029 | 1 | 0.000293109 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 2.92631e-05 | 0 | 2.92631e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599395 | -0.00599649 | 2.53198e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205535 | 0.0205843 | -3.08517e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797525 | 0.797125 | 0.000400105 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000438523 | 0.00043796 | 5.63085e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s1_Y | 0.827758 | 0.827595 | 0.000162918 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s2_Y | 0.879047 | 0.878893 | 0.00015395 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s3_Y | 0.916503 | 0.916379 | 0.000124009 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_Y | 0.0588235 | 0.0588235 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | -7.57404e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7168 | 35.7167 | 0.000154508 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | -4.54403e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.954233 | 0.95446 | -0.000226713 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562039 | 0.562107 | -6.7879e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.0003 | 1 | 0.000304851 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00032 | 1 | 0.000315455 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.0003 | 1 | 0.000301052 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.00024 | 1 | 0.000244559 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2853 | 33.2855 | -0.000159273 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cb | 36.1625 | 36.1625 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cr | 35.8225 | 35.8225 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.89098 | 0.890966 | 1.37013e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cb | 0.893296 | 0.893296 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cr | 0.889736 | 0.889736 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.806249 | 0.805723 | 0.000526196 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.00899 | 1.00892 | 7.26192e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.00410407 | 0.0041549 | -5.0823e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.681551 | 0.681551 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.01602 | 1.01602 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.00275653 | -0.00275653 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00065 | 1 | 0.000653073 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -5.0823e-05 | 0 | -5.0823e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130401 | -0.0130409 | 7.40077e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0374336 | 0.0375779 | -0.000144234 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.735935 | 0.734601 | 0.00133408 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000588234 | 0.000585954 | 2.28029e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.823989 | 0.823343 | 0.00064655 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.870151 | 0.869555 | 0.000596693 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.910724 | 0.910241 | 0.000483212 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_Y | 0.0693878 | 0.0693878 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0094 | 34.0091 | 0.000272777 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.9906 | 32.9909 | -0.000278077 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887712 | 0.887697 | 1.43795e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20465 | 1.20411 | 0.000540881 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414785 | 0.414546 | 0.00023975 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00057 | 1 | 0.000571872 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00067 | 1 | 0.000666503 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00064 | 1 | 0.000639915 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00075 | 1 | 0.000753066 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1661 | 33.1646 | 0.00144445 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cb | 38.3677 | 38.3677 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cr | 41.416 | 41.416 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918393 | 0.918374 | 1.91764e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cb | 0.916786 | 0.916786 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cr | 0.964397 | 0.964397 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Y | 0.904219 | 0.904219 | 4.45891e-08 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Y | 1.02684 | 1.02684 | 9.31979e-08 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Y | 0.0582393 | 0.0582394 | -1.00655e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.488734 | 0.488734 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.875006 | 0.875006 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.155928 | 0.155928 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 4.93123e-08 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | -1.00655e-07 | 0 | -1.00655e-07 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00777139 | -0.00781908 | 4.76843e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0651768 | 0.0654834 | -0.000306575 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.8071 | 0.806187 | 0.000913905 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000333303 | 0.000332055 | 1.24816e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s1_Y | 0.876631 | 0.876241 | 0.00038928 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s2_Y | 0.915574 | 0.91521 | 0.000364648 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s3_Y | 0.945777 | 0.945487 | 0.000289764 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_Y | 0.361775 | 0.358479 | 0.00329564 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.952 | 33.9524 | -0.000338471 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.893 | 32.8911 | 0.00188947 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916031 | 0.916007 | 2.44792e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21372 | 1.21593 | -0.00221098 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.061059 | 0.0618573 | -0.000798302 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1.12237e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | -1.16004e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | -7.01232e-08 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 2.48633e-07 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8169 | 28.8149 | 0.00199026 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cb | 37.1876 | 37.1876 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cr | 38.8413 | 38.8413 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851601 | 0.851496 | 0.00010528 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cb | 0.896615 | 0.896615 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cr | 0.941159 | 0.941159 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.773016 | 0.772567 | 0.000448695 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.995455 | 0.995531 | -7.65932e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.0149383 | 0.0149646 | -2.62385e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.20934 | 0.20934 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.518317 | 0.518317 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.40181 | 0.40181 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00058 | 1 | 0.000580785 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -2.62385e-05 | 0 | -2.62385e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0461546 | -0.0462079 | 5.32841e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.119248 | 0.120286 | -0.0010375 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.719728 | 0.716427 | 0.00330086 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000402246 | 0.000396541 | 5.70519e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.828849 | 0.827485 | 0.00136345 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.879129 | 0.877865 | 0.00126327 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.919756 | 0.918739 | 0.00101742 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_Y | 0.221654 | 0.218723 | 0.00293088 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.6557 | 29.658 | -0.0023502 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5267 | 28.5237 | 0.00303523 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.845586 | 0.84545 | 0.000136595 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.29924 | 1.30451 | -0.00527569 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.112507 | 0.114455 | -0.00194729 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00045 | 1 | 0.000454588 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00074 | 1 | 0.000736112 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00068 | 1 | 0.000680161 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00035 | 1 | 0.000354653 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1925 | 28.1923 | 0.000171865 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cb | 34.7596 | 34.7596 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cr | 35.3812 | 35.3812 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.747547 | 0.747524 | 2.3426e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cb | 0.850081 | 0.850081 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cr | 0.872604 | 0.872604 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Y | 0.321513 | 0.321503 | 1.00477e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Y | 0.218105 | 0.218103 | 2.26751e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Y | 0.457721 | 0.45772 | 1.0344e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.0833721 | 0.0833721 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.163468 | 0.163468 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.596199 | 0.596199 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00003 | 1 | 3.12523e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 1.0344e-06 | 0 | 1.0344e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101863 | -0.101988 | 0.00012508 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254659 | 0.254786 | -0.000126622 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.510182 | 0.509077 | 0.00110516 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000838641 | 0.000837101 | 1.54012e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s1_Y | 0.650321 | 0.649785 | 0.000535498 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s2_Y | 0.73654 | 0.736 | 0.000539702 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s3_Y | 0.810952 | 0.810482 | 0.00047004 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.964 | 28.9645 | -0.000576787 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9206 | 27.9203 | 0.000353831 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.742012 | 0.74198 | 3.12783e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20739 | 1.20829 | -0.000902904 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.295811 | 0.296664 | -0.000852712 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00003 | 1 | 2.86829e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00005 | 1 | 4.65979e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00005 | 1 | 4.59761e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.00002 | 1 | 2.33591e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6631 | 25.6645 | -0.00137438 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cb | 33.1651 | 33.1651 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cr | 33.6384 | 33.6384 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.685012 | 0.684926 | 8.52938e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cb | 0.819616 | 0.819616 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cr | 0.839597 | 0.839597 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.346754 | 0.346409 | 0.000344752 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.268527 | 0.269071 | -0.000543808 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.266903 | 0.266864 | 3.91429e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.104566 | 0.104566 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.142889 | 0.142889 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.465832 | 0.465832 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.001 | 1 | 0.000995215 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 3.91429e-05 | 0 | 3.91429e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128103 | -0.128237 | 0.000133852 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.31807 | 0.318236 | -0.000165527 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.478616 | 0.47579 | 0.00282542 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000856447 | 0.000850393 | 6.05442e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.661656 | 0.660218 | 0.00143758 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.741134 | 0.739764 | 0.0013699 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.80847 | 0.807301 | 0.0011689 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1299 | 26.1316 | -0.0017451 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.4867 | 25.488 | -0.00129973 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678313 | 0.678209 | 0.000104195 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753364 | 0.753705 | -0.000341028 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.225348 | 0.228373 | -0.00302482 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00112 | 1 | 0.00112355 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00004 | 1 | 4.2551e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 0.999899 | 1 | -0.000101163 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00009 | 1 | 8.72612e-05 | worse | PASS |  |
