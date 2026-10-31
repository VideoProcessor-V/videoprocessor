<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_b008_boost0 vs td_off (default)

Suite: `td_real_noprobe` (8 cases). Candidate `td_b008_boost0` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (INFO 20, LIMIT 12, NA 53, PASS 135)

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
| cost_ms_per_frame_pyref | 1236.35 | 0.000225002 | 1236.35 | 4 | 0 | INFO:4 |
| detail_pulse_Y | 0.23448 | 0.234484 | -3.91679e-06 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00303802 | -0.00303857 | 5.45127e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.00981184 | 0.00982271 | -1.08649e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.0172 | 1.0172 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.00801 | 1.008 | 8.3536e-07 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00012 | 1 | 0.000122447 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00012 | 1 | 0.000124184 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00012 | 1 | 0.000119323 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00013 | 1 | 0.000125814 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00012 | 1 | 0.00011927 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00100621 | -0.00100621 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00430321 | -0.0042998 | -3.41501e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -3.41501e-06 | 0 | -3.41501e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.692695 | 0.692695 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.875144 | 0.875037 | 0.000107045 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000112825 | 0.000112718 | 1.07182e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.928312 | 0.928301 | 1.1596e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.887024 | 0.88697 | 5.42439e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.930906 | 0.930857 | 4.92462e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.958992 | 0.958953 | 3.9166e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_all | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_B_Y | 42.5423 | 42.5423 | 3.50241e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 45.155 | 45.155 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 44.7478 | 44.7478 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 43.9596 | 43.9592 | 0.00042197 | 4 | 0 | PASS:4 |
| psnr_P_Y | 43.4387 | 43.4386 | 2.02062e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 42.7799 | 42.7799 | 3.74896e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.881583 | 0.881473 | 0.000109729 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.965919 | 0.965917 | 1.38185e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.965326 | 0.965326 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.963418 | 0.963418 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.966913 | 0.966911 | 1.24806e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.128268 | 0.128268 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 5, LIMIT 4, NA 70, PASS 32)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.285524 | 0.294797 | -0.00927327 | 2 | 1 | LIMIT:1 NA:1 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 1782.17 | 0.000249995 | 1782.17 | 2 | 1 | INFO:1 NA:1 |
| detail_pulse_Y | 0.0354355 | 0.0472241 | -0.0117887 | 2 | 1 | INFO:1 NA:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| error_candidate | NA | NA | NA | 1 | 1 | NA:1 |
| flicker_excess_Y | 0.000276099 | -0.00330246 | 0.00357856 | 2 | 1 | NA:1 PASS:1 |
| ghost_coeff_Y | 0.0335437 | 0.0502248 | -0.0166811 | 2 | 1 | NA:1 PASS:1 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.931765 | 0.782641 | 0.149123 | 2 | 1 | NA:1 PASS:1 |
| grain_hf_ratio_Y | 0.992675 | 0.98702 | 0.00565476 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_Y | 1.00006 | 1 | 5.65995e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band1_Y | 1.00005 | 1 | 5.29263e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band2_Y | 1.00004 | 1 | 4.37006e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band3_Y | 1.00005 | 1 | 5.47782e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_keep_vs_input_band4_Y | 1.00008 | 1 | 7.54166e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_delta_Cb | 0.0675888 | 0.136911 | -0.069322 | 2 | 1 | LIMIT:1 NA:1 |
| grain_lag1_delta_Y | 0.00409874 | 0.00913087 | -0.00503213 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_vs_input_Y | -1.03595e-05 | 0 | -1.03595e-05 | 2 | 1 | NA:1 PASS:1 |
| grain_rms_ratio_Cb | 0.693444 | 0.550554 | 0.14289 | 2 | 1 | LIMIT:1 NA:1 |
| grain_rms_ratio_Y | 0.939847 | 0.910134 | 0.0297131 | 2 | 1 | LIMIT:1 NA:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 1 | INFO:1 NA:1 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 1 | INFO:1 NA:1 |
| halo_overshoot_Y | 0.000132085 | 0.00021574 | -8.36549e-05 | 2 | 1 | NA:1 PASS:1 |
| ib_gap_db_Y | 0.835805 | 0.999714 | -0.163909 | 2 | 1 | INFO:1 NA:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s1_Y | 0.922079 | 0.911893 | 0.010186 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s2_Y | 0.950714 | 0.941077 | 0.00963788 | 2 | 1 | NA:1 PASS:1 |
| lc_ratio_s3_Y | 0.970016 | 0.962447 | 0.00756921 | 2 | 1 | NA:1 PASS:1 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 36.4102 | 33.9798 | 2.43041 | 2 | 1 | NA:1 PASS:1 |
| psnr_Cb | 39.8267 | 39.0374 | 0.789266 | 2 | 1 | NA:1 PASS:1 |
| psnr_Cr | 43.5368 | 42.0197 | 1.51707 | 2 | 1 | NA:1 PASS:1 |
| psnr_I_Y | 39.6411 | 37.6263 | 2.01476 | 2 | 1 | NA:1 PASS:1 |
| psnr_P_Y | 37.0888 | 34.8108 | 2.27796 | 2 | 1 | NA:1 PASS:1 |
| psnr_Y | 36.6267 | 34.2347 | 2.39205 | 2 | 1 | NA:1 PASS:1 |
| smear_Y | 0.891139 | 0.863896 | 0.0272437 | 2 | 1 | NA:1 PASS:1 |
| ssim_B_Y | 0.953514 | 0.934946 | 0.0185683 | 2 | 1 | NA:1 PASS:1 |
| ssim_Cb | 0.936384 | 0.924396 | 0.0119875 | 2 | 1 | NA:1 PASS:1 |
| ssim_Cr | 0.974154 | 0.963658 | 0.0104962 | 2 | 1 | NA:1 PASS:1 |
| ssim_Y | 0.954664 | 0.937353 | 0.0173116 | 2 | 1 | NA:1 PASS:1 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | NA | 1 | NA | 2 | 2 | NA:2 |

### sports50_60  (INFO 10, LIMIT 10, NA 27, PASS 63)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 1148.12 | 0.000250002 | 1148.12 | 2 | 0 | INFO:2 |
| detail_pulse_Y | 0.179038 | 0.179388 | -0.000350117 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0490879 | -0.0491109 | 2.30072e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.161923 | 0.161963 | -4.01142e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.0738616 | 0.0738616 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.287249 | 0.287294 | -4.52971e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00021 | 1 | 0.00021441 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.0002 | 1 | 0.000203367 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.0003 | 1 | 0.000304039 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00019 | 1 | 0.000186228 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1 | 1 | 4.97601e-06 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.554961 | 0.554961 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.280275 | 0.280274 | 1.28544e-06 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 1.28544e-06 | 0 | 1.28544e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.180821 | 0.180821 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.546482 | 0.546363 | 0.000118759 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000302719 | 0.000302285 | 4.33575e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.12435 | 1.12495 | -0.000597691 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.765472 | 0.7653 | 0.000171558 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.836234 | 0.836068 | 0.000166117 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.889124 | 0.888981 | 0.000142124 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | 29.5696 | 29.569 | 0.000547623 | 2 | 0 | PASS:2 |
| psnr_Cb | 35.0964 | 35.0964 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 35.7357 | 35.7357 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 33.316 | 33.3159 | 0.000182412 | 2 | 0 | PASS:2 |
| psnr_P_Y | 30.5915 | 30.5916 | -5.66004e-05 | 2 | 0 | PASS:2 |
| psnr_Y | 29.8421 | 29.8417 | 0.000416592 | 2 | 0 | PASS:2 |
| smear_Y | 0.671344 | 0.671 | 0.000344502 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.813046 | 0.813024 | 2.17734e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.857532 | 0.857532 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.87589 | 0.87589 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.818477 | 0.81846 | 1.68417e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0.0983607 | 0.0983607 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | psnr_Y | 48.0975 | 48.0976 | -0.000101343 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_Cb | 51.5196 | 51.5196 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_Cr | 51.1264 | 51.1264 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Y | 0.989694 | 0.989694 | -1.62774e-07 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Cb | 0.994572 | 0.994572 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_Cr | 0.994077 | 0.994077 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_12k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_rms_ratio_Y | 0.849027 | 0.849027 | 3.92712e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_12k | grain_hf_ratio_Y | 1.00763 | 1.00763 | 1.35827e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_delta_Y | -0.00507891 | -0.00507873 | -1.77515e-07 | better | PASS |  |
| film24 | tos | x265_2160p_12k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_rms_ratio_Cb | 0.59381 | 0.59381 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_12k | grain_hf_ratio_Cb | 1.01266 | 1.01266 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_delta_Cb | -0.00201065 | -0.00201065 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1 | 4.62544e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Y | -1.77515e-07 | 0 | -1.77515e-07 | better | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | flicker_excess_Y | -0.000143033 | -0.000143152 | 1.18836e-07 | same | PASS |  |
| film24 | tos | x265_2160p_12k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_12k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_12k | ghost_coeff_Y | 0.00218456 | 0.00218632 | -1.7569e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | smear_Y | 0.902703 | 0.902687 | 1.55919e-05 | better | PASS |  |
| film24 | tos | x265_2160p_12k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | halo_overshoot_Y | 8.26308e-06 | 8.25973e-06 | 3.34672e-09 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s1_Y | 0.872397 | 0.87239 | 7.57723e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s2_Y | 0.937883 | 0.937876 | 6.58452e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_ratio_s3_Y | 0.972368 | 0.972362 | 5.58952e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_12k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_12k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_12k | psnr_I_Y | 48.7149 | 48.7149 | 8.73499e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_P_Y | 48.7725 | 48.7726 | -5.78303e-06 | same | PASS |  |
| film24 | tos | x265_2160p_12k | psnr_B_Y | 47.8653 | 47.8654 | -0.000131667 | same | PASS |  |
| film24 | tos | x265_2160p_12k | ssim_B_Y | 0.989256 | 0.989256 | -2.22282e-07 | same | PASS |  |
| film24 | tos | x265_2160p_12k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_2160p_12k | ib_gap_db_Y | 0.901437 | 0.90131 | 0.000127353 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | detail_pulse_Y | 0.168421 | 0.168458 | -3.67806e-05 | NA | INFO |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1 | 5.59694e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1 | 2.49841e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1 | 3.67848e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1 | 7.01734e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_12k | cost_ms_per_frame_pyref | 1154.19 | 0.000200002 | 1154.19 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_2160p_12k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_2160p_12k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos | x265_1080p_4k | psnr_Y | 47.5555 | 47.5554 | 3.75867e-05 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_Cb | 51.2254 | 51.2254 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_Cr | 50.9453 | 50.9453 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Y | 0.990652 | 0.990652 | 6.03165e-08 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Cb | 0.993896 | 0.993896 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_Cr | 0.993512 | 0.993512 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_4k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_rms_ratio_Y | 0.901113 | 0.901109 | 4.11486e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_4k | grain_hf_ratio_Y | 1.01674 | 1.01674 | -1.61493e-07 | better | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_delta_Y | -0.0103727 | -0.010373 | 2.70349e-07 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_rms_ratio_Cb | 0.679129 | 0.679129 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_4k | grain_hf_ratio_Cb | 1.05862 | 1.05862 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_delta_Cb | -0.0102854 | -0.0102854 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1 | 4.56644e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Y | 2.70349e-07 | 0 | 2.70349e-07 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | flicker_excess_Y | -0.000479079 | -0.000480672 | 1.59374e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_4k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_4k | ghost_coeff_Y | 0.00289554 | 0.00289956 | -4.02495e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | smear_Y | 0.955968 | 0.955935 | 3.33803e-05 | better | PASS |  |
| film24 | tos | x265_1080p_4k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | halo_overshoot_Y | 2.0141e-05 | 2.01293e-05 | 1.17441e-08 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s1_Y | 0.937186 | 0.937169 | 1.68251e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s2_Y | 0.966236 | 0.966221 | 1.5485e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_ratio_s3_Y | 0.982564 | 0.982552 | 1.2127e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_4k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_4k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_4k | psnr_I_Y | 46.8961 | 46.8961 | 6.75319e-05 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_P_Y | 48.2199 | 48.2199 | 4.42577e-06 | same | PASS |  |
| film24 | tos | x265_1080p_4k | psnr_B_Y | 47.3734 | 47.3734 | 4.57471e-05 | same | PASS |  |
| film24 | tos | x265_1080p_4k | ssim_B_Y | 0.990408 | 0.990408 | 6.96068e-08 | same | PASS |  |
| film24 | tos | x265_1080p_4k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | tos | x265_1080p_4k | ib_gap_db_Y | 0.694418 | 0.694451 | -3.30563e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | detail_pulse_Y | 0.0585843 | 0.0585832 | 1.10161e-06 | NA | INFO |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1 | 4.33441e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1 | 4.8617e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1 | 3.61438e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1.00001 | 1 | 5.22895e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_4k | cost_ms_per_frame_pyref | 1610.85 | 0.000200002 | 1610.85 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_1080p_4k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_1080p_4k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_2160p_12k | psnr_Y | 39.4188 | 39.4187 | 0.000102976 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_Cb | 40.2126 | 40.2126 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_Cr | 39.7403 | 39.7403 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Y | 0.956944 | 0.956944 | 5.5548e-07 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Cb | 0.952627 | 0.952627 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_Cr | 0.950594 | 0.950594 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_12k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_rms_ratio_Y | 0.879577 | 0.879414 | 0.00016353 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | grain_hf_ratio_Y | 1.00101 | 1.00099 | 1.66058e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_delta_Y | 0.000144075 | 0.000152981 | -8.90604e-06 | better | PASS |  |
| film24 | sol | x265_2160p_12k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_rms_ratio_Cb | 0.771714 | 0.771714 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | grain_hf_ratio_Cb | 1.01749 | 1.01749 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_delta_Cb | -0.00789775 | -0.00789775 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Y | 1.00019 | 1 | 0.000185954 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Y | -8.90604e-06 | 0 | -8.90604e-06 | better | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | flicker_excess_Y | -0.00369979 | -0.00370042 | 6.25904e-07 | same | PASS |  |
| film24 | sol | x265_2160p_12k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_12k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_12k | ghost_coeff_Y | 0.0116714 | 0.0116768 | -5.36781e-06 | same | PASS |  |
| film24 | sol | x265_2160p_12k | smear_Y | 0.854042 | 0.853971 | 7.08893e-05 | better | PASS |  |
| film24 | sol | x265_2160p_12k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | halo_overshoot_Y | 0.000144998 | 0.000144943 | 5.50762e-08 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s1_Y | 0.868254 | 0.868219 | 3.48941e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s2_Y | 0.912082 | 0.912049 | 3.22669e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_ratio_s3_Y | 0.942595 | 0.942569 | 2.5464e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | wtw_kept_Y | 0.117647 | 0.117647 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | btb_kept_C | 0.470588 | 0.470588 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_12k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_12k | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_12k | psnr_I_Y | 41.1013 | 41.1011 | 0.00014107 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_P_Y | 39.933 | 39.933 | 2.64103e-05 | same | PASS |  |
| film24 | sol | x265_2160p_12k | psnr_B_Y | 39.2028 | 39.2027 | 0.000124431 | same | PASS |  |
| film24 | sol | x265_2160p_12k | ssim_B_Y | 0.956302 | 0.956301 | 6.77084e-07 | same | PASS |  |
| film24 | sol | x265_2160p_12k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_2160p_12k | ib_gap_db_Y | 0.833872 | 0.833961 | -8.90476e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | detail_pulse_Y | 0.408884 | 0.4089 | -1.62398e-05 | NA | INFO |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1.00018 | 1 | 0.000182088 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1.00017 | 1 | 0.000169613 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1.00019 | 1 | 0.00019398 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1.00019 | 1 | 0.0001916 | worse | PASS |  |
| film24 | sol | x265_2160p_12k | cost_ms_per_frame_pyref | 1104.56 | 0.000200002 | 1104.56 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_2160p_12k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_2160p_12k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_1080p_4k | psnr_Y | 36.048 | 36.0479 | 0.000110738 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_Cb | 37.6623 | 37.6623 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_Cr | 37.1791 | 37.1791 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Y | 0.93036 | 0.930355 | 4.53922e-06 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Cb | 0.920207 | 0.920207 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_Cr | 0.91549 | 0.91549 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_4k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_rms_ratio_Y | 0.87086 | 0.8706 | 0.000260142 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | grain_hf_ratio_Y | 1.00665 | 1.00666 | -1.32387e-05 | better | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_delta_Y | -0.00190532 | -0.00190048 | -4.84682e-06 | better | PASS |  |
| film24 | sol | x265_1080p_4k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_rms_ratio_Cb | 0.726128 | 0.726128 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | grain_hf_ratio_Cb | 0.980032 | 0.980032 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_delta_Cb | 0.0161689 | 0.0161689 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Y | 1.0003 | 1 | 0.000298807 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Y | -4.84682e-06 | 0 | -4.84682e-06 | better | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | flicker_excess_Y | -0.00783019 | -0.00783003 | -1.57966e-07 | same | PASS |  |
| film24 | sol | x265_1080p_4k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_4k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_4k | ghost_coeff_Y | 0.0224959 | 0.0225282 | -3.23098e-05 | same | PASS |  |
| film24 | sol | x265_1080p_4k | smear_Y | 0.81362 | 0.813301 | 0.000319056 | better | PASS |  |
| film24 | sol | x265_1080p_4k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | halo_overshoot_Y | 0.000277898 | 0.000277539 | 3.5856e-07 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s1_Y | 0.87026 | 0.870102 | 0.000157679 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s2_Y | 0.907425 | 0.907282 | 0.000142648 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_ratio_s3_Y | 0.938443 | 0.93833 | 0.000113483 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_4k | wtw_kept_Y | 0.138889 | 0.138889 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | btb_kept_C | 0.692308 | 0.692308 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_4k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_4k | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_4k | psnr_I_Y | 39.1262 | 39.1247 | 0.00147054 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_P_Y | 36.8291 | 36.8291 | 5.57716e-05 | same | PASS |  |
| film24 | sol | x265_1080p_4k | psnr_B_Y | 35.7278 | 35.7277 | 0.000101585 | same | PASS |  |
| film24 | sol | x265_1080p_4k | ssim_B_Y | 0.927708 | 0.927703 | 5.003e-06 | same | PASS |  |
| film24 | sol | x265_1080p_4k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | sol | x265_1080p_4k | ib_gap_db_Y | 1.28352 | 1.28348 | 4.11348e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | detail_pulse_Y | 0.302033 | 0.301997 | 3.62516e-05 | NA | INFO |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1.00031 | 1 | 0.000309754 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1.0003 | 1 | 0.000302567 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1.00031 | 1 | 0.000305294 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1.00028 | 1 | 0.000279549 | worse | PASS |  |
| film24 | sol | x265_1080p_4k | cost_ms_per_frame_pyref | 1075.81 | 0.000300002 | 1075.81 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_1080p_4k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_1080p_4k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_2160p_14k | psnr_Y | 36.6267 | 36.6265 | 0.000182941 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_Cb | 39.8267 | 39.8267 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_Cr | 43.5368 | 43.5368 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Y | 0.954664 | 0.954663 | 1.20773e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Cb | 0.936384 | 0.936384 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_Cr | 0.974154 | 0.974154 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_14k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_rms_ratio_Y | 0.939847 | 0.939794 | 5.31919e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_hf_ratio_Y | 0.992675 | 0.992658 | 1.6695e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_delta_Y | 0.00409874 | 0.0041091 | -1.03595e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_rms_ratio_Cb | 0.693444 | 0.693444 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_hf_ratio_Cb | 0.931765 | 0.931765 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_delta_Cb | 0.0675888 | 0.0675888 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Y | 1.00006 | 1 | 5.65995e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Y | -1.03595e-05 | 0 | -1.03595e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | flicker_excess_Y | 0.000276099 | 0.000279709 | -3.60996e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_2160p_14k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_2160p_14k | ghost_coeff_Y | 0.0335437 | 0.0335583 | -1.46144e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | smear_Y | 0.891139 | 0.891075 | 6.38382e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_14k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | halo_overshoot_Y | 0.000132085 | 0.000132016 | 6.87055e-08 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s1_Y | 0.922079 | 0.922051 | 2.84727e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s2_Y | 0.950714 | 0.950688 | 2.62375e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_ratio_s3_Y | 0.970016 | 0.969995 | 2.0973e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_Y | 0.285524 | 0.283406 | 0.00211802 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_14k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_14k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_14k | psnr_I_Y | 39.6411 | 39.641 | 9.89538e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_P_Y | 37.0888 | 37.0888 | -9.12296e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | psnr_B_Y | 36.4102 | 36.41 | 0.000239813 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | ssim_B_Y | 0.953514 | 0.953512 | 1.56997e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_14k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_2160p_14k | ib_gap_db_Y | 0.835805 | 0.836041 | -0.000236123 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | detail_pulse_Y | 0.0354355 | 0.0354826 | -4.7194e-05 | NA | INFO |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band1_Y | 1.00005 | 1 | 5.29263e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band2_Y | 1.00004 | 1 | 4.37006e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band3_Y | 1.00005 | 1 | 5.47782e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band4_Y | 1.00008 | 1 | 7.54166e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_14k | cost_ms_per_frame_pyref | 1782.17 | 0.000200002 | 1782.17 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_2160p_14k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_2160p_14k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_1080p_5k | psnr_Y | NA | 31.8428 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | psnr_Cb | NA | 38.2482 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | psnr_Cr | NA | 40.5027 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | ssim_Y | NA | 0.920042 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | ssim_Cb | NA | 0.912409 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | ssim_Cr | NA | 0.953162 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lpips | NA | NA | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_tiles_Y | NA | 16 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_rms_ratio_Y | NA | 0.880474 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_hf_ratio_Y | NA | 0.981382 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_delta_Y | NA | 0.0141526 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_tiles_Cb | NA | 16 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_rms_ratio_Cb | NA | 0.407663 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_hf_ratio_Cb | NA | 0.633518 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_delta_Cb | NA | 0.206233 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Y | NA | 0 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Cb | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Cb | NA | 0 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | flicker_excess_Y | NA | -0.00688464 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_1080p_5k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (90 frames) |
| nature30 | boat | x265_1080p_5k | ghost_coeff_Y | NA | 0.0668913 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | smear_Y | NA | 0.836716 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | duplicate_rate_Y | NA | 0 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | halo_overshoot_Y | NA | 0.000299464 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s1_Y | NA | 0.901736 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s2_Y | NA | 0.931465 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lc_ratio_s3_Y | NA | 0.954899 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lc_boost_fraction_Y | NA | 0 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | btb_kept_Y | NA | 0.306188 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | clip_detect_Y | NA | 0 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_5k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_5k | psnr_I_Y | NA | 35.6116 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | psnr_P_Y | NA | 32.5329 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | psnr_B_Y | NA | 31.5496 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | ssim_B_Y | NA | 0.916379 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | boat | x265_1080p_5k | ib_gap_db_Y | NA | 1.16339 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | detail_pulse_Y | NA | 0.0589656 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band1_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band2_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band3_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band4_Y | NA | 1 | NA | NA | NA |  |
| nature30 | boat | x265_1080p_5k | error_candidate | NA | NA | NA | NA | NA | MemoryError: Unable to allocate 3.98 MiB for an array with shape (544, 960) and data type float64 |
| nature30 | boat | x265_1080p_5k | cost_ms_per_frame_pyref | NA | 0.000299988 | NA | NA | NA | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_1080p_5k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_1080p_5k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_2160p_16k | psnr_Y | 31.1668 | 31.1665 | 0.000284273 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_Cb | 35.6664 | 35.6664 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_Cr | 36.3895 | 36.3895 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Y | 0.830028 | 0.83002 | 8.20347e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Cb | 0.862781 | 0.862781 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_Cr | 0.882359 | 0.882359 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_16k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_rms_ratio_Y | 0.528525 | 0.52846 | 6.56748e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_hf_ratio_Y | 0.337629 | 0.337682 | -5.27905e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_delta_Y | 0.336557 | 0.336551 | 6.08666e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_rms_ratio_Cb | 0.199264 | 0.199264 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_hf_ratio_Cb | 0.0578437 | 0.0578437 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_delta_Cb | 0.68596 | 0.68596 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Y | 1.00012 | 1 | 0.000124276 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Y | 6.08666e-06 | 0 | 6.08666e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | flicker_excess_Y | -0.0364381 | -0.0364489 | 1.08146e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_2160p_16k | ghost_coeff_Y | 0.137345 | 0.137376 | -3.15853e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | smear_Y | 0.684947 | 0.684746 | 0.000200748 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | halo_overshoot_Y | 0.000237779 | 0.000237639 | 1.3949e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s1_Y | 0.753041 | 0.752946 | 9.49595e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s2_Y | 0.834139 | 0.834046 | 9.3154e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_ratio_s3_Y | 0.892307 | 0.892228 | 7.85603e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_16k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_16k | psnr_I_Y | 34.641 | 34.6409 | 0.000136657 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_P_Y | 31.9202 | 31.9202 | -2.48123e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | psnr_B_Y | 30.8932 | 30.8928 | 0.00036969 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | ssim_B_Y | 0.825614 | 0.825603 | 1.07441e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_2160p_16k | ib_gap_db_Y | 1.12933 | 1.12972 | -0.000390084 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | detail_pulse_Y | 0.186309 | 0.186542 | -0.000233494 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band1_Y | 1.00011 | 1 | 0.000113241 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band2_Y | 1.00019 | 1 | 0.000185542 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band3_Y | 1.00008 | 1 | 7.70202e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band4_Y | 0.999967 | 1 | -3.30318e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_16k | cost_ms_per_frame_pyref | 1472.35 | 0.000200002 | 1472.35 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_2160p_16k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_2160p_16k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | 28.5174 | 28.5169 | 0.000548911 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cb | 34.5263 | 34.5263 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cr | 35.0819 | 35.0819 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | 0.806926 | 0.806901 | 2.54799e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cb | 0.852283 | 0.852283 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cr | 0.869421 | 0.869421 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Y | 0.564438 | 0.564267 | 0.000171844 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Y | 0.236869 | 0.236906 | -3.78036e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Y | 0.223993 | 0.223997 | -3.51579e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Cb | 0.162379 | 0.162379 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Cb | 0.0898795 | 0.0898795 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Cb | 0.423962 | 0.423962 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | 1.0003 | 1 | 0.000304544 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | -3.51579e-06 | 0 | -3.51579e-06 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | -0.0617376 | -0.0617728 | 3.51999e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | 0.186501 | 0.186549 | -4.86431e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | 0.657742 | 0.657254 | 0.000488256 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | 0.000367658 | 0.000366931 | 7.2766e-07 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s1_Y | 0.777903 | 0.777655 | 0.000248156 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s2_Y | 0.838329 | 0.83809 | 0.00023908 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s3_Y | 0.885941 | 0.885735 | 0.000205688 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_Y | 0.0983607 | 0.0983607 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | 31.9911 | 31.9908 | 0.000228168 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | 29.2629 | 29.263 | -8.83886e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | 28.246 | 28.2453 | 0.000725556 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | 0.800478 | 0.800445 | 3.28027e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | 1.11937 | 1.12018 | -0.000805297 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | 0.171767 | 0.172233 | -0.000466739 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | 1.00029 | 1 | 0.000293492 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | 1.00042 | 1 | 0.000422536 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | 1.0003 | 1 | 0.000295436 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | 1.00004 | 1 | 4.29839e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | 823.89 | 0.000300002 | 823.89 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_1080p_6k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_1080p_6k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
