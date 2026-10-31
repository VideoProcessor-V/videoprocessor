<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V prototype evaluation: td_b008_trust1 vs td_off (default)

Suite: `td_real_rerun_crowd_1080p` (1 cases). Candidate `td_b008_trust1` (TemporalDetail CPU model (td_cli), mode steady, grain budget 0.08, cleaner on, guard on; DIS-flow stand-in vectors, trust forced to 1 (vector-confidence sensitivity)), default `td_off`. LPIPS requested (CPU, tile crops). Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### sports50_60  (INFO 5, LIMIT 6, NA 11, PASS 37)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_Y | 0.142857 | 0 | 0.142857 | 1 | 0 | LIMIT:1 |
| btb_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| btb_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| clip_detect_C | NA | NA | NA | 1 | 1 | NA:1 |
| clip_detect_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| cost_est_3090_ms | NA | NA | NA | 1 | 1 | NA:1 |
| cost_gpu_ms_laptop | NA | NA | NA | 1 | 1 | NA:1 |
| cost_ms_per_frame_pyref | 517.087 | 0.000200002 | 517.087 | 1 | 0 | INFO:1 |
| detail_pulse_Y | 0.170972 | 0.172233 | -0.00126131 | 1 | 0 | INFO:1 |
| duplicate_rate_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| flicker_excess_Y | -0.0585519 | -0.0617728 | 0.00322092 | 1 | 0 | PASS:1 |
| ghost_coeff_Y | 0.179541 | 0.186549 | -0.0070079 | 1 | 0 | PASS:1 |
| gop_ratio_gop | NA | NA | NA | 1 | 1 | NA:1 |
| gop_ratio_minigop | NA | NA | NA | 1 | 1 | NA:1 |
| grain_hf_ratio_Cb | 0.0898795 | 0.0898795 | 0 | 1 | 0 | PASS:1 |
| grain_hf_ratio_Y | 0.239854 | 0.236906 | 0.0029475 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Cb | 1 | 1 | 0 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_Y | 1.02628 | 1 | 0.0262783 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band1_Y | 1.02492 | 1 | 0.0249189 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band2_Y | 1.03508 | 1 | 0.035085 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band3_Y | 1.03498 | 1 | 0.0349818 | 1 | 0 | PASS:1 |
| grain_keep_vs_input_band4_Y | 1.02699 | 1 | 0.0269914 | 1 | 0 | PASS:1 |
| grain_lag1_delta_Cb | 0.423962 | 0.423962 | 0 | 1 | 0 | LIMIT:1 |
| grain_lag1_delta_Y | 0.22274 | 0.223997 | -0.00125674 | 1 | 0 | LIMIT:1 |
| grain_lag1_vs_input_Cb | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| grain_lag1_vs_input_Y | -0.00125674 | 0 | -0.00125674 | 1 | 0 | PASS:1 |
| grain_probe_keep_edge_vs_input_Y | 1.02837 | 1 | 0.0283674 | 1 | 0 | PASS:1 |
| grain_probe_keep_flat_vs_input_Y | 1.01416 | 1 | 0.0141584 | 1 | 0 | PASS:1 |
| grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | 1 | 1 | NA:1 |
| grain_probe_lag1_flat_vs_input_Y | 0.00015777 | 0 | 0.00015777 | 1 | 0 | PASS:1 |
| grain_rms_ratio_Cb | 0.162379 | 0.162379 | 0 | 1 | 0 | LIMIT:1 |
| grain_rms_ratio_Y | 0.579094 | 0.564267 | 0.014828 | 1 | 0 | LIMIT:1 |
| grain_tiles_Cb | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| grain_tiles_Y | 16 | 16 | 0 | 1 | 0 | INFO:1 |
| halo_overshoot_Y | 0.000441706 | 0.000366931 | 7.47757e-05 | 1 | 0 | PASS:1 |
| ib_gap_db_Y | 1.08501 | 1.12018 | -0.0351715 | 1 | 0 | INFO:1 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 1 | 0 | PASS:1 |
| lc_ratio_s1_Y | 0.795486 | 0.777655 | 0.0178313 | 1 | 0 | PASS:1 |
| lc_ratio_s2_Y | 0.854581 | 0.83809 | 0.0164913 | 1 | 0 | PASS:1 |
| lc_ratio_s3_Y | 0.899602 | 0.885735 | 0.0138671 | 1 | 0 | PASS:1 |
| lpips | NA | NA | NA | 1 | 1 | NA:1 |
| lpips_B | 0.145435 | 0.147005 | -0.00156929 | 1 | 0 | PASS:1 |
| lpips_all | 0.13796 | 0.139355 | -0.00139487 | 1 | 0 | PASS:1 |
| psnr_B_Y | 28.2533 | 28.2453 | 0.00803817 | 1 | 0 | PASS:1 |
| psnr_Cb | 34.5263 | 34.5263 | 0 | 1 | 0 | PASS:1 |
| psnr_Cr | 35.0819 | 35.0819 | 0 | 1 | 0 | PASS:1 |
| psnr_I_Y | 31.9847 | 31.9908 | -0.00616277 | 1 | 0 | PASS:1 |
| psnr_P_Y | 29.2352 | 29.263 | -0.0277209 | 1 | 0 | PASS:1 |
| psnr_Y | 28.5172 | 28.5169 | 0.000299309 | 1 | 0 | PASS:1 |
| smear_Y | 0.69203 | 0.657254 | 0.0347757 | 1 | 0 | PASS:1 |
| ssim_B_Y | 0.801459 | 0.800445 | 0.00101338 | 1 | 0 | PASS:1 |
| ssim_Cb | 0.852283 | 0.852283 | 0 | 1 | 0 | PASS:1 |
| ssim_Cr | 0.869421 | 0.869421 | 0 | 1 | 0 | PASS:1 |
| ssim_Y | 0.807729 | 0.806901 | 0.000828772 | 1 | 0 | PASS:1 |
| wtw_kept_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_Y | 0.229508 | 0.0983607 | 0.131148 | 1 | 0 | LIMIT:1 |
| wtw_kept_vs_input_C | NA | NA | NA | 1 | 1 | NA:1 |
| wtw_kept_vs_input_Y | 1 | 1 | 0 | 1 | 0 | PASS:1 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | 28.5172 | 28.5169 | 0.000299309 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cb | 34.5263 | 34.5263 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_Cr | 35.0819 | 35.0819 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | 0.807729 | 0.806901 | 0.000828772 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cb | 0.852283 | 0.852283 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_Cr | 0.869421 | 0.869421 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lpips | NA | NA | NA | NA | NA | lpips NA (not requested, or torch/lpips unavailable) |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Y | 0.579094 | 0.564267 | 0.014828 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Y | 0.239854 | 0.236906 | 0.0029475 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Y | 0.22274 | 0.223997 | -0.00125674 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_rms_ratio_Cb | 0.162379 | 0.162379 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_hf_ratio_Cb | 0.0898795 | 0.0898795 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_delta_Cb | 0.423962 | 0.423962 | 0 | same | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | 1.02628 | 1 | 0.0262783 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | -0.00125674 | 0 | -0.00125674 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Cb | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Cb | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | 1 | 1 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_C | NA | NA | NA | NA | NA | candidate input has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | -0.0585519 | -0.0617728 | 0.00322092 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_gop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | NA | NA | NA | GOP 100 longer than half the window (150 frames) |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | 0.179541 | 0.186549 | -0.0070079 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | 0.69203 | 0.657254 | 0.0347757 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | 0.000441706 | 0.000366931 | 7.47757e-05 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s1_Y | 0.795486 | 0.777655 | 0.0178313 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s2_Y | 0.854581 | 0.83809 | 0.0164913 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_ratio_s3_Y | 0.899602 | 0.885735 | 0.0138671 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_Y | 0.142857 | 0 | 0.142857 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_Y | 0.229508 | 0.0983607 | 0.131148 | better | LIMIT | default also fails: input/codec-limited, candidate not worse |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | clip_detect_C | NA | NA | NA | NA | NA | reference has no excursions beyond nominal limits |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | 0.13796 | 0.139355 | -0.00139487 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | 31.9847 | 31.9908 | -0.00616277 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | 29.2352 | 29.263 | -0.0277209 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | 28.2533 | 28.2453 | 0.00803817 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | 0.801459 | 0.800445 | 0.00101338 | better | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | 0.145435 | 0.147005 | -0.00156929 | same | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | 1.08501 | 1.12018 | -0.0351715 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | 0.170972 | 0.172233 | -0.00126131 | NA | INFO |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | 1.02492 | 1 | 0.0249189 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | 1.03508 | 1 | 0.035085 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | 1.03498 | 1 | 0.0349818 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | 1.02699 | 1 | 0.0269914 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_edge_vs_input_Y | 1.02837 | 1 | 0.0283674 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_edge_vs_input_Y | NA | NA | NA | NA | NA | needs >= 3 frames (150 available) |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_flat_vs_input_Y | 1.01416 | 1 | 0.0141584 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_flat_vs_input_Y | 0.00015777 | 0 | 0.00015777 | worse | PASS |  |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | 517.087 | 0.000200002 | 517.087 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | crowd | x265_1080p_6k | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | crowd | x265_1080p_6k | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
