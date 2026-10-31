<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: mosquito_mc vs repair_none (default)

Suite: `proto_mosquito` (9 cases). Candidate `mosquito_mc` (mosquito repair: ringing zones near strong edges, MC temporal filter of the structure layer only, exact grain add-back, output grain guard (DIS flow = vector stand-in)), default `repair_none`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

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
| cost_ms_per_frame_pyref | 2920.55 | 0.0037 | 2920.55 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| edge_keep_vs_input_Y | 0.997239 | 1 | -0.00276142 | 1 | 0 | INFO:1 |
| edge_keep_vs_master_Y | 0.971006 | 0.973695 | -0.00268878 | 1 | 0 | INFO:1 |
| flicker_excess_Y | -0.0567687 | -0.0541334 | -0.00263532 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.0815418 | 0.0809106 | 0.000631206 | 1 | 0 | PASS:1 |
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
| halo_overshoot_Y | 1.93971e-05 | 1.9809e-05 | -4.1191e-07 | 1 | 0 | PASS:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.77902 | 0.779453 | -0.000432556 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.859795 | 0.860075 | -0.000279309 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.92139 | 0.921581 | -0.00019032 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | 54.9583 | 54.9583 | 0 | 1 | 0 | PASS:1 |
| psnr_Cr | 55.9319 | 55.9319 | 0 | 1 | 0 | PASS:1 |
| psnr_Y | 48.2786 | 48.277 | 0.00158728 | 1 | 0 | PASS:1 |
| psnr_band_Y | 43.9149 | 43.832 | 0.0829168 | 1 | 0 | PASS:1 |
| ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | 1 | 0 | INFO:1 |
| ringing_energy_ratio_Y | 0.997566 | 1 | -0.00243436 | 1 | 0 | INFO:1 |
| smear_Y | 0.826609 | 0.831391 | -0.00478172 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.997972 | 0.997972 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.998419 | 0.998419 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 0.98935 | 0.989348 | 2.63668e-06 | 1 | 0 | PASS:1 |
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
| cost_ms_per_frame_pyref | 3264.33 | 0.004725 | 3264.32 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 0.99793 | 1 | -0.0020702 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.89133 | 0.893096 | -0.00176581 | 4 | 0 | INFO:4 |
| flicker_excess_Y | -0.0311377 | -0.0302196 | -0.000918089 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0995021 | 0.0991625 | 0.000339684 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 0.769461 | 0.769461 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 0.919237 | 0.919837 | -0.0005997 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 0.998161 | 1 | -0.00183912 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.12298 | 0.12298 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.101642 | 0.101603 | 3.90197e-05 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | 3.90197e-05 | 0 | 3.90197e-05 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.653937 | 0.653937 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.511947 | 0.513238 | -0.00129052 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000119122 | 0.000120619 | -1.49658e-06 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.795681 | 0.796578 | -0.000897538 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.868647 | 0.869181 | -0.000533319 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.922438 | 0.922794 | -0.00035588 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 47.4652 | 47.4652 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 46.9951 | 46.9951 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 40.7629 | 40.7593 | 0.00361954 | 4 | 0 | PASS:4 |
| psnr_band_Y | 38.1235 | 38.1114 | 0.0121446 | 4 | 0 | FLAG:1 PASS:3 |
| ringing_band_share | 0.00558794 | 0.00558794 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.970735 | 1 | -0.0292653 | 4 | 0 | INFO:4 |
| smear_Y | 0.747294 | 0.749798 | -0.00250435 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.983663 | 0.983663 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.983634 | 0.983634 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.94225 | 0.942201 | 4.89625e-05 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.489204 | 0.483623 | 0.00558101 | 4 | 1 | LIMIT:3 NA:1 |
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
| cost_ms_per_frame_pyref | 2252.09 | 0.0062375 | 2252.09 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 0.999389 | 1 | -0.00061082 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.808441 | 0.809025 | -0.000583387 | 4 | 0 | INFO:4 |
| flicker_excess_Y | 0.188223 | 0.192813 | -0.00458987 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.15878 | 0.158667 | 0.000113389 | 4 | 0 | PASS:4 |
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
| halo_overshoot_Y | 5.8936e-05 | 5.9195e-05 | -2.58987e-07 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.688874 | 0.689102 | -0.000227588 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.806581 | 0.806737 | -0.000156268 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.886127 | 0.886248 | -0.000121313 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 42.7255 | 42.7255 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.4456 | 42.4456 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 37.7404 | 37.7365 | 0.00391002 | 4 | 0 | PASS:4 |
| psnr_band_Y | 34.7522 | 34.6986 | 0.0536208 | 4 | 0 | PASS:4 |
| ringing_band_share | 0.00616185 | 0.00616185 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.975083 | 1 | -0.0249171 | 4 | 0 | INFO:4 |
| smear_Y | 0.606407 | 0.60715 | -0.000742801 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.964541 | 0.964541 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.962116 | 0.962116 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.91204 | 0.912014 | 2.63521e-05 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.539862 | 0.524168 | 0.0156937 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 1 | NA:1 PASS:3 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Y | 38.0927 | 38.085 | 0.00775189 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cb | 43.3014 | 43.3014 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cr | 42.9608 | 42.9608 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ssim_Y | 0.932123 | 0.932067 | 5.56955e-05 | same | PASS |  |
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
| sports50_60 | Bosphorus | native_x264_crf28 | flicker_excess_Y | 0.337086 | 0.343094 | -0.00600815 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | ghost_coeff_Y | 0.214664 | 0.214583 | 8.06969e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | smear_Y | 0.691544 | 0.692724 | -0.00117933 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | halo_overshoot_Y | 3.28547e-05 | 3.32458e-05 | -3.91087e-07 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s1_Y | 0.683357 | 0.683742 | -0.000385377 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s2_Y | 0.794737 | 0.794984 | -0.000246353 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s3_Y | 0.876462 | 0.876645 | -0.000182654 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_Y | 0.497489 | 0.480854 | 0.0166353 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_energy_ratio_Y | 0.95459 | 1 | -0.0454105 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_band_Y | 35.3706 | 35.2985 | 0.0720962 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_input_Y | 0.999494 | 1 | -0.000505863 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_master_Y | 0.944314 | 0.944792 | -0.000477935 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_ms_per_frame_pyref | 2329.33 | 0.00455 | 2329.33 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Y | 33.9917 | 33.9916 | 4.28681e-05 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cb | 39.9996 | 39.9996 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cr | 40.2577 | 40.2577 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ssim_Y | 0.846728 | 0.846727 | 1.08234e-06 | same | PASS |  |
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
| sports50_60 | ShakeNDry | native_x265_crf32 | flicker_excess_Y | 0.177916 | 0.177958 | -4.22283e-05 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | ghost_coeff_Y | 0.252882 | 0.252882 | -3.34953e-07 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | smear_Y | 0.31601 | 0.316016 | -6.1276e-06 | worse | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | halo_overshoot_Y | 0.000164813 | 0.000164828 | -1.51082e-08 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s1_Y | 0.587375 | 0.587378 | -3.36171e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s2_Y | 0.722597 | 0.7226 | -3.07986e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s3_Y | 0.814556 | 0.814558 | -2.17969e-06 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_energy_ratio_Y | 1.00013 | 1 | 0.00013319 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_band_share | 0.000888507 | 0.000888507 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_band_Y | 28.0034 | 28.0034 | -1.09151e-06 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_input_Y | 1 | 1 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_master_Y | 0.379369 | 0.379369 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_ms_per_frame_pyref | 2200.53 | 0.0042 | 2200.53 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Y | 38.2727 | 38.2716 | 0.00103411 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cb | 41.9464 | 41.9464 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cr | 41.7519 | 41.7519 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | ssim_Y | 0.922325 | 0.922317 | 8.69033e-06 | same | PASS |  |
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
| sports50_60 | Jockey | native_x265_crf32 | flicker_excess_Y | -0.00487494 | -0.00487077 | -4.173e-06 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | ghost_coeff_Y | 0.0154614 | 0.0154427 | 1.87403e-05 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | smear_Y | 0.611942 | 0.612221 | -0.000279313 | worse | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | halo_overshoot_Y | 1.39161e-05 | 1.40403e-05 | -1.24144e-07 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s1_Y | 0.646841 | 0.64693 | -8.95208e-05 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s2_Y | 0.806497 | 0.806567 | -7.052e-05 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s3_Y | 0.909011 | 0.909063 | -5.19748e-05 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | ringing_energy_ratio_Y | 0.994671 | 1 | -0.00532876 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | ringing_band_share | 0.010979 | 0.010979 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_band_Y | 36.2997 | 36.2829 | 0.0168337 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_input_Y | 0.999143 | 1 | -0.000857251 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_master_Y | 0.93945 | 0.940256 | -0.000806035 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | cost_ms_per_frame_pyref | 2288.8 | 0.00825 | 2288.8 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Y | 40.6046 | 40.5978 | 0.0068112 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cb | 45.6546 | 45.6546 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cr | 44.8121 | 44.8121 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ssim_Y | 0.946984 | 0.946944 | 3.99404e-05 | same | PASS |  |
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
| sports50_60 | Bosphorus | native_av1fgs_crf35 | flicker_excess_Y | 0.242766 | 0.255071 | -0.0123049 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ghost_coeff_Y | 0.152113 | 0.151758 | 0.000354455 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | smear_Y | 0.806132 | 0.807639 | -0.00150644 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | halo_overshoot_Y | 2.41607e-05 | 2.46664e-05 | -5.05607e-07 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s1_Y | 0.837924 | 0.838357 | -0.000432091 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s2_Y | 0.902493 | 0.902798 | -0.00030512 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s3_Y | 0.94448 | 0.944728 | -0.000248444 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_Y | 0.582235 | 0.567483 | 0.014752 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_energy_ratio_Y | 0.950938 | 1 | -0.0490625 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_band_Y | 39.3353 | 39.2097 | 0.125555 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_input_Y | 0.99892 | 1 | -0.00108016 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_master_Y | 0.970632 | 0.971682 | -0.00104958 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_ms_per_frame_pyref | 2189.71 | 0.00795 | 2189.7 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 37.8727 | 37.8682 | 0.00441584 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 45.0394 | 45.0394 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.8627 | 44.8627 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.941971 | 0.941934 | 3.62923e-05 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.984485 | 0.984485 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984175 | 0.984175 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.797744 | 0.801284 | -0.00354079 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | 1.02687 | 1.02844 | -0.00157732 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0341677 | 0.034083 | 8.46922e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.679178 | 0.679178 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | 0.770116 | 0.770116 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.120005 | 0.120005 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Y | 0.995581 | 1 | -0.00441889 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Y | 8.46922e-05 | 0 | 8.46922e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.0496693 | -0.0480265 | -0.00164279 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.0756414 | 0.0749679 | 0.000673519 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.731343 | 0.735623 | -0.00428026 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 0.000134452 | 0.000136667 | -2.21517e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.880517 | 0.882063 | -0.0015457 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.923817 | 0.924701 | -0.000884248 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.95225 | 0.952805 | -0.000555361 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.575145 | 0.575145 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_energy_ratio_Y | 0.949025 | 1 | -0.050975 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_band_share | 0.00776768 | 0.00776768 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_band_Y | 35.7514 | 35.7173 | 0.0340619 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_input_Y | 0.99704 | 1 | -0.00296004 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_master_Y | 0.881253 | 0.883869 | -0.00261629 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 2867.94 | 0.0081 | 2867.93 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Y | 52.0834 | 52.0852 | -0.00182468 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cb | 58.2207 | 58.2207 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cr | 56.7815 | 56.7815 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Y | 0.996568 | 0.996568 | -2.76142e-07 | same | PASS |  |
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
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Y | 0.00809072 | 0.00895573 | -0.000865011 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ghost_coeff_Y | 0.00242732 | 0.00252922 | -0.000101896 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | smear_Y | 0.953739 | 0.953912 | -0.000173302 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | halo_overshoot_Y | 1.18825e-05 | 1.21607e-05 | -2.78202e-07 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s1_Y | 0.758274 | 0.758452 | -0.000177287 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s2_Y | 0.837911 | 0.837889 | 2.1836e-05 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s3_Y | 0.909433 | 0.909369 | 6.38832e-05 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_energy_ratio_Y | 1.01986 | 1 | 0.019864 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_band_share | 0.000114282 | 0.000114282 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_band_Y | 47.9244 | 47.9986 | -0.0741161 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_input_Y | 1.0004 | 1 | 0.000395092 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_master_Y | 0.999953 | 0.999558 | 0.000394918 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_ms_per_frame_pyref | 2862.33 | 0.00385 | 2862.32 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc32_12f | psnr_Y | 38.9089 | 38.9044 | 0.00454522 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cb | 43.0743 | 43.0743 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cr | 43.138 | 43.138 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | ssim_Y | 0.937456 | 0.937418 | 3.72657e-05 | same | PASS |  |
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
| film24 | breathing | enc32_12f | flicker_excess_Y | -0.126107 | -0.126336 | 0.000229084 | same | PASS |  |
| film24 | breathing | enc32_12f | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | ghost_coeff_Y | 0.15478 | 0.154427 | 0.000352755 | same | PASS |  |
| film24 | breathing | enc32_12f | smear_Y | 0.742999 | 0.74558 | -0.00258043 | worse | PASS |  |
| film24 | breathing | enc32_12f | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | halo_overshoot_Y | 3.74851e-05 | 3.81693e-05 | -6.84198e-07 | same | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s1_Y | 0.78237 | 0.783035 | -0.000664059 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s2_Y | 0.876387 | 0.876919 | -0.000532132 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s3_Y | 0.929451 | 0.929884 | -0.000433386 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_Y | 0.564342 | 0.551506 | 0.0128368 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | ringing_energy_ratio_Y | 0.952493 | 1 | -0.047507 | NA | INFO |  |
| film24 | breathing | enc32_12f | ringing_band_share | 0.00688595 | 0.00688595 | 0 | NA | INFO |  |
| film24 | breathing | enc32_12f | psnr_band_Y | 37.0235 | 36.9593 | 0.0641985 | better | PASS |  |
| film24 | breathing | enc32_12f | edge_keep_vs_input_Y | 0.997071 | 1 | -0.00292877 | NA | INFO |  |
| film24 | breathing | enc32_12f | edge_keep_vs_master_Y | 0.94732 | 0.950102 | -0.00278263 | NA | INFO |  |
| film24 | breathing | enc32_12f | cost_ms_per_frame_pyref | 4675.56 | 0.0036 | 4675.56 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc32_12f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc32_12f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Y | 48.2786 | 48.277 | 0.00158728 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cb | 54.9583 | 54.9583 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cr | 55.9319 | 55.9319 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Y | 0.98935 | 0.989348 | 2.63668e-06 | same | PASS |  |
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
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Y | -0.0567687 | -0.0541334 | -0.00263532 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | ghost_coeff_Y | 0.0815418 | 0.0809106 | 0.000631206 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | smear_Y | 0.826609 | 0.831391 | -0.00478172 | worse | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | halo_overshoot_Y | 1.93971e-05 | 1.9809e-05 | -4.1191e-07 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s1_Y | 0.77902 | 0.779453 | -0.000432556 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s2_Y | 0.859795 | 0.860075 | -0.000279309 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s3_Y | 0.92139 | 0.921581 | -0.00019032 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_energy_ratio_Y | 0.997566 | 1 | -0.00243436 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_band_Y | 43.9149 | 43.832 | 0.0829168 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_input_Y | 0.997239 | 1 | -0.00276142 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_master_Y | 0.971006 | 0.973695 | -0.00268878 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_ms_per_frame_pyref | 2920.55 | 0.0037 | 2920.55 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Y | 34.1868 | 34.1794 | 0.00734176 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cb | 43.5265 | 43.5265 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cr | 43.1985 | 43.1985 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Y | 0.893006 | 0.892883 | 0.000122568 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cb | 0.981901 | 0.981901 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cr | 0.980403 | 0.980403 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Y | 0.550285 | 0.551906 | -0.00162128 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Y | 0.918248 | 0.919069 | -0.000821482 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Y | 0.112533 | 0.112462 | 7.13864e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Cb | 0.910858 | 0.910858 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Cb | 0.723284 | 0.723284 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Cb | 0.125645 | 0.125645 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Y | 0.997062 | 1 | -0.0029376 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Y | 7.13864e-05 | 0 | 7.13864e-05 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | flicker_excess_Y | 0.0431348 | 0.0445284 | -0.00139364 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ghost_coeff_Y | 0.16516 | 0.164726 | 0.000434358 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | smear_Y | 0.561095 | 0.564078 | -0.00298341 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | halo_overshoot_Y | 0.000292669 | 0.000295477 | -2.80874e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s1_Y | 0.76156 | 0.762763 | -0.0012031 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s2_Y | 0.836476 | 0.837214 | -0.00073873 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s3_Y | 0.898619 | 0.899117 | -0.000498656 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_Y | 0.328125 | 0.324219 | 0.00390625 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_energy_ratio_Y | 0.961557 | 1 | -0.038443 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_band_share | 0.00758383 | 0.00758383 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_band_Y | 31.7948 | 31.7704 | 0.024434 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_input_Y | 0.997213 | 1 | -0.00278707 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_master_Y | 0.736795 | 0.738854 | -0.00205924 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_ms_per_frame_pyref | 2651.48 | 0.00335 | 2651.47 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
