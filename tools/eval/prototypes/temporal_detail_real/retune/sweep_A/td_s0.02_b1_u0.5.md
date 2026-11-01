<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.02_b1_u0.5 vs td_off (default)

Suite: `td_real_low_sweep` (8 cases). Candidate `td_s0.02_b1_u0.5` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

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
| detail_pulse_Y | 0.303694 | 0.303711 | -1.7712e-05 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00532809 | -0.00532854 | 4.48345e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0168531 | 0.0168709 | -1.78138e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.00376 | 1.00376 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.0147 | 1.0147 | 3.48527e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00009 | 1 | 9.12032e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00009 | 1 | 8.98293e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00009 | 1 | 8.94577e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00008 | 1 | 8.35782e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.0001 | 1 | 0.000103091 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00332618 | -0.00332618 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.0068407 | -0.00683871 | -1.98538e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -1.98538e-06 | 0 | -1.98538e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.598868 | 0.598868 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.829673 | 0.829599 | 7.46097e-05 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000276032 | 0.000275783 | 2.48924e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.807069 | 0.807136 | -6.71019e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.855867 | 0.855784 | 8.29385e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.906225 | 0.906148 | 7.64767e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.940857 | 0.940795 | 6.14521e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 39.5372 | 39.5372 | 5.68126e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 43.2064 | 43.2064 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.8076 | 42.8076 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 40.687 | 40.687 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 40.3821 | 40.3821 | -1.19503e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 39.7396 | 39.7395 | 4.04786e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.837525 | 0.83735 | 0.000174729 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.946557 | 0.946555 | 1.46755e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.951896 | 0.951896 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.950357 | 0.950357 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.947656 | 0.947655 | 1.28546e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0641056 | 0.0641056 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 9, NA 23, PASS 64)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.290821 | 0.288601 | 0.0022196 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.0870009 | 0.088156 | -0.00115509 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0269992 | -0.0270135 | 1.43491e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0925075 | 0.0928845 | -0.000377027 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.696661 | 0.696661 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.01116 | 1.01119 | -2.40841e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00011 | 1 | 0.000108383 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.0001 | 1 | 9.91879e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00014 | 1 | 0.000136282 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00012 | 1 | 0.000123272 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00006 | 1 | 6.12427e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.278869 | 0.278869 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0365998 | 0.036602 | -2.19986e-06 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -2.19986e-06 | 0 | -2.19986e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.349037 | 0.349037 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.838477 | 0.838393 | 8.37328e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000365903 | 0.000364298 | 1.60518e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.25797 | 1.26022 | -0.0022542 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.852332 | 0.851863 | 0.000468792 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.896967 | 0.896538 | 0.000429328 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.932455 | 0.932113 | 0.000341891 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 30.7094 | 30.7074 | 0.00205952 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.7776 | 37.7776 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 40.1287 | 40.1287 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 34.3295 | 34.3295 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 31.805 | 31.8052 | -0.000204285 | 2 | 0 | PASS:2 |
| psnr_Y | 30.9914 | 30.9898 | 0.00161144 | 2 | 0 | PASS:2 |
| smear_Y | 0.762444 | 0.761307 | 0.00113726 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.880785 | 0.880728 | 5.64602e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.9067 | 0.9067 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.952778 | 0.952778 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.884978 | 0.884935 | 4.3197e-05 | 2 | 0 | PASS:2 |
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
| detail_pulse_Y | 0.26184 | 0.262519 | -0.00067828 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.115104 | -0.115113 | 8.89519e-06 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.286473 | 0.286511 | -3.82892e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.153179 | 0.153179 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.243505 | 0.243587 | -8.17181e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00032 | 1 | 0.000319385 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00032 | 1 | 0.000324176 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00027 | 1 | 0.000268525 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00021 | 1 | 0.000209437 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00008 | 1 | 7.54765e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.531016 | 0.531016 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.362321 | 0.362292 | 2.95968e-05 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 2.95968e-05 | 0 | 2.95968e-05 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.093969 | 0.093969 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.334067 | 0.333956 | 0.000110575 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000844943 | 0.000843747 | 1.19562e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.980748 | 0.980999 | -0.000251077 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.655344 | 0.655002 | 0.000342447 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.738208 | 0.737882 | 0.000326288 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.809168 | 0.808892 | 0.00027587 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 26.7042 | 26.7041 | 3.6636e-05 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.9624 | 33.9624 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 34.5098 | 34.5098 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 29.6011 | 29.6011 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 27.5478 | 27.5481 | -0.000227436 | 2 | 0 | PASS:2 |
| psnr_Y | 26.9284 | 26.9284 | -1.68174e-05 | 2 | 0 | PASS:2 |
| smear_Y | 0.493096 | 0.492434 | 0.000662594 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.710125 | 0.710095 | 3.03222e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.834848 | 0.834848 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.8561 | 0.8561 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.716248 | 0.716225 | 2.31844e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | -4.61071e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cb | 49.1802 | 49.1802 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cr | 48.5561 | 48.5561 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | -1.39179e-08 | same | PASS |  |
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
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269886 | -0.000269955 | 6.87541e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355436 | 0.00355461 | -2.44488e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886198 | 0.886196 | 2.40308e-06 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.76501e-05 | 2.76493e-05 | 8.61615e-10 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s1_Y | 0.855859 | 0.855858 | 8.52275e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s2_Y | 0.925527 | 0.925526 | 8.25229e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s3_Y | 0.964091 | 0.96409 | 6.97508e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | -2.34745e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | -5.40789e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | -1.47207e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.64464 | 0.644637 | 3.28687e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164206 | 0.164206 | -3.75774e-07 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3918 | 44.3917 | 0.000117419 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cb | 49.3269 | 49.3269 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cr | 48.9454 | 48.9454 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 1.45954e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cb | 0.991923 | 0.991923 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cr | 0.990929 | 0.990929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.93926 | 0.939242 | 1.73497e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.0068 | 1.0068 | 2.8993e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.00570104 | -0.00570067 | -3.71005e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.486201 | 0.486201 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.00659 | 1.00659 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.0124173 | -0.0124173 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00002 | 1 | 1.8472e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -3.71005e-07 | 0 | -3.71005e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200603 | -0.00200683 | 8.04291e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00575919 | 0.00576678 | -7.59077e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.931552 | 0.93148 | 7.24344e-05 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.16072e-05 | 5.15697e-05 | 3.75087e-08 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.916371 | 0.91634 | 3.08965e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.950648 | 0.950619 | 2.87562e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.972494 | 0.972471 | 2.28087e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0902 | 45.0903 | -3.92872e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2807 | 44.2805 | 0.000168142 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984581 | 2.0891e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425135 | 0.425336 | -0.000201336 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.0739448 | 0.073986 | -4.12301e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00002 | 1 | 1.62075e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00002 | 1 | 1.72272e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00002 | 1 | 1.73447e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00002 | 1 | 2.24171e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9552 | -1.27456e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cb | 38.1562 | 38.1562 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cr | 37.9064 | 37.9064 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | -6.76493e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cb | 0.929937 | 0.929937 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cr | 0.92961 | 0.92961 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Y | 0.821739 | 0.821634 | 0.000105039 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00628 | 1.00629 | -7.9784e-06 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Y | -0.00424395 | -0.00424658 | 2.63031e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.7274 | 0.7274 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.01428 | 1.01428 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Cb | -0.00766725 | -0.00766725 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00013 | 1 | 0.000127842 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 2.63031e-06 | 0 | 2.63031e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599546 | -0.00599649 | 1.02968e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205728 | 0.0205843 | -1.15811e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.79727 | 0.797125 | 0.00014494 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000438178 | 0.00043796 | 2.18498e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s1_Y | 0.827662 | 0.827595 | 6.66501e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s2_Y | 0.878956 | 0.878893 | 6.28418e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s3_Y | 0.91643 | 0.916379 | 5.03719e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_Y | 0.0588235 | 0.0588235 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | -9.69531e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7167 | -1.379e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | -8.31094e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.954464 | 0.95446 | 4.54721e-06 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562048 | 0.562107 | -5.91824e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00014 | 1 | 0.00014222 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00012 | 1 | 0.000120244 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00012 | 1 | 0.000119249 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.00013 | 1 | 0.000126296 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2855 | 33.2855 | 6.1852e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cb | 36.1625 | 36.1625 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cr | 35.8225 | 35.8225 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.890972 | 0.890966 | 5.68629e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cb | 0.893296 | 0.893296 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cr | 0.889736 | 0.889736 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.805899 | 0.805723 | 0.00017605 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.00894 | 1.00892 | 1.90202e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.00414469 | 0.0041549 | -1.02008e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.681551 | 0.681551 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.01602 | 1.01602 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.00275653 | -0.00275653 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00022 | 1 | 0.000218499 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -1.02008e-05 | 0 | -1.02008e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.013041 | -0.0130409 | -1.09346e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.037526 | 0.0375779 | -5.18387e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.73508 | 0.734601 | 0.00047914 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000586693 | 0.000585954 | 7.38828e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.823576 | 0.823343 | 0.000233355 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.869768 | 0.869555 | 0.000213483 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.910413 | 0.910241 | 0.00017193 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_Y | 0.0693878 | 0.0693878 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0092 | 34.0091 | 3.52876e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.991 | 32.9909 | 7.8306e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887704 | 0.887697 | 6.50712e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20404 | 1.20411 | -7.49062e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414576 | 0.414546 | 2.99402e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000200889 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00022 | 1 | 0.00022036 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.0002 | 1 | 0.000197719 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00026 | 1 | 0.00026365 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1654 | 33.1646 | 0.000787642 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cb | 38.3677 | 38.3677 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cr | 41.416 | 41.416 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.918385 | 0.918374 | 1.05625e-05 | same | PASS |  |
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
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00780528 | -0.00781908 | 1.38037e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0653644 | 0.0654834 | -0.000118974 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.80659 | 0.806187 | 0.000403367 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332499 | 0.000332055 | 4.4404e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s1_Y | 0.876414 | 0.876241 | 0.000172732 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s2_Y | 0.915369 | 0.91521 | 0.000159635 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s3_Y | 0.945612 | 0.945487 | 0.000125406 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_Y | 0.361008 | 0.358479 | 0.00252816 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9523 | 33.9524 | -3.83346e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.8921 | 32.8911 | 0.000995274 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.91602 | 0.916007 | 1.33059e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.2149 | 1.21593 | -0.00103169 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0614185 | 0.0618573 | -0.000438771 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8173 | 28.8149 | 0.00243524 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cb | 37.1876 | 37.1876 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cr | 38.8413 | 38.8413 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851572 | 0.851496 | 7.58316e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cb | 0.896615 | 0.896615 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cr | 0.941159 | 0.941159 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.772735 | 0.772567 | 0.000167466 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.995483 | 0.995531 | -4.81683e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.0149602 | 0.0149646 | -4.39972e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.20934 | 0.20934 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.518317 | 0.518317 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.40181 | 0.40181 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00022 | 1 | 0.000216765 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -4.39972e-06 | 0 | -4.39972e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.046193 | -0.0462079 | 1.48946e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.119651 | 0.120286 | -0.000635081 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.718298 | 0.716427 | 0.00187116 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000399307 | 0.000396541 | 2.76633e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.82825 | 0.827485 | 0.000764853 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.878564 | 0.877865 | 0.000699022 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.919297 | 0.918739 | 0.000558376 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_Y | 0.220634 | 0.218723 | 0.00191104 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.6576 | 29.658 | -0.000370236 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5268 | 28.5237 | 0.00312377 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.845549 | 0.84545 | 9.96146e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30104 | 1.30451 | -0.00347671 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.112583 | 0.114455 | -0.0018714 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000198376 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00027 | 1 | 0.000272564 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00025 | 1 | 0.000246544 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00012 | 1 | 0.000122485 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1923 | 28.1923 | 4.04009e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cb | 34.7596 | 34.7596 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cr | 35.3812 | 35.3812 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.74753 | 0.747524 | 6.70289e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cb | 0.850081 | 0.850081 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cr | 0.872604 | 0.872604 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Y | 0.321504 | 0.321503 | 1.62511e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Y | 0.218106 | 0.218103 | 3.0041e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Y | 0.457718 | 0.45772 | -1.76657e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.0833721 | 0.0833721 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.163468 | 0.163468 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.596199 | 0.596199 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00001 | 1 | 5.05474e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | -1.76657e-06 | 0 | -1.76657e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101974 | -0.101988 | 1.43627e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.25476 | 0.254786 | -2.55132e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.509363 | 0.509077 | 0.00028536 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837513 | 0.000837101 | 4.11901e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s1_Y | 0.64993 | 0.649785 | 0.000144481 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s2_Y | 0.736144 | 0.736 | 0.000143878 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s3_Y | 0.810605 | 0.810482 | 0.000123148 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9644 | 28.9645 | -0.000102965 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9203 | 27.9203 | 7.53352e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.741989 | 0.74198 | 8.78939e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20812 | 1.20829 | -0.000173353 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.29638 | 0.296664 | -0.000284513 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 3.01972e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 9.40041e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00003 | 1 | 2.56184e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 0.999989 | 1 | -1.10851e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6644 | 25.6645 | -7.40358e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cb | 33.1651 | 33.1651 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cr | 33.6384 | 33.6384 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.684966 | 0.684926 | 3.96658e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cb | 0.819616 | 0.819616 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cr | 0.839597 | 0.839597 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.346629 | 0.346409 | 0.000219525 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.268904 | 0.269071 | -0.00016644 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.266924 | 0.266864 | 6.09601e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.104566 | 0.104566 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.142889 | 0.142889 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.465832 | 0.465832 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00063 | 1 | 0.000633715 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 6.09601e-05 | 0 | 6.09601e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128234 | -0.128237 | 3.42767e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318185 | 0.318236 | -5.10653e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.47683 | 0.47579 | 0.00103983 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000852372 | 0.000850393 | 1.97933e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.660758 | 0.660218 | 0.000540412 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.740273 | 0.739764 | 0.000508698 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.80773 | 0.807301 | 0.000428592 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1313 | 26.1316 | -0.000351907 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.488 | 25.488 | -2.06329e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678261 | 0.678209 | 5.18549e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753376 | 0.753705 | -0.0003288 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.227301 | 0.228373 | -0.00107205 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00065 | 1 | 0.000645333 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00053 | 1 | 0.00052765 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00039 | 1 | 0.000393255 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00016 | 1 | 0.000162038 | worse | PASS |  |
