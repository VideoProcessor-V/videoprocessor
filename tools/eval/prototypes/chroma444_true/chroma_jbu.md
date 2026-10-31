<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: chroma_jbu vs chroma_bicubic (default)

Suite: `proto_chroma444_true` (5 cases). Candidate `chroma_jbu` (joint-bilateral luma-guided chroma upsampler, +-2 chroma taps, siting-correct), default `chroma_bicubic`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (FAIL 7, FLAG 17, INFO 20, LIMIT 2, NA 91, PASS 103)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 5 | 5 | NA:5 |
| btb_kept_Y | 1 | 1 | 0 | 5 | 4 | NA:4 PASS:1 |
| btb_kept_vs_input_C | NA | NA | NA | 5 | 5 | NA:5 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 5 | 4 | NA:4 PASS:1 |
| clip_detect_C | NA | NA | NA | 5 | 5 | NA:5 |
| clip_detect_Y | 0 | 0 | 0 | 5 | 4 | NA:4 PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 5 | 5 | NA:5 |
| cost_gpu_ms_laptop | NA | NA | NA | 5 | 5 | NA:5 |
| cost_ms_per_frame_pyref | 2116.53 | 19.2901 | 2097.24 | 5 | 0 | INFO:5 |
| duplicate_rate_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| edge_band_share | 0.0224445 | 0.0224445 | 0 | 5 | 0 | INFO:5 |
| flicker_excess_Cb | -0.00419127 | 0.0014834 | -0.00567467 | 5 | 0 | PASS:5 |
| flicker_excess_Cr | -0.0316278 | -0.0259475 | -0.00568026 | 5 | 0 | PASS:5 |
| flicker_excess_Y | -0.000366472 | -0.000366472 | 0 | 5 | 0 | PASS:5 |
| ghost_coeff_Y | 0.00847372 | 0.00847372 | 0 | 5 | 0 | PASS:5 |
| gop_ratio_gop | NA | NA | NA | 5 | 5 | NA:5 |
| gop_ratio_minigop | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Y | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_delta_Cb | 0.0484027 | 0.0194471 | 0.0289556 | 5 | 0 | FAIL:2 PASS:3 |
| grain_lag1_delta_Y | 0.00838519 | 0.00838519 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_rms_ratio_Cb | 0.212999 | 0.508896 | -0.295897 | 5 | 0 | FAIL:5 |
| grain_rms_ratio_Y | 0.825842 | 0.825842 | 0 | 5 | 1 | LIMIT:2 NA:1 PASS:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 5 | 0 | INFO:5 |
| grain_tiles_Y | 12.8 | 12.8 | 0 | 5 | 0 | INFO:5 |
| halo_overshoot_Y | 2.66404e-05 | 2.66404e-05 | 0 | 5 | 0 | PASS:5 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s1_Y | 0.949446 | 0.949446 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s2_Y | 0.966764 | 0.966764 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s3_Y | 0.980425 | 0.980425 | 0 | 5 | 0 | PASS:5 |
| lpips | 0.0201242 | 0.0159145 | 0.0042097 | 5 | 0 | FLAG:4 PASS:1 |
| psnr_Cb | 51.7656 | 52.6964 | -0.930832 | 5 | 0 | FLAG:3 PASS:2 |
| psnr_Cr | 50.228 | 51.3082 | -1.08015 | 5 | 0 | FLAG:4 PASS:1 |
| psnr_Y | 46.92 | 46.92 | 0 | 5 | 0 | PASS:5 |
| psnr_edge_Cb | 44.4128 | 43.7946 | 0.618198 | 5 | 0 | FLAG:1 PASS:4 |
| psnr_edge_Cr | 43.8243 | 44.3019 | -0.477574 | 5 | 0 | FLAG:2 PASS:3 |
| smear_Y | 0.891571 | 0.891571 | 0 | 5 | 0 | PASS:5 |
| ssim_Cb | 0.985742 | 0.991072 | -0.00533002 | 5 | 0 | FLAG:1 PASS:4 |
| ssim_Cr | 0.988396 | 0.991413 | -0.00301646 | 5 | 0 | FLAG:2 PASS:3 |
| ssim_Y | 0.995481 | 0.995481 | 0 | 5 | 0 | PASS:5 |
| wtw_kept_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_Y | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_Y | NA | NA | NA | 5 | 5 | NA:5 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cb | 57.9171 | 59.6438 | -1.72662 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cr | 62.2026 | 63.9971 | -1.79447 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cb | 0.998566 | 0.998899 | -0.000333215 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cr | 0.999381 | 0.99956 | -0.000179036 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lpips | 0.00218245 | 0.000126307 | 0.00205615 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Cb | 0.1929 | 0.431328 | -0.238428 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Cb | 0.0434636 | 0.0262379 | 0.0172258 | worse | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_420_no_codec | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_420_no_codec | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t40_f48_420_no_codec | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t40_f48_420_no_codec | ghost_coeff_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | smear_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | halo_overshoot_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lc_ratio_s1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lc_ratio_s2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lc_ratio_s3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cb | 53.2203 | 50.4937 | 2.72659 | better | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cb | -0.000486539 | -1.92025e-05 | -0.000467336 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cr | 59.6429 | 61.1061 | -1.46321 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cr | 0.000182527 | 1.68291e-05 | 0.000165698 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | edge_band_share | 0.0387616 | 0.0387616 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | cost_ms_per_frame_pyref | 2387.73 | 22.5361 | 2365.19 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Y | 46.2879 | 46.2879 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cb | 49.336 | 48.668 | 0.668004 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cr | 53.1571 | 52.7808 | 0.376205 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Y | 0.988677 | 0.988677 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cb | 0.995777 | 0.994454 | 0.00132236 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cr | 0.99869 | 0.998437 | 0.000252963 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lpips | 0.049265 | 0.0482932 | 0.000971856 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Y | 0.753681 | 0.753681 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Y | 0.00110804 | 0.00110804 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Cb | 0.182762 | 0.383592 | -0.20083 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Cb | -0.0035198 | -0.00487881 | 0.001359 | worse | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_x265_crf22 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_x265_crf22 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Y | -0.00220901 | -0.00220901 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t40_f48_x265_crf22 | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t40_f48_x265_crf22 | ghost_coeff_Y | 0.00219387 | 0.00219387 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | smear_Y | 0.745294 | 0.745294 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | halo_overshoot_Y | 7.90835e-05 | 7.90835e-05 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lc_ratio_s1_Y | 0.92179 | 0.92179 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lc_ratio_s2_Y | 0.944679 | 0.944679 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lc_ratio_s3_Y | 0.965379 | 0.965379 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cb | 43.9099 | 42.4159 | 1.49403 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cb | -0.00409397 | -0.00262072 | -0.00147325 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cr | 49.4974 | 49.0125 | 0.484923 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cr | -0.161519 | -0.160345 | -0.00117357 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | edge_band_share | 0.0388109 | 0.0388109 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_ms_per_frame_pyref | 2353.36 | 23.0745 | 2330.28 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cb | 58.1784 | 59.5685 | -1.39012 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cr | 46.0451 | 47.2061 | -1.161 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cb | 0.998663 | 0.999002 | -0.000339023 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cr | 0.986992 | 0.990948 | -0.00395615 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | lpips | 0.00315475 | 0.000329739 | 0.00282501 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Cb | 0.295944 | 0.681075 | -0.38513 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Cb | 0.0502794 | 0.0132764 | 0.037003 | worse | FAIL |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_420_no_codec | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_420_no_codec | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t150_f120_420_no_codec | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t150_f120_420_no_codec | ghost_coeff_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | smear_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | halo_overshoot_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lc_ratio_s1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lc_ratio_s2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lc_ratio_s3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cb | 47.3671 | 46.2733 | 1.09377 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cb | -0.0029001 | 0.000210043 | -0.00311014 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cr | 35.714 | 34.9074 | 0.806631 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cr | -0.00714343 | 0.000486792 | -0.00763022 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | edge_band_share | 0.0171195 | 0.0171195 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | cost_ms_per_frame_pyref | 2368.06 | 19.8538 | 2348.21 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Y | 47.5521 | 47.5521 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cb | 51.7095 | 51.2516 | 0.45795 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cr | 43.6728 | 43.7938 | -0.120971 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Y | 0.988729 | 0.988729 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cb | 0.997134 | 0.99659 | 0.000543798 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cr | 0.981235 | 0.981383 | -0.00014839 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lpips | 0.0338932 | 0.0291738 | 0.00471947 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Y | 0.549688 | 0.549688 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Y | 0.0324327 | 0.0324327 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Cb | 0.224408 | 0.490709 | -0.266301 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Cb | 0.158964 | 0.0642722 | 0.0946917 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_x265_crf22 | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_x265_crf22 | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Y | 0.000376651 | 0.000376651 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t150_f120_x265_crf22 | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | sol_levante | t150_f120_x265_crf22 | ghost_coeff_Y | 0.0401747 | 0.0401747 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | smear_Y | 0.71256 | 0.71256 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | halo_overshoot_Y | 5.41187e-05 | 5.41187e-05 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lc_ratio_s1_Y | 0.82544 | 0.82544 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lc_ratio_s2_Y | 0.889141 | 0.889141 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lc_ratio_s3_Y | 0.936748 | 0.936748 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cb | 42.1425 | 40.7436 | 1.39889 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cb | 0.0107763 | 0.0149912 | -0.00421495 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cr | 33.3598 | 32.9588 | 0.400991 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cr | 0.0307269 | 0.0389533 | -0.0082264 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | edge_band_share | 0.0171522 | 0.0171522 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_ms_per_frame_pyref | 2311.88 | 23.3328 | 2288.54 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | nocturne | t300_f60_422_partial | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cb | 41.6868 | 44.3502 | -2.66338 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cr | 46.0626 | 48.7631 | -2.70053 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cb | 0.938572 | 0.966416 | -0.027844 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cr | 0.975683 | 0.986735 | -0.0110517 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | lpips | 0.0121254 | 0.00164938 | 0.010476 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Y | 0 | 0 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Y | NA | NA | NA | NA | NA | no flat tiles in the reference |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Y | NA | NA | NA | NA | NA | no flat tiles |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Cb | 0.168981 | 0.557776 | -0.388795 | worse | FAIL | default also fails and the candidate is worse |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Cb | -0.00717337 | -0.00167212 | -0.00550125 | better | PASS |  |
| film24 | nocturne | t300_f60_422_partial | grain_keep_vs_input_Y | NA | NA | NA | NA | NA | no flat tiles in the reference |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_vs_input_Y | NA | NA | NA | NA | NA | no flat tiles |
| film24 | nocturne | t300_f60_422_partial | grain_keep_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_vs_input_Cb | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | nocturne | t300_f60_422_partial | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | nocturne | t300_f60_422_partial | btb_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | nocturne | t300_f60_422_partial | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | input/output geometry differs (or no input given) |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nocturne | t300_f60_422_partial | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nocturne | t300_f60_422_partial | ghost_coeff_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | smear_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | halo_overshoot_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lc_ratio_s1_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lc_ratio_s2_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lc_ratio_s3_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | btb_kept_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nocturne | t300_f60_422_partial | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nocturne | t300_f60_422_partial | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nocturne | t300_f60_422_partial | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cb | 35.4243 | 39.0465 | -3.62228 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cb | -0.024252 | -0.00514434 | -0.0191077 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cr | 40.9076 | 43.5248 | -2.6172 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cr | -0.0203857 | -0.00884893 | -0.0115368 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | edge_band_share | 0.000378503 | 0.000378503 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | cost_ms_per_frame_pyref | 1161.62 | 7.65345 | 1153.96 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | nocturne | t300_f60_422_partial | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | nocturne | t300_f60_422_partial | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
