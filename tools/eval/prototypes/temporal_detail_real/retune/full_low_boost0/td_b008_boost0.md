<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_b008_boost0 vs td_off (default)

Suite: `td_real_low` (8 cases). Candidate `td_b008_boost0` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (INFO 20, LIMIT 12, NA 48, PASS 156)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | 0.506787 | 0.506787 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| btb_kept_Y | NA | NA | NA | 4 | 4 | NA:4 |
| btb_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_C | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| clip_detect_Y | 0 | 0 | 0 | 4 | 2 | NA:2 PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 4 | 4 | NA:4 |
| cost_gpu_ms_laptop | NA | NA | NA | 4 | 4 | NA:4 |
| cost_ms_per_frame_pyref | 1163.81 | 0.000350003 | 1163.81 | 4 | 0 | INFO:4 |
| detail_pulse_Y | 0.270864 | 0.270892 | -2.79309e-05 | 4 | 0 | INFO:4 |
| duplicate_rate_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| flicker_excess_Y | -0.00545946 | -0.00546011 | 6.50399e-07 | 4 | 0 | PASS:4 |
| ghost_coeff_Y | 0.016705 | 0.0167176 | -1.25513e-05 | 4 | 0 | PASS:4 |
| gop_ratio_gop | NA | NA | NA | 4 | 4 | NA:4 |
| gop_ratio_minigop | NA | NA | NA | 4 | 4 | NA:4 |
| grain_hf_ratio_Cb | 1.02462 | 1.02462 | 0 | 4 | 0 | PASS:4 |
| grain_hf_ratio_Y | 1.0079 | 1.00789 | 7.07265e-06 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_Y | 1.00014 | 1 | 0.0001416 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band1_Y | 1.00013 | 1 | 0.000128059 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band2_Y | 1.00015 | 1 | 0.000147987 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band3_Y | 1.00014 | 1 | 0.000142864 | 4 | 0 | PASS:4 |
| grain_keep_vs_input_band4_Y | 1.00015 | 1 | 0.000148968 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Cb | -0.00283522 | -0.00283522 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_delta_Y | -0.00425549 | -0.00424619 | -9.30471e-06 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| grain_lag1_vs_input_Y | -9.30471e-06 | 0 | -9.30471e-06 | 4 | 0 | PASS:4 |
| grain_probe_keep_edge_vs_input_Y | 1.00001 | 1 | 6.54137e-06 | 4 | 0 | PASS:4 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1 | 1.20769e-06 | 4 | 0 | PASS:4 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 4 | 4 | NA:4 |
| grain_probe_lag1_flat_vs_input_Y | 2.92152e-06 | 0 | 2.92152e-06 | 4 | 0 | PASS:4 |
| grain_rms_ratio_Cb | 0.649164 | 0.649164 | 0 | 4 | 0 | LIMIT:4 |
| grain_rms_ratio_Y | 0.838818 | 0.838701 | 0.000116049 | 4 | 0 | LIMIT:4 |
| grain_tiles_Cb | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| grain_tiles_Y | 16 | 16 | 0 | 4 | 0 | INFO:4 |
| halo_overshoot_Y | 0.000253811 | 0.000253633 | 1.78487e-07 | 4 | 0 | PASS:4 |
| ib_gap_db_Y | 0.831321 | 0.831376 | -5.57783e-05 | 4 | 0 | INFO:4 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 4 | 0 | PASS:4 |
| lc_ratio_s1_Y | 0.860813 | 0.86075 | 6.26172e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s2_Y | 0.909058 | 0.909001 | 5.74079e-05 | 4 | 0 | PASS:4 |
| lc_ratio_s3_Y | 0.942654 | 0.942608 | 4.6006e-05 | 4 | 0 | PASS:4 |
| lpips | NA | NA | NA | 4 | 4 | NA:4 |
| lpips_B | 0.0710035 | 0.0710092 | -5.66019e-06 | 4 | 0 | PASS:4 |
| lpips_all | 0.0700361 | 0.0700405 | -4.39699e-06 | 4 | 0 | PASS:4 |
| psnr_B_Y | 40.0234 | 40.0233 | 4.71929e-05 | 4 | 0 | PASS:4 |
| psnr_Cb | 43.5401 | 43.5401 | 0 | 4 | 0 | PASS:4 |
| psnr_Cr | 43.0227 | 43.0227 | 0 | 4 | 0 | PASS:4 |
| psnr_I_Y | 41.3725 | 41.3724 | 0.000122655 | 4 | 0 | PASS:4 |
| psnr_P_Y | 40.8325 | 40.8326 | -1.86652e-05 | 4 | 0 | PASS:4 |
| psnr_Y | 40.2373 | 40.2373 | 3.35318e-05 | 4 | 0 | PASS:4 |
| smear_Y | 0.838451 | 0.838324 | 0.000127512 | 4 | 0 | PASS:4 |
| ssim_B_Y | 0.949045 | 0.949044 | 1.22338e-06 | 4 | 0 | PASS:4 |
| ssim_Cb | 0.953979 | 0.953979 | 0 | 4 | 0 | PASS:4 |
| ssim_Cr | 0.950863 | 0.950863 | 0 | 4 | 0 | PASS:4 |
| ssim_Y | 0.950196 | 0.950195 | 9.99651e-07 | 4 | 0 | PASS:4 |
| wtw_kept_C | NA | NA | NA | 4 | 4 | NA:4 |
| wtw_kept_Y | 0.064134 | 0.064134 | 0 | 4 | 2 | LIMIT:2 NA:2 |
| wtw_kept_vs_input_C | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 4 | 2 | NA:2 PASS:2 |

### nature30  (INFO 10, LIMIT 8, NA 24, PASS 76)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | 0.289276 | 0.287521 | 0.0017558 | 2 | 0 | LIMIT:2 |
| btb_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 866.119 | 0.000200002 | 866.118 | 2 | 0 | INFO:2 |
| detail_pulse_Y | 0.0927897 | 0.0933414 | -0.000551754 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0189907 | -0.0189989 | 8.18554e-06 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.0947245 | 0.0948756 | -0.000151024 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.720117 | 0.720117 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.00821 | 1.0082 | 1.12098e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00058 | 1 | 0.000577926 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00064 | 1 | 0.0006411 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00051 | 1 | 0.0005055 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00061 | 1 | 0.000612493 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00053 | 1 | 0.000532759 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.228602 | 0.228602 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | -0.00595722 | -0.00594601 | -1.12088e-05 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -1.12088e-05 | 0 | -1.12088e-05 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 1.00017 | 1 | 0.000166544 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 1.00001 | 1 | 5.60534e-06 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | 0.000341411 | 0 | 0.000341411 | 2 | 1 | NA:1 PASS:1 |
| grain_probe_lag1_flat_vs_input_Y | 6.29254e-06 | 0 | 6.29254e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.434904 | 0.434904 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.844958 | 0.844482 | 0.000475748 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.00037149 | 0.000370788 | 7.02e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 1.10522 | 1.10678 | -0.00155461 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.849257 | 0.849023 | 0.000234197 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.894352 | 0.894136 | 0.000215619 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.93037 | 0.930197 | 0.000172782 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | 0.0807912 | 0.0808249 | -3.36571e-05 | 2 | 0 | PASS:2 |
| lpips_all | 0.0781825 | 0.0782072 | -2.46384e-05 | 2 | 0 | PASS:2 |
| psnr_B_Y | 30.5924 | 30.5908 | 0.00156199 | 2 | 0 | PASS:2 |
| psnr_Cb | 37.5782 | 37.5782 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 39.9767 | 39.9767 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 33.4866 | 33.4856 | 0.00104304 | 2 | 0 | PASS:2 |
| psnr_P_Y | 31.5289 | 31.5289 | -6.74717e-05 | 2 | 0 | PASS:2 |
| psnr_Y | 30.8578 | 30.8566 | 0.00122487 | 2 | 0 | PASS:2 |
| smear_Y | 0.756263 | 0.755747 | 0.000516164 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.877033 | 0.876996 | 3.75786e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.903244 | 0.903244 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.951399 | 0.951399 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.881166 | 0.881137 | 2.88696e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |

### sports50_60  (INFO 10, LIMIT 10, NA 25, PASS 73)

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
| cost_ms_per_frame_pyref | 948.96 | 0.000250002 | 948.96 | 2 | 0 | INFO:2 |
| detail_pulse_Y | 0.256739 | 0.257434 | -0.000695396 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.1161 | -0.116125 | 2.51848e-05 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.281039 | 0.281078 | -3.84267e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 0.144673 | 0.144673 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 0.211814 | 0.211829 | -1.5409e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00004 | 1 | 3.91774e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00004 | 1 | 3.95652e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00004 | 1 | 3.64098e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00002 | 1 | 1.63494e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 0.999974 | 1 | -2.60334e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0.522177 | 0.522177 | 0 | 2 | 0 | LIMIT:2 |
| grain_lag1_delta_Y | 0.344719 | 0.344719 | 4.81856e-07 | 2 | 0 | LIMIT:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | 4.81856e-07 | 0 | 4.81856e-07 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 1.00022 | 1 | 0.000220579 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1 | 3.00074e-06 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 2 | 2 | NA:2 |
| grain_probe_lag1_flat_vs_input_Y | 1.26161e-06 | 0 | 1.26161e-06 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.0883628 | 0.0883628 | 0 | 2 | 0 | LIMIT:2 |
| grain_rms_ratio_Y | 0.317394 | 0.317381 | 1.28703e-05 | 2 | 0 | LIMIT:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 0.000830656 | 0.000829834 | 8.21822e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | 0.943159 | 0.943495 | -0.000335801 | 2 | 0 | INFO:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.652395 | 0.652153 | 0.00024161 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.735275 | 0.735041 | 0.000233889 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.807048 | 0.806848 | 0.000200131 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | 0.301171 | 0.30124 | -6.90583e-05 | 2 | 0 | PASS:2 |
| lpips_all | 0.292853 | 0.292905 | -5.19033e-05 | 2 | 0 | PASS:2 |
| psnr_B_Y | 26.712 | 26.7117 | 0.000234975 | 2 | 0 | PASS:2 |
| psnr_Cb | 33.8982 | 33.8982 | 0 | 2 | 0 | PASS:2 |
| psnr_Cr | 34.4315 | 34.4315 | 0 | 2 | 0 | PASS:2 |
| psnr_I_Y | 29.6011 | 29.6011 | 0 | 2 | 0 | PASS:2 |
| psnr_P_Y | 27.5653 | 27.5654 | -0.000104743 | 2 | 0 | PASS:2 |
| psnr_Y | 26.9285 | 26.9283 | 0.000163157 | 2 | 0 | PASS:2 |
| smear_Y | 0.490879 | 0.490425 | 0.000453475 | 2 | 0 | PASS:2 |
| ssim_B_Y | 0.709048 | 0.709023 | 2.54107e-05 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.83228 | 0.83228 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.85275 | 0.85275 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.714836 | 0.714817 | 1.94002e-05 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0 | 0 | 0 | 2 | 1 | LIMIT:1 NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_6k_low | psnr_Y | 46.0115 | 46.0116 | -2.0977e-05 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cb | 49.8587 | 49.8587 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_Cr | 49.306 | 49.306 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Y | 0.986486 | 0.986486 | -5.7309e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cb | 0.993395 | 0.993395 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_Cr | 0.992462 | 0.992462 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Y | 0.832455 | 0.832454 | 2.91919e-07 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00607 | 1.00607 | -3.23184e-07 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Y | -0.00417631 | -0.00417656 | 2.47206e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.56695 | 0.56695 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.00613 | 1.00613 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.00274585 | 0.00274585 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 3.50673e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Y | 2.47206e-07 | 0 | 2.47206e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | flicker_excess_Y | -0.000201375 | -0.000201539 | 1.64207e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_2160p_6k_low | ghost_coeff_Y | 0.00353347 | 0.00353428 | -8.14398e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | smear_Y | 0.885169 | 0.885163 | 6.71448e-06 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | halo_overshoot_Y | 2.50309e-05 | 2.50286e-05 | 2.26948e-09 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s1_Y | 0.85659 | 0.856587 | 2.92492e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s2_Y | 0.926319 | 0.926316 | 2.85289e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_ratio_s3_Y | 0.964884 | 0.964882 | 2.51259e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_2160p_6k_low | lpips_all | 0.0582693 | 0.0582708 | -1.43045e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_I_Y | 46.31 | 46.31 | 1.64722e-05 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_P_Y | 46.5035 | 46.5035 | 9.92175e-07 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | psnr_B_Y | 45.8427 | 45.8428 | -2.88018e-05 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ssim_B_Y | 0.986186 | 0.986186 | -7.82323e-08 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | lpips_B | 0.0592305 | 0.0592325 | -1.95362e-06 | same | PASS |  |
| film24 | tos | x265_2160p_6k_low | ib_gap_db_Y | 0.641026 | 0.640994 | 3.14052e-05 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | detail_pulse_Y | 0.232474 | 0.232474 | 1.45587e-07 | NA | INFO |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 6.04754e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 3.45076e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 2.99575e-07 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1 | 1 | -3.09314e-08 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 0.999996 | 1 | -4.20388e-06 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | tos | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | -8.50379e-09 | worse | PASS |  |
| film24 | tos | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | -1.96759e-10 | 0 | -1.96759e-10 | better | PASS |  |
| film24 | tos | x265_2160p_6k_low | cost_ms_per_frame_pyref | 991.564 | 0.000200002 | 991.564 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_2160p_6k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_2160p_6k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos | x265_1080p_2.5k_low | psnr_Y | 45.1419 | 45.1418 | 7.57114e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cb | 49.7532 | 49.7532 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_Cr | 49.358 | 49.358 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Y | 0.98652 | 0.98652 | 1.21672e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cb | 0.992445 | 0.992445 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_Cr | 0.991597 | 0.991597 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.880063 | 0.88006 | 2.77476e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.01008 | 1.01008 | -3.85099e-07 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.00908628 | -0.00908631 | 3.95847e-08 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.621474 | 0.621474 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | tos | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 1.09208 | 1.09208 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_delta_Cb | -0.0248142 | -0.0248142 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1 | 1 | 3.15292e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 3.95847e-08 | 0 | 3.95847e-08 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | flicker_excess_Y | -0.00165484 | -0.00165591 | 1.06587e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | tos | x265_1080p_2.5k_low | ghost_coeff_Y | 0.00502624 | 0.00503012 | -3.88467e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | smear_Y | 0.936901 | 0.936867 | 3.37403e-05 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | halo_overshoot_Y | 4.47266e-05 | 4.47107e-05 | 1.59723e-08 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.922333 | 0.922315 | 1.73575e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.95513 | 0.955114 | 1.59016e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.975333 | 0.97532 | 1.23572e-05 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | tos | x265_1080p_2.5k_low | lpips_all | 0.0403068 | 0.0403076 | -7.9397e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_I_Y | 43.8621 | 43.862 | 4.12182e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_P_Y | 45.7633 | 45.7633 | -3.18424e-05 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | psnr_B_Y | 45.001 | 45.0009 | 0.000108672 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ssim_B_Y | 0.98637 | 0.98637 | 1.70162e-07 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | lpips_B | 0.0415714 | 0.0415726 | -1.12515e-06 | same | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | ib_gap_db_Y | 0.530066 | 0.530196 | -0.000129785 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | detail_pulse_Y | 0.0694638 | 0.069476 | -1.2177e-05 | NA | INFO |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 4.2214e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1 | 1 | 2.69279e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 2.81783e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1 | 1 | 3.1771e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1 | 1 | 1.86932e-06 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | -2.03825e-07 | worse | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | -1.61106e-08 | 0 | -1.61106e-08 | better | PASS |  |
| film24 | tos | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 1079.83 | 0.000600005 | 1079.83 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos | x265_1080p_2.5k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos | x265_1080p_2.5k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_2160p_6k_low | psnr_Y | 36.3358 | 36.3357 | 9.01701e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cb | 38.3665 | 38.3665 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_Cr | 37.7567 | 37.7567 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Y | 0.934444 | 0.934444 | -8.48764e-08 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cb | 0.935095 | 0.935095 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_Cr | 0.930773 | 0.930773 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Y | 0.829626 | 0.829447 | 0.000179013 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Y | 1.00606 | 1.00608 | -2.4589e-05 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Y | -0.00338802 | -0.00339858 | 1.05649e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.730734 | 0.730734 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | grain_hf_ratio_Cb | 1.0123 | 1.0123 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_delta_Cb | -0.000732844 | -0.000732844 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00022 | 1 | 0.000215823 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Y | 1.05649e-05 | 0 | 1.05649e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | flicker_excess_Y | -0.00701573 | -0.00701674 | 1.00943e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_2160p_6k_low | ghost_coeff_Y | 0.0209634 | 0.0209735 | -1.01698e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | smear_Y | 0.796521 | 0.796392 | 0.000128331 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | halo_overshoot_Y | 0.000401806 | 0.000401656 | 1.50051e-07 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s1_Y | 0.835289 | 0.83523 | 5.90878e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s2_Y | 0.881117 | 0.881062 | 5.50658e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_ratio_s3_Y | 0.917264 | 0.91722 | 4.3962e-05 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | wtw_kept_Y | 0.0588235 | 0.0588235 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | btb_kept_C | 0.705882 | 0.705882 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_2160p_6k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_all | 0.0839267 | 0.0839357 | -9.06598e-06 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_I_Y | 38.5995 | 38.5995 | 1.1702e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_P_Y | 36.8976 | 36.8976 | -1.04218e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | psnr_B_Y | 36.0925 | 36.0924 | 0.000120792 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ssim_B_Y | 0.933618 | 0.933618 | -2.8776e-08 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | lpips_B | 0.08447 | 0.0844818 | -1.18537e-05 | same | PASS |  |
| film24 | sol | x265_2160p_6k_low | ib_gap_db_Y | 0.94828 | 0.948409 | -0.000129669 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | detail_pulse_Y | 0.448704 | 0.448807 | -0.000102931 | NA | INFO |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00023 | 1 | 0.00023182 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00022 | 1 | 0.000224742 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00021 | 1 | 0.000206371 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.0002 | 1 | 0.000199716 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 0.999995 | 1 | -5.3228e-06 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | sol | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 1.81489e-07 | worse | PASS |  |
| film24 | sol | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | -3.46143e-08 | 0 | -3.46143e-08 | better | PASS |  |
| film24 | sol | x265_2160p_6k_low | cost_ms_per_frame_pyref | 1354.32 | 0.000400003 | 1354.32 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_2160p_6k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_2160p_6k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | sol | x265_1080p_2.5k_low | psnr_Y | 33.4601 | 33.4601 | -1.07773e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cb | 36.1821 | 36.1821 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_Cr | 35.67 | 35.67 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Y | 0.893332 | 0.893328 | 4.01912e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cb | 0.894982 | 0.894982 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_Cr | 0.88862 | 0.88862 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.813127 | 0.812844 | 0.000282117 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Y | 1.00939 | 1.00933 | 5.35879e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Y | -0.000371357 | -0.000323286 | -4.80705e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.677498 | 0.677498 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.987968 | 0.987968 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.0114603 | 0.0114603 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00035 | 1 | 0.000347074 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | -4.80705e-05 | 0 | -4.80705e-05 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | flicker_excess_Y | -0.0129659 | -0.0129662 | 3.62085e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 48 longer than half the window (72 frames) |
| film24 | sol | x265_1080p_2.5k_low | ghost_coeff_Y | 0.037297 | 0.0373323 | -3.53361e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | smear_Y | 0.735215 | 0.734874 | 0.000341263 | better | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000543683 | 0.000543137 | 5.45656e-07 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.829039 | 0.828868 | 0.000171098 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.873666 | 0.87351 | 0.000155811 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.913134 | 0.913009 | 0.000125192 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_Y | 0.0694444 | 0.0694444 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | btb_kept_C | 0.307692 | 0.307692 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| film24 | sol | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | sol | x265_1080p_2.5k_low | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_all | 0.0976417 | 0.097648 | -6.29756e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_I_Y | 36.7184 | 36.7179 | 0.000421226 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_P_Y | 34.1658 | 34.1658 | -3.33889e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | psnr_B_Y | 33.1573 | 33.1573 | -1.18908e-05 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ssim_B_Y | 0.890005 | 0.89 | 4.83035e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | lpips_B | 0.0987423 | 0.09875 | -7.70825e-06 | same | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | ib_gap_db_Y | 1.20591 | 1.20591 | 4.93498e-06 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | detail_pulse_Y | 0.332813 | 0.33281 | 3.23867e-06 | NA | INFO |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00028 | 1 | 0.000275589 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00036 | 1 | 0.000364167 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00036 | 1 | 0.000361967 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00039 | 1 | 0.000393009 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1.00003 | 1 | 3.38228e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 4.86161e-06 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 1.1737e-05 | 0 | 1.1737e-05 | worse | PASS |  |
| film24 | sol | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 1229.53 | 0.000200002 | 1229.53 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | sol | x265_1080p_2.5k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | sol | x265_1080p_2.5k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_2160p_6k_low | psnr_Y | 32.9628 | 32.962 | 0.00074694 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cb | 38.1011 | 38.1011 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_Cr | 41.2014 | 41.2014 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Y | 0.913945 | 0.913936 | 9.71635e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cb | 0.912287 | 0.912287 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_Cr | 0.962992 | 0.962992 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Y | 0.891917 | 0.891634 | 0.00028271 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Y | 1.05435 | 1.05432 | 2.31652e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Y | -0.0332906 | -0.0332502 | -4.04263e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.548917 | 0.548917 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.884798 | 0.884798 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.132443 | 0.132443 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Y | 1.00032 | 1 | 0.00031707 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Y | -4.04263e-05 | 0 | -4.04263e-05 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | flicker_excess_Y | -0.0032541 | -0.00326326 | 9.15683e-06 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (72 frames) |
| nature30 | boat | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (72 frames) |
| nature30 | boat | x265_2160p_6k_low | ghost_coeff_Y | 0.0670146 | 0.067087 | -7.23173e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | smear_Y | 0.796683 | 0.796423 | 0.000259574 | better | PASS |  |
| nature30 | boat | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | halo_overshoot_Y | 0.000331982 | 0.000331668 | 3.13417e-07 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s1_Y | 0.871235 | 0.871117 | 0.000118235 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s2_Y | 0.911083 | 0.910975 | 0.000108833 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_ratio_s3_Y | 0.942384 | 0.942298 | 8.60304e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_Y | 0.361501 | 0.359474 | 0.0020275 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_2160p_6k_low | lpips_all | 0.0658718 | 0.0658872 | -1.53698e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_I_Y | 35.3392 | 35.3389 | 0.000261854 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_P_Y | 33.6144 | 33.6145 | -7.52957e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | psnr_B_Y | 32.7073 | 32.7063 | 0.000970919 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ssim_B_Y | 0.911581 | 0.911568 | 1.29394e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | lpips_B | 0.0673473 | 0.067368 | -2.0684e-05 | same | PASS |  |
| nature30 | boat | x265_2160p_6k_low | ib_gap_db_Y | 1.05964 | 1.06066 | -0.0010215 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | detail_pulse_Y | 0.0722579 | 0.0725941 | -0.000336236 | NA | INFO |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1.00039 | 1 | 0.000391918 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00023 | 1 | 0.000225033 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1.00027 | 1 | 0.000269243 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 1.00039 | 1 | 0.000390373 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1.00005 | 1 | 5.14613e-05 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| nature30 | boat | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 1.71448e-06 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 2.8147e-07 | 0 | 2.8147e-07 | worse | PASS |  |
| nature30 | boat | x265_2160p_6k_low | cost_ms_per_frame_pyref | 858.56 | 0.000200002 | 858.56 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_2160p_6k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_2160p_6k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Y | 28.7528 | 28.7511 | 0.0017028 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cb | 37.0553 | 37.0553 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_Cr | 38.7521 | 38.7521 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Y | 0.848387 | 0.848339 | 4.80229e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cb | 0.894201 | 0.894201 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_Cr | 0.939806 | 0.939806 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.797999 | 0.79733 | 0.000668786 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.962079 | 0.96208 | -7.45517e-07 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.0213762 | 0.0213582 | 1.80088e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.320892 | 0.320892 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.555435 | 0.555435 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.324761 | 0.324761 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00084 | 1 | 0.000838781 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 1.80088e-05 | 0 | 1.80088e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | flicker_excess_Y | -0.0347273 | -0.0347345 | 7.21425e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (72 frames) |
| nature30 | boat | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 60 longer than half the window (72 frames) |
| nature30 | boat | x265_1080p_2.5k_low | ghost_coeff_Y | 0.122434 | 0.122664 | -0.000229731 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | smear_Y | 0.715844 | 0.715071 | 0.000772755 | better | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000410998 | 0.000409907 | 1.09058e-06 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.827279 | 0.826929 | 0.00035016 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.877621 | 0.877298 | 0.000322405 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.918356 | 0.918096 | 0.000259534 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_Y | 0.217051 | 0.215567 | 0.00148411 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | boat | x265_1080p_2.5k_low | lpips_all | 0.0904933 | 0.0905272 | -3.3907e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_I_Y | 31.6341 | 31.6322 | 0.00182423 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_P_Y | 29.4433 | 29.4434 | -5.96478e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | psnr_B_Y | 28.4775 | 28.4754 | 0.00215306 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ssim_B_Y | 0.842486 | 0.842423 | 6.22178e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | lpips_B | 0.0942351 | 0.0942817 | -4.66301e-05 | same | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | ib_gap_db_Y | 1.1508 | 1.15289 | -0.00208772 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | detail_pulse_Y | 0.113321 | 0.114089 | -0.000767273 | NA | INFO |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00089 | 1 | 0.000890283 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00079 | 1 | 0.000785966 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00096 | 1 | 0.000955742 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 1.00068 | 1 | 0.000675146 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1.00028 | 1 | 0.000281627 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | 0.000341411 | 0 | 0.000341411 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1.00001 | 1 | 9.4962e-06 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 1.23036e-05 | 0 | 1.23036e-05 | worse | PASS |  |
| nature30 | boat | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 873.677 | 0.000200002 | 873.677 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | boat | x265_1080p_2.5k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | boat | x265_1080p_2.5k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Y | 28.1054 | 28.1053 | 0.000120299 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cb | 34.6286 | 34.6286 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_Cr | 35.2309 | 35.2309 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Y | 0.74304 | 0.743033 | 7.14543e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cb | 0.846094 | 0.846094 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_Cr | 0.867872 | 0.867872 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Y | 0.3048 | 0.304799 | 1.3767e-06 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Y | 0.211947 | 0.211949 | -2.53464e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Y | 0.449196 | 0.449195 | 4.78232e-07 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_rms_ratio_Cb | 0.0854915 | 0.0854915 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_hf_ratio_Cb | 0.147825 | 0.147825 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_delta_Cb | 0.60079 | 0.60079 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Y | 1 | 1 | 4.51675e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Y | 4.78232e-07 | 0 | 4.78232e-07 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | flicker_excess_Y | -0.107554 | -0.10757 | 1.55454e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (72 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (72 frames) |
| sports50_60 | crowd | x265_2160p_6k_low | ghost_coeff_Y | 0.252596 | 0.252626 | -2.95695e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | smear_Y | 0.502421 | 0.502166 | 0.000255048 | better | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | halo_overshoot_Y | 0.000838719 | 0.000838367 | 3.51664e-07 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s1_Y | 0.643622 | 0.643493 | 0.000129298 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s2_Y | 0.730014 | 0.729884 | 0.0001301 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_ratio_s3_Y | 0.805959 | 0.805846 | 0.00011237 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_all | 0.314786 | 0.314836 | -5.00974e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_I_Y | 31.5195 | 31.5195 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_P_Y | 28.8774 | 28.8775 | -7.01852e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | psnr_B_Y | 27.8468 | 27.8466 | 0.000168554 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ssim_B_Y | 0.737856 | 0.737846 | 9.44429e-06 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | lpips_B | 0.322556 | 0.322622 | -6.5865e-05 | same | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | ib_gap_db_Y | 1.14195 | 1.14219 | -0.000236562 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | detail_pulse_Y | 0.292197 | 0.292613 | -0.000416656 | NA | INFO |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band1_Y | 1 | 1 | 4.32182e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band2_Y | 1.00001 | 1 | 7.90547e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band3_Y | 1 | 1 | 2.75428e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_keep_vs_input_band4_Y | 0.999992 | 1 | -8.39166e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_keep_edge_vs_input_Y | 1.00018 | 1 | 0.000184978 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 3.29594e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | grain_probe_lag1_flat_vs_input_Y | 1.02466e-06 | 0 | 1.02466e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_2160p_6k_low | cost_ms_per_frame_pyref | 1010.74 | 0.000300002 | 1010.74 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_2160p_6k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_2160p_6k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Y | 25.7516 | 25.7514 | 0.000206015 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cb | 33.1679 | 33.1679 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_Cr | 33.6322 | 33.6322 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Y | 0.686633 | 0.686601 | 3.1655e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cb | 0.818466 | 0.818466 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_Cr | 0.837628 | 0.837628 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Y | 0.329988 | 0.329964 | 2.43639e-05 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Y | 0.211681 | 0.21171 | -2.82833e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Y | 0.240243 | 0.240242 | 4.85481e-07 | worse | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_rms_ratio_Cb | 0.091234 | 0.091234 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_hf_ratio_Cb | 0.141521 | 0.141521 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_delta_Cb | 0.443565 | 0.443565 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Y | 1.00007 | 1 | 7.3838e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Y | 4.85481e-07 | 0 | 4.85481e-07 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | flicker_excess_Y | -0.124645 | -0.12468 | 3.48242e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (72 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (72 frames) |
| sports50_60 | crowd | x265_1080p_2.5k_low | ghost_coeff_Y | 0.309482 | 0.309529 | -4.72838e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | smear_Y | 0.479337 | 0.478685 | 0.000651901 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | halo_overshoot_Y | 0.000822593 | 0.000821301 | 1.29198e-06 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s1_Y | 0.661167 | 0.660813 | 0.000353921 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s2_Y | 0.740535 | 0.740197 | 0.000337678 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_ratio_s3_Y | 0.808138 | 0.80785 | 0.000287893 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_Y | 0 | 0 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_all | 0.270919 | 0.270973 | -5.37091e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_I_Y | 27.6828 | 27.6828 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_P_Y | 26.2533 | 26.2534 | -0.000139301 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | psnr_B_Y | 25.5771 | 25.5768 | 0.000301397 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ssim_B_Y | 0.68024 | 0.680199 | 4.13771e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | lpips_B | 0.279786 | 0.279859 | -7.22515e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | ib_gap_db_Y | 0.744369 | 0.744804 | -0.000435041 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | detail_pulse_Y | 0.221281 | 0.222255 | -0.000974136 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band1_Y | 1.00007 | 1 | 7.48086e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band2_Y | 1.00006 | 1 | 6.49142e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band3_Y | 1.00003 | 1 | 2.99444e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_keep_vs_input_band4_Y | 0.999956 | 1 | -4.36751e-05 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_keep_edge_vs_input_Y | 1.00026 | 1 | 0.00025618 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 2.70554e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | grain_probe_lag1_flat_vs_input_Y | 1.49856e-06 | 0 | 1.49856e-06 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_2.5k_low | cost_ms_per_frame_pyref | 887.182 | 0.000200002 | 887.182 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_1080p_2.5k_low | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_1080p_2.5k_low | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
