<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: mosquito_naive_mc vs repair_none (default)

Suite: `proto_mosquito` (9 cases). Candidate `mosquito_naive_mc` (CONTRAST ONLY: MC temporal average of Y in the zone, no grain split, no guard (the original idea)), default `repair_none`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### dark  (INFO 7, LIMIT 2, NA 15, PASS 24)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_C | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_Y | NA | NA | NA | 1 | 1 | NA:1 |
| cost_est_3090_ms | NA | NA | NA | 1 | 1 | NA:1 |
| cost_gpu_ms_laptop | NA | NA | NA | 1 | 1 | NA:1 |
| cost_ms_per_frame_pyref | 2052.37 | 0.0037 | 2052.37 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| edge_keep_vs_input_Y | 0.997067 | 1 | -0.00293267 | 1 | 0 | INFO:1 |
| edge_keep_vs_master_Y | 0.97084 | 0.973695 | -0.00285552 | 1 | 0 | INFO:1 |
| flicker_excess_Y | -0.0570266 | -0.0541334 | -0.00289326 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.0816875 | 0.0809106 | 0.00077692 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 1.03045 | 1.03045 | 0 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 1.2542 | 1.2542 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | -0.0270899 | -0.0270899 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Y | -0.112417 | -0.112417 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 0.76667 | 0.76667 | 0 | 1 | 0 | LIMIT:1 |
| grain_rms_ratio_Y | 0.319742 | 0.319742 | 0 | 1 | 0 | LIMIT:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 1.92841e-05 | 1.9809e-05 | -5.2494e-07 | 1 | 0 | PASS:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.778967 | 0.779453 | -0.000485924 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.859769 | 0.860075 | -0.000305744 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.921373 | 0.921581 | -0.00020768 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | 54.9583 | 54.9583 | 0 | 1 | 0 | PASS:1 |
| psnr_Cr | 55.9319 | 55.9319 | 0 | 1 | 0 | PASS:1 |
| psnr_Y | 48.2791 | 48.277 | 0.00209461 | 1 | 0 | PASS:1 |
| psnr_band_Y | 43.9394 | 43.832 | 0.107406 | 1 | 0 | PASS:1 |
| ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | 1 | 0 | INFO:1 |
| ringing_energy_ratio_Y | 0.977172 | 1 | -0.0228276 | 1 | 0 | INFO:1 |
| smear_Y | 0.826169 | 0.831391 | -0.00522171 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.997972 | 0.997972 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.998419 | 0.998419 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 0.989351 | 0.989348 | 3.38453e-06 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |

### film24  (FLAG 1, INFO 28, LIMIT 17, NA 51, PASS 95)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_Y | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_C | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 1 | NA:1 PASS:3 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 2182.53 | 0.004725 | 2182.53 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 0.997859 | 1 | -0.00214096 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.89128 | 0.893096 | -0.00181542 | 4 | 0 | INFO:4 |
| flicker_excess_Y | -0.0312297 | -0.0302196 | -0.00101013 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0995174 | 0.0991625 | 0.000354941 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 0.769461 | 0.769461 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 0.919095 | 0.919837 | -0.000741708 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 0.997833 | 1 | -0.00216659 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.12298 | 0.12298 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.101661 | 0.101603 | 5.89132e-05 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | 5.89132e-05 | 0 | 5.89132e-05 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.653937 | 0.653937 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.511712 | 0.513238 | -0.0015262 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000118913 | 0.000120619 | -1.70563e-06 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.795612 | 0.796578 | -0.000965598 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.868623 | 0.869181 | -0.000557902 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.922427 | 0.922794 | -0.000366535 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 47.4652 | 47.4652 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 46.9951 | 46.9951 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 40.7629 | 40.7593 | 0.00361748 | 4 | 0 | PASS:4 |
| psnr_band_Y | 38.1192 | 38.1114 | 0.00783723 | 4 | 0 | FLAG:1 PASS:3 |
| ringing_band_share | 0.00558794 | 0.00558794 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.962115 | 1 | -0.0378855 | 4 | 0 | INFO:4 |
| smear_Y | 0.747166 | 0.749798 | -0.0026322 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.983663 | 0.983663 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.983634 | 0.983634 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.942253 | 0.942201 | 5.15829e-05 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.48931 | 0.483623 | 0.00568666 | 4 | 1 | LIMIT:3 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 1 | NA:1 PASS:3 |

### sports50_60  (INFO 28, LIMIT 13, NA 61, PASS 90)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_Y | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_C | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 1567.71 | 0.0062375 | 1567.7 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 0.999357 | 1 | -0.000642544 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.808411 | 0.809025 | -0.000613925 | 4 | 0 | INFO:4 |
| flicker_excess_Y | 0.187723 | 0.192813 | -0.00508986 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.158788 | 0.158667 | 0.000120921 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Y | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.0507803 | 0.0507803 | 0 | 4 | 0 | LIMIT:1 PASS:3 |
| grain_lag1_delta_Y | 0.0699614 | 0.0699614 | 0 | 4 | 0 | LIMIT:2 PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.325415 | 0.325415 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.297538 | 0.297538 | 0 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 5.88763e-05 | 5.9195e-05 | -3.18757e-07 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.688841 | 0.689102 | -0.000260471 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.806568 | 0.806737 | -0.000169364 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.886119 | 0.886248 | -0.000129107 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 42.7255 | 42.7255 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.4456 | 42.4456 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 37.7411 | 37.7365 | 0.00457747 | 4 | 0 | PASS:4 |
| psnr_band_Y | 34.7635 | 34.6986 | 0.0648983 | 4 | 0 | PASS:4 |
| ringing_band_share | 0.00616185 | 0.00616185 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.966662 | 1 | -0.0333379 | 4 | 0 | INFO:4 |
| smear_Y | 0.606331 | 0.60715 | -0.000818593 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.964541 | 0.964541 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.962116 | 0.962116 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.912045 | 0.912014 | 3.17332e-05 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.543471 | 0.524168 | 0.0193032 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 1 | NA:1 PASS:3 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Y | 38.0932 | 38.085 | 0.0082351 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cb | 43.3014 | 43.3014 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cr | 42.9608 | 42.9608 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ssim_Y | 0.932127 | 0.932067 | 5.99313e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ssim_Cb | 0.970557 | 0.970557 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ssim_Cr | 0.970481 | 0.970481 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_rms_ratio_Y | 0.225393 | 0.225393 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_lag1_delta_Y | 0.198603 | 0.198603 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_rms_ratio_Cb | 0.223676 | 0.223676 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_lag1_delta_Cb | 0.18438 | 0.18438 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | flicker_excess_Y | 0.337602 | 0.343094 | -0.00549204 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | ghost_coeff_Y | 0.214665 | 0.214583 | 8.18756e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | smear_Y | 0.69151 | 0.692724 | -0.00121388 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | halo_overshoot_Y | 3.28373e-05 | 3.32458e-05 | -4.0855e-07 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s1_Y | 0.683336 | 0.683742 | -0.000406023 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s2_Y | 0.794729 | 0.794984 | -0.000254964 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s3_Y | 0.876457 | 0.876645 | -0.000187697 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_Y | 0.498431 | 0.480854 | 0.0175769 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_energy_ratio_Y | 0.952227 | 1 | -0.0477731 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_band_Y | 35.3744 | 35.2985 | 0.0759558 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_input_Y | 0.999506 | 1 | -0.000493562 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_master_Y | 0.944325 | 0.944792 | -0.000466313 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_ms_per_frame_pyref | 1619.52 | 0.00455 | 1619.51 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Y | 33.9917 | 33.9916 | 4.26072e-05 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cb | 39.9996 | 39.9996 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cr | 40.2577 | 40.2577 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ssim_Y | 0.846728 | 0.846727 | 1.07383e-06 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ssim_Cb | 0.944076 | 0.944076 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ssim_Cr | 0.94257 | 0.94257 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_rms_ratio_Y | 0.245633 | 0.245633 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_lag1_delta_Y | 0.0616194 | 0.0616194 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_rms_ratio_Cb | 0.284426 | 0.284426 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_lag1_delta_Cb | 0.0252387 | 0.0252387 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | flicker_excess_Y | 0.177914 | 0.177958 | -4.35672e-05 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | ghost_coeff_Y | 0.252882 | 0.252882 | -3.41644e-07 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | smear_Y | 0.31601 | 0.316016 | -6.08353e-06 | worse | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | halo_overshoot_Y | 0.000164812 | 0.000164828 | -1.52877e-08 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s1_Y | 0.587375 | 0.587378 | -3.36257e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s2_Y | 0.722597 | 0.7226 | -3.07546e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s3_Y | 0.814556 | 0.814558 | -2.1729e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_energy_ratio_Y | 1.00015 | 1 | 0.000145494 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_band_share | 0.000888507 | 0.000888507 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_band_Y | 28.0034 | 28.0034 | 4.87235e-06 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_input_Y | 1 | 1 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_master_Y | 0.379369 | 0.379369 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_ms_per_frame_pyref | 1575.18 | 0.0042 | 1575.17 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Y | 38.2728 | 38.2716 | 0.00120239 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cb | 41.9464 | 41.9464 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cr | 41.7519 | 41.7519 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | ssim_Y | 0.922328 | 0.922317 | 1.09238e-05 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | ssim_Cb | 0.965136 | 0.965136 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | ssim_Cr | 0.960554 | 0.960554 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | Jockey | native_x265_crf32 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_rms_ratio_Y | 0.115777 | 0.115777 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x265_crf32 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x265_crf32 | grain_lag1_delta_Y | -0.000761686 | -0.000761686 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_rms_ratio_Cb | 0.156428 | 0.156428 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x265_crf32 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x265_crf32 | grain_lag1_delta_Cb | 0.0122679 | 0.0122679 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | flicker_excess_Y | -0.00487504 | -0.00487077 | -4.27177e-06 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | ghost_coeff_Y | 0.0154624 | 0.0154427 | 1.97053e-05 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | smear_Y | 0.611928 | 0.612221 | -0.000293091 | worse | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | halo_overshoot_Y | 1.3901e-05 | 1.40403e-05 | -1.3932e-07 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s1_Y | 0.646823 | 0.64693 | -0.000107436 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s2_Y | 0.806488 | 0.806567 | -7.89233e-05 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s3_Y | 0.909006 | 0.909063 | -5.71394e-05 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | ringing_energy_ratio_Y | 0.992544 | 1 | -0.00745604 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | ringing_band_share | 0.010979 | 0.010979 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_band_Y | 36.3028 | 36.2829 | 0.0199211 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_input_Y | 0.999096 | 1 | -0.000904353 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_master_Y | 0.939406 | 0.940256 | -0.000850323 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | cost_ms_per_frame_pyref | 1580.18 | 0.00825 | 1580.17 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Y | 40.6066 | 40.5978 | 0.0088298 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cb | 45.6546 | 45.6546 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cr | 44.8121 | 44.8121 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ssim_Y | 0.946999 | 0.946944 | 5.5004e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ssim_Cb | 0.978396 | 0.978396 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ssim_Cr | 0.974858 | 0.974858 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_rms_ratio_Y | 0.603347 | 0.603347 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_lag1_delta_Y | 0.0203852 | 0.0203852 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_rms_ratio_Cb | 0.637128 | 0.637128 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_lag1_delta_Cb | -0.018765 | -0.018765 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | flicker_excess_Y | 0.240251 | 0.255071 | -0.0148196 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ghost_coeff_Y | 0.152141 | 0.151758 | 0.000382446 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | smear_Y | 0.805878 | 0.807639 | -0.00176132 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | halo_overshoot_Y | 2.39545e-05 | 2.46664e-05 | -7.11868e-07 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s1_Y | 0.837831 | 0.838357 | -0.000525062 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s2_Y | 0.902458 | 0.902798 | -0.000340492 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s3_Y | 0.944459 | 0.944728 | -0.000269419 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_Y | 0.588512 | 0.567483 | 0.0210295 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_energy_ratio_Y | 0.921732 | 1 | -0.0782679 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_band_Y | 39.3734 | 39.2097 | 0.163712 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_input_Y | 0.998828 | 1 | -0.00117226 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_master_Y | 0.970542 | 0.971682 | -0.00113906 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_ms_per_frame_pyref | 1495.95 | 0.00795 | 1495.94 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 37.8728 | 37.8682 | 0.00455604 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 45.0394 | 45.0394 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.8627 | 44.8627 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.941974 | 0.941934 | 3.94378e-05 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.984485 | 0.984485 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984175 | 0.984175 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.797037 | 0.801284 | -0.00424707 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | 1.02636 | 1.02844 | -0.00208679 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0342151 | 0.034083 | 0.000132073 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.679178 | 0.679178 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | 0.770116 | 0.770116 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.120005 | 0.120005 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Y | 0.9947 | 1 | -0.00530033 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Y | 0.000132073 | 0 | 0.000132073 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.0497369 | -0.0480265 | -0.00171036 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.0756768 | 0.0749679 | 0.000708988 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.731129 | 0.735623 | -0.00449457 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 0.000134107 | 0.000136667 | -2.56027e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.880416 | 0.882063 | -0.00164716 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.923772 | 0.924701 | -0.000929477 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.952226 | 0.952805 | -0.000578867 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.575145 | 0.575145 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_energy_ratio_Y | 0.930316 | 1 | -0.0696841 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_band_share | 0.00776768 | 0.00776768 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_band_Y | 35.7531 | 35.7173 | 0.0357889 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_input_Y | 0.996939 | 1 | -0.00306123 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_master_Y | 0.881163 | 0.883869 | -0.00270573 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 1806.53 | 0.0081 | 1806.52 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Y | 52.0828 | 52.0852 | -0.00236532 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cb | 58.2207 | 58.2207 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cr | 56.7815 | 56.7815 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Y | 0.996568 | 0.996568 | -4.3439e-07 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cb | 0.99899 | 0.99899 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cr | 0.998693 | 0.998693 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Y | 0.318042 | 0.318042 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Y | 0.849859 | 0.849859 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Y | 0.125011 | 0.125011 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Cb | 0.811874 | 0.811874 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Cb | 0.810022 | 0.810022 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Cb | 0.00391578 | 0.00391578 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Y | 0.00787081 | 0.00895573 | -0.00108492 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ghost_coeff_Y | 0.00240779 | 0.00252922 | -0.000121424 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | smear_Y | 0.95369 | 0.953912 | -0.000222769 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | halo_overshoot_Y | 1.17052e-05 | 1.21607e-05 | -4.55517e-07 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s1_Y | 0.758218 | 0.758452 | -0.000233643 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s2_Y | 0.837916 | 0.837889 | 2.67826e-05 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s3_Y | 0.909448 | 0.909369 | 7.89351e-05 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_energy_ratio_Y | 1.02328 | 1 | 0.0232779 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_band_share | 0.000114282 | 0.000114282 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_band_Y | 47.9027 | 47.9986 | -0.0958654 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_input_Y | 1.00049 | 1 | 0.000492328 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_master_Y | 1.00005 | 0.999558 | 0.00049211 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_ms_per_frame_pyref | 1791.27 | 0.00385 | 1791.26 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc32_12f | psnr_Y | 38.909 | 38.9044 | 0.00463354 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cb | 43.0743 | 43.0743 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cr | 43.138 | 43.138 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | ssim_Y | 0.937457 | 0.937418 | 3.8515e-05 | same | PASS |  |
| film24 | breathing | enc32_12f | ssim_Cb | 0.969275 | 0.969275 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | ssim_Cr | 0.971263 | 0.971263 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | breathing | enc32_12f | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc32_12f | grain_rms_ratio_Y | 0.381719 | 0.381719 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | grain_hf_ratio_Y | 0.881974 | 0.881974 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | grain_lag1_delta_Y | 0.134854 | 0.134854 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc32_12f | grain_rms_ratio_Cb | 0.213839 | 0.213839 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | grain_hf_ratio_Cb | 0.774421 | 0.774421 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | grain_lag1_delta_Cb | 0.242352 | 0.242352 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | flicker_excess_Y | -0.126124 | -0.126336 | 0.000212061 | same | PASS |  |
| film24 | breathing | enc32_12f | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | ghost_coeff_Y | 0.154781 | 0.154427 | 0.000353838 | same | PASS |  |
| film24 | breathing | enc32_12f | smear_Y | 0.742988 | 0.74558 | -0.00259199 | worse | PASS |  |
| film24 | breathing | enc32_12f | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | halo_overshoot_Y | 3.7456e-05 | 3.81693e-05 | -7.13334e-07 | same | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s1_Y | 0.782362 | 0.783035 | -0.000672526 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s2_Y | 0.876385 | 0.876919 | -0.000533988 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s3_Y | 0.92945 | 0.929884 | -0.000433923 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_Y | 0.564659 | 0.551506 | 0.0131537 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | ringing_energy_ratio_Y | 0.948081 | 1 | -0.0519194 | NA | INFO |  |
| film24 | breathing | enc32_12f | ringing_band_share | 0.00688595 | 0.00688595 | 0 | NA | INFO |  |
| film24 | breathing | enc32_12f | psnr_band_Y | 37.0249 | 36.9593 | 0.0655563 | better | PASS |  |
| film24 | breathing | enc32_12f | edge_keep_vs_input_Y | 0.997071 | 1 | -0.00292863 | NA | INFO |  |
| film24 | breathing | enc32_12f | edge_keep_vs_master_Y | 0.94732 | 0.950102 | -0.0027825 | NA | INFO |  |
| film24 | breathing | enc32_12f | cost_ms_per_frame_pyref | 3007.8 | 0.0036 | 3007.8 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc32_12f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc32_12f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Y | 48.2791 | 48.277 | 0.00209461 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cb | 54.9583 | 54.9583 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cr | 55.9319 | 55.9319 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Y | 0.989351 | 0.989348 | 3.38453e-06 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cb | 0.997972 | 0.997972 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cr | 0.998419 | 0.998419 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Y | 0.319742 | 0.319742 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Y | 1.2542 | 1.2542 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Y | -0.112417 | -0.112417 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Cb | 0.76667 | 0.76667 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Cb | 1.03045 | 1.03045 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Cb | -0.0270899 | -0.0270899 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Y | -0.0570266 | -0.0541334 | -0.00289326 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | ghost_coeff_Y | 0.0816875 | 0.0809106 | 0.00077692 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | smear_Y | 0.826169 | 0.831391 | -0.00522171 | worse | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | halo_overshoot_Y | 1.92841e-05 | 1.9809e-05 | -5.2494e-07 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s1_Y | 0.778967 | 0.779453 | -0.000485924 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s2_Y | 0.859769 | 0.860075 | -0.000305744 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s3_Y | 0.921373 | 0.921581 | -0.00020768 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_energy_ratio_Y | 0.977172 | 1 | -0.0228276 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_band_Y | 43.9394 | 43.832 | 0.107406 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_input_Y | 0.997067 | 1 | -0.00293267 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_master_Y | 0.97084 | 0.973695 | -0.00285552 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_ms_per_frame_pyref | 2052.37 | 0.0037 | 2052.37 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Y | 34.1871 | 34.1794 | 0.00764567 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cb | 43.5265 | 43.5265 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cr | 43.1985 | 43.1985 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Y | 0.893012 | 0.892883 | 0.000128813 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cb | 0.981901 | 0.981901 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cr | 0.980403 | 0.980403 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Y | 0.550048 | 0.551906 | -0.00185774 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Y | 0.918189 | 0.919069 | -0.000880046 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Y | 0.112566 | 0.112462 | 0.000103579 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Cb | 0.910858 | 0.910858 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Cb | 0.723284 | 0.723284 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Cb | 0.125645 | 0.125645 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Y | 0.996634 | 1 | -0.00336604 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Y | 0.000103579 | 0 | 0.000103579 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | flicker_excess_Y | 0.0430711 | 0.0445284 | -0.00145732 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ghost_coeff_Y | 0.165204 | 0.164726 | 0.000478363 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | smear_Y | 0.560859 | 0.564078 | -0.00321946 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | halo_overshoot_Y | 0.000292384 | 0.000295477 | -3.09342e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s1_Y | 0.761454 | 0.762763 | -0.00130906 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s2_Y | 0.83642 | 0.837214 | -0.000794925 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s3_Y | 0.898585 | 0.899117 | -0.000532284 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_Y | 0.328125 | 0.324219 | 0.00390625 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_energy_ratio_Y | 0.946784 | 1 | -0.0532164 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_band_share | 0.00758383 | 0.00758383 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_band_Y | 31.7962 | 31.7704 | 0.025869 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_input_Y | 0.996934 | 1 | -0.00306631 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_master_Y | 0.736588 | 0.738854 | -0.00226556 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_ms_per_frame_pyref | 2124.52 | 0.00335 | 2124.51 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
