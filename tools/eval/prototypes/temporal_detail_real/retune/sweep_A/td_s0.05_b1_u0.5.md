<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.05_b1_u0.5 vs td_off (default)

Suite: `td_real_low_sweep` (8 cases). Candidate `td_s0.05_b1_u0.5` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

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
| detail_pulse_Y | 0.303694 | 0.303711 | -1.75773e-05 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00532806 | -0.00532854 | 4.83156e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0168565 | 0.0168709 | -1.44342e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.00376 | 1.00376 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.0147 | 1.0147 | 4.05441e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00009 | 1 | 9.44881e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00009 | 1 | 9.38604e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00009 | 1 | 9.07189e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00009 | 1 | 9.22181e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.0001 | 1 | 0.000100926 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00332618 | -0.00332618 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.0068421 | -0.00683871 | -3.39264e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -3.39264e-06 | 0 | -3.39264e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.598868 | 0.598868 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.829676 | 0.829599 | 7.78595e-05 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000275984 | 0.000275783 | 2.0121e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.807063 | 0.807136 | -7.33783e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.855851 | 0.855784 | 6.67581e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.90621 | 0.906148 | 6.15425e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.940845 | 0.940795 | 4.93721e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 39.5372 | 39.5372 | 8.24218e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 43.2064 | 43.2064 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.8076 | 42.8076 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 40.687 | 40.687 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 40.3821 | 40.3821 | 7.45185e-06 | 4 | 0 | PASS:4 |
| psnr_Y | 39.7396 | 39.7395 | 6.38586e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.837492 | 0.83735 | 0.00014131 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.946556 | 0.946555 | 1.14003e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.951896 | 0.951896 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.950357 | 0.950357 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.947656 | 0.947655 | 1.05766e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0641056 | 0.0641056 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 9, NA 23, PASS 64)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.290634 | 0.288601 | 0.00203244 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 0.999997 | 1 | -3.36934e-06 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.0871627 | 0.088156 | -0.000993319 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0270147 | -0.0270135 | -1.20424e-06 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0925738 | 0.0928845 | -0.0003107 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.696661 | 0.696661 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.01116 | 1.01119 | -2.18516e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.0001 | 1 | 9.91101e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00009 | 1 | 8.95909e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00013 | 1 | 0.000125309 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00011 | 1 | 0.000114485 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00005 | 1 | 5.39806e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.278869 | 0.278869 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0366005 | 0.036602 | -1.51308e-06 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -1.51308e-06 | 0 | -1.51308e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.349037 | 0.349037 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.83847 | 0.838393 | 7.65692e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000365575 | 0.000364298 | 1.27701e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.25813 | 1.26022 | -0.00209681 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.852254 | 0.851863 | 0.000390737 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.896894 | 0.896538 | 0.000356684 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.932396 | 0.932113 | 0.000283041 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 30.7093 | 30.7074 | 0.00195329 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.7776 | 37.7776 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 40.1287 | 40.1287 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 34.3295 | 34.3295 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 31.805 | 31.8052 | -0.000150625 | 2 | 0 | PASS:2 |
| psnr_Y | 30.9913 | 30.9898 | 0.00153648 | 2 | 0 | PASS:2 |
| smear_Y | 0.762232 | 0.761307 | 0.000924972 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.88078 | 0.880728 | 5.13645e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.9067 | 0.9067 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.952778 | 0.952778 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.884974 | 0.884935 | 3.91737e-05 | 2 | 0 | PASS:2 |
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
| detail_pulse_Y | 0.261839 | 0.262519 | -0.000680065 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.115085 | -0.115113 | 2.80489e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.286475 | 0.286511 | -3.57597e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.153179 | 0.153179 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.243579 | 0.243587 | -8.08962e-06 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00021 | 1 | 0.00020678 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000199449 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.0003 | 1 | 0.00030345 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.0002 | 1 | 0.000198234 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00019 | 1 | 0.000191539 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.531016 | 0.531016 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.3623 | 0.362292 | 8.15006e-06 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 8.15006e-06 | 0 | 8.15006e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.093969 | 0.093969 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.334028 | 0.333956 | 7.15681e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000844761 | 0.000843747 | 1.01455e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.980844 | 0.980999 | -0.000155353 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.655294 | 0.655002 | 0.000291967 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.738162 | 0.737882 | 0.000279639 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.809129 | 0.808892 | 0.000237572 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 26.7041 | 26.7041 | 2.48611e-05 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.9624 | 33.9624 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 34.5098 | 34.5098 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 29.6011 | 29.6011 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 27.5479 | 27.5481 | -0.000138204 | 2 | 0 | PASS:2 |
| psnr_Y | 26.9284 | 26.9284 | -7.71258e-06 | 2 | 0 | PASS:2 |
| smear_Y | 0.492995 | 0.492434 | 0.000561725 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.710121 | 0.710095 | 2.61256e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.834848 | 0.834848 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.8561 | 0.8561 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.716245 | 0.716225 | 2.01973e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | -3.73059e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cb | 49.1802 | 49.1802 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cr | 48.5561 | 48.5561 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | -1.08936e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cb | 0.992429 | 0.992429 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cr | 0.991153 | 0.991153 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Y | 0.751795 | 0.751795 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Y | 1.03677 | 1.03677 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Y | -0.0215625 | -0.0215625 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.50032 | 0.50032 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.978132 | 0.978132 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.00953641 | 0.00953641 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269907 | -0.000269955 | 4.80194e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.0035544 | 0.00355461 | -2.03339e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886198 | 0.886196 | 1.90499e-06 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.765e-05 | 2.76493e-05 | 7.63527e-10 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s1_Y | 0.855859 | 0.855858 | 7.21264e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s2_Y | 0.925526 | 0.925526 | 6.95474e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s3_Y | 0.964091 | 0.96409 | 5.89078e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | -1.38887e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | -4.52329e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | -1.16831e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.64464 | 0.644637 | 3.26839e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164206 | 0.164206 | 1.08288e-07 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3918 | 44.3917 | 0.000176302 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cb | 49.3269 | 49.3269 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cr | 48.9454 | 48.9454 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 2.01147e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cb | 0.991923 | 0.991923 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cr | 0.990929 | 0.990929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.939277 | 0.939242 | 3.45915e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.0068 | 1.0068 | 4.29893e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.00570208 | -0.00570067 | -1.41526e-06 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.486201 | 0.486201 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.00659 | 1.00659 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.0124173 | -0.0124173 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00004 | 1 | 3.68292e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -1.41526e-06 | 0 | -1.41526e-06 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200556 | -0.00200683 | 1.2772e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00575791 | 0.00576678 | -8.87401e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.931555 | 0.93148 | 7.51513e-05 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.16022e-05 | 5.15697e-05 | 3.24777e-08 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.916372 | 0.91634 | 3.14957e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.950649 | 0.950619 | 2.9394e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.972495 | 0.972471 | 2.35919e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0902 | 45.0903 | -6.12456e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2808 | 44.2805 | 0.000253106 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984581 | 2.92501e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425031 | 0.425336 | -0.000304851 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.0739306 | 0.073986 | -5.54028e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00003 | 1 | 3.31235e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00003 | 1 | 3.45776e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00004 | 1 | 3.88342e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00004 | 1 | 3.83129e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9552 | -9.4767e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cb | 38.1562 | 38.1562 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cr | 37.9064 | 37.9064 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | -5.43691e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cb | 0.929937 | 0.929937 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cr | 0.92961 | 0.92961 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Y | 0.821737 | 0.821634 | 0.000103057 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00628 | 1.00629 | -9.13491e-06 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Y | -0.0042433 | -0.00424658 | 3.28168e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.7274 | 0.7274 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.01428 | 1.01428 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Cb | -0.00766725 | -0.00766725 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00013 | 1 | 0.000125429 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 3.28168e-06 | 0 | 3.28168e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.0059953 | -0.00599649 | 1.19104e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205739 | 0.0205843 | -1.04238e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797256 | 0.797125 | 0.00013094 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000438154 | 0.00043796 | 1.94606e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s1_Y | 0.827656 | 0.827595 | 6.02939e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s2_Y | 0.87895 | 0.878893 | 5.68911e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s3_Y | 0.916425 | 0.916379 | 4.56486e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_Y | 0.0588235 | 0.0588235 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | -1.10801e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7167 | -9.15286e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | -6.78455e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.954458 | 0.95446 | -1.41005e-06 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562052 | 0.562107 | -5.4916e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00014 | 1 | 0.000140458 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00012 | 1 | 0.000118303 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00012 | 1 | 0.000116649 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.00012 | 1 | 0.000122805 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2856 | 33.2855 | 9.23398e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cb | 36.1625 | 36.1625 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cr | 35.8225 | 35.8225 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.890971 | 0.890966 | 4.58408e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cb | 0.893296 | 0.893296 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cr | 0.889736 | 0.889736 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.805897 | 0.805723 | 0.00017379 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.00894 | 1.00892 | 2.10536e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.00413946 | 0.0041549 | -1.5437e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.681551 | 0.681551 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.01602 | 1.01602 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.00275653 | -0.00275653 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00022 | 1 | 0.000215694 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -1.5437e-05 | 0 | -1.5437e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130415 | -0.0130409 | -5.8363e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0375396 | 0.0375779 | -3.82356e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734958 | 0.734601 | 0.000357243 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000586531 | 0.000585954 | 5.76992e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.823517 | 0.823343 | 0.000174522 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.869714 | 0.869555 | 0.00015919 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.910368 | 0.910241 | 0.000127659 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_Y | 0.0693878 | 0.0693878 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0093 | 34.0091 | 0.000103522 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.991 | 32.9909 | 9.02575e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887702 | 0.887697 | 4.95776e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20412 | 1.20411 | 9.47976e-06 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414585 | 0.414546 | 3.99013e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000201861 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00021 | 1 | 0.000209995 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00021 | 1 | 0.000213389 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00024 | 1 | 0.000242585 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1654 | 33.1646 | 0.000740244 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cb | 38.3677 | 38.3677 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cr | 41.416 | 41.416 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918384 | 0.918374 | 9.74218e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cb | 0.916786 | 0.916786 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cr | 0.964397 | 0.964397 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Y | 0.904219 | 0.904219 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Y | 1.02684 | 1.02684 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Y | 0.0582394 | 0.0582394 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.488734 | 0.488734 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.875006 | 0.875006 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.155928 | 0.155928 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00781259 | -0.00781908 | 6.49351e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0653802 | 0.0654834 | -0.000103258 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.806534 | 0.806187 | 0.00034698 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332441 | 0.000332055 | 3.86258e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s1_Y | 0.876392 | 0.876241 | 0.000151149 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s2_Y | 0.915349 | 0.91521 | 0.000139569 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s3_Y | 0.945596 | 0.945487 | 0.000109201 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_Y | 0.360872 | 0.358479 | 0.00239272 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9523 | 33.9524 | -3.98476e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.892 | 32.8911 | 0.000936294 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916019 | 0.916007 | 1.23508e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21496 | 1.21593 | -0.000974145 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0614587 | 0.0618573 | -0.000398585 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8172 | 28.8149 | 0.00233271 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cb | 37.1876 | 37.1876 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cr | 38.8413 | 38.8413 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851565 | 0.851496 | 6.86051e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cb | 0.896615 | 0.896615 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cr | 0.941159 | 0.941159 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.77272 | 0.772567 | 0.000153138 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.995488 | 0.995531 | -4.37033e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.0149616 | 0.0149646 | -3.02616e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.20934 | 0.20934 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.518317 | 0.518317 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.40181 | 0.40181 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.0002 | 1 | 0.00019822 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -3.02616e-06 | 0 | -3.02616e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 0.999993 | 1 | -6.73868e-06 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0462168 | -0.0462079 | -8.90199e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.119767 | 0.120286 | -0.000518141 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.71793 | 0.716427 | 0.00150296 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000398709 | 0.000396541 | 2.16776e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.828116 | 0.827485 | 0.000630326 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.878439 | 0.877865 | 0.000573799 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.919196 | 0.918739 | 0.000456881 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_Y | 0.220395 | 0.218723 | 0.00167216 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.6577 | 29.658 | -0.000261403 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5266 | 28.5237 | 0.00297028 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.84554 | 0.84545 | 9.03782e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30129 | 1.30451 | -0.00321947 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.112867 | 0.114455 | -0.00158805 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00018 | 1 | 0.000179182 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00025 | 1 | 0.000250617 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00023 | 1 | 0.000228969 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00011 | 1 | 0.000107961 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1923 | 28.1923 | 4.20794e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cb | 34.7596 | 34.7596 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cr | 35.3812 | 35.3812 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.74753 | 0.747524 | 6.18946e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cb | 0.850081 | 0.850081 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cr | 0.872604 | 0.872604 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Y | 0.321504 | 0.321503 | 1.61125e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Y | 0.218106 | 0.218103 | 2.84244e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Y | 0.457718 | 0.45772 | -1.76513e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.0833721 | 0.0833721 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.163468 | 0.163468 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.596199 | 0.596199 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00001 | 1 | 5.01163e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | -1.76513e-06 | 0 | -1.76513e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101975 | -0.101988 | 1.32612e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254762 | 0.254786 | -2.35862e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.509343 | 0.509077 | 0.000265377 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837482 | 0.000837101 | 3.81555e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s1_Y | 0.649919 | 0.649785 | 0.000133229 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s2_Y | 0.736133 | 0.736 | 0.000132687 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s3_Y | 0.810596 | 0.810482 | 0.000113549 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9644 | 28.9645 | -9.40629e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9203 | 27.9203 | 7.52985e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.741988 | 0.74198 | 8.14929e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20813 | 1.20829 | -0.000164841 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.296407 | 0.296664 | -0.000257486 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 2.94144e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 9.71184e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00002 | 1 | 2.48237e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 0.999989 | 1 | -1.0881e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6644 | 25.6645 | -5.75046e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cb | 33.1651 | 33.1651 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cr | 33.6384 | 33.6384 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.68496 | 0.684926 | 3.42051e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cb | 0.819616 | 0.819616 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cr | 0.839597 | 0.839597 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.346551 | 0.346409 | 0.000141525 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.269052 | 0.269071 | -1.90217e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.266882 | 0.266864 | 1.80653e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.104566 | 0.104566 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.142889 | 0.142889 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.465832 | 0.465832 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00041 | 1 | 0.000408548 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 1.80653e-05 | 0 | 1.80653e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128194 | -0.128237 | 4.28367e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318188 | 0.318236 | -4.79332e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.476648 | 0.47579 | 0.000858073 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000852041 | 0.000850393 | 1.64755e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.660669 | 0.660218 | 0.000450706 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.740191 | 0.739764 | 0.000426592 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.807663 | 0.807301 | 0.000361595 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1314 | 26.1316 | -0.000182345 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.488 | 25.488 | -2.55763e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678253 | 0.678209 | 4.41018e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753559 | 0.753705 | -0.000145864 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.22727 | 0.228373 | -0.00110264 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.0004 | 1 | 0.000395956 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.0006 | 1 | 0.000597189 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00037 | 1 | 0.000371644 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00039 | 1 | 0.000393959 | worse | PASS |  |
