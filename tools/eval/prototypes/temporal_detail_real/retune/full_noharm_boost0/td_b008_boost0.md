<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_b008_boost0 vs td_off (default)

Suite: `td_noharm` (5 cases). Candidate `td_b008_boost0` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors (+forward)), default `td_off`. LPIPS not requested (NA). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### dark  (FLAG 1, INFO 4, NA 20, PASS 34)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| btb_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| clip_detect_C | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 1 | 1 | NA:1 |
| cost_gpu_ms_laptop | NA | NA | NA | 1 | 1 | NA:1 |
| cost_ms_per_frame_pyref | 1207.44 | 0.000399988 | 1207.44 | 1 | 0 | INFO:1 |
| detail_pulse_Y | 9.12838e-05 | 0 | 9.12838e-05 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| flicker_excess_Y | 4.89945e-07 | 0 | 4.89945e-07 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | -9.66394e-06 | 0 | -9.66394e-06 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band1_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band2_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band3_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band4_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_probe_keep_edge_vs_input_Y | 0.999993 | 1 | -6.54611e-06 | 1 | 0 | PASS:1 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1 | -4.57115e-07 | 1 | 0 | PASS:1 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |
| grain_probe_lag1_flat_vs_input_Y | -2.36398e-07 | 0 | -2.36398e-07 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 1.75029e-07 | 0 | 1.75029e-07 | 1 | 0 | PASS:1 |
| ib_gap_db_Y | NA | NA | NA | 1 | 1 | NA:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 1.00004 | 1 | 3.91987e-05 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 1.00004 | 1 | 3.76596e-05 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 1.00003 | 1 | 3.04594e-05 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_B | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_all | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_Cr | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_I_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_P_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Y | 95.4204 | NA | NA | 1 | 0 | FLAG:1 |
| smear_Y | 1.00007 | 1 | 6.82558e-05 | 1 | 0 | PASS:1 |
| ssim_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| ssim_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 1 | 1 | -9.31493e-08 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |

### film24  (FLAG 2, INFO 8, NA 35, PASS 73)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| btb_kept_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| btb_kept_vs_input_C | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 2 | 1 | NA:1 PASS:1 |
| clip_detect_C | 0 | 0 | 0 | 2 | 1 | NA:1 PASS:1 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 657.472 | 0.000250002 | 657.472 | 2 | 0 | INFO:2 |
| detail_pulse_Y | 0.000559417 | 0 | 0.000559417 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | 2.14042e-08 | 0 | 2.14042e-08 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | -1.24586e-05 | 0 | -1.24586e-05 | 2 | 0 | PASS:2 |
| gop_ratio_gop | NA | NA | NA | 2 | 2 | NA:2 |
| gop_ratio_minigop | NA | NA | NA | 2 | 2 | NA:2 |
| grain_hf_ratio_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_hf_ratio_Y | 1.00001 | 1 | 6.70156e-06 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_Y | 1.00008 | 1 | 7.61861e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band1_Y | 1.00007 | 1 | 7.06818e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band2_Y | 1.00008 | 1 | 7.80361e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band3_Y | 1.00008 | 1 | 7.88007e-05 | 2 | 0 | PASS:2 |
| grain_keep_vs_input_band4_Y | 1.00008 | 1 | 8.11401e-05 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_delta_Y | -1.41272e-05 | 0 | -1.41272e-05 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| grain_lag1_vs_input_Y | -1.41272e-05 | 0 | -1.41272e-05 | 2 | 0 | PASS:2 |
| grain_probe_keep_edge_vs_input_Y | 1.00007 | 1 | 6.69548e-05 | 2 | 0 | PASS:2 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1 | 8.91211e-07 | 2 | 0 | PASS:2 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 2 | 2 | NA:2 |
| grain_probe_lag1_flat_vs_input_Y | -9.52845e-09 | 0 | -9.52845e-09 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Y | 1.00008 | 1 | 7.61861e-05 | 2 | 0 | PASS:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 3.54283e-07 | 0 | 3.54283e-07 | 2 | 0 | PASS:2 |
| ib_gap_db_Y | NA | NA | NA | 2 | 2 | NA:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 1.00016 | 1 | 0.000158345 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 1.00015 | 1 | 0.000147379 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 1.00012 | 1 | 0.000120985 | 2 | 0 | PASS:2 |
| lpips | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_B | NA | NA | NA | 2 | 2 | NA:2 |
| lpips_all | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_B_Y | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_Cb | NA | NA | NA | 2 | 0 | PASS:2 |
| psnr_Cr | NA | NA | NA | 2 | 0 | PASS:2 |
| psnr_I_Y | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_P_Y | NA | NA | NA | 2 | 2 | NA:2 |
| psnr_Y | 95.2892 | NA | NA | 2 | 0 | FLAG:2 |
| smear_Y | 1.00056 | 1 | 0.000560897 | 2 | 0 | PASS:2 |
| ssim_B_Y | NA | NA | NA | 2 | 2 | NA:2 |
| ssim_Cb | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| ssim_Cr | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| ssim_Y | 0.999999 | 1 | -5.52117e-07 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |
| wtw_kept_vs_input_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 2 | 0 | PASS:2 |

### nature30  (FLAG 1, INFO 4, NA 18, PASS 36)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| btb_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| clip_detect_C | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 1 | 1 | NA:1 |
| cost_gpu_ms_laptop | NA | NA | NA | 1 | 1 | NA:1 |
| cost_ms_per_frame_pyref | 742.353 | 0.000400003 | 742.352 | 1 | 0 | INFO:1 |
| detail_pulse_Y | 0.00323663 | 0 | 0.00323663 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| flicker_excess_Y | 7.48688e-05 | 0 | 7.48688e-05 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | -0.000283267 | 0 | -0.000283267 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 0.999836 | 1 | -0.000164063 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Y | 1.00071 | 1 | 0.000708863 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band1_Y | 1.00067 | 1 | 0.000672244 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band2_Y | 1.00083 | 1 | 0.000834881 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band3_Y | 1.00069 | 1 | 0.000687394 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band4_Y | 1.00054 | 1 | 0.000543039 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Y | 3.28103e-05 | 0 | 3.28103e-05 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Y | 3.28103e-05 | 0 | 3.28103e-05 | 1 | 0 | PASS:1 |
| grain_probe_keep_edge_vs_input_Y | 1.00463 | 1 | 0.00462508 | 1 | 0 | PASS:1 |
| grain_probe_keep_flat_vs_input_Y | 1.00006 | 1 | 6.03447e-05 | 1 | 0 | PASS:1 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |
| grain_probe_lag1_flat_vs_input_Y | 4.15275e-06 | 0 | 4.15275e-06 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Y | 1.00071 | 1 | 0.000708863 | 1 | 0 | PASS:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 3.77071e-06 | 0 | 3.77071e-06 | 1 | 0 | PASS:1 |
| ib_gap_db_Y | NA | NA | NA | 1 | 1 | NA:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 1.00108 | 1 | 0.001079 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 1.00092 | 1 | 0.000923319 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 1.00074 | 1 | 0.000743301 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_B | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_all | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_Cr | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_I_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_P_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Y | 69.2824 | NA | NA | 1 | 0 | FLAG:1 |
| smear_Y | 1.00422 | 1 | 0.00422083 | 1 | 0 | PASS:1 |
| ssim_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| ssim_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 0.999992 | 1 | -8.35923e-06 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |

### sports50_60  (FLAG 1, INFO 4, NA 17, PASS 37)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| btb_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| clip_detect_C | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 1 | 1 | NA:1 |
| cost_gpu_ms_laptop | NA | NA | NA | 1 | 1 | NA:1 |
| cost_ms_per_frame_pyref | 1027.58 | 0.000299988 | 1027.58 | 1 | 0 | INFO:1 |
| detail_pulse_Y | 0.000196051 | 0 | 0.000196051 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| flicker_excess_Y | 3.10965e-05 | 0 | 3.10965e-05 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | -6.05094e-06 | 0 | -6.05094e-06 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 1 | 1 | 6.58807e-07 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Y | 1 | 1 | 1.68071e-06 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band1_Y | 1 | 1 | 1.72117e-06 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band2_Y | 1 | 1 | 2.25978e-06 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band3_Y | 1 | 1 | 4.01505e-06 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band4_Y | 1 | 1 | -2.95869e-07 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Y | 8.74175e-08 | 0 | 8.74175e-08 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Y | 8.74175e-08 | 0 | 8.74175e-08 | 1 | 0 | PASS:1 |
| grain_probe_keep_edge_vs_input_Y | 1.00001 | 1 | 9.92609e-06 | 1 | 0 | PASS:1 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1 | 4.24626e-08 | 1 | 0 | PASS:1 |
| grain_probe_lag1_edge_vs_input_Y | 6.43807e-06 | 0 | 6.43807e-06 | 1 | 0 | PASS:1 |
| grain_probe_lag1_flat_vs_input_Y | 8.26624e-08 | 0 | 8.26624e-08 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Y | 1 | 1 | 1.68071e-06 | 1 | 0 | PASS:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 3.42581e-07 | 0 | 3.42581e-07 | 1 | 0 | PASS:1 |
| ib_gap_db_Y | NA | NA | NA | 1 | 1 | NA:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 1.00006 | 1 | 5.53686e-05 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 1.00005 | 1 | 4.86104e-05 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 1.00004 | 1 | 3.97296e-05 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_B | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_all | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Cb | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_Cr | NA | NA | NA | 1 | 0 | PASS:1 |
| psnr_I_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_P_Y | NA | NA | NA | 1 | 1 | NA:1 |
| psnr_Y | 94.2022 | NA | NA | 1 | 0 | FLAG:1 |
| smear_Y | 1.00015 | 1 | 0.000148561 | 1 | 0 | PASS:1 |
| ssim_B_Y | NA | NA | NA | 1 | 1 | NA:1 |
| ssim_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 1 | 1 | -3.62391e-08 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| film24 | nh_35mm | noharm_input_ref | psnr_Y | 110.259 | inf | inf | worse | FLAG |  |
| film24 | nh_35mm | noharm_input_ref | psnr_Cb | inf | inf | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | psnr_Cr | inf | inf | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | ssim_Y | 1 | 1 | -2.02301e-09 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | ssim_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | ssim_Cr | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | nh_35mm | noharm_input_ref | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | nh_35mm | noharm_input_ref | grain_rms_ratio_Y | 1 | 1 | 4.49037e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_hf_ratio_Y | 1 | 1 | 4.90617e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_lag1_delta_Y | -6.59314e-09 | 0 | -6.59314e-09 | better | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | nh_35mm | noharm_input_ref | grain_rms_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_hf_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_lag1_delta_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_Y | 1 | 1 | 4.49037e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_lag1_vs_input_Y | -6.59314e-09 | 0 | -6.59314e-09 | better | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | btb_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | flicker_excess_Y | -3.36011e-08 | 0 | -3.36011e-08 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nh_35mm | noharm_input_ref | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nh_35mm | noharm_input_ref | ghost_coeff_Y | -1.11089e-07 | 0 | -1.11089e-07 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | smear_Y | 1.00001 | 1 | 7.50728e-06 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | halo_overshoot_Y | 5.95736e-09 | 0 | 5.95736e-09 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | lc_ratio_s1_Y | 1 | 1 | 3.17649e-06 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | lc_ratio_s2_Y | 1 | 1 | 2.90285e-06 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | lc_ratio_s3_Y | 1 | 1 | 2.24689e-06 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | btb_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | wtw_kept_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nh_35mm | noharm_input_ref | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | nh_35mm | noharm_input_ref | psnr_I_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | psnr_P_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | psnr_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | ssim_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | lpips_B | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | ib_gap_db_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_35mm | noharm_input_ref | detail_pulse_Y | 2.73281e-05 | 0 | 2.73281e-05 | NA | INFO |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_band1_Y | 1 | 1 | 8.95037e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_band2_Y | 1 | 1 | -4.8633e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_band3_Y | 1 | 1 | 7.21096e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_keep_vs_input_band4_Y | 1 | 1 | 6.65833e-08 | worse | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_probe_keep_edge_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | nh_35mm | noharm_input_ref | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 6.21332e-10 | same | PASS |  |
| film24 | nh_35mm | noharm_input_ref | grain_probe_lag1_flat_vs_input_Y | -8.00099e-10 | 0 | -8.00099e-10 | better | PASS |  |
| film24 | nh_35mm | noharm_input_ref | cost_ms_per_frame_pyref | 682.317 | 0.000300002 | 682.316 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | nh_35mm | noharm_input_ref | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | nh_35mm | noharm_input_ref | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | nh_16mm | noharm_input_ref | psnr_Y | 80.319 | inf | inf | worse | FLAG |  |
| film24 | nh_16mm | noharm_input_ref | psnr_Cb | inf | inf | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | psnr_Cr | inf | inf | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | ssim_Y | 0.999999 | 1 | -1.10221e-06 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | ssim_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | ssim_Cr | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| film24 | nh_16mm | noharm_input_ref | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | nh_16mm | noharm_input_ref | grain_rms_ratio_Y | 1.00015 | 1 | 0.000152327 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_hf_ratio_Y | 1.00001 | 1 | 1.33541e-05 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_lag1_delta_Y | -2.82478e-05 | 0 | -2.82478e-05 | better | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | nh_16mm | noharm_input_ref | grain_rms_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_hf_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_lag1_delta_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_Y | 1.00015 | 1 | 0.000152327 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_lag1_vs_input_Y | -2.82478e-05 | 0 | -2.82478e-05 | better | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | btb_kept_vs_input_C | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| film24 | nh_16mm | noharm_input_ref | flicker_excess_Y | 7.64094e-08 | 0 | 7.64094e-08 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nh_16mm | noharm_input_ref | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| film24 | nh_16mm | noharm_input_ref | ghost_coeff_Y | -2.4806e-05 | 0 | -2.4806e-05 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | smear_Y | 1.00111 | 1 | 0.00111429 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | halo_overshoot_Y | 7.02608e-07 | 0 | 7.02608e-07 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lc_ratio_s1_Y | 1.00031 | 1 | 0.000313513 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lc_ratio_s2_Y | 1.00029 | 1 | 0.000291855 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lc_ratio_s3_Y | 1.00024 | 1 | 0.000239724 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | btb_kept_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | wtw_kept_Y | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | btb_kept_C | 1 | 1 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| film24 | nh_16mm | noharm_input_ref | clip_detect_C | 0 | 0 | 0 | same | PASS |  |
| film24 | nh_16mm | noharm_input_ref | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| film24 | nh_16mm | noharm_input_ref | psnr_I_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | psnr_P_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | psnr_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | ssim_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | lpips_B | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | ib_gap_db_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| film24 | nh_16mm | noharm_input_ref | detail_pulse_Y | 0.00109151 | 0 | 0.00109151 | NA | INFO |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_band1_Y | 1.00014 | 1 | 0.000141274 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_band2_Y | 1.00016 | 1 | 0.000156121 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_band3_Y | 1.00016 | 1 | 0.000157529 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_keep_vs_input_band4_Y | 1.00016 | 1 | 0.000162214 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_probe_keep_edge_vs_input_Y | 1.00013 | 1 | 0.00013391 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| film24 | nh_16mm | noharm_input_ref | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 1.7818e-06 | worse | PASS |  |
| film24 | nh_16mm | noharm_input_ref | grain_probe_lag1_flat_vs_input_Y | -1.82568e-08 | 0 | -1.82568e-08 | better | PASS |  |
| film24 | nh_16mm | noharm_input_ref | cost_ms_per_frame_pyref | 632.628 | 0.000200002 | 632.628 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | nh_16mm | noharm_input_ref | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | nh_16mm | noharm_input_ref | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| dark | nh_dark | noharm_input_ref | psnr_Y | 95.4204 | inf | inf | worse | FLAG |  |
| dark | nh_dark | noharm_input_ref | psnr_Cb | inf | inf | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | psnr_Cr | inf | inf | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | ssim_Y | 1 | 1 | -9.31493e-08 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | ssim_Cb | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | ssim_Cr | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| dark | nh_dark | noharm_input_ref | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| dark | nh_dark | noharm_input_ref | grain_rms_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_hf_ratio_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_lag1_delta_Y | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| dark | nh_dark | noharm_input_ref | grain_rms_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_hf_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_lag1_delta_Cb | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_lag1_vs_input_Y | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | wtw_kept_vs_input_Y | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | flicker_excess_Y | 4.89945e-07 | 0 | 4.89945e-07 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| dark | nh_dark | noharm_input_ref | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| dark | nh_dark | noharm_input_ref | ghost_coeff_Y | -9.66394e-06 | 0 | -9.66394e-06 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | smear_Y | 1.00007 | 1 | 6.82558e-05 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | halo_overshoot_Y | 1.75029e-07 | 0 | 1.75029e-07 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | lc_ratio_s1_Y | 1.00004 | 1 | 3.91987e-05 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | lc_ratio_s2_Y | 1.00004 | 1 | 3.76596e-05 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | lc_ratio_s3_Y | 1.00003 | 1 | 3.04594e-05 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | btb_kept_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | wtw_kept_Y | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| dark | nh_dark | noharm_input_ref | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| dark | nh_dark | noharm_input_ref | psnr_I_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | psnr_P_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | psnr_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | ssim_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | lpips_B | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | ib_gap_db_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| dark | nh_dark | noharm_input_ref | detail_pulse_Y | 9.12838e-05 | 0 | 9.12838e-05 | NA | INFO |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_band1_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_band2_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_band3_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_keep_vs_input_band4_Y | 1 | 1 | 0 | same | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_probe_keep_edge_vs_input_Y | 0.999993 | 1 | -6.54611e-06 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (72 available) |
| dark | nh_dark | noharm_input_ref | grain_probe_keep_flat_vs_input_Y | 1 | 1 | -4.57115e-07 | worse | PASS |  |
| dark | nh_dark | noharm_input_ref | grain_probe_lag1_flat_vs_input_Y | -2.36398e-07 | 0 | -2.36398e-07 | better | PASS |  |
| dark | nh_dark | noharm_input_ref | cost_ms_per_frame_pyref | 1207.44 | 0.000399988 | 1207.44 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| dark | nh_dark | noharm_input_ref | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| dark | nh_dark | noharm_input_ref | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| nature30 | nh_river | noharm_input_ref | psnr_Y | 69.2824 | inf | inf | worse | FLAG |  |
| nature30 | nh_river | noharm_input_ref | psnr_Cb | inf | inf | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | psnr_Cr | inf | inf | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | ssim_Y | 0.999992 | 1 | -8.35923e-06 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | ssim_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | ssim_Cr | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| nature30 | nh_river | noharm_input_ref | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| nature30 | nh_river | noharm_input_ref | grain_rms_ratio_Y | 1.00071 | 1 | 0.000708863 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_hf_ratio_Y | 0.999836 | 1 | -0.000164063 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_lag1_delta_Y | 3.28103e-05 | 0 | 3.28103e-05 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| nature30 | nh_river | noharm_input_ref | grain_rms_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_hf_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_lag1_delta_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_Y | 1.00071 | 1 | 0.000708863 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_lag1_vs_input_Y | 3.28103e-05 | 0 | 3.28103e-05 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | nh_river | noharm_input_ref | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| nature30 | nh_river | noharm_input_ref | flicker_excess_Y | 7.48688e-05 | 0 | 7.48688e-05 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| nature30 | nh_river | noharm_input_ref | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| nature30 | nh_river | noharm_input_ref | ghost_coeff_Y | -0.000283267 | 0 | -0.000283267 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | smear_Y | 1.00422 | 1 | 0.00422083 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | halo_overshoot_Y | 3.77071e-06 | 0 | 3.77071e-06 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | lc_ratio_s1_Y | 1.00108 | 1 | 0.001079 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | lc_ratio_s2_Y | 1.00092 | 1 | 0.000923319 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | lc_ratio_s3_Y | 1.00074 | 1 | 0.000743301 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | btb_kept_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | wtw_kept_Y | 1 | 1 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| nature30 | nh_river | noharm_input_ref | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | nh_river | noharm_input_ref | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | nh_river | noharm_input_ref | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| nature30 | nh_river | noharm_input_ref | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| nature30 | nh_river | noharm_input_ref | psnr_I_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | psnr_P_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | psnr_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | ssim_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | lpips_B | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | ib_gap_db_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| nature30 | nh_river | noharm_input_ref | detail_pulse_Y | 0.00323663 | 0 | 0.00323663 | NA | INFO |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_band1_Y | 1.00067 | 1 | 0.000672244 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_band2_Y | 1.00083 | 1 | 0.000834881 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_band3_Y | 1.00069 | 1 | 0.000687394 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_keep_vs_input_band4_Y | 1.00054 | 1 | 0.000543039 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_probe_keep_edge_vs_input_Y | 1.00463 | 1 | 0.00462508 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (90 available) |
| nature30 | nh_river | noharm_input_ref | grain_probe_keep_flat_vs_input_Y | 1.00006 | 1 | 6.03447e-05 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | grain_probe_lag1_flat_vs_input_Y | 4.15275e-06 | 0 | 4.15275e-06 | worse | PASS |  |
| nature30 | nh_river | noharm_input_ref | cost_ms_per_frame_pyref | 742.353 | 0.000400003 | 742.352 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| nature30 | nh_river | noharm_input_ref | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| nature30 | nh_river | noharm_input_ref | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | nh_goal | noharm_input_ref | psnr_Y | 94.2022 | inf | inf | worse | FLAG |  |
| sports50_60 | nh_goal | noharm_input_ref | psnr_Cb | inf | inf | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | psnr_Cr | inf | inf | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | ssim_Y | 1 | 1 | -3.62391e-08 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | ssim_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | ssim_Cr | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | nh_goal | noharm_input_ref | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_rms_ratio_Y | 1 | 1 | 1.68071e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_hf_ratio_Y | 1 | 1 | 6.58807e-07 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_lag1_delta_Y | 8.74175e-08 | 0 | 8.74175e-08 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_rms_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_hf_ratio_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_lag1_delta_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_Y | 1 | 1 | 1.68071e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_lag1_vs_input_Y | 8.74175e-08 | 0 | 8.74175e-08 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | nh_goal | noharm_input_ref | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | nh_goal | noharm_input_ref | flicker_excess_Y | 3.10965e-05 | 0 | 3.10965e-05 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | gop_ratio_gop | NA | NA | NA | NA | NA | no GOP metadata |
| sports50_60 | nh_goal | noharm_input_ref | gop_ratio_minigop | NA | NA | NA | NA | NA | no GOP metadata |
| sports50_60 | nh_goal | noharm_input_ref | ghost_coeff_Y | -6.05094e-06 | 0 | -6.05094e-06 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | smear_Y | 1.00015 | 1 | 0.000148561 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | halo_overshoot_Y | 3.42581e-07 | 0 | 3.42581e-07 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | lc_ratio_s1_Y | 1.00006 | 1 | 5.53686e-05 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | lc_ratio_s2_Y | 1.00005 | 1 | 4.86104e-05 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | lc_ratio_s3_Y | 1.00004 | 1 | 3.97296e-05 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | btb_kept_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | wtw_kept_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | nh_goal | noharm_input_ref | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | nh_goal | noharm_input_ref | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | nh_goal | noharm_input_ref | lpips_all | NA | NA | NA | NA | NA | lpips not requested / unavailable |
| sports50_60 | nh_goal | noharm_input_ref | psnr_I_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | psnr_P_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | psnr_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | ssim_B_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | lpips_B | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | ib_gap_db_Y | NA | NA | NA | NA | NA | no frame_types in the case |
| sports50_60 | nh_goal | noharm_input_ref | detail_pulse_Y | 0.000196051 | 0 | 0.000196051 | NA | INFO |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_band1_Y | 1 | 1 | 1.72117e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_band2_Y | 1 | 1 | 2.25978e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_band3_Y | 1 | 1 | 4.01505e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_keep_vs_input_band4_Y | 1 | 1 | -2.95869e-07 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_probe_keep_edge_vs_input_Y | 1.00001 | 1 | 9.92609e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_probe_lag1_edge_vs_input_Y | 6.43807e-06 | 0 | 6.43807e-06 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_probe_keep_flat_vs_input_Y | 1 | 1 | 4.24626e-08 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | grain_probe_lag1_flat_vs_input_Y | 8.26624e-08 | 0 | 8.26624e-08 | worse | PASS |  |
| sports50_60 | nh_goal | noharm_input_ref | cost_ms_per_frame_pyref | 1027.58 | 0.000299988 | 1027.58 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | nh_goal | noharm_input_ref | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | nh_goal | noharm_input_ref | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
