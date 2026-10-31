<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: mosquito_spatial vs repair_none (default)

Suite: `proto_mosquito_edgegrain` (4 cases). Candidate `mosquito_spatial` (mosquito repair, spatial only: zone bilateral of the structure layer, exact grain add-back, guard), default `repair_none`. LPIPS not requested (NA). Cost: omitted.

## Per-content-type summary

### film24  (INFO 12, LIMIT 9, NA 24, PASS 53)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| edge_keep_vs_input_Y | 0.999988 | 1 | -1.20589e-05 | 2 | 0 | INFO:2 |
| edge_keep_vs_master_Y | 0.808855 | 0.808865 | -1.00061e-05 | 2 | 0 | INFO:2 |
| flicker_excess_Y | -0.223803 | -0.223808 | 4.6009e-06 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.119862 | 0.119855 | 7.57458e-06 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Y | NA | NA | NA | 2 | 2 | NA:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 0.999975 | 1 | -2.47948e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.0987671 | 0.0987671 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.0497277 | 0.0497276 | 1.36951e-07 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 1.36951e-07 | 0 | 1.36951e-07 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 0.994679 | 1 | -0.00532117 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 0.999729 | 1 | -0.000271405 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | -0.000487369 | 0 | -0.000487369 | 2 | 0 | PASS:2 |
| grain_probe_lag1_flat_vs_input_Y | 2.82563e-06 | 0 | 2.82563e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.698995 | 0.698995 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.716928 | 0.716947 | -1.91809e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000190442 | 0.000190649 | -2.06548e-07 | 2 | 0 | PASS:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.813198 | 0.813237 | -3.96656e-05 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.875317 | 0.875335 | -1.73755e-05 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.922312 | 0.922322 | -9.85385e-06 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_Cb | 44.0364 | 44.0364 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 43.8249 | 43.8249 | 0 | 2 | 0 | PASS:2 |
| psnr_Y | 35.8292 | 35.8289 | 0.000292014 | 2 | 0 | PASS:2 |
| psnr_band_Y | 33.617 | 33.6159 | 0.0010643 | 2 | 0 | PASS:2 |
| ringing_band_share | 0.00780667 | 0.00780667 | 0 | 2 | 0 | INFO:2 |
| ringing_energy_ratio_Y | 0.994727 | 1 | -0.00527297 | 2 | 0 | INFO:2 |
| smear_Y | 0.638582 | 0.638628 | -4.58568e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.982062 | 0.982062 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.98103 | 0.98103 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.913397 | 0.913392 | 4.20394e-06 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0.448864 | 0.448864 | 0 | 2 | 0 | LIMIT:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

### sports50_60  (INFO 12, LIMIT 8, NA 24, PASS 54)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| edge_keep_vs_input_Y | 1.00015 | 1 | 0.000148808 | 2 | 0 | INFO:2 |
| edge_keep_vs_master_Y | 0.958381 | 0.958237 | 0.000144253 | 2 | 0 | INFO:2 |
| flicker_excess_Y | 0.299082 | 0.299082 | -9.75459e-07 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.183165 | 0.183171 | -6.07047e-06 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Y | NA | NA | NA | 2 | 2 | NA:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.0828073 | 0.0828073 | 0 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_delta_Y | 0.109494 | 0.109494 | 0 | 2 | 0 | LIMIT:1 PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 0.99397 | 1 | -0.00603018 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 0.99996 | 1 | -4.01877e-05 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | 0.000115586 | 0 | 0.000115586 | 2 | 0 | PASS:2 |
| grain_probe_lag1_flat_vs_input_Y | 4.37715e-07 | 0 | 4.37715e-07 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.430402 | 0.430402 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.41437 | 0.41437 | 0 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 2.8901e-05 | 2.89561e-05 | -5.50913e-08 | 2 | 0 | PASS:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.761035 | 0.761049 | -1.40235e-05 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.848893 | 0.848891 | 2.03839e-06 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.910692 | 0.910686 | 5.26884e-06 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_Cb | 44.478 | 44.478 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 43.8865 | 43.8865 | 0 | 2 | 0 | PASS:2 |
| psnr_Y | 39.3416 | 39.3414 | 0.000213904 | 2 | 0 | PASS:2 |
| psnr_band_Y | 37.2576 | 37.2541 | 0.00349353 | 2 | 0 | PASS:2 |
| ringing_band_share | 0.00638994 | 0.00638994 | 0 | 2 | 0 | INFO:2 |
| ringing_energy_ratio_Y | 0.99795 | 1 | -0.00205002 | 2 | 0 | INFO:2 |
| smear_Y | 0.750223 | 0.750181 | 4.16982e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.974476 | 0.974476 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.972669 | 0.972669 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.939507 | 0.939505 | 1.49316e-06 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0.532172 | 0.524168 | 0.00800377 | 2 | 0 | LIMIT:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

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
| sports50_60 | Bosphorus | native_x264_crf28 | grain_probe_keep_edge_vs_input_Y | 0.994672 | 1 | -0.00532812 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_probe_lag1_edge_vs_input_Y | 0.000201016 | 0 | 0.000201016 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_probe_keep_flat_vs_input_Y | 0.999953 | 1 | -4.74063e-05 | worse | PASS |  |
| sports50_60 | Bosphorus | native_x264_crf28 | grain_probe_lag1_flat_vs_input_Y | -2.81794e-07 | 0 | -2.81794e-07 | better | PASS |  |
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
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_probe_keep_edge_vs_input_Y | 0.993268 | 1 | -0.00673225 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_probe_lag1_edge_vs_input_Y | 3.01554e-05 | 0 | 3.01554e-05 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_probe_keep_flat_vs_input_Y | 0.999967 | 1 | -3.29691e-05 | worse | PASS |  |
| sports50_60 | Bosphorus | native_av1fgs_crf35 | grain_probe_lag1_flat_vs_input_Y | 1.15722e-06 | 0 | 1.15722e-06 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 37.7131 | 37.7127 | 0.000450295 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 44.8414 | 44.8414 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.788 | 44.788 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.939924 | 0.939918 | 5.7841e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.983823 | 0.983823 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.983633 | 0.983633 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.823371 | 0.823402 | -3.12786e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0279666 | 0.0279679 | -1.34628e-06 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.628659 | 0.628659 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.0755582 | 0.0755582 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Y | 0.999962 | 1 | -3.7987e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Y | -1.34628e-06 | 0 | -1.34628e-06 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.104278 | -0.104273 | -4.40829e-06 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (6 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (6 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.074812 | 0.0748002 | 1.17871e-05 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.72584 | 0.72591 | -6.98944e-05 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 0.000125009 | 0.000125298 | -2.89331e-07 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.875939 | 0.875992 | -5.25093e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.920753 | 0.920775 | -2.29123e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.950349 | 0.950362 | -1.28377e-05 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.556818 | 0.556818 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_energy_ratio_Y | 0.991672 | 1 | -0.00832795 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ringing_band_share | 0.00778813 | 0.00778813 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_band_Y | 35.5798 | 35.5782 | 0.00167532 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_input_Y | 0.999984 | 1 | -1.56371e-05 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_keep_vs_master_Y | 0.879315 | 0.879329 | -1.37502e-05 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_probe_keep_edge_vs_input_Y | 0.994163 | 1 | -0.00583678 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_probe_lag1_edge_vs_input_Y | -0.000245358 | 0 | -0.000245358 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_probe_keep_flat_vs_input_Y | 0.999668 | 1 | -0.000331783 | worse | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_probe_lag1_flat_vs_input_Y | 4.79715e-06 | 0 | 4.79715e-06 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Y | 33.9453 | 33.9452 | 0.000133733 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cb | 43.2314 | 43.2314 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_Cr | 42.8618 | 42.8618 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Y | 0.886869 | 0.886866 | 2.62378e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cb | 0.980302 | 0.980302 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ssim_Cr | 0.978427 | 0.978427 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Y | 0.610486 | 0.610493 | -7.08327e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Y | 0.0714889 | 0.0714873 | 1.62018e-06 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_rms_ratio_Cb | 0.76933 | 0.76933 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_delta_Cb | 0.121976 | 0.121976 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Y | 0.999988 | 1 | -1.16025e-05 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Y | 1.62018e-06 | 0 | 1.62018e-06 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | flicker_excess_Y | -0.343329 | -0.343343 | 1.36101e-05 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (6 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (6 frames) |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ghost_coeff_Y | 0.164913 | 0.16491 | 3.36208e-06 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | smear_Y | 0.551324 | 0.551346 | -2.18193e-05 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | halo_overshoot_Y | 0.000255876 | 0.000255999 | -1.23764e-07 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s1_Y | 0.750456 | 0.750483 | -2.68219e-05 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s2_Y | 0.829882 | 0.829894 | -1.18386e-05 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_ratio_s3_Y | 0.894276 | 0.894283 | -6.87e-06 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_Y | 0.340909 | 0.340909 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_energy_ratio_Y | 0.997782 | 1 | -0.00221799 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | ringing_band_share | 0.00782522 | 0.00782522 | 0 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | psnr_band_Y | 31.6541 | 31.6537 | 0.000453289 | same | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_input_Y | 0.999992 | 1 | -8.48059e-06 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | edge_keep_vs_master_Y | 0.738395 | 0.738401 | -6.26207e-06 | NA | INFO |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_probe_keep_edge_vs_input_Y | 0.995194 | 1 | -0.00480556 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_probe_lag1_edge_vs_input_Y | -0.00072938 | 0 | -0.00072938 | better | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_probe_keep_flat_vs_input_Y | 0.999789 | 1 | -0.000211028 | worse | PASS |  |
| film24 | tos4k | stress_x264_crf36_crop1024_w2400 | grain_probe_lag1_flat_vs_input_Y | 8.54117e-07 | 0 | 8.54117e-07 | worse | PASS |  |
