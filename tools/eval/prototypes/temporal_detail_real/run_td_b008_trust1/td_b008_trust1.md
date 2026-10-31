<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_b008_trust1 vs td_off (default)

Suite: `td_real` (8 cases). Candidate `td_b008_trust1` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors, trust forced to 1 (vector-confidence sensitivity)), default `td_off`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (INFO 20, LIMIT 12, NA 49, PASS 155)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | 0.581448 | 0.581448 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 4 | 3 | NA:3 PASS:1 |
| clip_detect_C | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 868.718 | 0.000412499 | 868.718 | 4 | 0 | INFO:4 |
| detail_pulse_Y | 0.238322 | 0.234484 | 0.00383721 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00295383 | -0.00303857 | 8.47353e-05 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.00877419 | 0.00982271 | -0.00104852 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.0172 | 1.0172 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.00816 | 1.008 | 0.000158132 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00481 | 1 | 0.0048076 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00474 | 1 | 0.00474239 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00469 | 1 | 0.00469399 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00482 | 1 | 0.00481927 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00495 | 1 | 0.0049492 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00100621 | -0.00100621 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00439101 | -0.0042998 | -9.12187e-05 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -9.12187e-05 | 0 | -9.12187e-05 | 4 | 0 | PASS:4 |
| grain_probe_keep_edge_vs_input_Y | 1.01407 | 1 | 0.0140665 | 4 | 0 | PASS:4 |
| grain_probe_keep_flat_vs_input_Y | 1.00186 | 1 | 0.00185991 | 4 | 0 | PASS:4 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 4 | 4 | NA:4 |
| grain_probe_lag1_flat_vs_input_Y | -0.000125522 | 0 | -0.000125522 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.692695 | 0.692695 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.879251 | 0.875037 | 0.00421348 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000124011 | 0.000112718 | 1.12934e-05 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.926361 | 0.928301 | -0.00193943 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.891065 | 0.88697 | 0.00409499 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.934586 | 0.930857 | 0.00372921 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.961955 | 0.958953 | 0.00300194 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | 0.0496155 | 0.0497176 | -0.000102086 | 4 | 0 | PASS:4 |
| lpips_all | 0.0480428 | 0.0481325 | -8.97105e-05 | 4 | 0 | PASS:4 |
| psnr_B_Y | 42.5349 | 42.5423 | -0.00734625 | 4 | 0 | PASS:4 |
| psnr_Cb | 45.155 | 45.155 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 44.7478 | 44.7478 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 43.957 | 43.9592 | -0.0022299 | 4 | 0 | PASS:4 |
| psnr_P_Y | 43.4285 | 43.4386 | -0.0101114 | 4 | 0 | PASS:4 |
| psnr_Y | 42.7721 | 42.7799 | -0.00780848 | 4 | 0 | PASS:4 |
| smear_Y | 0.893535 | 0.881473 | 0.012062 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.965911 | 0.965917 | -6.58696e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.965326 | 0.965326 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.963418 | 0.963418 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.966901 | 0.966911 | -1.03094e-05 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.142157 | 0.128268 | 0.0138889 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 10, LIMIT 8, NA 24, PASS 76)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.333415 | 0.294797 | 0.0386177 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 0.999998 | 1 | -1.87952e-06 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 586.044 | 0.000300002 | 586.044 | 2 | 0 | INFO:2 |
| detail_pulse_Y | 0.0443007 | 0.0472241 | -0.00292337 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.00263045 | -0.00330246 | 0.000672011 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0431084 | 0.0502248 | -0.00711648 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.782641 | 0.782641 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.987466 | 0.98702 | 0.000446022 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.01066 | 1 | 0.010663 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.01047 | 1 | 0.0104684 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.01039 | 1 | 0.0103927 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.01084 | 1 | 0.0108391 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.01095 | 1 | 0.0109533 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.136911 | 0.136911 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.00887653 | 0.00913087 | -0.000254339 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -0.000254339 | 0 | -0.000254339 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 1.0188 | 1 | 0.0188033 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 1.01032 | 1 | 0.0103234 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | 0.00103023 | 0 | 0.00103023 | 2 | 1 | NA:1 PASS:1 |
| grain_probe_lag1_flat_vs_input_Y | 0.000682737 | 0 | 0.000682737 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.550554 | 0.550554 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.919716 | 0.910134 | 0.00958282 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000262353 | 0.00021574 | 4.66133e-05 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.988055 | 0.999714 | -0.0116593 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.922947 | 0.911893 | 0.0110538 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.95099 | 0.941077 | 0.00991338 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.970295 | 0.962447 | 0.0078486 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | 0.0382244 | 0.0384375 | -0.000213086 | 2 | 0 | PASS:2 |
| lpips_all | 0.037248 | 0.037454 | -0.000206002 | 2 | 0 | PASS:2 |
| psnr_B_Y | 33.9726 | 33.9798 | -0.00717878 | 2 | 0 | PASS:2 |
| psnr_Cb | 39.0374 | 39.0374 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 42.0197 | 42.0197 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 37.6324 | 37.6263 | 0.00612216 | 2 | 0 | PASS:2 |
| psnr_P_Y | 34.7909 | 34.8108 | -0.0199754 | 2 | 0 | PASS:2 |
| psnr_Y | 34.2248 | 34.2347 | -0.00988506 | 2 | 0 | PASS:2 |
| smear_Y | 0.890361 | 0.863896 | 0.0264656 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.935188 | 0.934946 | 0.000242859 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.924396 | 0.924396 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.963658 | 0.963658 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.937563 | 0.937353 | 0.000210397 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |

### sports50_60  (INFO 5, LIMIT 4, NA 74, PASS 36)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | NA | 0 | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | NA | 0 | NA | 2 | 2 | NA:2 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 688.072 | 0.000300002 | 688.072 | 2 | 1 | INFO:1 NA:1 |
| detail_pulse_Y | 0.186619 | 0.179388 | 0.00723141 | 2 | 1 | INFO:1 NA:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| error_candidate | NA | NA | NA | 1 | 1 | NA:1 |
| flicker_excess_Y | -0.034269 | -0.0491109 | 0.0148418 | 2 | 1 | NA:1 PASS:1 |
| ghost_coeff_Y | 0.131892 | 0.161963 | -0.0300712 | 2 | 1 | NA:1 PASS:1 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.0578437 | 0.0738616 | -0.0160179 | 2 | 1 | NA:1 PASS:1 |
| grain_hf_ratio_Y | 0.33948 | 0.287294 | 0.0521857 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_Y | 1.01599 | 1 | 0.0159911 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band1_Y | 1.0148 | 1 | 0.0147956 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band2_Y | 1.01847 | 1 | 0.0184729 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band3_Y | 1.01936 | 1 | 0.019357 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band4_Y | 1.01729 | 1 | 0.0172896 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_delta_Cb | 0.68596 | 0.554961 | 0.130999 | 2 | 1 | LIMIT:1 NA:1 |
| grain_lag1_delta_Y | 0.335506 | 0.280274 | 0.0552327 | 2 | 1 | LIMIT:1 NA:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_vs_input_Y | -0.0010443 | 0 | -0.0010443 | 2 | 1 | NA:1 PASS:1 |
| grain_probe_keep_edge_vs_input_Y | 1.02591 | 1 | 0.025909 | 2 | 1 | NA:1 PASS:1 |
| grain_probe_keep_flat_vs_input_Y | 1.00905 | 1 | 0.00904703 | 2 | 1 | NA:1 PASS:1 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 2 | 2 | NA:2 |
| grain_probe_lag1_flat_vs_input_Y | 0.000104002 | 0 | 0.000104002 | 2 | 1 | NA:1 PASS:1 |
| grain_rms_ratio_Cb | 0.199264 | 0.180821 | 0.0184423 | 2 | 1 | LIMIT:1 NA:1 |
| grain_rms_ratio_Y | 0.53691 | 0.546363 | -0.00945283 | 2 | 1 | LIMIT:1 NA:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 1 | INFO:1 NA:1 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 1 | INFO:1 NA:1 |
| halo_overshoot_Y | 0.000267885 | 0.000302285 | -3.43999e-05 | 2 | 1 | NA:1 PASS:1 |
| ib_gap_db_Y | 1.10691 | 1.12495 | -0.0180393 | 2 | 1 | INFO:1 NA:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s1_Y | 0.764685 | 0.7653 | -0.000615435 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s2_Y | 0.845096 | 0.836068 | 0.00902755 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s3_Y | 0.901291 | 0.888981 | 0.0123093 | 2 | 1 | NA:1 PASS:1 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | 0.196298 | 0.172156 | 0.0241423 | 2 | 1 | NA:1 PASS:1 |
| lpips_all | 0.18967 | 0.164949 | 0.0247205 | 2 | 1 | NA:1 PASS:1 |
| psnr_B_Y | 30.8894 | 29.569 | 1.32034 | 2 | 1 | NA:1 PASS:1 |
| psnr_Cb | 35.6664 | 35.0964 | 0.570028 | 2 | 1 | NA:1 PASS:1 |
| psnr_Cr | 36.3895 | 35.7357 | 0.653811 | 2 | 1 | NA:1 PASS:1 |
| psnr_I_Y | 34.6355 | 33.3159 | 1.31968 | 2 | 1 | NA:1 PASS:1 |
| psnr_P_Y | 31.8934 | 30.5916 | 1.30181 | 2 | 1 | NA:1 PASS:1 |
| psnr_Y | 31.1581 | 29.8417 | 1.31638 | 2 | 1 | NA:1 PASS:1 |
| smear_Y | 0.711087 | 0.671 | 0.0400869 | 2 | 1 | NA:1 PASS:1 |
| ssim_B_Y | 0.825744 | 0.813024 | 0.0127198 | 2 | 1 | NA:1 PASS:1 |
| ssim_Cb | 0.862781 | 0.857532 | 0.00524927 | 2 | 1 | NA:1 PASS:1 |
| ssim_Cr | 0.882359 | 0.87589 | 0.00646922 | 2 | 1 | NA:1 PASS:1 |
| ssim_Y | 0.830076 | 0.81846 | 0.0116156 | 2 | 1 | NA:1 PASS:1 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | NA | 0.0983607 | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | psnr_Y | 48.0929 | 48.0976 | -0.0047322 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_Cb | 51.5196 | 51.5196 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_Cr | 51.1264 | 51.1264 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Y | 0.98969 | 0.989694 | -4.00406e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Cb | 0.994572 | 0.994572 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Cr | 0.994077 | 0.994077 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_12k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_rms_ratio_Y | 0.849264 | 0.849027 | 0.000237479 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_12k | grain_hf_ratio_Y | 1.00766 | 1.00763 | 2.91723e-05 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_delta_Y | -0.00508081 | -0.00507873 | -2.08413e-06 | better | PASS |  |
| film24 | tos | x265_2160p_12k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_rms_ratio_Cb | 0.59381 | 0.59381 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_12k | grain_hf_ratio_Cb | 1.01266 | 1.01266 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_delta_Cb | -0.00201065 | -0.00201065 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Y | 1.00028 | 1 | 0.000279708 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Y | -2.08413e-06 | 0 | -2.08413e-06 | better | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | flicker_excess_Y | -0.0001271 | -0.000143152 | 1.60524e-05 | same | PASS |  |
| film24 | tos | x265_2160p_12k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_12k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_12k | ghost_coeff_Y | 0.00204793 | 0.00218632 | -0.000138388 | same | PASS |  |
| film24 | tos | x265_2160p_12k | smear_Y | 0.906703 | 0.902687 | 0.00401546 | better | PASS |  |
| film24 | tos | x265_2160p_12k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | halo_overshoot_Y | 9.41841e-06 | 8.25973e-06 | 1.15867e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s1_Y | 0.873002 | 0.87239 | 0.000612123 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s2_Y | 0.938464 | 0.937876 | 0.000587991 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s3_Y | 0.972847 | 0.972362 | 0.000484654 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | lpips_all | 0.041457 | 0.0415157 | -5.86692e-05 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_I_Y | 48.7109 | 48.7149 | -0.00395944 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_P_Y | 48.768 | 48.7726 | -0.0045383 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_B_Y | 47.8606 | 47.8654 | -0.0048111 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_B_Y | 0.989252 | 0.989256 | -4.31863e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lpips_B | 0.0435994 | 0.0436596 | -6.0183e-05 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ib_gap_db_Y | 0.901641 | 0.90131 | 0.000331377 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | detail_pulse_Y | 0.174178 | 0.168458 | 0.0057205 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1.00027 | 1 | 0.000271689 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1.00026 | 1 | 0.000256168 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1.0003 | 1 | 0.000296637 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1.00029 | 1 | 0.000289595 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1.01587 | 1 | 0.015872 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | tos | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1.0003 | 1 | 0.000296118 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | -2.67976e-06 | 0 | -2.67976e-06 | better | PASS |  |
| film24 | tos | x265_2160p_12k | cost_ms_per_frame_pyref | 1459.9 | 0.000249998 | 1459.9 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_2160p_12k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_2160p_12k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos | x265_1080p_4k | psnr_Y | 47.5355 | 47.5554 | -0.0199563 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_Cb | 51.2254 | 51.2254 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_Cr | 50.9453 | 50.9453 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Y | 0.990644 | 0.990652 | -8.12123e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Cb | 0.993896 | 0.993896 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Cr | 0.993512 | 0.993512 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_4k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_rms_ratio_Y | 0.903201 | 0.901109 | 0.00209254 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_4k | grain_hf_ratio_Y | 1.01683 | 1.01674 | 8.95007e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_delta_Y | -0.0104301 | -0.010373 | -5.71513e-05 | better | PASS |  |
| film24 | tos | x265_1080p_4k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_rms_ratio_Cb | 0.679129 | 0.679129 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_4k | grain_hf_ratio_Cb | 1.05862 | 1.05862 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_delta_Cb | -0.0102854 | -0.0102854 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Y | 1.00232 | 1 | 0.00232218 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Y | -5.71513e-05 | 0 | -5.71513e-05 | better | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | flicker_excess_Y | -0.000269241 | -0.000480672 | 0.000211432 | same | PASS |  |
| film24 | tos | x265_1080p_4k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_4k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_4k | ghost_coeff_Y | 0.00173779 | 0.00289956 | -0.00116177 | same | PASS |  |
| film24 | tos | x265_1080p_4k | smear_Y | 0.969358 | 0.955935 | 0.0134232 | better | PASS |  |
| film24 | tos | x265_1080p_4k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | halo_overshoot_Y | 2.56124e-05 | 2.01293e-05 | 5.48306e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s1_Y | 0.941504 | 0.937169 | 0.00433439 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s2_Y | 0.97017 | 0.966221 | 0.00394965 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s3_Y | 0.985652 | 0.982552 | 0.00309987 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | lpips_all | 0.0286299 | 0.028675 | -4.51165e-05 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_I_Y | 46.8896 | 46.8961 | -0.00641911 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_P_Y | 48.1951 | 48.2199 | -0.0248167 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_B_Y | 47.3542 | 47.3734 | -0.0191511 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_B_Y | 0.990402 | 0.990408 | -6.89583e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lpips_B | 0.0300775 | 0.030132 | -5.44625e-05 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ib_gap_db_Y | 0.69119 | 0.694451 | -0.00326047 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | detail_pulse_Y | 0.0636011 | 0.0585832 | 0.00501796 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1.00228 | 1 | 0.00228107 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1.00226 | 1 | 0.00225907 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1.00237 | 1 | 0.0023744 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1.00235 | 1 | 0.00235471 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1.0136 | 1 | 0.0136045 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | tos | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1.00226 | 1 | 0.00225563 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 4.21748e-06 | 0 | 4.21748e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | cost_ms_per_frame_pyref | 654.912 | 0.000400003 | 654.912 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_1080p_4k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_1080p_4k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_2160p_12k | psnr_Y | 39.4177 | 39.4187 | -0.000987394 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_Cb | 40.2126 | 40.2126 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_Cr | 39.7403 | 39.7403 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Y | 0.956917 | 0.956944 | -2.73819e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Cb | 0.952627 | 0.952627 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Cr | 0.950594 | 0.950594 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_12k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_rms_ratio_Y | 0.884115 | 0.879414 | 0.00470108 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | grain_hf_ratio_Y | 1.00087 | 1.00099 | -0.000120434 | better | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_delta_Y | 0.000141297 | 0.000152981 | -1.16832e-05 | better | PASS |  |
| film24 | sol | x265_2160p_12k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_rms_ratio_Cb | 0.771714 | 0.771714 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | grain_hf_ratio_Cb | 1.01749 | 1.01749 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_delta_Cb | -0.00789775 | -0.00789775 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Y | 1.00535 | 1 | 0.0053457 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Y | -1.16832e-05 | 0 | -1.16832e-05 | better | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | flicker_excess_Y | -0.00365407 | -0.00370042 | 4.63551e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_12k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_12k | ghost_coeff_Y | 0.0110944 | 0.0116768 | -0.000582335 | same | PASS |  |
| film24 | sol | x265_2160p_12k | smear_Y | 0.861801 | 0.853971 | 0.00783038 | better | PASS |  |
| film24 | sol | x265_2160p_12k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | halo_overshoot_Y | 0.000152934 | 0.000144943 | 7.99125e-06 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s1_Y | 0.870762 | 0.868219 | 0.00254303 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s2_Y | 0.914438 | 0.912049 | 0.00238852 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s3_Y | 0.94455 | 0.942569 | 0.00198047 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | wtw_kept_Y | 0.117647 | 0.117647 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_C | 0.470588 | 0.470588 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips_all | 0.0556796 | 0.05574 | -6.04882e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_I_Y | 41.1019 | 41.1011 | 0.00078028 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_P_Y | 39.9297 | 39.933 | -0.00328364 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_B_Y | 39.2023 | 39.2027 | -0.000359249 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_B_Y | 0.956277 | 0.956301 | -2.4088e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips_B | 0.056884 | 0.0569565 | -7.24441e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ib_gap_db_Y | 0.831355 | 0.833961 | -0.00260648 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | detail_pulse_Y | 0.410154 | 0.4089 | 0.00125387 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1.00549 | 1 | 0.00549025 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1.00532 | 1 | 0.00531795 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1.0053 | 1 | 0.0052971 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1.00526 | 1 | 0.00526184 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1.009 | 1 | 0.00899707 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | sol | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1.00127 | 1 | 0.00127301 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 4.20779e-05 | 0 | 4.20779e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | cost_ms_per_frame_pyref | 756.397 | 0.000399996 | 756.397 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_2160p_12k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_2160p_12k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_1080p_4k | psnr_Y | 36.0423 | 36.0479 | -0.00555807 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_Cb | 37.6623 | 37.6623 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_Cr | 37.1791 | 37.1791 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Y | 0.930353 | 0.930355 | -1.73039e-06 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Cb | 0.920207 | 0.920207 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Cr | 0.91549 | 0.91549 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_4k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_rms_ratio_Y | 0.880423 | 0.8706 | 0.00982282 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | grain_hf_ratio_Y | 1.00729 | 1.00666 | 0.00063429 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_delta_Y | -0.00219443 | -0.00190048 | -0.000293956 | better | PASS |  |
| film24 | sol | x265_1080p_4k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_rms_ratio_Cb | 0.726128 | 0.726128 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | grain_hf_ratio_Cb | 0.980032 | 0.980032 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_delta_Cb | 0.0161689 | 0.0161689 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Y | 1.01128 | 1 | 0.0112828 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Y | -0.000293956 | 0 | -0.000293956 | better | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | flicker_excess_Y | -0.00776493 | -0.00783003 | 6.5102e-05 | same | PASS |  |
| film24 | sol | x265_1080p_4k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_4k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_4k | ghost_coeff_Y | 0.0202166 | 0.0225282 | -0.00231159 | same | PASS |  |
| film24 | sol | x265_1080p_4k | smear_Y | 0.83628 | 0.813301 | 0.0229791 | better | PASS |  |
| film24 | sol | x265_1080p_4k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | halo_overshoot_Y | 0.00030808 | 0.000277539 | 3.05407e-05 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s1_Y | 0.878992 | 0.870102 | 0.00889043 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s2_Y | 0.915273 | 0.907282 | 0.00799067 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s3_Y | 0.944772 | 0.93833 | 0.00644276 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_4k | wtw_kept_Y | 0.166667 | 0.138889 | 0.0277778 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_C | 0.692308 | 0.692308 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_4k | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips_all | 0.0664046 | 0.0665992 | -0.000194568 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_I_Y | 39.1254 | 39.1247 | 0.000678643 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_P_Y | 36.8213 | 36.8291 | -0.00780676 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_B_Y | 35.7226 | 35.7277 | -0.0050635 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_B_Y | 0.927712 | 0.927703 | 8.95462e-06 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips_B | 0.067901 | 0.0681223 | -0.000221255 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ib_gap_db_Y | 1.28126 | 1.28348 | -0.00222217 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | detail_pulse_Y | 0.305353 | 0.301997 | 0.00335654 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1.01093 | 1 | 0.0109265 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1.01094 | 1 | 0.0109428 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1.01131 | 1 | 0.0113089 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1.01189 | 1 | 0.0118907 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1.01779 | 1 | 0.0177926 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | sol | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1.00361 | 1 | 0.00361489 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | -0.000545705 | 0 | -0.000545705 | better | PASS |  |
| film24 | sol | x265_1080p_4k | cost_ms_per_frame_pyref | 603.661 | 0.000599997 | 603.66 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_1080p_4k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_1080p_4k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_2160p_14k | psnr_Y | 36.614 | 36.6265 | -0.0125576 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_Cb | 39.8267 | 39.8267 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_Cr | 43.5368 | 43.5368 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Y | 0.954729 | 0.954663 | 6.58736e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Cb | 0.936384 | 0.936384 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Cr | 0.974154 | 0.974154 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_14k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_rms_ratio_Y | 0.94595 | 0.939794 | 0.00615667 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_hf_ratio_Y | 0.993472 | 0.992658 | 0.000813991 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_delta_Y | 0.00364806 | 0.0041091 | -0.000461038 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_rms_ratio_Cb | 0.693444 | 0.693444 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_hf_ratio_Cb | 0.931765 | 0.931765 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_delta_Cb | 0.0675888 | 0.0675888 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Y | 1.00655 | 1 | 0.00655108 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Y | -0.000461038 | 0 | -0.000461038 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | flicker_excess_Y | 0.000885213 | 0.000279709 | 0.000605503 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_2160p_14k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_2160p_14k | ghost_coeff_Y | 0.0279552 | 0.0335583 | -0.0056031 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | smear_Y | 0.912548 | 0.891075 | 0.0214729 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | halo_overshoot_Y | 0.000157716 | 0.000132016 | 2.56991e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s1_Y | 0.931121 | 0.922051 | 0.00906974 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s2_Y | 0.958809 | 0.950688 | 0.00812124 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s3_Y | 0.976336 | 0.969995 | 0.00634077 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_Y | 0.329183 | 0.283406 | 0.0457774 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | lpips_all | 0.0307965 | 0.030902 | -0.000105435 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_I_Y | 39.6488 | 39.641 | 0.00783509 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_P_Y | 37.0691 | 37.0888 | -0.0197327 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_B_Y | 36.3993 | 36.41 | -0.0106813 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_B_Y | 0.95359 | 0.953512 | 7.75074e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lpips_B | 0.0312637 | 0.0313815 | -0.000117747 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ib_gap_db_Y | 0.828256 | 0.836041 | -0.0077847 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | detail_pulse_Y | 0.0313049 | 0.0354826 | -0.00417777 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band1_Y | 1.00599 | 1 | 0.00598937 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band2_Y | 1.00623 | 1 | 0.00622672 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band3_Y | 1.00697 | 1 | 0.00696815 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band4_Y | 1.00696 | 1 | 0.00696387 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_edge_vs_input_Y | 1.01936 | 1 | 0.0193561 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (90 available) |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_flat_vs_input_Y | 1.01065 | 1 | 0.0106497 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_flat_vs_input_Y | 0.000524634 | 0 | 0.000524634 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | cost_ms_per_frame_pyref | 590.386 | 0.000300002 | 590.385 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_2160p_14k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_2160p_14k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_1080p_5k | psnr_Y | 31.8356 | 31.8428 | -0.00721257 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | psnr_Cb | 38.2482 | 38.2482 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | psnr_Cr | 40.5027 | 40.5027 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | ssim_Y | 0.920397 | 0.920042 | 0.00035492 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | ssim_Cb | 0.912409 | 0.912409 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | ssim_Cr | 0.953162 | 0.953162 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_5k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_5k | grain_rms_ratio_Y | 0.893483 | 0.880474 | 0.013009 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_5k | grain_hf_ratio_Y | 0.98146 | 0.981382 | 7.80525e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_delta_Y | 0.014105 | 0.0141526 | -4.76404e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_5k | grain_rms_ratio_Cb | 0.407663 | 0.407663 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_5k | grain_hf_ratio_Cb | 0.633518 | 0.633518 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_delta_Cb | 0.206233 | 0.206233 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Y | 1.01477 | 1 | 0.014775 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Y | -4.76404e-05 | 0 | -4.76404e-05 | better | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_Y | 0.999996 | 1 | -3.75903e-06 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | flicker_excess_Y | -0.00614612 | -0.00688464 | 0.000738518 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_1080p_5k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_1080p_5k | ghost_coeff_Y | 0.0582615 | 0.0668913 | -0.00862985 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | smear_Y | 0.868174 | 0.836716 | 0.0314584 | better | PASS |  |
| nature30 | boat | x265_1080p_5k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | halo_overshoot_Y | 0.000366991 | 0.000299464 | 6.75275e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s1_Y | 0.914774 | 0.901736 | 0.0130379 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s2_Y | 0.94317 | 0.931465 | 0.0117055 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s3_Y | 0.964255 | 0.954899 | 0.00935643 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | btb_kept_Y | 0.337646 | 0.306188 | 0.0314579 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_5k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | lpips_all | 0.0436995 | 0.0440061 | -0.000306569 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | psnr_I_Y | 35.6161 | 35.6116 | 0.00440924 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | psnr_P_Y | 32.5127 | 32.5329 | -0.020218 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | psnr_B_Y | 31.5459 | 31.5496 | -0.00367623 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | ssim_B_Y | 0.916787 | 0.916379 | 0.00040821 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | lpips_B | 0.0451852 | 0.0454936 | -0.000308426 | same | PASS |  |
| nature30 | boat | x265_1080p_5k | ib_gap_db_Y | 1.14785 | 1.16339 | -0.0155338 | NA | INFO |  |
| nature30 | boat | x265_1080p_5k | detail_pulse_Y | 0.0572966 | 0.0589656 | -0.00166897 | NA | INFO |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band1_Y | 1.01495 | 1 | 0.0149475 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band2_Y | 1.01456 | 1 | 0.0145586 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band3_Y | 1.01471 | 1 | 0.01471 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band4_Y | 1.01494 | 1 | 0.0149427 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_edge_vs_input_Y | 1.01825 | 1 | 0.0182506 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_edge_vs_input_Y | 0.00103023 | 0 | 0.00103023 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_flat_vs_input_Y | 1.01 | 1 | 0.00999701 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_flat_vs_input_Y | 0.00084084 | 0 | 0.00084084 | worse | PASS |  |
| nature30 | boat | x265_1080p_5k | cost_ms_per_frame_pyref | 581.702 | 0.000300002 | 581.702 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_1080p_5k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_1080p_5k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_2160p_16k | psnr_Y | 31.1581 | 31.1665 | -0.00843638 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_Cb | 35.6664 | 35.6664 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_Cr | 36.3895 | 36.3895 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Y | 0.830076 | 0.83002 | 5.58633e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Cb | 0.862781 | 0.862781 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Cr | 0.882359 | 0.882359 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_16k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_rms_ratio_Y | 0.53691 | 0.52846 | 0.00845067 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_hf_ratio_Y | 0.33948 | 0.337682 | 0.00179799 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_delta_Y | 0.335506 | 0.336551 | -0.0010443 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_rms_ratio_Cb | 0.199264 | 0.199264 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_hf_ratio_Cb | 0.0578437 | 0.0578437 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_delta_Cb | 0.68596 | 0.68596 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Y | 1.01599 | 1 | 0.0159911 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Y | -0.0010443 | 0 | -0.0010443 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | flicker_excess_Y | -0.034269 | -0.0364489 | 0.00217989 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_2160p_16k | ghost_coeff_Y | 0.131892 | 0.137376 | -0.00548482 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | smear_Y | 0.711087 | 0.684746 | 0.0263411 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | halo_overshoot_Y | 0.000267885 | 0.000237639 | 3.02458e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s1_Y | 0.764685 | 0.752946 | 0.0117389 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s2_Y | 0.845096 | 0.834046 | 0.0110492 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s3_Y | 0.901291 | 0.892228 | 0.00906278 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | lpips_all | 0.18967 | 0.190543 | -0.000873735 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_I_Y | 34.6355 | 34.6409 | -0.00533404 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_P_Y | 31.8934 | 31.9202 | -0.0268277 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_B_Y | 30.8894 | 30.8928 | -0.00343177 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_B_Y | 0.825744 | 0.825603 | 0.000141008 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lpips_B | 0.196298 | 0.197307 | -0.00100904 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ib_gap_db_Y | 1.10691 | 1.12972 | -0.0228093 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | detail_pulse_Y | 0.186619 | 0.186542 | 7.69248e-05 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band1_Y | 1.0148 | 1 | 0.0147956 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band2_Y | 1.01847 | 1 | 0.0184729 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band3_Y | 1.01936 | 1 | 0.019357 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band4_Y | 1.01729 | 1 | 0.0172896 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_edge_vs_input_Y | 1.02591 | 1 | 0.025909 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (150 available) |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_flat_vs_input_Y | 1.00905 | 1 | 0.00904703 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_flat_vs_input_Y | 0.000104002 | 0 | 0.000104002 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | cost_ms_per_frame_pyref | 688.072 | 0.000400003 | 688.072 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_2160p_16k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_2160p_16k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | NA | 28.5169 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cb | NA | 34.5263 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cr | NA | 35.0819 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | NA | 0.806901 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cb | NA | 0.852283 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cr | NA | 0.869421 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lpips | NA | NA | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Y | NA | 16 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Y | NA | 0.564267 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Y | NA | 0.236906 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Y | NA | 0.223997 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Cb | NA | 16 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Cb | NA | 0.162379 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Cb | NA | 0.0898795 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Cb | NA | 0.423962 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Cb | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Cb | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | NA | -0.0617728 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | NA | 0.186549 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | NA | 0.657254 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | duplicate_rate_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | NA | 0.000366931 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s1_Y | NA | 0.777655 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s2_Y | NA | 0.83809 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s3_Y | NA | 0.885735 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_Y | NA | 0.0983607 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | NA | 0.139355 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | NA | 31.9908 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | NA | 29.263 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | NA | 28.2453 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | NA | 0.800445 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | NA | 0.147005 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | NA | 1.12018 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | NA | 0.172233 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_edge_vs_input_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (150 available) |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_flat_vs_input_Y | NA | 1 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_flat_vs_input_Y | NA | 0 | NA | NA | NA |  |
| sports50_60 | crowd | x265_1080p_6k | error_candidate | NA | NA | NA | NA | NA | RuntimeError: td_cli: bad reply b'' (exit None) |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | NA | 0.000200002 | NA | NA | NA | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_1080p_6k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_1080p_6k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
