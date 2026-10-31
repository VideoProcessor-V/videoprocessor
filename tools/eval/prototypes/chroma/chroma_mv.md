<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: chroma_mv vs chroma_bicubic (default)

Suite: `proto_chroma` (9 cases). Candidate `chroma_mv` (MV temporal chroma: neighbour chroma samples along DIS flow (stand-in for VP vectors), validated per sample, confidence-mixed with residual JBU), default `chroma_bicubic`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### dark  (FLAG 2, INFO 4, LIMIT 3, NA 16, PASS 23)

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
| cost_ms_per_frame_pyref | 2463.14 | 13.8463 | 2449.3 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| edge_band_share | 0.0156498 | 0.0156498 | 0 | 1 | 0 | INFO:1 |
| flicker_excess_Cb | -0.0109307 | -0.00067741 | -0.0102533 | 1 | 0 | PASS:1 |
| flicker_excess_Cr | -0.00876439 | -0.00346717 | -0.00529721 | 1 | 0 | PASS:1 |
| flicker_excess_Y | -0.0575861 | -0.0575861 | 0 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.0560969 | 0.0560969 | 0 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 0.987228 | 1.92511 | -0.937878 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 0.934351 | 0.934351 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | NA | NA | NA | 1 | 1 | NA:1 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | -0.144223 | -0.00644587 | -0.137777 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Y | 0.0532603 | 0.0532603 | 0 | 1 | 0 | LIMIT:1 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 1 | 1 | NA:1 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 2.16794 | 2.26495 | -0.0970118 | 1 | 0 | LIMIT:1 |
| grain_rms_ratio_Y | 0.58004 | 0.58004 | 0 | 1 | 0 | LIMIT:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 2.51987e-05 | 2.51987e-05 | 0 | 1 | 0 | PASS:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.850117 | 0.850117 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.905164 | 0.905164 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.946763 | 0.946763 | 0 | 1 | 0 | PASS:1 |
| lpips | 0.0178215 | 0.0177066 | 0.000114905 | 1 | 0 | PASS:1 |
| psnr_Cb | 55.3589 | 55.3471 | 0.011769 | 1 | 0 | PASS:1 |
| psnr_Cr | 56.6623 | 56.6386 | 0.0236554 | 1 | 0 | PASS:1 |
| psnr_Y | 50.3844 | 50.3844 | 0 | 1 | 0 | PASS:1 |
| psnr_edge_Cb | 50.7048 | 51.8664 | -1.16156 | 1 | 0 | FLAG:1 |
| psnr_edge_Cr | 51.7979 | 52.7353 | -0.937432 | 1 | 0 | FLAG:1 |
| smear_Y | 0.930265 | 0.930265 | 0 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.998192 | 0.998195 | -2.96835e-06 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.998732 | 0.998729 | 2.63086e-06 | 1 | 0 | PASS:1 |
| ssim_Y | 0.992657 | 0.992657 | 0 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |

### film24  (FLAG 15, INFO 16, LIMIT 16, NA 52, PASS 93)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_Y | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_C | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 2499.87 | 16.9608 | 2482.91 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_band_share | 0.270395 | 0.270395 | 0 | 4 | 0 | INFO:4 |
| flicker_excess_Cb | -0.216608 | -0.0851996 | -0.131409 | 4 | 0 | PASS:4 |
| flicker_excess_Cr | -0.24258 | -0.115712 | -0.126867 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.0213181 | -0.0213181 | 0 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0290726 | 0.0290726 | 0 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 0.533171 | 0.538947 | -0.0057766 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 0.944298 | 0.944298 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.217881 | 0.213179 | 0.00470234 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.0506189 | 0.0506189 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.690615 | 0.69641 | -0.00579504 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.77166 | 0.77166 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 6.06898e-05 | 6.06898e-05 | 0 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.931949 | 0.931949 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.957486 | 0.957486 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.976102 | 0.976102 | 0 | 4 | 0 | PASS:4 |
| lpips | 0.0166447 | 0.016589 | 5.56772e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 50.2988 | 50.9334 | -0.63455 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_Cr | 49.7716 | 50.8032 | -1.03167 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_Y | 45.411 | 45.411 | 0 | 4 | 0 | PASS:4 |
| psnr_edge_Cb | 46.1022 | 48.1908 | -2.08857 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_edge_Cr | 45.4768 | 48.2399 | -2.76305 | 4 | 0 | FLAG:4 |
| smear_Y | 0.954204 | 0.954204 | 0 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.9894 | 0.989472 | -7.20127e-05 | 4 | 0 | FLAG:1 PASS:3 |
| ssim_Cr | 0.989778 | 0.990149 | -0.000370854 | 4 | 0 | FLAG:1 PASS:3 |
| ssim_Y | 0.984167 | 0.984167 | 0 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.668149 | 0.668149 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |

### sports50_60  (FAIL 1, FLAG 7, INFO 16, LIMIT 16, NA 60, PASS 92)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_Y | 0 | 0 | 0 | 4 | 3 | LIMIT:1 NA:3 |
| btb_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 4 | 3 | NA:3 PASS:1 |
| clip_detect_C | NA | NA | NA | 4 | 4 | NA:4 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 1 | NA:1 PASS:3 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 2612.39 | 17.7695 | 2594.62 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_band_share | 0.281572 | 0.281572 | 0 | 4 | 0 | INFO:4 |
| flicker_excess_Cb | -0.0947049 | -0.103 | 0.008295 | 4 | 0 | FLAG:1 PASS:3 |
| flicker_excess_Cr | -0.163234 | -0.151316 | -0.011918 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00672003 | -0.00672003 | 0 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.051668 | 0.051668 | 0 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Y | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.147693 | 0.117754 | 0.0299387 | 4 | 0 | FAIL:1 LIMIT:2 PASS:1 |
| grain_lag1_delta_Y | 0.0927216 | 0.0927216 | 0 | 4 | 0 | LIMIT:2 PASS:2 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.138756 | 0.144513 | -0.00575692 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.524696 | 0.524696 | 0 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 7.68252e-05 | 7.68252e-05 | 0 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.886513 | 0.886513 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.935174 | 0.935174 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.963139 | 0.963139 | 0 | 4 | 0 | PASS:4 |
| lpips | 0.0444195 | 0.0444811 | -6.15782e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 41.7631 | 41.7176 | 0.0455176 | 4 | 0 | FLAG:1 PASS:3 |
| psnr_Cr | 41.8062 | 41.9466 | -0.140313 | 4 | 0 | FLAG:2 PASS:2 |
| psnr_Y | 40.869 | 40.869 | 0 | 4 | 0 | PASS:4 |
| psnr_edge_Cb | 38.9935 | 39.0803 | -0.086824 | 4 | 0 | FLAG:1 PASS:3 |
| psnr_edge_Cr | 39.1143 | 39.5631 | -0.448833 | 4 | 0 | FLAG:2 PASS:2 |
| smear_Y | 0.914938 | 0.914938 | 0 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.947209 | 0.9425 | 0.00470869 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.945269 | 0.944356 | 0.000913465 | 4 | 0 | PASS:4 |
| ssim_Y | 0.966414 | 0.966414 | 0 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.5713 | 0.5713 | 0 | 4 | 1 | LIMIT:3 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Y | 41.0019 | 41.0019 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cb | 43.9712 | 43.9154 | 0.0558134 | better | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cr | 43.2249 | 43.5937 | -0.368857 | worse | FLAG |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Y | 0.959528 | 0.959528 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cb | 0.974644 | 0.973853 | 0.000791461 | better | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cr | 0.973418 | 0.973357 | 6.02605e-05 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lpips | 0.0458301 | 0.0464757 | -0.000645584 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Y | 0.506753 | 0.506753 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Y | 0.215182 | 0.215182 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Cb | 0.116843 | 0.124143 | -0.00730026 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Cb | 0.333387 | 0.259766 | 0.0736207 | worse | FAIL | default also fails and the candidate is worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Y | 0.0760045 | 0.0760045 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x265_crf26 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Bosphorus | native_x265_crf26 | ghost_coeff_Y | 0.143044 | 0.143044 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | smear_Y | 0.882012 | 0.882012 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | halo_overshoot_Y | 8.10893e-05 | 8.10893e-05 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s1_Y | 0.860571 | 0.860571 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s2_Y | 0.915079 | 0.915079 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s3_Y | 0.948825 | 0.948825 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_Y | 0.527821 | 0.527821 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x265_crf26 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_edge_Cb | 41.0344 | 41.0159 | 0.0185592 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Cb | -0.0968312 | -0.0857659 | -0.0110653 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_edge_Cr | 39.1921 | 40.3065 | -1.11436 | worse | FLAG |  |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Cr | -0.149136 | -0.142884 | -0.00625221 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | edge_band_share | 0.156062 | 0.156062 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_ms_per_frame_pyref | 2638.23 | 16.7173 | 2621.51 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Y | 42.0854 | 42.0854 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cb | 43.0746 | 43.1536 | -0.0790511 | worse | FLAG |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cr | 42.8473 | 42.7496 | 0.0977915 | better | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Y | 0.964841 | 0.964841 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cb | 0.967127 | 0.966972 | 0.000155835 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cr | 0.962057 | 0.961442 | 0.000615167 | better | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lpips | 0.100938 | 0.1013 | -0.00036182 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Y | 0.542623 | 0.542623 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Y | -0.00351741 | -0.00351741 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Cb | 0.191934 | 0.203272 | -0.011338 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Cb | -0.00241115 | -0.00701917 | 0.00460802 | worse | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Y | 0.00922671 | 0.00922671 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x264_crf23 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | Jockey | native_x264_crf23 | ghost_coeff_Y | 0.00504856 | 0.00504856 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | smear_Y | 0.92589 | 0.92589 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | halo_overshoot_Y | 3.18976e-05 | 3.18976e-05 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s1_Y | 0.851052 | 0.851052 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s2_Y | 0.932868 | 0.932868 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s3_Y | 0.973031 | 0.973031 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | Jockey | native_x264_crf23 | psnr_edge_Cb | 40.3726 | 40.8862 | -0.513593 | worse | FLAG |  |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Cb | 0.00813535 | 0.00282487 | 0.00531048 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_edge_Cr | 41.2729 | 41.0336 | 0.239359 | better | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Cr | -0.026409 | -0.0258829 | -0.000526109 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | edge_band_share | 0.133898 | 0.133898 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | cost_ms_per_frame_pyref | 2648.83 | 17.4326 | 2631.4 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x264_crf23 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x264_crf23 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Y | 44.4982 | 44.4982 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Cb | 45.4025 | 45.3728 | 0.0297599 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Cr | 44.1372 | 44.4217 | -0.284491 | worse | FLAG |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Y | 0.983093 | 0.983093 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Cb | 0.980451 | 0.979763 | 0.000687708 | better | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Cr | 0.975523 | 0.97538 | 0.000143259 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lpips | 0.0125633 | 0.0131008 | -0.000537499 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_rms_ratio_Y | 0.157251 | 0.157251 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_delta_Y | 0.157801 | 0.157801 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_rms_ratio_Cb | 0.0673434 | 0.0720717 | -0.00472831 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_delta_Cb | 0.192653 | 0.156404 | 0.0362489 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | YachtRide | native_x265_crf20 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | YachtRide | native_x265_crf20 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | YachtRide | native_x265_crf20 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | YachtRide | native_x265_crf20 | flicker_excess_Y | -0.0205148 | -0.0205148 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | YachtRide | native_x265_crf20 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | YachtRide | native_x265_crf20 | ghost_coeff_Y | 0.0300594 | 0.0300594 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | smear_Y | 0.944454 | 0.944454 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | halo_overshoot_Y | 6.70254e-05 | 6.70254e-05 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lc_ratio_s1_Y | 0.899376 | 0.899376 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lc_ratio_s2_Y | 0.937185 | 0.937185 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lc_ratio_s3_Y | 0.961791 | 0.961791 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | YachtRide | native_x265_crf20 | wtw_kept_Y | 0.576987 | 0.576987 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | YachtRide | native_x265_crf20 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | YachtRide | native_x265_crf20 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_edge_Cb | 41.2337 | 41.2737 | -0.0400505 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | flicker_excess_Cb | -0.130854 | -0.117444 | -0.0134098 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_edge_Cr | 40.0723 | 40.9725 | -0.900187 | worse | FLAG |  |
| sports50_60 | YachtRide | native_x265_crf20 | flicker_excess_Cr | -0.087181 | -0.042726 | -0.044455 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | edge_band_share | 0.161606 | 0.161606 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | cost_ms_per_frame_pyref | 2563.34 | 16.7209 | 2546.62 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | YachtRide | native_x265_crf20 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | YachtRide | native_x265_crf20 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Y | 35.8906 | 35.8906 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cb | 34.6042 | 34.4287 | 0.175548 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cr | 37.0156 | 37.0213 | -0.00569437 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Y | 0.958194 | 0.958194 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cb | 0.866614 | 0.849414 | 0.0171998 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cr | 0.87008 | 0.867244 | 0.00283517 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lpips | 0.0183466 | 0.017048 | 0.00129859 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Y | 0.892158 | 0.892158 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Y | 0.00142003 | 0.00142003 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Cb | 0.178903 | 0.178564 | 0.00033887 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Cb | 0.067143 | 0.061866 | 0.00527698 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Y | -0.0915965 | -0.0915965 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | park_joy | native_x265_crf26 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 250 longer than half the window (4 frames) |
| sports50_60 | park_joy | native_x265_crf26 | ghost_coeff_Y | 0.0285201 | 0.0285201 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | smear_Y | 0.907398 | 0.907398 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | halo_overshoot_Y | 0.000127289 | 0.000127289 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s1_Y | 0.935053 | 0.935053 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s2_Y | 0.955565 | 0.955565 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s3_Y | 0.968907 | 0.968907 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_Y | 0.609091 | 0.609091 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | park_joy | native_x265_crf26 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | park_joy | native_x265_crf26 | psnr_edge_Cb | 33.3331 | 33.1453 | 0.187789 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Cb | -0.15927 | -0.211614 | 0.0523446 | worse | FLAG |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_edge_Cr | 35.9197 | 35.9398 | -0.0201443 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Cr | -0.390209 | -0.39377 | 0.00356143 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | edge_band_share | 0.674721 | 0.674721 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | cost_ms_per_frame_pyref | 2599.15 | 20.2072 | 2578.95 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | park_joy | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | park_joy | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 39.132 | 39.132 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 44.7413 | 44.6618 | 0.0794976 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 45.075 | 45.1137 | -0.0387036 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.961872 | 0.961872 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.982867 | 0.982645 | 0.000222731 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984958 | 0.985053 | -9.45449e-05 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | 0.0394329 | 0.0400135 | -0.000580572 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.85289 | 0.85289 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | 0.952946 | 0.952946 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0534701 | 0.0534701 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.551596 | 0.54944 | 0.00215637 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | 0.365686 | 0.348264 | 0.0174225 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.33194 | 0.322787 | 0.00915252 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.0511641 | -0.0511641 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.0553219 | 0.0553219 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.87703 | 0.87703 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 0.00016182 | 0.00016182 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.9273 | 0.9273 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.951077 | 0.951077 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.968673 | 0.968673 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.773504 | 0.773504 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_edge_Cb | 43.6843 | 43.6176 | 0.066698 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Cb | -0.49982 | -0.33667 | -0.163151 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_edge_Cr | 44.1326 | 44.3017 | -0.169161 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Cr | -0.551414 | -0.479652 | -0.0717625 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_band_share | 0.43306 | 0.43306 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 2710.54 | 15.8502 | 2694.69 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Cb | 53.4421 | 54.7967 | -1.3546 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Cr | 52.9287 | 55.5848 | -2.65604 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Cb | 0.996091 | 0.996807 | -0.000715545 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Cr | 0.996544 | 0.997518 | -0.0009745 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lpips | 0.000787209 | 0.000124715 | 0.000662493 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_hf_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_rms_ratio_Cb | 0.683836 | 0.681802 | 0.00203429 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_hf_ratio_Cb | 0.641108 | 0.666408 | -0.0252996 | worse | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_delta_Cb | 0.145428 | 0.11806 | 0.0273686 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos4k | ffv1_master_no_codec_w2400 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | flicker_excess_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ghost_coeff_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | smear_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | halo_overshoot_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lc_ratio_s1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lc_ratio_s2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lc_ratio_s3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | ffv1_master_no_codec_w2400 | wtw_kept_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | ffv1_master_no_codec_w2400 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | ffv1_master_no_codec_w2400 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_edge_Cb | 51.2284 | 52.8594 | -1.631 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | flicker_excess_Cb | -0.325114 | -0.000917751 | -0.324196 | better | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_edge_Cr | 50.4978 | 53.9324 | -3.43457 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | flicker_excess_Cr | -0.345574 | 0.000130259 | -0.345704 | better | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | edge_band_share | 0.434521 | 0.434521 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_ms_per_frame_pyref | 2814.13 | 17.7011 | 2796.43 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Y | 53.2548 | 53.2548 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cb | 57.4122 | 58.6046 | -1.19241 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cr | 56.0017 | 57.0807 | -1.07895 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Y | 0.997368 | 0.997368 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cb | 0.998827 | 0.999074 | -0.00024751 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cr | 0.998467 | 0.998756 | -0.000289167 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lpips | 0.00446027 | 0.00429423 | 0.000166043 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Y | 0.389306 | 0.389306 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Y | 0.924005 | 0.924005 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Y | 0.0880063 | 0.0880063 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Cb | 1.26586 | 1.28624 | -0.0203797 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Cb | 0.799356 | 0.793942 | 0.00541442 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Cb | -0.0115789 | 0.0298279 | -0.0414069 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Y | 0.0213057 | 0.0213057 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (8 frames) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ghost_coeff_Y | 0.000445088 | 0.000445088 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | smear_Y | 0.993363 | 0.993363 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | halo_overshoot_Y | 2.89153e-05 | 2.89153e-05 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s1_Y | 0.877822 | 0.877822 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s2_Y | 0.923059 | 0.923059 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_ratio_s3_Y | 0.9602 | 0.9602 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_Y | 0.307692 | 0.307692 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_edge_Cb | 46.0666 | 52.3701 | -6.30358 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Cb | 0.0719274 | 0.103162 | -0.0312343 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_edge_Cr | 44.9082 | 51.1054 | -6.19723 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Cr | 0.104641 | 0.186639 | -0.0819981 | better | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_band_share | 0.0239045 | 0.0239045 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_ms_per_frame_pyref | 2636.66 | 21.1154 | 2615.54 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc24_12f | psnr_Y | 43.8462 | 43.8462 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | psnr_Cb | 45.5996 | 45.6703 | -0.0706822 | worse | FLAG |  |
| film24 | breathing | enc24_12f | psnr_Cr | 45.0808 | 45.4337 | -0.352973 | worse | FLAG |  |
| film24 | breathing | enc24_12f | ssim_Y | 0.977426 | 0.977426 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | ssim_Cb | 0.979814 | 0.979362 | 0.000452273 | same | PASS |  |
| film24 | breathing | enc24_12f | ssim_Cr | 0.979144 | 0.979269 | -0.000125202 | same | PASS |  |
| film24 | breathing | enc24_12f | lpips | 0.0218985 | 0.0219237 | -2.52554e-05 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | grain_rms_ratio_Y | 0.844445 | 0.844445 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_hf_ratio_Y | 0.900241 | 0.900241 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_lag1_delta_Y | 0.0609991 | 0.0609991 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | grain_rms_ratio_Cb | 0.261168 | 0.268159 | -0.00699114 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_hf_ratio_Cb | 0.326532 | 0.347176 | -0.0206437 | worse | PASS |  |
| film24 | breathing | enc24_12f | grain_lag1_delta_Cb | 0.405736 | 0.382041 | 0.0236951 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | breathing | enc24_12f | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | breathing | enc24_12f | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | breathing | enc24_12f | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | breathing | enc24_12f | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | breathing | enc24_12f | flicker_excess_Y | -0.0554141 | -0.0554141 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc24_12f | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 24 longer than half the window (12 frames) |
| film24 | breathing | enc24_12f | ghost_coeff_Y | 0.0605233 | 0.0605233 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | smear_Y | 0.946423 | 0.946423 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | halo_overshoot_Y | 5.20243e-05 | 5.20243e-05 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | lc_ratio_s1_Y | 0.922675 | 0.922675 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | lc_ratio_s2_Y | 0.95581 | 0.95581 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | lc_ratio_s3_Y | 0.975537 | 0.975537 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc24_12f | wtw_kept_Y | 0.591401 | 0.591401 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc24_12f | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc24_12f | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | breathing | enc24_12f | psnr_edge_Cb | 43.4297 | 43.9161 | -0.486404 | worse | FLAG |  |
| film24 | breathing | enc24_12f | flicker_excess_Cb | -0.113426 | -0.106372 | -0.00705356 | same | PASS |  |
| film24 | breathing | enc24_12f | psnr_edge_Cr | 42.3686 | 43.6199 | -1.25126 | worse | FLAG |  |
| film24 | breathing | enc24_12f | flicker_excess_Cr | -0.177971 | -0.169967 | -0.0080045 | same | PASS |  |
| film24 | breathing | enc24_12f | edge_band_share | 0.190094 | 0.190094 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | cost_ms_per_frame_pyref | 1838.16 | 13.1766 | 1824.98 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc24_12f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc24_12f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Y | 50.3844 | 50.3844 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cb | 55.3589 | 55.3471 | 0.011769 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cr | 56.6623 | 56.6386 | 0.0236554 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Y | 0.992657 | 0.992657 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cb | 0.998192 | 0.998195 | -2.96835e-06 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cr | 0.998732 | 0.998729 | 2.63086e-06 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lpips | 0.0178215 | 0.0177066 | 0.000114905 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Y | 0.58004 | 0.58004 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Y | 0.934351 | 0.934351 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Y | 0.0532603 | 0.0532603 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Cb | 2.16794 | 2.26495 | -0.0970118 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Cb | 0.987228 | 1.92511 | -0.937878 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Cb | -0.144223 | -0.00644587 | -0.137777 | better | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Y | -0.0575861 | -0.0575861 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 120 longer than half the window (8 frames) |
| dark | meridian4k | x265_2160p_6000k_w1800 | ghost_coeff_Y | 0.0560969 | 0.0560969 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | smear_Y | 0.930265 | 0.930265 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | halo_overshoot_Y | 2.51987e-05 | 2.51987e-05 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s1_Y | 0.850117 | 0.850117 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s2_Y | 0.905164 | 0.905164 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_ratio_s3_Y | 0.946763 | 0.946763 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_edge_Cb | 50.7048 | 51.8664 | -1.16156 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Cb | -0.0109307 | -0.00067741 | -0.0102533 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_edge_Cr | 51.7979 | 52.7353 | -0.937432 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Cr | -0.00876439 | -0.00346717 | -0.00529721 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_band_share | 0.0156498 | 0.0156498 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_ms_per_frame_pyref | 2463.14 | 13.8463 | 2449.3 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
