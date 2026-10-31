<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: chroma_mv vs chroma_bicubic (default)

Suite: `proto_chroma444_true` (5 cases). Candidate `chroma_mv` (MV temporal chroma: neighbour chroma samples along DIS flow (stand-in for VP vectors), validated per sample, confidence-mixed with residual JBU), default `chroma_bicubic`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (FAIL 1, FLAG 3, INFO 20, LIMIT 6, NA 91, PASS 119)

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
| cost_ms_per_frame_pyref | 6604.52 | 19.2901 | 6585.23 | 5 | 0 | INFO:5 |
| duplicate_rate_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| edge_band_share | 0.0224445 | 0.0224445 | 0 | 5 | 0 | INFO:5 |
| flicker_excess_Cb | -0.036684 | 0.0014834 | -0.0381674 | 5 | 0 | PASS:5 |
| flicker_excess_Cr | -0.0482767 | -0.0259475 | -0.0223292 | 5 | 0 | PASS:5 |
| flicker_excess_Y | -0.000366472 | -0.000366472 | 0 | 5 | 0 | PASS:5 |
| ghost_coeff_Y | 0.00847372 | 0.00847372 | 0 | 5 | 0 | PASS:5 |
| gop_ratio_gop | NA | NA | NA | 5 | 5 | NA:5 |
| gop_ratio_minigop | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_hf_ratio_Y | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_delta_Cb | 0.0196275 | 0.0194471 | 0.000180368 | 5 | 0 | PASS:5 |
| grain_lag1_delta_Y | 0.00838519 | 0.00838519 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_lag1_vs_input_Cb | NA | NA | NA | 5 | 5 | NA:5 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 5 | 1 | NA:1 PASS:4 |
| grain_rms_ratio_Cb | 0.495046 | 0.508896 | -0.0138499 | 5 | 0 | FAIL:1 LIMIT:4 |
| grain_rms_ratio_Y | 0.825842 | 0.825842 | 0 | 5 | 1 | LIMIT:2 NA:1 PASS:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 5 | 0 | INFO:5 |
| grain_tiles_Y | 12.8 | 12.8 | 0 | 5 | 0 | INFO:5 |
| halo_overshoot_Y | 2.66404e-05 | 2.66404e-05 | 0 | 5 | 0 | PASS:5 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s1_Y | 0.949446 | 0.949446 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s2_Y | 0.966764 | 0.966764 | 0 | 5 | 0 | PASS:5 |
| lc_ratio_s3_Y | 0.980425 | 0.980425 | 0 | 5 | 0 | PASS:5 |
| lpips | 0.0161028 | 0.0159145 | 0.000188298 | 5 | 0 | PASS:5 |
| psnr_Cb | 52.7433 | 52.6964 | 0.0468735 | 5 | 0 | FLAG:1 PASS:4 |
| psnr_Cr | 51.3881 | 51.3082 | 0.079916 | 5 | 0 | PASS:5 |
| psnr_Y | 46.92 | 46.92 | 0 | 5 | 0 | PASS:5 |
| psnr_edge_Cb | 44.2161 | 43.7946 | 0.42148 | 5 | 0 | FLAG:2 PASS:3 |
| psnr_edge_Cr | 44.6411 | 44.3019 | 0.339222 | 5 | 0 | PASS:5 |
| smear_Y | 0.891571 | 0.891571 | 0 | 5 | 0 | PASS:5 |
| ssim_Cb | 0.991221 | 0.991072 | 0.000148981 | 5 | 0 | PASS:5 |
| ssim_Cr | 0.991634 | 0.991413 | 0.000221435 | 5 | 0 | PASS:5 |
| ssim_Y | 0.995481 | 0.995481 | 0 | 5 | 0 | PASS:5 |
| wtw_kept_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_Y | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_C | NA | NA | NA | 5 | 5 | NA:5 |
| wtw_kept_vs_input_Y | NA | NA | NA | 5 | 5 | NA:5 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cb | 59.6154 | 59.6438 | -0.0283598 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_Cr | 63.9699 | 63.9971 | -0.0271603 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cb | 0.998984 | 0.998899 | 8.4959e-05 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | ssim_Cr | 0.999563 | 0.99956 | 3.07117e-06 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | lpips | 0.000212174 | 0.000126307 | 8.58669e-05 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | grain_rms_ratio_Cb | 0.454005 | 0.431328 | 0.0226774 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_420_no_codec | grain_lag1_delta_Cb | 0.0234358 | 0.0262379 | -0.00280208 | better | PASS |  |
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
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cb | 50.8419 | 50.4937 | 0.348226 | better | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cb | 0.000540797 | -1.92025e-05 | 0.000559999 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | psnr_edge_Cr | 61.5283 | 61.1061 | 0.422169 | better | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | flicker_excess_Cr | -0.00106629 | 1.68291e-05 | -0.00108312 | same | PASS |  |
| film24 | sol_levante | t40_f48_420_no_codec | edge_band_share | 0.0387616 | 0.0387616 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_420_no_codec | cost_ms_per_frame_pyref | 6564.25 | 22.5361 | 6541.72 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Y | 46.2879 | 46.2879 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cb | 48.8037 | 48.668 | 0.135769 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_Cr | 52.7939 | 52.7808 | 0.013084 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Y | 0.988677 | 0.988677 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cb | 0.994739 | 0.994454 | 0.00028495 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | ssim_Cr | 0.998438 | 0.998437 | 1.03821e-06 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | lpips | 0.0482894 | 0.0482932 | -3.73015e-06 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Y | 0.753681 | 0.753681 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Y | 0.00110804 | 0.00110804 | 0 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_rms_ratio_Cb | 0.388103 | 0.383592 | 0.00451053 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t40_f48_x265_crf22 | grain_lag1_delta_Cb | -0.00564248 | -0.00487881 | -0.000763678 | better | PASS |  |
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
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cb | 43.0181 | 42.4159 | 0.602211 | better | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cb | -0.00232548 | -0.00262072 | 0.000295235 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | psnr_edge_Cr | 48.9639 | 49.0125 | -0.0485517 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | flicker_excess_Cr | -0.160967 | -0.160345 | -0.000621629 | same | PASS |  |
| film24 | sol_levante | t40_f48_x265_crf22 | edge_band_share | 0.0388109 | 0.0388109 | 0 | NA | INFO |  |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_ms_per_frame_pyref | 6618.53 | 23.0745 | 6595.46 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t40_f48_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cb | 59.298 | 59.5685 | -0.270556 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_Cr | 47.421 | 47.2061 | 0.214886 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cb | 0.999072 | 0.999002 | 7.00979e-05 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | ssim_Cr | 0.991345 | 0.990948 | 0.000397171 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | lpips | 0.000914559 | 0.000329739 | 0.00058482 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | grain_rms_ratio_Cb | 0.614404 | 0.681075 | -0.0666709 | worse | FAIL | default also fails and the candidate is worse |
| film24 | sol_levante | t150_f120_420_no_codec | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_420_no_codec | grain_lag1_delta_Cb | 0.0405707 | 0.0132764 | 0.0272943 | worse | PASS |  |
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
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cb | 45.6777 | 46.2733 | -0.595663 | worse | FLAG |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cb | -0.0788797 | 0.000210043 | -0.0790898 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | psnr_edge_Cr | 35.22 | 34.9074 | 0.312646 | better | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | flicker_excess_Cr | -0.0398376 | 0.000486792 | -0.0403244 | same | PASS |  |
| film24 | sol_levante | t150_f120_420_no_codec | edge_band_share | 0.0171195 | 0.0171195 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_420_no_codec | cost_ms_per_frame_pyref | 7245.7 | 19.8538 | 7225.85 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_420_no_codec | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_420_no_codec | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Y | 47.5521 | 47.5521 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cb | 51.6716 | 51.2516 | 0.419999 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_Cr | 44.0309 | 43.7938 | 0.237115 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Y | 0.988729 | 0.988729 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cb | 0.997049 | 0.99659 | 0.0004589 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | ssim_Cr | 0.982191 | 0.981383 | 0.00080734 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | lpips | 0.0293352 | 0.0291738 | 0.000161447 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Y | 0.549688 | 0.549688 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Y | 0.0324327 | 0.0324327 | 0 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_rms_ratio_Cb | 0.464526 | 0.490709 | -0.0261829 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | sol_levante | t150_f120_x265_crf22 | grain_lag1_delta_Cb | 0.0409272 | 0.0642722 | -0.0233451 | better | PASS |  |
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
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cb | 42.7225 | 40.7436 | 1.97891 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cb | -0.0610074 | 0.0149912 | -0.0759986 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | psnr_edge_Cr | 33.8405 | 32.9588 | 0.881729 | better | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | flicker_excess_Cr | 0.00324351 | 0.0389533 | -0.0357098 | same | PASS |  |
| film24 | sol_levante | t150_f120_x265_crf22 | edge_band_share | 0.0171522 | 0.0171522 | 0 | NA | INFO |  |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_ms_per_frame_pyref | 8942.77 | 23.3328 | 8919.44 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol_levante | t150_f120_x265_crf22 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | nocturne | t300_f60_422_partial | psnr_Y | inf | inf | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cb | 44.3277 | 44.3502 | -0.0224851 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_Cr | 48.7247 | 48.7631 | -0.0383445 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cb | 0.966262 | 0.966416 | -0.000154001 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | ssim_Cr | 0.986633 | 0.986735 | -0.000101446 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | lpips | 0.00176246 | 0.00164938 | 0.000113086 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Y | 0 | 0 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Y | NA | NA | NA | NA | NA | no flat tiles in the reference |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Y | NA | NA | NA | NA | NA | no flat tiles |
| film24 | nocturne | t300_f60_422_partial | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | grain_rms_ratio_Cb | 0.554192 | 0.557776 | -0.00358339 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | nocturne | t300_f60_422_partial | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (6 available) |
| film24 | nocturne | t300_f60_422_partial | grain_lag1_delta_Cb | -0.00115375 | -0.00167212 | 0.000518371 | worse | PASS |  |
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
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cb | 38.8202 | 39.0465 | -0.226291 | worse | FLAG |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cb | -0.0417481 | -0.00514434 | -0.0366038 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | psnr_edge_Cr | 43.6529 | 43.5248 | 0.128117 | better | PASS |  |
| film24 | nocturne | t300_f60_422_partial | flicker_excess_Cr | -0.042756 | -0.00884893 | -0.0339071 | same | PASS |  |
| film24 | nocturne | t300_f60_422_partial | edge_band_share | 0.000378503 | 0.000378503 | 0 | NA | INFO |  |
| film24 | nocturne | t300_f60_422_partial | cost_ms_per_frame_pyref | 3651.34 | 7.65345 | 3643.69 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | nocturne | t300_f60_422_partial | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | nocturne | t300_f60_422_partial | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
