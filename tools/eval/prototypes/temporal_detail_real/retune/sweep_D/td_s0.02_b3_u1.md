<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_s0.02_b3_u1 vs td_off (default)

Suite: `td_real_vlow_sweep` (8 cases). Candidate `td_s0.02_b3_u1` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: omitted.

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
| detail_pulse_Y | 0.34099 | 0.341107 | -0.000116627 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00961152 | -0.00961523 | 3.7166e-06 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0251147 | 0.0251748 | -6.00524e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.02043 | 1.02043 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.02097 | 1.02096 | 9.91353e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00036 | 1 | 0.000360912 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00035 | 1 | 0.000347302 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00036 | 1 | 0.00035974 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00037 | 1 | 0.000366683 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00036 | 1 | 0.00036275 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.0172803 | -0.0172803 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00920128 | -0.00918324 | -1.80427e-05 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -1.80427e-05 | 0 | -1.80427e-05 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.565777 | 0.565777 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.803639 | 0.803353 | 0.000286117 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000506231 | 0.000505068 | 1.16276e-06 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 1.19095 | 1.1911 | -0.000145817 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.833952 | 0.833667 | 0.000285181 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.886282 | 0.886017 | 0.000265051 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.924641 | 0.924428 | 0.00021327 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 37.4022 | 37.4022 | -2.80718e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 42.2254 | 42.2254 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 41.8148 | 41.8148 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 38.7797 | 38.7797 | 0 | 4 | 0 | PASS:4 |
| psnr_P_Y | 38.6533 | 38.6534 | -0.000183178 | 4 | 0 | PASS:4 |
| psnr_Y | 37.6908 | 37.6909 | -6.42506e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.800274 | 0.799685 | 0.000589311 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.930765 | 0.930762 | 2.75028e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.945793 | 0.945793 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.943527 | 0.943527 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.932696 | 0.932694 | 1.95888e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.0142857 | 0.0142857 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 8, LIMIT 8, NA 23, PASS 65)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.245337 | 0.24251 | 0.00282725 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 0.999997 | 1 | -3.47841e-06 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| detail_pulse_Y | 0.136204 | 0.138409 | -0.00220453 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0869146 | -0.087083 | 0.000168418 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.147527 | 0.148496 | -0.000969645 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.575491 | 0.575491 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.969282 | 0.9692 | 8.24508e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00054 | 1 | 0.000535793 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.0005 | 1 | 0.000501797 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.0005 | 1 | 0.000499323 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00047 | 1 | 0.000470001 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00074 | 1 | 0.000741323 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.305461 | 0.305461 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0354549 | 0.0354851 | -3.01527e-05 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -3.01527e-05 | 0 | -3.01527e-05 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.338515 | 0.338515 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.784666 | 0.784308 | 0.000357587 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000526185 | 0.000521029 | 5.15677e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.49284 | 1.49578 | -0.00294479 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.787543 | 0.786252 | 0.00129065 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.846579 | 0.845369 | 0.00120985 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.896653 | 0.895681 | 0.000972442 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 28.4548 | 28.4534 | 0.00139098 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.03 | 37.03 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 39.0886 | 39.0886 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 31.7108 | 31.7108 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 29.8178 | 29.8195 | -0.00164631 | 2 | 0 | PASS:2 |
| psnr_Y | 28.7816 | 28.7808 | 0.000816755 | 2 | 0 | PASS:2 |
| smear_Y | 0.652398 | 0.649487 | 0.00291081 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.819332 | 0.819185 | 0.000146724 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.895333 | 0.895333 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.945511 | 0.945511 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.825607 | 0.825494 | 0.000113582 | 2 | 0 | PASS:2 |
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
| detail_pulse_Y | 0.311031 | 0.312991 | -0.00196008 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.170301 | -0.17042 | 0.000119607 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.35567 | 0.355733 | -6.38002e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.185477 | 0.185477 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.224695 | 0.225062 | -0.000366891 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00093 | 1 | 0.000925191 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00105 | 1 | 0.00105229 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00054 | 1 | 0.000544406 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00049 | 1 | 0.00048972 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00013 | 1 | 0.000126722 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.501815 | 0.501815 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.366852 | 0.366857 | -5.18746e-06 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -5.18746e-06 | 0 | -5.18746e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.0814355 | 0.0814355 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.244951 | 0.244716 | 0.000235212 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.0015289 | 0.00152474 | 4.1654e-06 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.938001 | 0.938206 | -0.000205578 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.580772 | 0.57992 | 0.000852335 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.669582 | 0.668742 | 0.000839864 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.750469 | 0.749744 | 0.000725003 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 25.2928 | 25.2934 | -0.000587885 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.0699 | 33.0699 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 33.5721 | 33.5721 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 27.4594 | 27.4594 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 26.1375 | 26.1383 | -0.000852232 | 2 | 0 | PASS:2 |
| psnr_Y | 25.5065 | 25.5071 | -0.000627834 | 2 | 0 | PASS:2 |
| smear_Y | 0.387987 | 0.386504 | 0.00148338 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.651303 | 0.651259 | 4.33956e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.813987 | 0.813987 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.837917 | 0.837917 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.657561 | 0.657528 | 3.281e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_4k_vlow | psnr_Y | 43.7214 | 43.7214 | 9.93593e-06 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_Cb | 48.3104 | 48.3104 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_Cr | 47.7201 | 47.7201 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Y | 0.981886 | 0.981886 | -3.53045e-08 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Cb | 0.991835 | 0.991835 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_Cr | 0.990422 | 0.990422 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.734477 | 0.734476 | 5.03407e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_4k_vlow | grain_hf_ratio_Y | 1.04899 | 1.04899 | -3.25179e-07 | better | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_delta_Y | -0.0295808 | -0.0295813 | 4.99848e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.482275 | 0.482275 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.989264 | 0.989264 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.0111776 | 0.0111776 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1 | 1 | 6.85396e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 4.99848e-07 | 0 | 4.99848e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | flicker_excess_Y | -0.00117254 | -0.00117308 | 5.42385e-07 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_2160p_4k_vlow | ghost_coeff_Y | 0.00572834 | 0.00572971 | -1.36404e-06 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | smear_Y | 0.87284 | 0.872822 | 1.79034e-05 | better | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | halo_overshoot_Y | 5.73324e-05 | 5.7325e-05 | 7.47661e-09 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.846683 | 0.846678 | 5.55466e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.916675 | 0.916669 | 5.61105e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.956756 | 0.956751 | 4.6949e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_4k_vlow | psnr_I_Y | 42.93 | 42.93 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_P_Y | 44.6031 | 44.6031 | 1.18788e-05 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | psnr_B_Y | 43.4795 | 43.4795 | 9.74384e-06 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | ssim_B_Y | 0.981368 | 0.981368 | -4.36974e-08 | same | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_4k_vlow | ib_gap_db_Y | 0.96939 | 0.969389 | 8.38582e-07 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | detail_pulse_Y | 0.176126 | 0.176139 | -1.30289e-05 | NA | INFO |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1 | 1 | 2.16944e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1 | 1 | -2.21191e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1 | 6.04969e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1 | 1 | 4.18731e-07 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Y | 41.5263 | 41.5258 | 0.000460901 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Cb | 47.8395 | 47.8395 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_Cr | 47.5037 | 47.5037 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Y | 0.975928 | 0.975926 | 1.36443e-06 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Cb | 0.990155 | 0.990155 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_Cr | 0.989054 | 0.989054 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.902159 | 0.90205 | 0.000108845 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 1.0225 | 1.02248 | 2.37957e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | -0.0138205 | -0.0137928 | -2.77091e-05 | better | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.464075 | 0.464075 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 1.06376 | 1.06376 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | -0.0561306 | -0.0561306 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00012 | 1 | 0.000120664 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -2.77091e-05 | 0 | -2.77091e-05 | better | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.00329784 | -0.00330796 | 1.01187e-05 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | tos | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0106751 | 0.0107045 | -2.94734e-05 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | smear_Y | 0.894474 | 0.894222 | 0.000252729 | better | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000114881 | 0.000114725 | 1.55869e-07 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.894627 | 0.894526 | 0.000100446 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.931296 | 0.9312 | 9.51484e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.958361 | 0.958284 | 7.69633e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_I_Y | 38.7929 | 38.7929 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_P_Y | 42.4362 | 42.4363 | -6.93692e-05 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | psnr_B_Y | 41.3569 | 41.3563 | 0.000626455 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | ssim_B_Y | 0.97569 | 0.975688 | 1.8541e-06 | same | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.661184 | 0.661868 | -0.00068461 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.12447 | 0.124538 | -6.79897e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.0001 | 1 | 9.65844e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00011 | 1 | 0.000113101 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00012 | 1 | 0.000116311 | worse | PASS |  |
| film24 | tos | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00015 | 1 | 0.000146323 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Y | 34.147 | 34.1472 | -0.000191801 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Cb | 37.444 | 37.444 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_Cr | 37.1149 | 37.1149 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Y | 0.914246 | 0.914251 | -4.69167e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Cb | 0.922476 | 0.922476 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_Cr | 0.921187 | 0.921187 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.807682 | 0.807307 | 0.000374563 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | grain_hf_ratio_Y | 1.00316 | 1.00321 | -5.56723e-05 | better | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_delta_Y | -5.82631e-05 | -7.62645e-05 | 1.80013e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.690017 | 0.690017 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 1.0049 | 1.0049 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_delta_Cb | -0.00507875 | -0.00507875 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00046 | 1 | 0.000463966 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | 1.80013e-05 | 0 | 1.80013e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | flicker_excess_Y | -0.010126 | -0.010129 | 2.94545e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_2160p_4k_vlow | ghost_coeff_Y | 0.0294767 | 0.0295286 | -5.1973e-05 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | smear_Y | 0.761121 | 0.760447 | 0.000674007 | better | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000805327 | 0.000804027 | 1.29964e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.80848 | 0.808158 | 0.000321258 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.85988 | 0.859579 | 0.000300795 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.89872 | 0.898481 | 0.000239402 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_4k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | btb_kept_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_4k_vlow | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_4k_vlow | psnr_I_Y | 37.8151 | 37.8151 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_P_Y | 35.1851 | 35.1853 | -0.000255051 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | psnr_B_Y | 33.776 | 33.7762 | -0.000178286 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | ssim_B_Y | 0.912241 | 0.912246 | -4.7456e-06 | same | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_4k_vlow | ib_gap_db_Y | 1.56354 | 1.5636 | -6.56692e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | detail_pulse_Y | 0.582018 | 0.582332 | -0.000313914 | NA | INFO |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.00051 | 1 | 0.000507887 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1.00048 | 1 | 0.00047549 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1.00043 | 1 | 0.000431091 | worse | PASS |  |
| film24 | sol | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1.00044 | 1 | 0.000437091 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Y | 31.3687 | 31.3693 | -0.000536039 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Cb | 35.3078 | 35.3078 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_Cr | 34.9204 | 34.9204 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Y | 0.858726 | 0.858715 | 1.1198e-05 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Cb | 0.878707 | 0.878707 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_Cr | 0.873444 | 0.873444 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.770238 | 0.769578 | 0.000660555 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 1.00925 | 1.00917 | 7.18559e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.00665441 | 0.00671737 | -6.29627e-05 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.626741 | 0.626741 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 1.02379 | 1.02379 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | -0.0190894 | -0.0190894 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00086 | 1 | 0.000858334 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -6.29627e-05 | 0 | -6.29627e-05 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.0238497 | -0.0238509 | 1.25989e-06 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (48 frames) |
| film24 | sol | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.0545789 | 0.0547363 | -0.000157399 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | smear_Y | 0.672662 | 0.671249 | 0.00141261 | better | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00104738 | 0.00104419 | 3.18807e-06 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.786017 | 0.785304 | 0.000713467 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.837278 | 0.836619 | 0.000658649 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.884728 | 0.884196 | 0.000532021 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_Y | 0.0285714 | 0.0285714 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | btb_kept_C | 0.222222 | 0.222222 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_1.5k_vlow | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_I_Y | 35.581 | 35.581 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_P_Y | 32.3887 | 32.3891 | -0.000420171 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | psnr_B_Y | 30.9964 | 30.997 | -0.0005702 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | ssim_B_Y | 0.853762 | 0.853748 | 1.39363e-05 | same | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.5697 | 1.56953 | 0.000166173 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.481346 | 0.481418 | -7.15748e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00078 | 1 | 0.000782567 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00085 | 1 | 0.00085059 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00092 | 1 | 0.000918723 | worse | PASS |  |
| film24 | sol | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00087 | 1 | 0.000867167 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Y | 30.921 | 30.9197 | 0.00131007 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Cb | 37.6715 | 37.6715 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_Cr | 40.2841 | 40.2841 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Y | 0.879958 | 0.879927 | 3.10575e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Cb | 0.906004 | 0.906004 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_Cr | 0.958124 | 0.958124 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.904667 | 0.904653 | 1.38279e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_hf_ratio_Y | 0.991561 | 0.99156 | 7.0932e-07 | better | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_delta_Y | 0.0213751 | 0.0213762 | -1.10815e-06 | better | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.484433 | 0.484433 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.854387 | 0.854387 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.22829 | 0.22829 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00002 | 1 | 1.52853e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | -1.10815e-06 | 0 | -1.10815e-06 | better | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | flicker_excess_Y | -0.0581795 | -0.0582624 | 8.29046e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_2160p_4k_vlow | ghost_coeff_Y | 0.111718 | 0.112015 | -0.000297937 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | smear_Y | 0.716786 | 0.715935 | 0.000851435 | better | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | halo_overshoot_Y | 0.000531774 | 0.000530408 | 1.36653e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.828309 | 0.827899 | 0.000410083 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.875989 | 0.875601 | 0.000387891 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.917006 | 0.916694 | 0.00031142 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_Y | 0.340805 | 0.337374 | 0.00343107 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_4k_vlow | psnr_I_Y | 34.1013 | 34.1013 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_P_Y | 32.1528 | 32.1529 | -7.49603e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | psnr_B_Y | 30.5473 | 30.5456 | 0.00161847 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | ssim_B_Y | 0.875409 | 0.87537 | 3.92442e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_4k_vlow | ib_gap_db_Y | 1.73838 | 1.74007 | -0.00168932 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | detail_pulse_Y | 0.107905 | 0.108617 | -0.000712462 | NA | INFO |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.00002 | 1 | 2.46145e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 0.999997 | 1 | -2.9358e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1 | 1 | 4.96567e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1.00002 | 1 | 1.87887e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Y | 26.6421 | 26.6418 | 0.000323443 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Cb | 36.3885 | 36.3885 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_Cr | 37.8932 | 37.8932 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Y | 0.771257 | 0.77106 | 0.000196107 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Cb | 0.884661 | 0.884661 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_Cr | 0.932897 | 0.932897 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.664666 | 0.663964 | 0.000701346 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 0.947004 | 0.94684 | 0.000164192 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.0495347 | 0.0495939 | -5.91972e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.192598 | 0.192598 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 0.296595 | 0.296595 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | 0.382633 | 0.382633 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00106 | 1 | 0.0010563 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -5.91972e-05 | 0 | -5.91972e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 0.999993 | 1 | -6.95681e-06 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.11565 | -0.115904 | 0.000253932 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (48 frames) |
| nature30 | boat | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.183336 | 0.184977 | -0.00164135 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | smear_Y | 0.58801 | 0.58304 | 0.00497018 | better | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.000520596 | 0.000511649 | 8.94701e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.746777 | 0.744606 | 0.00217122 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.81717 | 0.815138 | 0.0020318 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.876301 | 0.874668 | 0.00163346 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_Y | 0.14987 | 0.147647 | 0.00222343 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_I_Y | 29.3204 | 29.3204 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_P_Y | 27.4829 | 27.4861 | -0.00321765 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | psnr_B_Y | 26.3623 | 26.3611 | 0.00116349 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | ssim_B_Y | 0.763255 | 0.763001 | 0.000254205 | same | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_1.5k_vlow | ib_gap_db_Y | 1.24729 | 1.25149 | -0.00420025 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.164503 | 0.1682 | -0.0036966 | NA | INFO |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00098 | 1 | 0.000978979 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.001 | 1 | 0.00100158 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00094 | 1 | 0.000935036 | worse | PASS |  |
| nature30 | boat | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00146 | 1 | 0.00146386 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Y | 26.7844 | 26.7843 | 9.3569e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Cb | 34.0229 | 34.0229 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_Cr | 34.5942 | 34.5942 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Y | 0.703497 | 0.703481 | 1.61032e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Cb | 0.8386 | 0.8386 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_Cr | 0.862717 | 0.862717 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_rms_ratio_Y | 0.232089 | 0.232035 | 5.35985e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_hf_ratio_Y | 0.159297 | 0.15929 | 6.53421e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_delta_Y | 0.486018 | 0.486024 | -6.23284e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_rms_ratio_Cb | 0.0734333 | 0.0734333 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_hf_ratio_Cb | 0.221772 | 0.221772 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_delta_Cb | 0.567679 | 0.567679 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_Y | 1.00023 | 1 | 0.000230993 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_vs_input_Y | -6.23284e-06 | 0 | -6.23284e-06 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | flicker_excess_Y | -0.159823 | -0.159927 | 0.000103931 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_4k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_2160p_4k_vlow | ghost_coeff_Y | 0.320931 | 0.321029 | -9.76888e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | smear_Y | 0.410728 | 0.409866 | 0.000861824 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | halo_overshoot_Y | 0.00153676 | 0.00153505 | 1.71756e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s1_Y | 0.584424 | 0.583934 | 0.000490275 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s2_Y | 0.674083 | 0.673586 | 0.000497008 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_ratio_s3_Y | 0.756552 | 0.756122 | 0.000429778 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_I_Y | 29.6122 | 29.6122 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_P_Y | 27.6601 | 27.6604 | -0.000298168 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | psnr_B_Y | 26.4943 | 26.4941 | 0.000186349 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | ssim_B_Y | 0.696802 | 0.696779 | 2.262e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_4k_vlow | ib_gap_db_Y | 1.29888 | 1.29935 | -0.000468171 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | detail_pulse_Y | 0.351224 | 0.352532 | -0.00130805 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000196021 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band2_Y | 1.00041 | 1 | 0.000405214 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band3_Y | 1.00034 | 1 | 0.000335531 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_4k_vlow | grain_keep_vs_input_band4_Y | 1.00011 | 1 | 0.000110707 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Y | 24.2286 | 24.2299 | -0.00134924 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Cb | 32.1169 | 32.1169 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_Cr | 32.55 | 32.55 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Y | 0.611624 | 0.611574 | 4.95169e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Cb | 0.789373 | 0.789373 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_Cr | 0.813117 | 0.813117 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_rms_ratio_Y | 0.257813 | 0.257396 | 0.000416824 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_hf_ratio_Y | 0.290093 | 0.290834 | -0.000740316 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_delta_Y | 0.247685 | 0.247689 | -4.14208e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_rms_ratio_Cb | 0.0894376 | 0.0894376 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_hf_ratio_Cb | 0.149181 | 0.149181 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_delta_Cb | 0.435951 | 0.435951 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_Y | 1.00162 | 1 | 0.00161939 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Y | -4.14208e-06 | 0 | -4.14208e-06 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | flicker_excess_Y | -0.180779 | -0.180914 | 0.000135282 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (48 frames) |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ghost_coeff_Y | 0.390408 | 0.390438 | -2.99116e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | smear_Y | 0.365246 | 0.363142 | 0.00210494 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | halo_overshoot_Y | 0.00152104 | 0.00151443 | 6.61324e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s1_Y | 0.57712 | 0.575906 | 0.0012144 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s2_Y | 0.66508 | 0.663898 | 0.00118272 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_ratio_s3_Y | 0.744386 | 0.743365 | 0.00102023 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_I_Y | 25.3065 | 25.3065 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_P_Y | 24.6149 | 24.6163 | -0.0014063 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | psnr_B_Y | 24.0913 | 24.0927 | -0.00136212 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ssim_B_Y | 0.605803 | 0.605739 | 6.41712e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | ib_gap_db_Y | 0.577122 | 0.577065 | 5.70164e-05 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | detail_pulse_Y | 0.270838 | 0.27345 | -0.00261211 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band1_Y | 1.00191 | 1 | 0.00190857 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band2_Y | 1.00068 | 1 | 0.000683597 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band3_Y | 1.00064 | 1 | 0.000643909 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_1.5k_vlow | grain_keep_vs_input_band4_Y | 1.00014 | 1 | 0.000142736 | worse | PASS |  |
