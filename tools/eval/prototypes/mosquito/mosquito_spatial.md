<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: mosquito_spatial vs repair_none (default)

Suite: `proto_mosquito` (9 cases). Candidate `mosquito_spatial` (mosquito repair, spatial only: zone bilateral of the structure layer, exact grain add-back, guard), default `repair_none`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

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
| cost_ms_per_frame_pyref | 1551.42 | 0.0037 | 1551.41 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| edge_keep_vs_input_Y | 1.00009 | 1 | 8.88529e-05 | 1 | 0 | INFO:1 |
| edge_keep_vs_master_Y | 0.973782 | 0.973695 | 8.65157e-05 | 1 | 0 | INFO:1 |
| flicker_excess_Y | -0.0541323 | -0.0541334 | 1.11749e-06 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.0808975 | 0.0809106 | -1.31519e-05 | 1 | 0 | PASS:1 |
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
| halo_overshoot_Y | 1.97781e-05 | 1.9809e-05 | -3.09644e-08 | 1 | 0 | PASS:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.779446 | 0.779453 | -6.99783e-06 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.860074 | 0.860075 | -6.58646e-07 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.921582 | 0.921581 | 1.01432e-06 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | 54.9583 | 54.9583 | 0 | 1 | 0 | PASS:1 |
| psnr_Cr | 55.9319 | 55.9319 | 0 | 1 | 0 | PASS:1 |
| psnr_Y | 48.2771 | 48.277 | 9.45811e-05 | 1 | 0 | PASS:1 |
| psnr_band_Y | 43.8365 | 43.832 | 0.00449631 | 1 | 0 | PASS:1 |
| ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | 1 | 0 | INFO:1 |
| ringing_energy_ratio_Y | 0.998023 | 1 | -0.0019773 | 1 | 0 | INFO:1 |
| smear_Y | 0.831408 | 0.831391 | 1.66031e-05 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.997972 | 0.997972 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.998419 | 0.998419 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 0.989348 | 0.989348 | 1.67346e-07 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |

### film24  (INFO 28, LIMIT 17, NA 51, PASS 96)

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
| cost_ms_per_frame_pyref | 1772.51 | 0.004725 | 1772.5 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 1.00001 | 1 | 1.02747e-05 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.893106 | 0.893096 | 1.01196e-05 | 4 | 0 | INFO:4 |
| flicker_excess_Y | -0.0302173 | -0.0302196 | 2.33723e-06 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0991595 | 0.0991625 | -2.92127e-06 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 0.769461 | 0.769461 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 0.919837 | 0.919837 | 7.50626e-07 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 0.999986 | 1 | -1.4464e-05 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.12298 | 0.12298 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.101601 | 0.101603 | -1.26195e-06 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -1.26195e-06 | 0 | -1.26195e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.653937 | 0.653937 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.513227 | 0.513238 | -1.03834e-05 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000120489 | 0.000120619 | -1.29491e-07 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.796551 | 0.796578 | -2.73843e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.869172 | 0.869181 | -8.30779e-06 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.922791 | 0.922794 | -2.80129e-06 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 47.4652 | 47.4652 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 46.9951 | 46.9951 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 40.7595 | 40.7593 | 0.000150468 | 4 | 0 | PASS:4 |
| psnr_band_Y | 38.1115 | 38.1114 | 0.000139083 | 4 | 0 | PASS:4 |
| ringing_band_share | 0.00558794 | 0.00558794 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.997146 | 1 | -0.00285435 | 4 | 0 | INFO:4 |
| smear_Y | 0.749776 | 0.749798 | -2.20174e-05 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.983663 | 0.983663 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.983634 | 0.983634 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.942203 | 0.942201 | 2.30697e-06 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.488865 | 0.483623 | 0.00524231 | 4 | 1 | LIMIT:3 NA:1 |
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
| cost_ms_per_frame_pyref | 1566.1 | 0.0062375 | 1566.09 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_keep_vs_input_Y | 1.00009 | 1 | 8.58679e-05 | 4 | 0 | INFO:4 |
| edge_keep_vs_master_Y | 0.809107 | 0.809025 | 8.25984e-05 | 4 | 0 | INFO:4 |
| flicker_excess_Y | 0.192812 | 0.192813 | -5.83619e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.158663 | 0.158667 | -3.2967e-06 | 4 | 0 | PASS:4 |
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
| halo_overshoot_Y | 5.91639e-05 | 5.9195e-05 | -3.11252e-08 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.689093 | 0.689102 | -8.70558e-06 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.806738 | 0.806737 | 7.32223e-07 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.886251 | 0.886248 | 2.71487e-06 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| psnr_Cb | 42.7255 | 42.7255 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 42.4456 | 42.4456 | 0 | 4 | 0 | PASS:4 |
| psnr_Y | 37.7366 | 37.7365 | 0.000123353 | 4 | 0 | PASS:4 |
| psnr_band_Y | 34.7007 | 34.6986 | 0.00207978 | 4 | 0 | PASS:4 |
| ringing_band_share | 0.00616185 | 0.00616185 | 0 | 4 | 0 | INFO:4 |
| ringing_energy_ratio_Y | 0.998887 | 1 | -0.00111286 | 4 | 0 | INFO:4 |
| smear_Y | 0.607173 | 0.60715 | 2.28504e-05 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.964541 | 0.964541 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.962116 | 0.962116 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.912015 | 0.912014 | 9.344e-07 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.532172 | 0.524168 | 0.00800377 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 1 | NA:1 PASS:3 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Y | 38.085 | 38.085 | 4.57075e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cb | 43.3014 | 43.3014 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_Cr | 42.9608 | 42.9608 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ssim_Y | 0.932068 | 0.932067 | 4.91433e-07 | same | PASS |  |
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
| sports50_60 | Bosphorus | native_x264_crf28 | flicker_excess_Y | 0.343113 | 0.343094 | 1.92157e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x264_crf28 | ghost_coeff_Y | 0.214582 | 0.214583 | -1.44245e-06 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | smear_Y | 0.692739 | 0.692724 | 1.49618e-05 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | halo_overshoot_Y | 3.32326e-05 | 3.32458e-05 | -1.32125e-08 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s1_Y | 0.683738 | 0.683742 | -3.79882e-06 | better | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s2_Y | 0.794985 | 0.794984 | 1.76159e-06 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_ratio_s3_Y | 0.876648 | 0.876645 | 2.94763e-06 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_Y | 0.492153 | 0.480854 | 0.0112994 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_energy_ratio_Y | 0.9995 | 1 | -0.00050042 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | psnr_band_Y | 35.2991 | 35.2985 | 0.000574076 | same | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_input_Y | 1.00003 | 1 | 2.53596e-05 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | edge_keep_vs_master_Y | 0.944816 | 0.944792 | 2.39596e-05 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_ms_per_frame_pyref | 1623.39 | 0.00455 | 1623.39 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x264_crf28 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Y | 33.9916 | 33.9916 | -5.93325e-07 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cb | 39.9996 | 39.9996 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_Cr | 40.2577 | 40.2577 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ssim_Y | 0.846727 | 0.846727 | -8.17619e-09 | same | PASS |  |
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
| sports50_60 | ShakeNDry | native_x265_crf32 | flicker_excess_Y | 0.177958 | 0.177958 | -3.10164e-07 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | ShakeNDry | native_x265_crf32 | ghost_coeff_Y | 0.252882 | 0.252882 | -1.36292e-08 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | smear_Y | 0.316016 | 0.316016 | 8.26758e-08 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | halo_overshoot_Y | 0.000164828 | 0.000164828 | -1.94915e-11 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s1_Y | 0.587378 | 0.587378 | -7.39112e-09 | better | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s2_Y | 0.7226 | 0.7226 | 5.42875e-09 | worse | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_ratio_s3_Y | 0.814558 | 0.814558 | 6.87953e-09 | worse | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_energy_ratio_Y | 1.00004 | 1 | 3.60416e-05 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | ringing_band_share | 0.000888507 | 0.000888507 | 0 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | psnr_band_Y | 28.0033 | 28.0034 | -9.06377e-06 | same | PASS |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_input_Y | 1 | 1 | 2.18997e-06 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | edge_keep_vs_master_Y | 0.37937 | 0.379369 | 8.30808e-07 | NA | INFO |  |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_ms_per_frame_pyref | 1552.14 | 0.0042 | 1552.14 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | ShakeNDry | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Y | 38.2717 | 38.2716 | 6.61969e-05 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cb | 41.9464 | 41.9464 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_Cr | 41.7519 | 41.7519 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | ssim_Y | 0.922317 | 0.922317 | 7.59461e-07 | same | PASS |  |
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
| sports50_60 | Jockey | native_x265_crf32 | flicker_excess_Y | -0.00487084 | -0.00487077 | -7.33951e-08 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x265_crf32 | ghost_coeff_Y | 0.0154417 | 0.0154427 | -1.03221e-06 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | smear_Y | 0.612229 | 0.612221 | 7.92256e-06 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | halo_overshoot_Y | 1.4026e-05 | 1.40403e-05 | -1.42988e-08 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s1_Y | 0.646924 | 0.64693 | -6.76796e-06 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s2_Y | 0.806566 | 0.806567 | -1.15331e-06 | better | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_ratio_s3_Y | 0.909063 | 0.909063 | 3.1493e-07 | worse | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x265_crf32 | ringing_energy_ratio_Y | 0.999613 | 1 | -0.000387458 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | ringing_band_share | 0.010979 | 0.010979 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | psnr_band_Y | 36.2842 | 36.2829 | 0.0013411 | same | PASS |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_input_Y | 1.00004 | 1 | 4.36664e-05 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | edge_keep_vs_master_Y | 0.940297 | 0.940256 | 4.10576e-05 | NA | INFO |  |
| sports50_60 | Jockey | native_x265_crf32 | cost_ms_per_frame_pyref | 1566.29 | 0.00825 | 1566.28 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x265_crf32 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x265_crf32 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Y | 40.5982 | 40.5978 | 0.000382101 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cb | 45.6546 | 45.6546 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_Cr | 44.8121 | 44.8121 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ssim_Y | 0.946946 | 0.946944 | 2.49488e-06 | same | PASS |  |
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
| sports50_60 | Bosphorus | native_av1fgs_crf35 | flicker_excess_Y | 0.25505 | 0.255071 | -2.11666e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ghost_coeff_Y | 0.151748 | 0.151758 | -1.06985e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | smear_Y | 0.807707 | 0.807639 | 6.84345e-05 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | halo_overshoot_Y | 2.45694e-05 | 2.46664e-05 | -9.697e-08 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s1_Y | 0.838332 | 0.838357 | -2.42481e-05 | better | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s2_Y | 0.902801 | 0.902798 | 2.31519e-06 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_ratio_s3_Y | 0.944736 | 0.944728 | 7.59006e-06 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_Y | 0.572191 | 0.567483 | 0.0047081 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_energy_ratio_Y | 0.9964 | 1 | -0.00359961 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | ringing_band_share | 0.00638994 | 0.00638994 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | psnr_band_Y | 39.2161 | 39.2097 | 0.00641299 | same | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_input_Y | 1.00027 | 1 | 0.000272256 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | edge_keep_vs_master_Y | 0.971946 | 0.971682 | 0.000264546 | NA | INFO |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_ms_per_frame_pyref | 1522.58 | 0.00795 | 1522.57 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 37.8687 | 37.8682 | 0.000466548 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 45.0394 | 45.0394 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.8627 | 44.8627 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.94194 | 0.941934 | 5.77219e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.984485 | 0.984485 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984175 | 0.984175 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.801254 | 0.801284 | -3.0855e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | 1.02843 | 1.02844 | -1.12393e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0340795 | 0.034083 | -3.53288e-06 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.679178 | 0.679178 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | 0.770116 | 0.770116 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.120005 | 0.120005 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Y | 0.999961 | 1 | -3.8507e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Y | -3.53288e-06 | 0 | -3.53288e-06 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.0480316 | -0.0480265 | -5.07377e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.0749788 | 0.0749679 | 1.09258e-05 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.735559 | 0.735623 | -6.44236e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 0.000136367 | 0.000136667 | -3.00024e-07 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.882013 | 0.882063 | -5.03314e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.924679 | 0.924701 | -2.15569e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.952793 | 0.952805 | -1.19918e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.578035 | 0.575145 | 0.00289017 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_energy_ratio_Y | 0.992026 | 1 | -0.00797431 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_band_share | 0.00776768 | 0.00776768 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_band_Y | 35.7191 | 35.7173 | 0.00174833 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_input_Y | 0.99999 | 1 | -9.69397e-06 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_master_Y | 0.88386 | 0.883869 | -8.5682e-06 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 1609.62 | 0.0081 | 1609.61 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Y | 52.0851 | 52.0852 | -6.27824e-05 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cb | 58.2207 | 58.2207 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cr | 56.7815 | 56.7815 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Y | 0.996568 | 0.996568 | -8.24477e-08 | same | PASS |  |
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
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Y | 0.0089592 | 0.00895573 | 3.47058e-06 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ghost_coeff_Y | 0.00250502 | 0.00252922 | -2.41975e-05 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | smear_Y | 0.953897 | 0.953912 | -1.48974e-05 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | halo_overshoot_Y | 1.20986e-05 | 1.21607e-05 | -6.20478e-08 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s1_Y | 0.758424 | 0.758452 | -2.72421e-05 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s2_Y | 0.837888 | 0.837889 | -1.01621e-06 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s3_Y | 0.909374 | 0.909369 | 5.17723e-06 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_energy_ratio_Y | 0.999948 | 1 | -5.21716e-05 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ringing_band_share | 0.000114282 | 0.000114282 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_band_Y | 47.996 | 47.9986 | -0.0025253 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_input_Y | 1.00002 | 1 | 1.76487e-05 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_keep_vs_master_Y | 0.999575 | 0.999558 | 1.76409e-05 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_ms_per_frame_pyref | 1578.18 | 0.00385 | 1578.18 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc32_12f | psnr_Y | 38.9044 | 38.9044 | 5.53284e-05 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cb | 43.0743 | 43.0743 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | psnr_Cr | 43.138 | 43.138 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | ssim_Y | 0.937419 | 0.937418 | 6.52201e-07 | same | PASS |  |
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
| film24 | breathing | enc32_12f | flicker_excess_Y | -0.126331 | -0.126336 | 5.0049e-06 | same | PASS |  |
| film24 | breathing | enc32_12f | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc32_12f | ghost_coeff_Y | 0.154425 | 0.154427 | -1.66596e-06 | same | PASS |  |
| film24 | breathing | enc32_12f | smear_Y | 0.745592 | 0.74558 | 1.17265e-05 | better | PASS |  |
| film24 | breathing | enc32_12f | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | halo_overshoot_Y | 3.81446e-05 | 3.81693e-05 | -2.47892e-08 | same | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s1_Y | 0.783029 | 0.783035 | -5.21853e-06 | better | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s2_Y | 0.876919 | 0.876919 | 6.22523e-07 | worse | PASS |  |
| film24 | breathing | enc32_12f | lc_ratio_s3_Y | 0.929886 | 0.929884 | 2.01315e-06 | worse | PASS |  |
| film24 | breathing | enc32_12f | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_Y | 0.564342 | 0.551506 | 0.0128368 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc32_12f | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc32_12f | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc32_12f | ringing_energy_ratio_Y | 0.998897 | 1 | -0.00110318 | NA | INFO |  |
| film24 | breathing | enc32_12f | ringing_band_share | 0.00688595 | 0.00688595 | 0 | NA | INFO |  |
| film24 | breathing | enc32_12f | psnr_band_Y | 36.9602 | 36.9593 | 0.000854334 | same | PASS |  |
| film24 | breathing | enc32_12f | edge_keep_vs_input_Y | 1.00003 | 1 | 3.27435e-05 | NA | INFO |  |
| film24 | breathing | enc32_12f | edge_keep_vs_master_Y | 0.950133 | 0.950102 | 3.11097e-05 | NA | INFO |  |
| film24 | breathing | enc32_12f | cost_ms_per_frame_pyref | 2350.39 | 0.0036 | 2350.38 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc32_12f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc32_12f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Y | 48.2771 | 48.277 | 9.45811e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cb | 54.9583 | 54.9583 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cr | 55.9319 | 55.9319 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Y | 0.989348 | 0.989348 | 1.67346e-07 | same | PASS |  |
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
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Y | -0.0541323 | -0.0541334 | 1.11749e-06 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | ghost_coeff_Y | 0.0808975 | 0.0809106 | -1.31519e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | smear_Y | 0.831408 | 0.831391 | 1.66031e-05 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | halo_overshoot_Y | 1.97781e-05 | 1.9809e-05 | -3.09644e-08 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s1_Y | 0.779446 | 0.779453 | -6.99783e-06 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s2_Y | 0.860074 | 0.860075 | -6.58646e-07 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s3_Y | 0.921582 | 0.921581 | 1.01432e-06 | worse | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_energy_ratio_Y | 0.998023 | 1 | -0.0019773 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ringing_band_share | 9.04401e-05 | 9.04401e-05 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_band_Y | 43.8365 | 43.832 | 0.00449631 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_input_Y | 1.00009 | 1 | 8.88529e-05 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_keep_vs_master_Y | 0.973782 | 0.973695 | 8.65157e-05 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_ms_per_frame_pyref | 1551.42 | 0.0037 | 1551.41 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Y | 34.1796 | 34.1794 | 0.00014278 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cb | 43.5265 | 43.5265 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cr | 43.1985 | 43.1985 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Y | 0.892886 | 0.892883 | 2.88592e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cb | 0.981901 | 0.981901 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cr | 0.980403 | 0.980403 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Y | 0.551896 | 0.551906 | -1.06787e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Y | 0.919084 | 0.919069 | 1.42418e-05 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Y | 0.112461 | 0.112462 | -1.51491e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Cb | 0.910858 | 0.910858 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Cb | 0.723284 | 0.723284 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Cb | 0.125645 | 0.125645 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Y | 0.999981 | 1 | -1.93488e-05 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Y | -1.51491e-06 | 0 | -1.51491e-06 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | flicker_excess_Y | 0.0445344 | 0.0445284 | 5.9472e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (12 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ghost_coeff_Y | 0.164729 | 0.164726 | 3.2526e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | smear_Y | 0.564058 | 0.564078 | -2.04749e-05 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | halo_overshoot_Y | 0.000295346 | 0.000295477 | -1.31103e-07 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s1_Y | 0.762736 | 0.762763 | -2.67454e-05 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s2_Y | 0.837203 | 0.837214 | -1.12805e-05 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s3_Y | 0.899111 | 0.899117 | -6.40374e-06 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_Y | 0.324219 | 0.324219 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_energy_ratio_Y | 0.997712 | 1 | -0.00228774 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_band_share | 0.00758383 | 0.00758383 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_band_Y | 31.7708 | 31.7704 | 0.000478964 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_input_Y | 1 | 1 | 4.00507e-07 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_master_Y | 0.738854 | 0.738854 | 2.95916e-07 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_ms_per_frame_pyref | 1551.83 | 0.00335 | 1551.83 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
