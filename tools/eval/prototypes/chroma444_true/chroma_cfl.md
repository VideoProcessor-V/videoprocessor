<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: chroma_cfl vs chroma_bicubic (default)

Suite: `proto_chroma444_true` (5 cases). Candidate `chroma_cfl` (chroma-from-luma: guided-filter local linear regression, correlation-mixed with Catmull-Rom), default `chroma_bicubic`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (FAIL 1, FLAG 7, INFO 20, LIMIT 7, NA 91, PASS 114)

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
| cost_ms_per_frame_pyref | 279.087 | 19.2901 | 259.797 | 5 | 0 | INFO:5 |
| duplicate_rate_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| edge_band_share | 0.0224445 | 0.0224445 | 0 | 5 | 0 | INFO:5 |
| flicker_excess_Cb | -0.000588765 | 0.0014834 | -0.00207217 | 5 | 0 | PASS:5 |
| flicker_excess_Cr | -0.0278103 | -0.0259475 | -0.00186276 | 5 | 0 | PASS:5 |
| flicker_excess_Y | -0.000366472 | -0.000366472 | 0 | 5 | 0 | PASS:5 |
| ghost_coeff_Y | 0.00847372 | 0.00847372 | 0 | 5 | 0 | PASS:5 |
| gop_ratio_gop | NA | NA | NA | 5 | 5 | NA:5 |
| gop_ratio_minigop | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Y | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_delta_Cb | 0.0229661 | 0.0194471 | 0.00351901 | 5 | 0 | LIMIT:1 PASS:4 |
| grain_lag1_delta_Y | 0.00838519 | 0.00838519 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_rms_ratio_Cb | 0.477567 | 0.508896 | -0.0313295 | 5 | 0 | FAIL:1 LIMIT:4 |
| grain_rms_ratio_Y | 0.825842 | 0.825842 | 0 | 5 | 1 | LIMIT:2 NA:1 PASS:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 5 | 0 | INFO:5 |
| grain_tiles_Y | 12.8 | 12.8 | 0 | 5 | 0 | INFO:5 |
| halo_overshoot_Y | 2.66404e-05 | 2.66404e-05 | 0 | 5 | 0 | PASS:5 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s1_Y | 0.949446 | 0.949446 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s2_Y | 0.966764 | 0.966764 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s3_Y | 0.980425 | 0.980425 | 0 | 5 | 0 | PASS:5 |
| lpips | 0.0164198 | 0.0159145 | 0.000505308 | 5 | 0 | PASS:5 |
| psnr_Cb | 53.3258 | 52.6964 | 0.629385 | 5 | 0 | FLAG:2 PASS:3 |
| psnr_Cr | 51.548 | 51.3082 | 0.239854 | 5 | 0 | FLAG:2 PASS:3 |
| psnr_Y | 46.92 | 46.92 | 0 | 5 | 0 | PASS:5 |
| psnr_edge_Cb | 45.8559 | 43.7946 | 2.06127 | 5 | 0 | FLAG:1 PASS:4 |
| psnr_edge_Cr | 44.3007 | 44.3019 | -0.00124865 | 5 | 0 | FLAG:1 PASS:4 |
| smear_Y | 0.891571 | 0.891571 | 0 | 5 | 0 | PASS:5 |
| ssim_Cb | 0.99136 | 0.991072 | 0.000287816 | 5 | 0 | FLAG:1 PASS:4 |
| ssim_Cr | 0.992083 | 0.991413 | 0.000670179 | 5 | 0 | PASS:5 |
| ssim_Y | 0.995481 | 0.995481 | 0 | 5 | 0 | PASS:5 |
| wtw_kept_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_Y | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_Y | NA | NA | NA | 5 | 5 | NA:5 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cb | 59.0004 | 59.6438 | -0.643322 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cr | 63.7382 | 63.9971 | -0.258859 | worse | FLAG |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cb | 0.998914 | 0.998899 | 1.55905e-05 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cr | 0.999546 | 0.99956 | -1.38604e-05 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lpips | 0.000400088 | 0.000126307 | 0.000273781 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Cb | 0.424364 | 0.431328 | -0.00696418 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Cb | 0.0258739 | 0.0262379 | -0.000363911 | better | PASS |  |
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
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cb | 53.5766 | 50.4937 | 3.08287 | better | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cb | -0.00159788 | -1.92025e-05 | -0.00157868 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cr | 61.1038 | 61.1061 | -0.00232025 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cr | -9.00298e-05 | 1.68291e-05 | -0.000106859 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | edge_band_share | 0.0387616 | 0.0387616 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | cost_ms_per_frame_pyref | 311.147 | 22.5361 | 288.611 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Y | 46.2879 | 46.2879 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cb | 49.2557 | 48.668 | 0.587684 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cr | 52.8914 | 52.7808 | 0.110508 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Y | 0.988677 | 0.988677 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cb | 0.995503 | 0.994454 | 0.00104908 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cr | 0.998515 | 0.998437 | 7.73089e-05 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lpips | 0.0479081 | 0.0482932 | -0.000385039 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Y | 0.753681 | 0.753681 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Y | 0.00110804 | 0.00110804 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Cb | 0.340487 | 0.383592 | -0.0431049 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Cb | -0.00523732 | -0.00487881 | -0.000358517 | better | PASS |  |
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
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cb | 44.0957 | 42.4159 | 1.67983 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cb | -0.00358902 | -0.00262072 | -0.000968302 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cr | 49.2007 | 49.0125 | 0.188255 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cr | -0.161068 | -0.160345 | -0.000722716 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | edge_band_share | 0.0388109 | 0.0388109 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_ms_per_frame_pyref | 321.49 | 23.0745 | 298.415 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cb | 61.9844 | 59.5685 | 2.41588 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cr | 48.4311 | 47.2061 | 1.22494 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cb | 0.999459 | 0.999002 | 0.000457558 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cr | 0.993277 | 0.990948 | 0.00232903 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lpips | 0.00114111 | 0.000329739 | 0.000811376 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Cb | 0.661263 | 0.681075 | -0.0198122 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Cb | 0.0150767 | 0.0132764 | 0.00180029 | worse | PASS |  |
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
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cb | 49.7252 | 46.2733 | 3.45189 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cb | -0.000565836 | 0.000210043 | -0.000775879 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cr | 34.9715 | 34.9074 | 0.0641473 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cr | 0.000610687 | 0.000486792 | 0.000123896 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | edge_band_share | 0.0171195 | 0.0171195 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | cost_ms_per_frame_pyref | 305.246 | 19.8538 | 285.392 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Y | 47.5521 | 47.5521 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cb | 52.1716 | 51.2516 | 0.92001 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cr | 44.044 | 43.7938 | 0.25024 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Y | 0.988729 | 0.988729 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cb | 0.997489 | 0.99659 | 0.000898659 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cr | 0.982715 | 0.981383 | 0.00133151 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lpips | 0.0306647 | 0.0291738 | 0.0014909 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Y | 0.549688 | 0.549688 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Y | 0.0324327 | 0.0324327 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Cb | 0.437517 | 0.490709 | -0.053192 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Cb | 0.0811317 | 0.0642722 | 0.0168595 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
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
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cb | 44.2896 | 40.7436 | 3.546 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cb | 0.0137686 | 0.0149912 | -0.00122263 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cr | 33.2022 | 32.9588 | 0.243467 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cr | 0.0379697 | 0.0389533 | -0.000983603 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | edge_band_share | 0.0171522 | 0.0171522 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_ms_per_frame_pyref | 324.096 | 23.3328 | 300.764 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | nocturne | t300_f60_422_partial | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cb | 44.2168 | 44.3502 | -0.133326 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cr | 48.6355 | 48.7631 | -0.127555 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cb | 0.965435 | 0.966416 | -0.000981806 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cr | 0.986362 | 0.986735 | -0.000373093 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lpips | 0.0019849 | 0.00164938 | 0.00033552 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Y | 0 | 0 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Y | NA | NA | NA | NA | NA | no flat tiles in the reference |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Y | NA | NA | NA | NA | NA | no flat tiles |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Cb | 0.524201 | 0.557776 | -0.0335744 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Cb | -0.00201442 | -0.00167212 | -0.000342302 | better | PASS |  |
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
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cb | 37.5923 | 39.0465 | -1.45424 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cb | -0.0109597 | -0.00514434 | -0.00581535 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cr | 43.025 | 43.5248 | -0.499793 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cr | -0.0164734 | -0.00884893 | -0.00762451 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | edge_band_share | 0.000378503 | 0.000378503 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | cost_ms_per_frame_pyref | 133.455 | 7.65345 | 125.801 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | nocturne | t300_f60_422_partial | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | nocturne | t300_f60_422_partial | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
