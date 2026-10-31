<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: chroma_jbu_res vs chroma_bicubic (default)

Suite: `proto_chroma` (9 cases). Candidate `chroma_jbu_res` (residual JBU: Catmull-Rom + luma-guided correction (JBU minus same-kernel Gaussian)), default `chroma_bicubic`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### dark  (FAIL 1, FLAG 4, INFO 4, LIMIT 3, NA 16, PASS 20)

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
| cost_ms_per_frame_pyref | 3085.2 | 13.8463 | 3071.35 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| edge_band_share | 0.0156498 | 0.0156498 | 0 | 1 | 0 | INFO:1 |
| flicker_excess_Cb | -0.000633512 | -0.00067741 | 4.38987e-05 | 1 | 0 | PASS:1 |
| flicker_excess_Cr | -0.00340296 | -0.00346717 | 6.42093e-05 | 1 | 0 | PASS:1 |
| flicker_excess_Y | -0.0575861 | -0.0575861 | 0 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.0560969 | 0.0560969 | 0 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 1.92531 | 1.92511 | 0.00020056 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 0.934351 | 0.934351 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | NA | NA | NA | 1 | 1 | NA:1 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | 0.124667 | -0.00644587 | 0.131113 | 1 | 0 | FAIL:1 |
| grain_lag1_delta_Y | 0.0532603 | 0.0532603 | 0 | 1 | 0 | LIMIT:1 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 1 | 1 | NA:1 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 2.26812 | 2.26495 | 0.00316253 | 1 | 0 | LIMIT:1 |
| grain_rms_ratio_Y | 0.58004 | 0.58004 | 0 | 1 | 0 | LIMIT:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 2.51987e-05 | 2.51987e-05 | 0 | 1 | 0 | PASS:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.850117 | 0.850117 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.905164 | 0.905164 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.946763 | 0.946763 | 0 | 1 | 0 | PASS:1 |
| lpips | 0.0177491 | 0.0177066 | 4.25148e-05 | 1 | 0 | PASS:1 |
| psnr_Cb | 55.2839 | 55.3471 | -0.0632447 | 1 | 0 | FLAG:1 |
| psnr_Cr | 56.5833 | 56.6386 | -0.0552643 | 1 | 0 | FLAG:1 |
| psnr_Y | 50.3844 | 50.3844 | 0 | 1 | 0 | PASS:1 |
| psnr_edge_Cb | 50.4754 | 51.8664 | -1.39099 | 1 | 0 | FLAG:1 |
| psnr_edge_Cr | 51.6181 | 52.7353 | -1.11724 | 1 | 0 | FLAG:1 |
| smear_Y | 0.930265 | 0.930265 | 0 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.998156 | 0.998195 | -3.92052e-05 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.998703 | 0.998729 | -2.57101e-05 | 1 | 0 | PASS:1 |
| ssim_Y | 0.992657 | 0.992657 | 0 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |

### film24  (FLAG 18, INFO 16, LIMIT 16, NA 52, PASS 90)

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
| cost_ms_per_frame_pyref | 1709.84 | 16.9608 | 1692.88 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_band_share | 0.270395 | 0.270395 | 0 | 4 | 0 | INFO:4 |
| flicker_excess_Cb | -0.0808361 | -0.0851996 | 0.00436351 | 4 | 0 | PASS:4 |
| flicker_excess_Cr | -0.108563 | -0.115712 | 0.00714945 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.0213181 | -0.0213181 | 0 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.0290726 | 0.0290726 | 0 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 0.553508 | 0.538947 | 0.0145609 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 0.944298 | 0.944298 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.201817 | 0.213179 | -0.0113623 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.0506189 | 0.0506189 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.718636 | 0.69641 | 0.0222256 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.77166 | 0.77166 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 6.06898e-05 | 6.06898e-05 | 0 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.931949 | 0.931949 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.957486 | 0.957486 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.976102 | 0.976102 | 0 | 4 | 0 | PASS:4 |
| lpips | 0.0166536 | 0.016589 | 6.45305e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 50.1752 | 50.9334 | -0.758189 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_Cr | 49.4889 | 50.8032 | -1.31434 | 4 | 0 | FLAG:4 |
| psnr_Y | 45.411 | 45.411 | 0 | 4 | 0 | PASS:4 |
| psnr_edge_Cb | 45.8415 | 48.1908 | -2.34928 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_edge_Cr | 45.0439 | 48.2399 | -3.19597 | 4 | 0 | FLAG:4 |
| smear_Y | 0.954204 | 0.954204 | 0 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.989197 | 0.989472 | -0.000274936 | 4 | 0 | FLAG:1 PASS:3 |
| ssim_Cr | 0.989469 | 0.990149 | -0.000680024 | 4 | 0 | FLAG:3 PASS:1 |
| ssim_Y | 0.984167 | 0.984167 | 0 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.668149 | 0.668149 | 0 | 4 | 0 | LIMIT:3 PASS:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |

### sports50_60  (FLAG 9, INFO 16, LIMIT 17, NA 60, PASS 90)

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
| cost_ms_per_frame_pyref | 1771.55 | 17.7695 | 1753.78 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| edge_band_share | 0.281572 | 0.281572 | 0 | 4 | 0 | INFO:4 |
| flicker_excess_Cb | -0.0786315 | -0.103 | 0.0243683 | 4 | 0 | FLAG:1 PASS:3 |
| flicker_excess_Cr | -0.141121 | -0.151316 | 0.010195 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00672003 | -0.00672003 | 0 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.051668 | 0.051668 | 0 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Y | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | 0.117445 | 0.117754 | -0.000309072 | 4 | 0 | LIMIT:3 PASS:1 |
| grain_lag1_delta_Y | 0.0927216 | 0.0927216 | 0 | 4 | 0 | LIMIT:2 PASS:2 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 4 | 4 | NA:4 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.14706 | 0.144513 | 0.00254772 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.524696 | 0.524696 | 0 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 7.68252e-05 | 7.68252e-05 | 0 | 4 | 0 | PASS:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.886513 | 0.886513 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.935174 | 0.935174 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.963139 | 0.963139 | 0 | 4 | 0 | PASS:4 |
| lpips | 0.0444455 | 0.0444811 | -3.55543e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 41.692 | 41.7176 | -0.0255553 | 4 | 0 | FLAG:1 PASS:3 |
| psnr_Cr | 41.7441 | 41.9466 | -0.202503 | 4 | 0 | FLAG:2 PASS:2 |
| psnr_Y | 40.869 | 40.869 | 0 | 4 | 0 | PASS:4 |
| psnr_edge_Cb | 38.9 | 39.0803 | -0.180242 | 4 | 0 | FLAG:3 PASS:1 |
| psnr_edge_Cr | 39.0293 | 39.5631 | -0.533812 | 4 | 0 | FLAG:2 PASS:2 |
| smear_Y | 0.914938 | 0.914938 | 0 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.946861 | 0.9425 | 0.00436019 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.945019 | 0.944356 | 0.000663347 | 4 | 0 | PASS:4 |
| ssim_Y | 0.966414 | 0.966414 | 0 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.5713 | 0.5713 | 0 | 4 | 1 | LIMIT:3 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 0 | PASS:4 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Y | 41.0019 | 41.0019 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cb | 43.9148 | 43.9154 | -0.000592237 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cr | 43.1207 | 43.5937 | -0.472999 | worse | FLAG |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Y | 0.959528 | 0.959528 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cb | 0.97443 | 0.973853 | 0.000576685 | better | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cr | 0.973082 | 0.973357 | -0.000275843 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lpips | 0.0459361 | 0.0464757 | -0.000539564 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Y | 0.506753 | 0.506753 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Y | 0.215182 | 0.215182 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Cb | 0.125121 | 0.124143 | 0.000977908 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Cb | 0.26006 | 0.259766 | 0.000294193 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_edge_Cb | 40.9486 | 41.0159 | -0.0673312 | worse | FLAG |  |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Cb | -0.08299 | -0.0857659 | 0.00277586 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_edge_Cr | 39.0146 | 40.3065 | -1.29186 | worse | FLAG |  |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Cr | -0.144549 | -0.142884 | -0.00166562 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | edge_band_share | 0.156062 | 0.156062 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_ms_per_frame_pyref | 1741.53 | 16.7173 | 1724.81 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Y | 42.0854 | 42.0854 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cb | 42.9956 | 43.1536 | -0.157993 | worse | FLAG |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cr | 42.7984 | 42.7496 | 0.0488243 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Y | 0.964841 | 0.964841 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cb | 0.96681 | 0.966972 | -0.000161135 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cr | 0.961815 | 0.961442 | 0.000373833 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lpips | 0.100976 | 0.1013 | -0.000324231 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Y | 0.542623 | 0.542623 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Y | -0.00351741 | -0.00351741 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Cb | 0.204159 | 0.203272 | 0.000887079 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Cb | -0.00714841 | -0.00701917 | -0.000129239 | better | PASS |  |
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
| sports50_60 | Jockey | native_x264_crf23 | psnr_edge_Cb | 40.2533 | 40.8862 | -0.63296 | worse | FLAG |  |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Cb | 0.0112214 | 0.00282487 | 0.00839657 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_edge_Cr | 41.2186 | 41.0336 | 0.185024 | better | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Cr | -0.0218736 | -0.0258829 | 0.00400925 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | edge_band_share | 0.133898 | 0.133898 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | cost_ms_per_frame_pyref | 1768.66 | 17.4326 | 1751.23 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x264_crf23 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x264_crf23 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Y | 44.4982 | 44.4982 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Cb | 45.3277 | 45.3728 | -0.045038 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_Cr | 44.0602 | 44.4217 | -0.361436 | worse | FLAG |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Y | 0.983093 | 0.983093 | 0 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Cb | 0.980205 | 0.979763 | 0.000441927 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | ssim_Cr | 0.975222 | 0.97538 | -0.000157672 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | lpips | 0.0125354 | 0.0131008 | -0.000565379 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_rms_ratio_Y | 0.157251 | 0.157251 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_delta_Y | 0.157801 | 0.157801 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | grain_rms_ratio_Cb | 0.072178 | 0.0720717 | 0.000106307 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | YachtRide | native_x265_crf20 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | YachtRide | native_x265_crf20 | grain_lag1_delta_Cb | 0.155295 | 0.156404 | -0.00110942 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| sports50_60 | YachtRide | native_x265_crf20 | psnr_edge_Cb | 41.1413 | 41.2737 | -0.132427 | worse | FLAG |  |
| sports50_60 | YachtRide | native_x265_crf20 | flicker_excess_Cb | -0.112152 | -0.117444 | 0.00529185 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | psnr_edge_Cr | 39.9821 | 40.9725 | -0.990416 | worse | FLAG |  |
| sports50_60 | YachtRide | native_x265_crf20 | flicker_excess_Cr | -0.0310061 | -0.042726 | 0.0117199 | same | PASS |  |
| sports50_60 | YachtRide | native_x265_crf20 | edge_band_share | 0.161606 | 0.161606 | 0 | NA | INFO |  |
| sports50_60 | YachtRide | native_x265_crf20 | cost_ms_per_frame_pyref | 1808.2 | 16.7209 | 1791.48 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | YachtRide | native_x265_crf20 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | YachtRide | native_x265_crf20 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Y | 35.8906 | 35.8906 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cb | 34.5301 | 34.4287 | 0.101402 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cr | 36.9969 | 37.0213 | -0.0243989 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Y | 0.958194 | 0.958194 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cb | 0.865998 | 0.849414 | 0.0165833 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cr | 0.869958 | 0.867244 | 0.00271307 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lpips | 0.018335 | 0.017048 | 0.00128696 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Y | 0.892158 | 0.892158 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Y | 0.00142003 | 0.00142003 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Cb | 0.186783 | 0.178564 | 0.00821958 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Cb | 0.0615742 | 0.061866 | -0.000291818 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| sports50_60 | park_joy | native_x265_crf26 | psnr_edge_Cb | 33.2571 | 33.1453 | 0.111749 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Cb | -0.130605 | -0.211614 | 0.0810091 | worse | FLAG |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_edge_Cr | 35.9018 | 35.9398 | -0.0379991 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Cr | -0.367054 | -0.39377 | 0.0267163 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | edge_band_share | 0.674721 | 0.674721 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | cost_ms_per_frame_pyref | 1767.8 | 20.2072 | 1747.59 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | park_joy | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | park_joy | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 39.132 | 39.132 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 44.6484 | 44.6618 | -0.0134782 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.9154 | 45.1137 | -0.198313 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.961872 | 0.961872 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.982476 | 0.982645 | -0.000168813 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984416 | 0.985053 | -0.000636987 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | 0.0393769 | 0.0400135 | -0.000636554 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.85289 | 0.85289 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | 0.952946 | 0.952946 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0534701 | 0.0534701 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.576926 | 0.54944 | 0.0274856 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | 0.385987 | 0.348264 | 0.037723 | better | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.307734 | 0.322787 | -0.0150532 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_edge_Cb | 43.603 | 43.6176 | -0.0145728 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Cb | -0.336035 | -0.33667 | 0.000635191 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_edge_Cr | 43.9386 | 44.3017 | -0.363186 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Cr | -0.473553 | -0.479652 | 0.00609863 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | edge_band_share | 0.43306 | 0.43306 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 1753.55 | 15.8502 | 1737.7 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Cb | 53.3319 | 54.7967 | -1.46479 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_Cr | 52.3081 | 55.5848 | -3.27668 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Cb | 0.995988 | 0.996807 | -0.000818932 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | ssim_Cr | 0.996266 | 0.997518 | -0.00125247 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | lpips | 0.00111864 | 0.000124715 | 0.000993922 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_hf_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_rms_ratio_Cb | 0.734969 | 0.681802 | 0.0531672 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_hf_ratio_Cb | 0.686777 | 0.666408 | 0.0203688 | better | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | grain_lag1_delta_Cb | 0.108579 | 0.11806 | -0.00948088 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_edge_Cb | 50.8807 | 52.8594 | -1.97866 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | flicker_excess_Cb | -0.0022426 | -0.000917751 | -0.00132484 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | psnr_edge_Cr | 49.666 | 53.9324 | -4.26643 | worse | FLAG |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | flicker_excess_Cr | 0.00855322 | 0.000130259 | 0.00842296 | same | PASS |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | edge_band_share | 0.434521 | 0.434521 | 0 | NA | INFO |  |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_ms_per_frame_pyref | 1859.38 | 17.7011 | 1841.67 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | ffv1_master_no_codec_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Y | 53.2548 | 53.2548 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cb | 57.2449 | 58.6046 | -1.35968 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_Cr | 55.8229 | 57.0807 | -1.2578 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Y | 0.997368 | 0.997368 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cb | 0.998798 | 0.999074 | -0.00027588 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | ssim_Cr | 0.998432 | 0.998756 | -0.000324006 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | lpips | 0.00444541 | 0.00429423 | 0.000151181 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Y | 0.389306 | 0.389306 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Y | 0.924005 | 0.924005 | 0 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Y | 0.0880063 | 0.0880063 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_rms_ratio_Cb | 1.28631 | 1.28624 | 7.07198e-05 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_hf_ratio_Cb | 0.793932 | 0.793942 | -9.61051e-06 | worse | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | grain_lag1_delta_Cb | 0.00963375 | 0.0298279 | -0.0201942 | better | PASS |  |
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
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_edge_Cb | 45.7336 | 52.3701 | -6.63654 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Cb | 0.115845 | 0.103162 | 0.0126836 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | psnr_edge_Cr | 44.5626 | 51.1054 | -6.54278 | worse | FLAG |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | flicker_excess_Cr | 0.198859 | 0.186639 | 0.0122204 | same | PASS |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | edge_band_share | 0.0239045 | 0.0239045 | 0 | NA | INFO |  |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_ms_per_frame_pyref | 1857.11 | 21.1154 | 1835.99 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sintel4k | svtav1_2160p_6000k_w1200 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc24_12f | psnr_Y | 43.8462 | 43.8462 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | psnr_Cb | 45.4755 | 45.6703 | -0.194813 | worse | FLAG |  |
| film24 | breathing | enc24_12f | psnr_Cr | 44.9091 | 45.4337 | -0.524586 | worse | FLAG |  |
| film24 | breathing | enc24_12f | ssim_Y | 0.977426 | 0.977426 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | ssim_Cb | 0.979526 | 0.979362 | 0.000163882 | same | PASS |  |
| film24 | breathing | enc24_12f | ssim_Cr | 0.978762 | 0.979269 | -0.000506635 | worse | FLAG |  |
| film24 | breathing | enc24_12f | lpips | 0.0216733 | 0.0219237 | -0.000250427 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | grain_rms_ratio_Y | 0.844445 | 0.844445 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_hf_ratio_Y | 0.900241 | 0.900241 | 0 | same | PASS |  |
| film24 | breathing | enc24_12f | grain_lag1_delta_Y | 0.0609991 | 0.0609991 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | grain_rms_ratio_Cb | 0.276338 | 0.268159 | 0.00817898 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | breathing | enc24_12f | grain_hf_ratio_Cb | 0.347337 | 0.347176 | 0.000161409 | better | PASS |  |
| film24 | breathing | enc24_12f | grain_lag1_delta_Cb | 0.38132 | 0.382041 | -0.000720853 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| film24 | breathing | enc24_12f | psnr_edge_Cb | 43.1488 | 43.9161 | -0.767339 | worse | FLAG |  |
| film24 | breathing | enc24_12f | flicker_excess_Cb | -0.100912 | -0.106372 | 0.00546011 | same | PASS |  |
| film24 | breathing | enc24_12f | psnr_edge_Cr | 42.0084 | 43.6199 | -1.61148 | worse | FLAG |  |
| film24 | breathing | enc24_12f | flicker_excess_Cr | -0.168111 | -0.169967 | 0.00185577 | same | PASS |  |
| film24 | breathing | enc24_12f | edge_band_share | 0.190094 | 0.190094 | 0 | NA | INFO |  |
| film24 | breathing | enc24_12f | cost_ms_per_frame_pyref | 1369.33 | 13.1766 | 1356.15 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc24_12f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc24_12f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Y | 50.3844 | 50.3844 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cb | 55.2839 | 55.3471 | -0.0632447 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_Cr | 56.5833 | 56.6386 | -0.0552643 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Y | 0.992657 | 0.992657 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cb | 0.998156 | 0.998195 | -3.92052e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | ssim_Cr | 0.998703 | 0.998729 | -2.57101e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | lpips | 0.0177491 | 0.0177066 | 4.25148e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Y | 0.58004 | 0.58004 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Y | 0.934351 | 0.934351 | 0 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Y | 0.0532603 | 0.0532603 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_rms_ratio_Cb | 2.26812 | 2.26495 | 0.00316253 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_hf_ratio_Cb | 1.92531 | 1.92511 | 0.00020056 | worse | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | grain_lag1_delta_Cb | 0.124667 | -0.00644587 | 0.131113 | worse | FAIL |  |
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
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_edge_Cb | 50.4754 | 51.8664 | -1.39099 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Cb | -0.000633512 | -0.00067741 | 4.38987e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | psnr_edge_Cr | 51.6181 | 52.7353 | -1.11724 | worse | FLAG |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | flicker_excess_Cr | -0.00340296 | -0.00346717 | 6.42093e-05 | same | PASS |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | edge_band_share | 0.0156498 | 0.0156498 | 0 | NA | INFO |  |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_ms_per_frame_pyref | 3085.2 | 13.8463 | 3071.35 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | meridian4k | x265_2160p_6000k_w1800 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
