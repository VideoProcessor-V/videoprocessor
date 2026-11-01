<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.15_b1_u0.5 vs td_off (default)

Suite: `td_real_vlow_sweep` (8 cases). Candidate `td_s0.15_b1_u0.5` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

## Per-content-type summary

### film24  (INFO 16, LIMIT 11, NA 45, PASS 136)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | 0.611111 | 0.611111 | 0 | 4 | 2 | LIMIT:1 NA:2 PASS:1 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 4 | 1 | NA:1 PASS:3 |
| clip_detect_C | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| detail_pulse_Y | 0.341066 | 0.341107 | -4.07247e-05 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00961458 | -0.00961523 | 6.51433e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0251619 | 0.0251748 | -1.29259e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.02043 | 1.02043 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.02097 | 1.02096 | 2.17821e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00009 | 1 | 8.59951e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00009 | 1 | 9.13379e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00008 | 1 | 8.19011e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00009 | 1 | 8.75846e-05 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00009 | 1 | 8.98554e-05 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.0172803 | -0.0172803 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00918462 | -0.00918324 | -1.38008e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -1.38008e-06 | 0 | -1.38008e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.565777 | 0.565777 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.80342 | 0.803353 | 6.74618e-05 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000505331 | 0.000505068 | 2.63038e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 1.19105 | 1.1911 | -4.32795e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.833735 | 0.833667 | 6.8724e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.88608 | 0.886017 | 6.31647e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.924478 | 0.924428 | 4.98288e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 37.4022 | 37.4022 | 1.06323e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 42.2254 | 42.2254 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 41.8148 | 41.8148 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 38.7797 | 38.7797 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 38.6534 | 38.6534 | -3.39382e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 37.6909 | 37.6909 | -2.47244e-07 | 4 | 0 | PASS:4 |
| smear_Y | 0.799812 | 0.799685 | 0.00012672 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.930763 | 0.930762 | 1.51626e-07 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.945793 | 0.945793 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.943527 | 0.943527 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.932695 | 0.932694 | 9.31669e-08 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0142857 | 0.0142857 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 8, NA 23, PASS 65)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.243707 | 0.24251 | 0.0011972 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.137139 | 0.138409 | -0.00126947 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0870356 | -0.087083 | 4.7391e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.148102 | 0.148496 | -0.000394696 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.575491 | 0.575491 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.969151 | 0.9692 | -4.83789e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00019 | 1 | 0.000187972 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000198122 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00023 | 1 | 0.000229677 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00018 | 1 | 0.00018412 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00014 | 1 | 0.000140597 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.305461 | 0.305461 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0354768 | 0.0354851 | -8.30224e-06 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -8.30224e-06 | 0 | -8.30224e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.338515 | 0.338515 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.784447 | 0.784308 | 0.000138168 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000522451 | 0.000521029 | 1.42211e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.49364 | 1.49578 | -0.00213974 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.786727 | 0.786252 | 0.000474468 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.845809 | 0.845369 | 0.000439942 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.89603 | 0.895681 | 0.000349342 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 28.4554 | 28.4534 | 0.00204234 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.03 | 37.03 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 39.0886 | 39.0886 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 31.7108 | 31.7108 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 29.8194 | 29.8195 | -0.00010318 | 2 | 0 | PASS:2 |
| psnr_Y | 28.7824 | 28.7808 | 0.00162194 | 2 | 0 | PASS:2 |
| smear_Y | 0.650512 | 0.649487 | 0.0010249 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.819274 | 0.819185 | 8.8535e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.895333 | 0.895333 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.945511 | 0.945511 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.825561 | 0.825494 | 6.74163e-05 | 2 | 0 | PASS:2 |
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
| detail_pulse_Y | 0.312335 | 0.312991 | -0.000656135 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.170399 | -0.17042 | 2.18847e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.355716 | 0.355733 | -1.7686e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.185477 | 0.185477 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.225346 | 0.225062 | 0.000283787 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00027 | 1 | 0.000266986 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00024 | 1 | 0.000235493 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00048 | 1 | 0.000483985 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00064 | 1 | 0.00063946 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00097 | 1 | 0.000968157 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.501815 | 0.501815 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.366858 | 0.366857 | 1.5417e-06 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 1.5417e-06 | 0 | 1.5417e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.0814355 | 0.0814355 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.244784 | 0.244716 | 6.79388e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.00152578 | 0.00152474 | 1.04461e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.938251 | 0.938206 | 4.47797e-05 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.58013 | 0.57992 | 0.000210563 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.668946 | 0.668742 | 0.000203814 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.749917 | 0.749744 | 0.000172784 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 25.2933 | 25.2934 | -0.00010524 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.0699 | 33.0699 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 33.5721 | 33.5721 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 27.4594 | 27.4594 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 26.1383 | 26.1383 | -6.48856e-05 | 2 | 0 | PASS:2 |
| psnr_Y | 25.507 | 25.5071 | -9.47062e-05 | 2 | 0 | PASS:2 |
| smear_Y | 0.386854 | 0.386504 | 0.000349888 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.65127 | 0.651259 | 1.08728e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.813987 | 0.813987 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.837917 | 0.837917 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.657536 | 0.657528 | 8.05791e-06 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_4k_vlow | psnr_Y | 43.7214 | 43.7214 | 2.71715e-06 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_Cb | 48.3104 | 48.3104 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_Cr | 47.7201 | 47.7201 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Y | 0.981886 | 0.981886 | -9.18198e-09 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Cb | 0.991835 | 0.991835 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Cr | 0.990422 | 0.990422 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.734477 | 0.734476 | 1.08894e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_4k_vlow | grain_hf_ratio_Y | 1.04899 | 1.04899 | -1.48917e-07 | better | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_delta_Y | -0.0295811 | -0.0295813 | 1.08266e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.482275 | 0.482275 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.989264 | 0.989264 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.0111776 | 0.0111776 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1 | 1.48261e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 1.08266e-07 | 0 | 1.08266e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | flicker_excess_Y | -0.00117297 | -0.00117308 | 1.1347e-07 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_4k_vlow | ghost_coeff_Y | 0.00572943 | 0.00572971 | -2.71528e-07 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | smear_Y | 0.872825 | 0.872822 | 2.74418e-06 | better | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | halo_overshoot_Y | 5.73256e-05 | 5.7325e-05 | 6.24102e-10 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.846679 | 0.846678 | 9.14819e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.91667 | 0.916669 | 9.20155e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.956752 | 0.956751 | 7.71153e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_4k_vlow | psnr_I_Y | 42.93 | 42.93 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_P_Y | 44.6031 | 44.6031 | -6.30458e-07 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_B_Y | 43.4795 | 43.4795 | 3.69136e-06 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_B_Y | 0.981368 | 0.981368 | -1.1827e-08 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_4k_vlow | ib_gap_db_Y | 0.969385 | 0.969389 | -4.25301e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | detail_pulse_Y | 0.176136 | 0.176139 | -3.20816e-06 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1 | 4.41799e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1 | 6.00869e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1 | 5.09929e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1 | 9.67576e-08 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Y | 41.526 | 41.5258 | 0.000164854 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Cb | 47.8395 | 47.8395 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Cr | 47.5037 | 47.5037 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Y | 0.975927 | 0.975926 | 5.32071e-07 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Cb | 0.990155 | 0.990155 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Cr | 0.989054 | 0.989054 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.902062 | 0.90205 | 1.16913e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 1.02248 | 1.02248 | -5.9566e-07 | better | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | -0.0137917 | -0.0137928 | 1.12415e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.464075 | 0.464075 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 1.06376 | 1.06376 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | -0.0561306 | -0.0561306 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00001 | 1 | 1.29608e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 1.12415e-06 | 0 | 1.12415e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.00330591 | -0.00330796 | 2.0487e-06 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0106989 | 0.0107045 | -5.66624e-06 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | smear_Y | 0.894271 | 0.894222 | 4.92152e-05 | better | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000114755 | 0.000114725 | 2.99422e-08 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.894548 | 0.894526 | 2.22955e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.931221 | 0.9312 | 2.06019e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.9583 | 0.958284 | 1.61874e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_I_Y | 38.7929 | 38.7929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_P_Y | 42.4363 | 42.4363 | 1.06458e-06 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_B_Y | 41.3565 | 41.3563 | 0.000217147 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_B_Y | 0.975688 | 0.975688 | 7.26646e-07 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.661652 | 0.661868 | -0.000216255 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.124488 | 0.124538 | -5.01619e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00002 | 1 | 1.61725e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 1.1076e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00001 | 1 | 1.26601e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00001 | 1 | 1.27751e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Y | 34.1472 | 34.1472 | -3.08443e-05 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Cb | 37.444 | 37.444 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Cr | 37.1149 | 37.1149 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Y | 0.91425 | 0.914251 | -1.35818e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Cb | 0.922476 | 0.922476 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Cr | 0.921187 | 0.921187 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.80738 | 0.807307 | 7.30925e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | grain_hf_ratio_Y | 1.0032 | 1.00321 | -1.40278e-05 | better | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_delta_Y | -7.45456e-05 | -7.62645e-05 | 1.71889e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.690017 | 0.690017 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 1.0049 | 1.0049 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_delta_Cb | -0.00507875 | -0.00507875 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00009 | 1 | 9.05387e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 1.71889e-06 | 0 | 1.71889e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | flicker_excess_Y | -0.010128 | -0.010129 | 9.75401e-07 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_4k_vlow | ghost_coeff_Y | 0.0295168 | 0.0295286 | -1.17952e-05 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | smear_Y | 0.760593 | 0.760447 | 0.000145924 | better | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000804323 | 0.000804027 | 2.95971e-07 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.808239 | 0.808158 | 8.05057e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.859654 | 0.859579 | 7.53272e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.89854 | 0.898481 | 5.917e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_4k_vlow | psnr_I_Y | 37.8151 | 37.8151 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_P_Y | 35.1853 | 35.1853 | -6.74286e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_B_Y | 33.7761 | 33.7762 | -3.71656e-05 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_B_Y | 0.912244 | 0.912246 | -1.7551e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_4k_vlow | ib_gap_db_Y | 1.56364 | 1.5636 | 3.07161e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | detail_pulse_Y | 0.582249 | 0.582332 | -8.35561e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.0001 | 1 | 9.97062e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1.0001 | 1 | 9.68048e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1.00008 | 1 | 8.17342e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1.00009 | 1 | 8.57421e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Y | 31.3691 | 31.3693 | -0.000137716 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Cb | 35.3078 | 35.3078 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Cr | 34.9204 | 34.9204 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Y | 0.858716 | 0.858715 | 1.20796e-06 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Cb | 0.878707 | 0.878707 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Cr | 0.873444 | 0.873444 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.769763 | 0.769578 | 0.000184955 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 1.0092 | 1.00917 | 2.34852e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.0067089 | 0.00671737 | -8.47162e-06 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.626741 | 0.626741 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 1.02379 | 1.02379 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | -0.0190894 | -0.0190894 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00024 | 1 | 0.000240333 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -8.47162e-06 | 0 | -8.47162e-06 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.0238514 | -0.0238509 | -5.3184e-07 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0547023 | 0.0547363 | -3.39707e-05 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | smear_Y | 0.671558 | 0.671249 | 0.000308998 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00104492 | 0.00104419 | 7.25616e-07 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.785475 | 0.785304 | 0.00017118 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.836775 | 0.836619 | 0.00015581 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.884319 | 0.884196 | 0.000123187 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_Y | 0.0285714 | 0.0285714 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_C | 0.222222 | 0.222222 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_I_Y | 35.581 | 35.581 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_P_Y | 32.389 | 32.3891 | -0.000129444 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_B_Y | 30.9969 | 30.997 | -0.000141144 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_B_Y | 0.85375 | 0.853748 | 1.64678e-06 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.56955 | 1.56953 | 1.66737e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.481392 | 0.481418 | -2.59727e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00025 | 1 | 0.000249031 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00022 | 1 | 0.000219664 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00026 | 1 | 0.000255893 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00026 | 1 | 0.000260807 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Y | 30.9204 | 30.9197 | 0.000679466 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Cb | 37.6715 | 37.6715 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Cr | 40.2841 | 40.2841 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Y | 0.87994 | 0.879927 | 1.29257e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Cb | 0.906004 | 0.906004 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Cr | 0.958124 | 0.958124 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.904753 | 0.904653 | 0.00010044 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_hf_ratio_Y | 0.991463 | 0.99156 | -9.66494e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_delta_Y | 0.0213809 | 0.0213762 | 4.68253e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.484433 | 0.484433 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.854387 | 0.854387 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.22829 | 0.22829 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00011 | 1 | 0.000111027 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 4.68253e-06 | 0 | 4.68253e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | flicker_excess_Y | -0.0582509 | -0.0582624 | 1.15301e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_4k_vlow | ghost_coeff_Y | 0.111887 | 0.112015 | -0.000128238 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | smear_Y | 0.716243 | 0.715935 | 0.00030784 | better | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000530764 | 0.000530408 | 3.56567e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.828044 | 0.827899 | 0.000145348 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.875738 | 0.875601 | 0.000137155 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.916803 | 0.916694 | 0.000108631 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_Y | 0.338886 | 0.337374 | 0.00151238 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_4k_vlow | psnr_I_Y | 34.1013 | 34.1013 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_P_Y | 32.1528 | 32.1529 | -2.64237e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_B_Y | 30.5465 | 30.5456 | 0.000836777 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_B_Y | 0.875387 | 0.87537 | 1.69557e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_4k_vlow | ib_gap_db_Y | 1.73921 | 1.74007 | -0.000861751 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | detail_pulse_Y | 0.108217 | 0.108617 | -0.000400083 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.00018 | 1 | 0.00018332 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1.00015 | 1 | 0.000153884 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1.00008 | 1 | 8.48464e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1.00004 | 1 | 4.06127e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Y | 26.6444 | 26.6418 | 0.00256441 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Cb | 36.3885 | 36.3885 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Cr | 37.8932 | 37.8932 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Y | 0.771182 | 0.77106 | 0.000121907 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Cb | 0.884661 | 0.884661 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Cr | 0.932897 | 0.932897 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.66414 | 0.663964 | 0.000175896 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 0.946839 | 0.94684 | -1.08451e-07 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.0495726 | 0.0495939 | -2.1287e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.192598 | 0.192598 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 0.296595 | 0.296595 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | 0.382633 | 0.382633 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00026 | 1 | 0.000264917 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -2.1287e-05 | 0 | -2.1287e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.11582 | -0.115904 | 8.32519e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.184316 | 0.184977 | -0.000661154 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | smear_Y | 0.584781 | 0.58304 | 0.00174197 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000514137 | 0.000511649 | 2.48765e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.74541 | 0.744606 | 0.000803589 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.815881 | 0.815138 | 0.000742729 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.875258 | 0.874668 | 0.000590053 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_Y | 0.148529 | 0.147647 | 0.000882021 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_I_Y | 29.3204 | 29.3204 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_P_Y | 27.4859 | 27.4861 | -0.000179936 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_B_Y | 26.3644 | 26.3611 | 0.00324791 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_B_Y | 0.763161 | 0.763001 | 0.000160114 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.24807 | 1.25149 | -0.00341772 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.166061 | 0.1682 | -0.00213887 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00021 | 1 | 0.000212924 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00031 | 1 | 0.000305471 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00028 | 1 | 0.000283393 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00024 | 1 | 0.000240582 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Y | 26.7843 | 26.7843 | -2.81304e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Cb | 34.0229 | 34.0229 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Cr | 34.5942 | 34.5942 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Y | 0.703484 | 0.703481 | 2.65181e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Cb | 0.8386 | 0.8386 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Cr | 0.862717 | 0.862717 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.23205 | 0.232035 | 1.43137e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_hf_ratio_Y | 0.15928 | 0.15929 | -1.00203e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_delta_Y | 0.486024 | 0.486024 | -3.45943e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.0734333 | 0.0734333 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.221772 | 0.221772 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.567679 | 0.567679 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00006 | 1 | 6.16875e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | -3.45943e-07 | 0 | -3.45943e-07 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | flicker_excess_Y | -0.159901 | -0.159927 | 2.53335e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_4k_vlow | ghost_coeff_Y | 0.321011 | 0.321029 | -1.79333e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | smear_Y | 0.410041 | 0.409866 | 0.000175073 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | halo_overshoot_Y | 0.00153545 | 0.00153505 | 4.02954e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.584043 | 0.583934 | 0.000109487 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.673696 | 0.673586 | 0.000110157 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.756216 | 0.756122 | 9.37701e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_I_Y | 29.6122 | 29.6122 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_P_Y | 27.6604 | 27.6604 | -2.84593e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_B_Y | 26.4941 | 26.4941 | 3.13964e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_B_Y | 0.696783 | 0.696779 | 3.72431e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_4k_vlow | ib_gap_db_Y | 1.29932 | 1.29935 | -3.00388e-05 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | detail_pulse_Y | 0.352128 | 0.352532 | -0.000403781 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.00005 | 1 | 5.04305e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1.00011 | 1 | 0.000106777 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1.00004 | 1 | 4.16923e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1 | -1.23351e-07 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Y | 24.2298 | 24.2299 | -0.000186599 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Cb | 32.1169 | 32.1169 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Cr | 32.55 | 32.55 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Y | 0.611587 | 0.611574 | 1.3464e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Cb | 0.789373 | 0.789373 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Cr | 0.813117 | 0.813117 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.257518 | 0.257396 | 0.000121564 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 0.291411 | 0.290834 | 0.000577594 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.247693 | 0.247689 | 3.42934e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.0894376 | 0.0894376 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 0.149181 | 0.149181 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | 0.435951 | 0.435951 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00047 | 1 | 0.000472284 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | 3.42934e-06 | 0 | 3.42934e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.180896 | -0.180914 | 1.84359e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.39042 | 0.390438 | -1.74386e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | smear_Y | 0.363666 | 0.363142 | 0.000524703 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00151612 | 0.00151443 | 1.68626e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.576217 | 0.575906 | 0.000311639 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.664195 | 0.663898 | 0.000297472 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.743617 | 0.743365 | 0.000251798 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_I_Y | 25.3065 | 25.3065 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_P_Y | 24.6162 | 24.6163 | -0.000101312 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_B_Y | 24.0925 | 24.0927 | -0.000213619 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_B_Y | 0.605757 | 0.605739 | 1.80214e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.577185 | 0.577065 | 0.000119598 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.272542 | 0.27345 | -0.000908489 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00042 | 1 | 0.000420556 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00086 | 1 | 0.000861192 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00124 | 1 | 0.00123723 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00194 | 1 | 0.00193644 | worse | PASS |  |
