<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.15_b1_u0.5 vs td_off (default)

Suite: `td_real_low_sweep` (8 cases). Candidate `td_s0.15_b1_u0.5` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

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
| detail_pulse_Y | 0.303702 | 0.303711 | -9.13343e-06 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00532817 | -0.00532854 | 3.66717e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0168585 | 0.0168709 | -1.23602e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.00376 | 1.00376 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.01469 | 1.0147 | -7.3279e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00014 | 1 | 0.000135549 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00014 | 1 | 0.000137861 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00014 | 1 | 0.000141227 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00013 | 1 | 0.000126745 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00014 | 1 | 0.000137207 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00332618 | -0.00332618 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00684027 | -0.00683871 | -1.55565e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -1.55565e-06 | 0 | -1.55565e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.598868 | 0.598868 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.829708 | 0.829599 | 0.000109784 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000275974 | 0.000275783 | 1.9091e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.807093 | 0.807136 | -4.36702e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.855845 | 0.855784 | 6.07353e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.906204 | 0.906148 | 5.56699e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.94084 | 0.940795 | 4.44766e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 39.5372 | 39.5372 | 2.91565e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 43.2064 | 43.2064 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.8076 | 42.8076 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 40.687 | 40.687 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 40.3821 | 40.3821 | -1.57377e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 39.7396 | 39.7395 | 1.85059e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.837474 | 0.83735 | 0.000123374 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.946556 | 0.946555 | 1.14218e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.951896 | 0.951896 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.950357 | 0.950357 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.947656 | 0.947655 | 9.59132e-07 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0641056 | 0.0641056 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 9, NA 23, PASS 64)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.290208 | 0.288601 | 0.00160708 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.0875684 | 0.088156 | -0.000587597 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.027008 | -0.0270135 | 5.49334e-06 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0927187 | 0.0928845 | -0.000165777 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.696661 | 0.696661 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.01113 | 1.01119 | -5.62805e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00014 | 1 | 0.00014052 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00015 | 1 | 0.000148317 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00018 | 1 | 0.000183318 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00015 | 1 | 0.000153374 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00006 | 1 | 5.82591e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.278869 | 0.278869 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0365975 | 0.036602 | -4.51016e-06 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -4.51016e-06 | 0 | -4.51016e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.349037 | 0.349037 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.838502 | 0.838393 | 0.0001086 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000364979 | 0.000364298 | 6.81414e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.25869 | 1.26022 | -0.00153279 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.852094 | 0.851863 | 0.000230918 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.896749 | 0.896538 | 0.000211816 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.932282 | 0.932113 | 0.000168717 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 30.7089 | 30.7074 | 0.00148844 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.7776 | 37.7776 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 40.1287 | 40.1287 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 34.3295 | 34.3295 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 31.8051 | 31.8052 | -4.65877e-05 | 2 | 0 | PASS:2 |
| psnr_Y | 30.991 | 30.9898 | 0.00118372 | 2 | 0 | PASS:2 |
| smear_Y | 0.761836 | 0.761307 | 0.000528785 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.880763 | 0.880728 | 3.48716e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.9067 | 0.9067 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.952778 | 0.952778 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.884962 | 0.884935 | 2.67027e-05 | 2 | 0 | PASS:2 |
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
| detail_pulse_Y | 0.261916 | 0.262519 | -0.000602442 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.115077 | -0.115113 | 3.53516e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.286476 | 0.286511 | -3.5299e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.153179 | 0.153179 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.243501 | 0.243587 | -8.55541e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00014 | 1 | 0.000142656 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00014 | 1 | 0.000140961 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00024 | 1 | 0.000237135 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00001 | 1 | 7.42224e-06 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 0.999945 | 1 | -5.53559e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.531016 | 0.531016 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.362293 | 0.362292 | 1.82802e-06 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 1.82802e-06 | 0 | 1.82802e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.093969 | 0.093969 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.334005 | 0.333956 | 4.93949e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000844566 | 0.000843747 | 8.19473e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.980738 | 0.980999 | -0.000261329 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.655242 | 0.655002 | 0.000240323 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.738114 | 0.737882 | 0.000231869 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.80909 | 0.808892 | 0.000197767 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 26.7043 | 26.7041 | 0.000148768 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.9624 | 33.9624 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 34.5098 | 34.5098 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 29.6011 | 29.6011 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 27.548 | 27.5481 | -0.000119276 | 2 | 0 | PASS:2 |
| psnr_Y | 26.9285 | 26.9284 | 9.31463e-05 | 2 | 0 | PASS:2 |
| smear_Y | 0.492882 | 0.492434 | 0.000448043 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.710118 | 0.710095 | 2.33714e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.834848 | 0.834848 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.8561 | 0.8561 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.716243 | 0.716225 | 1.79087e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 45.3258 | 45.3258 | -2.3851e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cb | 49.1802 | 49.1802 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cr | 48.5561 | 48.5561 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.984698 | 0.984698 | -6.41889e-09 | same | PASS |  |
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
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000269913 | -0.000269955 | 4.17798e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00355448 | 0.00355461 | -1.28336e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.886197 | 0.886196 | 1.16893e-06 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.76499e-05 | 2.76493e-05 | 6.36671e-10 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s1_Y | 0.855859 | 0.855858 | 4.78339e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s2_Y | 0.925526 | 0.925526 | 4.55137e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s3_Y | 0.96409 | 0.96409 | 3.83953e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 44.8223 | 44.8223 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 45.898 | 45.898 | 5.12798e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.1605 | 45.1605 | -3.29715e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.984316 | 0.984316 | -8.59746e-09 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.644641 | 0.644637 | 3.76049e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.164207 | 0.164206 | 7.2839e-07 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 44.3918 | 44.3917 | 8.41292e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cb | 49.3269 | 49.3269 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cr | 48.9454 | 48.9454 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.984669 | 0.984669 | 1.07861e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cb | 0.991923 | 0.991923 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cr | 0.990929 | 0.990929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.939247 | 0.939242 | 4.88845e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.0068 | 1.0068 | -5.76074e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.00570066 | -0.00570067 | 2.66471e-09 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.486201 | 0.486201 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.00659 | 1.00659 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.0124173 | -0.0124173 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00001 | 1 | 5.20467e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 2.66471e-09 | 0 | 2.66471e-09 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00200624 | -0.00200683 | 5.94711e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00576415 | 0.00576678 | -2.62584e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.931503 | 0.93148 | 2.35071e-05 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 5.15806e-05 | 5.15697e-05 | 1.09022e-08 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.916352 | 0.91634 | 1.18501e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.95063 | 0.950619 | 1.08558e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.97248 | 0.972471 | 8.48828e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 41.6599 | 41.6599 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.0902 | 45.0903 | -2.19017e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 44.2806 | 44.2805 | 0.000118694 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.984581 | 0.984581 | 1.54752e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.425199 | 0.425336 | -0.000137199 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.07396 | 0.073986 | -2.60325e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00001 | 1 | 5.33942e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 5.77898e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 4.71007e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00001 | 1 | 5.19723e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 35.9552 | 35.9552 | -1.11538e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cb | 38.1562 | 38.1562 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cr | 37.9064 | 37.9064 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.930287 | 0.930287 | -4.3552e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cb | 0.929937 | 0.929937 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cr | 0.92961 | 0.92961 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Y | 0.821716 | 0.821634 | 8.15905e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00628 | 1.00629 | -2.26329e-06 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Y | -0.00424613 | -0.00424658 | 4.45026e-07 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.7274 | 0.7274 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.01428 | 1.01428 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Cb | -0.00766725 | -0.00766725 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.0001 | 1 | 9.93028e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 4.45026e-07 | 0 | 4.45026e-07 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00599574 | -0.00599649 | 7.45933e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0205771 | 0.0205843 | -7.2562e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.797218 | 0.797125 | 9.32716e-05 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000438101 | 0.00043796 | 1.416e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s1_Y | 0.827638 | 0.827595 | 4.31606e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s2_Y | 0.878934 | 0.878893 | 4.05378e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s3_Y | 0.916412 | 0.916379 | 3.23283e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_Y | 0.0588235 | 0.0588235 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.8405 | 38.8405 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.5311 | 36.5311 | -2.79442e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 35.7167 | 35.7167 | -1.3685e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.929626 | 0.929626 | -5.68588e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.954471 | 0.95446 | 1.1021e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.562066 | 0.562107 | -4.14322e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00011 | 1 | 0.000108662 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00009 | 1 | 9.0897e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.0001 | 1 | 9.56846e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.0001 | 1 | 9.88673e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.2855 | 33.2855 | 3.43339e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cb | 36.1625 | 36.1625 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cr | 35.8225 | 35.8225 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.89097 | 0.890966 | 4.17061e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cb | 0.893296 | 0.893296 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cr | 0.889736 | 0.889736 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.806076 | 0.805723 | 0.000352657 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.00889 | 1.00892 | -2.64722e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.00414822 | 0.0041549 | -6.67027e-06 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.681551 | 0.681551 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.01602 | 1.01602 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.00275653 | -0.00275653 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00044 | 1 | 0.00043769 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -6.67027e-06 | 0 | -6.67027e-06 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0130408 | -0.0130409 | 8.44446e-08 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.0375384 | 0.0375779 | -3.94303e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.734976 | 0.734601 | 0.000375549 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000586564 | 0.000585954 | 6.10501e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.82353 | 0.823343 | 0.000187452 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.869726 | 0.869555 | 0.000170831 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.910377 | 0.910241 | 0.000136706 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_Y | 0.0693878 | 0.0693878 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_C | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 37.4255 | 37.4255 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.0091 | 34.0091 | -3.87674e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 32.9909 | 32.9909 | 1.49136e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.887702 | 0.887697 | 4.99115e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20406 | 1.20411 | -5.22637e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.414576 | 0.414546 | 3.02026e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00044 | 1 | 0.000437444 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00047 | 1 | 0.000468234 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00041 | 1 | 0.000406586 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00044 | 1 | 0.000444762 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 33.1652 | 33.1646 | 0.000556245 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cb | 38.3677 | 38.3677 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cr | 41.416 | 41.416 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.91838 | 0.918374 | 6.46155e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cb | 0.916786 | 0.916786 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cr | 0.964397 | 0.964397 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Y | 0.904219 | 0.904219 | 5.31583e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Y | 1.02684 | 1.02684 | 1.2666e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Y | 0.058239 | 0.0582394 | -3.82499e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.488734 | 0.488734 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.875006 | 0.875006 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.155928 | 0.155928 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 5.87892e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | -3.82499e-07 | 0 | -3.82499e-07 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.00781416 | -0.00781908 | 4.92108e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0654231 | 0.0654834 | -6.02743e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.806378 | 0.806187 | 0.000191092 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000332284 | 0.000332055 | 2.29781e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s1_Y | 0.876326 | 0.876241 | 8.50451e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s2_Y | 0.915289 | 0.91521 | 7.92456e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s3_Y | 0.945549 | 0.945487 | 6.24741e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_Y | 0.360398 | 0.358479 | 0.00191869 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 36.3165 | 36.3165 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.9523 | 33.9524 | -3.51987e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.8918 | 32.8911 | 0.000704817 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.916015 | 0.916007 | 8.38799e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.21519 | 1.21593 | -0.000738252 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0616137 | 0.0618573 | -0.000243533 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | -8.84823e-08 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | -3.22871e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 4.79639e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 1.87732e-06 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.8167 | 28.8149 | 0.0018112 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cb | 37.1876 | 37.1876 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cr | 38.8413 | 38.8413 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.851543 | 0.851496 | 4.69438e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cb | 0.896615 | 0.896615 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cr | 0.941159 | 0.941159 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.772784 | 0.772567 | 0.000216669 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.995418 | 0.995531 | -0.000113828 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.0149559 | 0.0149646 | -8.63783e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.20934 | 0.20934 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.518317 | 0.518317 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.40181 | 0.40181 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00028 | 1 | 0.000280453 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -8.63783e-06 | 0 | -8.63783e-06 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0462019 | -0.0462079 | 6.06559e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.120014 | 0.120286 | -0.00027128 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.717294 | 0.716427 | 0.000866477 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000397674 | 0.000396541 | 1.13305e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.827862 | 0.827485 | 0.00037679 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.87821 | 0.877865 | 0.000344386 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.919014 | 0.918739 | 0.00027496 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_Y | 0.220018 | 0.218723 | 0.00129547 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 32.3426 | 32.3426 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.658 | 29.658 | -5.79767e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.5259 | 28.5237 | 0.00227206 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.845511 | 0.84545 | 6.13553e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.30219 | 1.30451 | -0.00232733 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.113523 | 0.114455 | -0.000931661 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.0003 | 1 | 0.000296723 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00037 | 1 | 0.000366958 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00031 | 1 | 0.000306268 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00011 | 1 | 0.000114641 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1924 | 28.1923 | 7.77604e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cb | 34.7596 | 34.7596 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cr | 35.3812 | 35.3812 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.74753 | 0.747524 | 6.71151e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cb | 0.850081 | 0.850081 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cr | 0.872604 | 0.872604 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Y | 0.321503 | 0.321503 | 5.81057e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Y | 0.218102 | 0.218103 | -1.19638e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Y | 0.45772 | 0.45772 | 1.78608e-07 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.0833721 | 0.0833721 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.163468 | 0.163468 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.596199 | 0.596199 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 1.80731e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 1.78608e-07 | 0 | 1.78608e-07 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.101973 | -0.101988 | 1.48346e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.254754 | 0.254786 | -3.15598e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.50934 | 0.509077 | 0.000262693 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000837474 | 0.000837101 | 3.73182e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s1_Y | 0.64992 | 0.649785 | 0.000134462 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s2_Y | 0.736134 | 0.736 | 0.000134249 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s3_Y | 0.810597 | 0.810482 | 0.000115162 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.9645 | 28.9645 | -7.10342e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.9204 | 27.9203 | 0.000114452 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.741989 | 0.74198 | 8.85533e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.20811 | 1.20829 | -0.000182073 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.296272 | 0.296664 | -0.000391964 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 1.36021e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 5.42261e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 4.85723e-07 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 0.999997 | 1 | -3.08627e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.6646 | 25.6645 | 0.000108532 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cb | 33.1651 | 33.1651 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cr | 33.6384 | 33.6384 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.684955 | 0.684926 | 2.91059e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cb | 0.819616 | 0.819616 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cr | 0.839597 | 0.839597 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.346508 | 0.346409 | 9.82088e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.268901 | 0.269071 | -0.000169912 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.266867 | 0.266864 | 3.47742e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.104566 | 0.104566 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.142889 | 0.142889 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.465832 | 0.465832 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00028 | 1 | 0.000283505 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 3.47742e-06 | 0 | 3.47742e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.128181 | -0.128237 | 5.58687e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.318197 | 0.318236 | -3.90383e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.476424 | 0.47579 | 0.000633392 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000851659 | 0.000850393 | 1.26576e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.660564 | 0.660218 | 0.000346184 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.740094 | 0.739764 | 0.000329488 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.807582 | 0.807301 | 0.000280371 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.1314 | 26.1316 | -0.000167518 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.4882 | 25.488 | 0.000183084 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.678247 | 0.678209 | 3.78875e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.753365 | 0.753705 | -0.000340585 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.22756 | 0.228373 | -0.000812921 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00028 | 1 | 0.000280562 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00047 | 1 | 0.000468847 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00001 | 1 | 1.43588e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 0.999892 | 1 | -0.000107626 | worse | PASS |  |
