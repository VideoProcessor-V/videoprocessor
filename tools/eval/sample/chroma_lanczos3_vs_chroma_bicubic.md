<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# VP-V evaluation harness: chroma_lanczos3 vs chroma_bicubic (default)

Suite: `sample_chroma` (5 cases). Candidate `chroma_lanczos3`, default `chroma_bicubic`. LPIPS requested. Cost: py-ref wall-clock on CPU (not GPU-representative).

## Per-content-type summary

### film24  (FAIL 8, FLAG 2, INFO 6, NA 16, PASS 38)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| btb_kept_Y | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_C | NA | NA | NA | 2 | 2 | NA:2 |
| clip_detect_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| cost_est_3090_ms | NA | NA | NA | 2 | 2 | NA:2 |
| cost_gpu_ms_laptop | NA | NA | NA | 2 | 2 | NA:2 |
| cost_ms_per_frame_pyref | 43.8723 | 38.6428 | 5.22952 | 2 | 0 | INFO:2 |
| duplicate_rate_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| flicker_excess_Y | -0.0472495 | -0.0472495 | 0 | 2 | 0 | PASS:2 |
| ghost_coeff_Y | 0.052667 | 0.052667 | 0 | 2 | 0 | PASS:2 |
| gop_ratio_gop | 1.06515 | 1.06515 | 0 | 2 | 1 | NA:1 PASS:1 |
| gop_ratio_minigop | 1.01828 | 1.01828 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_hf_ratio_Cb | 0.572241 | 0.537071 | 0.0351698 | 2 | 1 | NA:1 PASS:1 |
| grain_hf_ratio_Y | 0.953573 | 0.953573 | 0 | 2 | 1 | NA:1 PASS:1 |
| grain_lag1_delta_Cb | 0.214941 | 0.232195 | -0.0172533 | 2 | 0 | FAIL:2 |
| grain_lag1_delta_Y | 0.0258021 | 0.0258021 | 0 | 2 | 0 | PASS:2 |
| grain_rms_ratio_Cb | 0.439689 | 0.415506 | 0.0241829 | 2 | 0 | FAIL:2 |
| grain_rms_ratio_Y | 0.790794 | 0.790794 | 0 | 2 | 0 | FAIL:2 |
| grain_tiles_Cb | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| grain_tiles_Y | 16 | 16 | 0 | 2 | 0 | INFO:2 |
| halo_overshoot_Y | 6.28578e-05 | 6.28578e-05 | 0 | 2 | 0 | PASS:2 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s1_Y | 0.927013 | 0.927013 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s2_Y | 0.955069 | 0.955069 | 0 | 2 | 0 | PASS:2 |
| lc_ratio_s3_Y | 0.973374 | 0.973374 | 0 | 2 | 0 | PASS:2 |
| lpips | 0.0425956 | 0.0428915 | -0.000295893 | 2 | 0 | PASS:2 |
| psnr_Cb | 44.8369 | 44.865 | -0.0280359 | 2 | 0 | FLAG:1 PASS:1 |
| psnr_Cr | 45.167 | 45.1962 | -0.0292052 | 2 | 0 | FLAG:1 PASS:1 |
| psnr_Y | 41.3145 | 41.3145 | 0 | 2 | 0 | PASS:2 |
| smear_Y | 0.901043 | 0.901043 | 0 | 2 | 0 | PASS:2 |
| ssim_Cb | 0.979622 | 0.979823 | -0.000200964 | 2 | 0 | PASS:2 |
| ssim_Cr | 0.981712 | 0.981932 | -0.00022052 | 2 | 0 | PASS:2 |
| ssim_Y | 0.967641 | 0.967641 | 0 | 2 | 0 | PASS:2 |
| wtw_kept_C | NA | NA | NA | 2 | 2 | NA:2 |
| wtw_kept_Y | 0.598881 | 0.598881 | 0 | 2 | 0 | FAIL:2 |

### sports50_60  (FAIL 13, INFO 9, NA 29, PASS 54)

| metric | candidate (mean) | default (mean) | delta | n | NA | flags |
|---|---|---|---|---|---|---|
| btb_kept_C | NA | NA | NA | 3 | 3 | NA:3 |
| btb_kept_Y | 0 | 0 | 0 | 3 | 2 | FAIL:1 NA:2 |
| clip_detect_C | NA | NA | NA | 3 | 3 | NA:3 |
| clip_detect_Y | 0 | 0 | 0 | 3 | 0 | PASS:3 |
| cost_est_3090_ms | NA | NA | NA | 3 | 3 | NA:3 |
| cost_gpu_ms_laptop | NA | NA | NA | 3 | 3 | NA:3 |
| cost_ms_per_frame_pyref | 89.3222 | 77.6332 | 11.689 | 3 | 0 | INFO:3 |
| duplicate_rate_Y | 0 | 0 | 0 | 3 | 0 | PASS:3 |
| flicker_excess_Y | 0.00268899 | 0.00268899 | 0 | 3 | 0 | PASS:3 |
| ghost_coeff_Y | 0.0626999 | 0.0626999 | 0 | 3 | 0 | PASS:3 |
| gop_ratio_gop | NA | NA | NA | 3 | 3 | NA:3 |
| gop_ratio_minigop | NA | NA | NA | 3 | 3 | NA:3 |
| grain_hf_ratio_Cb | NA | NA | NA | 3 | 3 | NA:3 |
| grain_hf_ratio_Y | NA | NA | NA | 3 | 3 | NA:3 |
| grain_lag1_delta_Cb | 0.107377 | 0.113502 | -0.00612547 | 3 | 0 | FAIL:2 PASS:1 |
| grain_lag1_delta_Y | 0.0686974 | 0.0686974 | 0 | 3 | 0 | FAIL:1 PASS:2 |
| grain_rms_ratio_Cb | 0.152609 | 0.14397 | 0.0086393 | 3 | 0 | FAIL:3 |
| grain_rms_ratio_Y | 0.484992 | 0.484992 | 0 | 3 | 0 | FAIL:3 |
| grain_tiles_Cb | 16 | 16 | 0 | 3 | 0 | INFO:3 |
| grain_tiles_Y | 16 | 16 | 0 | 3 | 0 | INFO:3 |
| halo_overshoot_Y | 3.02172e-05 | 3.02172e-05 | 0 | 3 | 0 | PASS:3 |
| lc_boost_fraction_Y | 0 | 0 | 0 | 3 | 0 | PASS:3 |
| lc_ratio_s1_Y | 0.922915 | 0.922915 | 0 | 3 | 0 | PASS:3 |
| lc_ratio_s2_Y | 0.956134 | 0.956134 | 0 | 3 | 0 | PASS:3 |
| lc_ratio_s3_Y | 0.973379 | 0.973379 | 0 | 3 | 0 | PASS:3 |
| lpips | 0.0731741 | 0.0736768 | -0.00050271 | 3 | 0 | PASS:3 |
| psnr_Cb | 40.8603 | 40.8443 | 0.0160646 | 3 | 0 | PASS:3 |
| psnr_Cr | 41.4502 | 41.4588 | -0.00864488 | 3 | 0 | PASS:3 |
| psnr_Y | 39.9521 | 39.9521 | 0 | 3 | 0 | PASS:3 |
| smear_Y | 0.877956 | 0.877956 | 0 | 3 | 0 | PASS:3 |
| ssim_Cb | 0.93211 | 0.931053 | 0.00105745 | 3 | 0 | PASS:3 |
| ssim_Cr | 0.935495 | 0.935286 | 0.000209198 | 3 | 0 | PASS:3 |
| ssim_Y | 0.961234 | 0.961234 | 0 | 3 | 0 | PASS:3 |
| wtw_kept_C | NA | NA | NA | 3 | 3 | NA:3 |
| wtw_kept_Y | 0.564955 | 0.564955 | 0 | 3 | 0 | FAIL:3 |

## All rows

| content_type | clip | case | metric | candidate | default | delta | trend | flag | note |
|---|---|---|---|---|---|---|---|---|---|
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Y | 41.7004 | 41.7004 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cb | 44.574 | 44.6101 | -0.0361792 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | psnr_Cr | 44.4854 | 44.518 | -0.0325946 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Y | 0.967556 | 0.967556 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cb | 0.978297 | 0.978508 | -0.00021028 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ssim_Cr | 0.977306 | 0.977515 | -0.000209182 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lpips | 0.0574456 | 0.0577751 | -0.000329531 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Y | 0.0562593 | 0.0562593 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Y | 0.186232 | 0.186232 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_rms_ratio_Cb | 0.0708487 | 0.0668199 | 0.00402882 | better | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Bosphorus | native_x265_crf26 | grain_lag1_delta_Cb | 0.235203 | 0.242929 | -0.00772636 | better | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Bosphorus | native_x265_crf26 | flicker_excess_Y | 0.0810099 | 0.0810099 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | gop_ratio_gop | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | gop_ratio_minigop | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | ghost_coeff_Y | 0.150898 | 0.150898 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | smear_Y | 0.859492 | 0.859492 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | halo_overshoot_Y | 3.23308e-05 | 3.23308e-05 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s1_Y | 0.910295 | 0.910295 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s2_Y | 0.945422 | 0.945422 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_ratio_s3_Y | 0.966444 | 0.966444 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_Y | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_Y | 0.491276 | 0.491276 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Bosphorus | native_x265_crf26 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Bosphorus | native_x265_crf26 | btb_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | wtw_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | clip_detect_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_ms_per_frame_pyref | 88.1333 | 77.1974 | 10.9359 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Bosphorus | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Y | 42.3823 | 42.3823 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cb | 43.4773 | 43.4971 | -0.0198002 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | psnr_Cr | 43.1861 | 43.1923 | -0.00615304 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Y | 0.964423 | 0.964423 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cb | 0.967837 | 0.968048 | -0.00021142 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | ssim_Cr | 0.964256 | 0.964399 | -0.000143246 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lpips | 0.111801 | 0.112359 | -0.000557711 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Y | 0.527247 | 0.527247 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Y | 0.0148708 | 0.0148708 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | Jockey | native_x264_crf23 | grain_rms_ratio_Cb | 0.227069 | 0.214987 | 0.0120819 | better | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Jockey | native_x264_crf23 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | Jockey | native_x264_crf23 | grain_lag1_delta_Cb | 0.0313167 | 0.032466 | -0.00114931 | better | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | flicker_excess_Y | 0.00655615 | 0.00655615 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | gop_ratio_gop | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | gop_ratio_minigop | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | ghost_coeff_Y | 0.00672052 | 0.00672052 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | smear_Y | 0.887106 | 0.887106 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | halo_overshoot_Y | 9.03965e-06 | 9.03965e-06 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s1_Y | 0.911581 | 0.911581 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s2_Y | 0.96108 | 0.96108 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_ratio_s3_Y | 0.982305 | 0.982305 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_Y | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_Y | 0.737569 | 0.737569 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | Jockey | native_x264_crf23 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | Jockey | native_x264_crf23 | btb_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | wtw_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | clip_detect_C | NA | NA | NA | NA | NA |  |
| sports50_60 | Jockey | native_x264_crf23 | cost_ms_per_frame_pyref | 92.3588 | 80.1005 | 12.2583 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | Jockey | native_x264_crf23 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | Jockey | native_x264_crf23 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Y | 35.7737 | 35.7737 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cb | 34.5297 | 34.4255 | 0.104173 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | psnr_Cr | 36.679 | 36.6661 | 0.012813 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Y | 0.951724 | 0.951724 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cb | 0.850196 | 0.846602 | 0.00359405 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | ssim_Cr | 0.864924 | 0.863944 | 0.000980022 | better | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lpips | 0.0502752 | 0.0508961 | -0.000620888 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Y | 0.871471 | 0.871471 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Y | 0.00498915 | 0.00498915 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| sports50_60 | park_joy | native_x265_crf26 | grain_rms_ratio_Cb | 0.159909 | 0.150102 | 0.00980719 | better | FAIL | default also fails (input/codec-limited) |
| sports50_60 | park_joy | native_x265_crf26 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| sports50_60 | park_joy | native_x265_crf26 | grain_lag1_delta_Cb | 0.0556106 | 0.0651114 | -0.00950075 | better | FAIL | default also fails (input/codec-limited) |
| sports50_60 | park_joy | native_x265_crf26 | flicker_excess_Y | -0.0794991 | -0.0794991 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | gop_ratio_gop | NA | NA | NA | NA | NA |  |
| sports50_60 | park_joy | native_x265_crf26 | gop_ratio_minigop | NA | NA | NA | NA | NA |  |
| sports50_60 | park_joy | native_x265_crf26 | ghost_coeff_Y | 0.0304811 | 0.0304811 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | smear_Y | 0.887269 | 0.887269 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | halo_overshoot_Y | 4.92811e-05 | 4.92811e-05 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s1_Y | 0.946868 | 0.946868 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s2_Y | 0.961901 | 0.961901 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_ratio_s3_Y | 0.971388 | 0.971388 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_Y | 0 | 0 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_Y | 0.466019 | 0.466019 | 0 | same | FAIL | default also fails (input/codec-limited) |
| sports50_60 | park_joy | native_x265_crf26 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| sports50_60 | park_joy | native_x265_crf26 | btb_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | park_joy | native_x265_crf26 | wtw_kept_C | NA | NA | NA | NA | NA |  |
| sports50_60 | park_joy | native_x265_crf26 | clip_detect_C | NA | NA | NA | NA | NA |  |
| sports50_60 | park_joy | native_x265_crf26 | cost_ms_per_frame_pyref | 87.4746 | 75.6018 | 11.8728 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| sports50_60 | park_joy | native_x265_crf26 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| sports50_60 | park_joy | native_x265_crf26 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Y | 38.8455 | 38.8455 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cb | 43.924 | 43.9859 | -0.0618183 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | psnr_Cr | 44.8618 | 44.9431 | -0.0813718 | worse | FLAG |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Y | 0.958211 | 0.958211 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cb | 0.979471 | 0.979896 | -0.000425376 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ssim_Cr | 0.984029 | 0.984471 | -0.000442324 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lpips | 0.0640881 | 0.0644057 | -0.000317605 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Y | 0.714245 | 0.714245 | 0 | same | FAIL | default also fails (input/codec-limited) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Y | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Y | 0.0204453 | 0.0204453 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_rms_ratio_Cb | 0.484403 | 0.455732 | 0.0286711 | better | FAIL | default also fails (input/codec-limited) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_hf_ratio_Cb | NA | NA | NA | NA | NA | needs >= 8 frames (4 available) |
| film24 | tos4k | x265_2160p_6000k_w2400 | grain_lag1_delta_Cb | 0.0686017 | 0.0794448 | -0.0108432 | better | FAIL | default also fails (input/codec-limited) |
| film24 | tos4k | x265_2160p_6000k_w2400 | flicker_excess_Y | -0.0667484 | -0.0667484 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_gop | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | gop_ratio_minigop | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | ghost_coeff_Y | 0.0490126 | 0.0490126 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | smear_Y | 0.856301 | 0.856301 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | halo_overshoot_Y | 7.28305e-05 | 7.28305e-05 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s1_Y | 0.933163 | 0.933163 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s2_Y | 0.954963 | 0.954963 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_ratio_s3_Y | 0.971433 | 0.971433 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_Y | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_Y | 0.599488 | 0.599488 | 0 | same | FAIL | default also fails (input/codec-limited) |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | btb_kept_C | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | wtw_kept_C | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | clip_detect_C | NA | NA | NA | NA | NA |  |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_ms_per_frame_pyref | 75.6026 | 66.7399 | 8.8627 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | tos4k | x265_2160p_6000k_w2400 | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
| film24 | breathing | enc24_48f | psnr_Y | 43.7835 | 43.7835 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | psnr_Cb | 45.7499 | 45.7441 | 0.00574657 | same | PASS |  |
| film24 | breathing | enc24_48f | psnr_Cr | 45.4723 | 45.4493 | 0.0229614 | same | PASS |  |
| film24 | breathing | enc24_48f | ssim_Y | 0.977071 | 0.977071 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | ssim_Cb | 0.979772 | 0.979749 | 2.34468e-05 | same | PASS |  |
| film24 | breathing | enc24_48f | ssim_Cr | 0.979395 | 0.979394 | 1.28496e-06 | same | PASS |  |
| film24 | breathing | enc24_48f | lpips | 0.021103 | 0.0213772 | -0.00027418 | same | PASS |  |
| film24 | breathing | enc24_48f | grain_tiles_Y | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_48f | grain_rms_ratio_Y | 0.867342 | 0.867342 | 0 | same | FAIL | default also fails (input/codec-limited) |
| film24 | breathing | enc24_48f | grain_hf_ratio_Y | 0.953573 | 0.953573 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | grain_lag1_delta_Y | 0.031159 | 0.031159 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | grain_tiles_Cb | 16 | 16 | 0 | NA | INFO |  |
| film24 | breathing | enc24_48f | grain_rms_ratio_Cb | 0.394975 | 0.37528 | 0.0196948 | better | FAIL | default also fails (input/codec-limited) |
| film24 | breathing | enc24_48f | grain_hf_ratio_Cb | 0.572241 | 0.537071 | 0.0351698 | better | PASS |  |
| film24 | breathing | enc24_48f | grain_lag1_delta_Cb | 0.361281 | 0.384944 | -0.0236634 | better | FAIL | default also fails (input/codec-limited) |
| film24 | breathing | enc24_48f | flicker_excess_Y | -0.0277507 | -0.0277507 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | gop_ratio_gop | 1.06515 | 1.06515 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | gop_ratio_minigop | 1.01828 | 1.01828 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | ghost_coeff_Y | 0.0563214 | 0.0563214 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | smear_Y | 0.945785 | 0.945785 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | duplicate_rate_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | halo_overshoot_Y | 5.2885e-05 | 5.2885e-05 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | lc_ratio_s1_Y | 0.920863 | 0.920863 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | lc_ratio_s2_Y | 0.955174 | 0.955174 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | lc_ratio_s3_Y | 0.975315 | 0.975315 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | lc_boost_fraction_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | btb_kept_Y | NA | NA | NA | NA | NA |  |
| film24 | breathing | enc24_48f | wtw_kept_Y | 0.598273 | 0.598273 | 0 | same | FAIL | default also fails (input/codec-limited) |
| film24 | breathing | enc24_48f | clip_detect_Y | 0 | 0 | 0 | same | PASS |  |
| film24 | breathing | enc24_48f | btb_kept_C | NA | NA | NA | NA | NA |  |
| film24 | breathing | enc24_48f | wtw_kept_C | NA | NA | NA | NA | NA |  |
| film24 | breathing | enc24_48f | clip_detect_C | NA | NA | NA | NA | NA |  |
| film24 | breathing | enc24_48f | cost_ms_per_frame_pyref | 12.142 | 10.5457 | 1.59635 | worse | INFO | py-ref: CPU numpy wall-clock, not GPU-representative |
| film24 | breathing | enc24_48f | cost_gpu_ms_laptop | NA | NA | NA | NA | NA | capture sidecar gpu_ms (laptop GPU) |
| film24 | breathing | enc24_48f | cost_est_3090_ms | NA | NA | NA | NA | NA | ESTIMATE: laptop gpu_ms x 0.20 (cost_ratios.json) |
