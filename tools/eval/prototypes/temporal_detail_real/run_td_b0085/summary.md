<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b0085 |
|---|---|---|
| psnr_B_Y | 37.1584 | 38.0485 |
| psnr_P_Y | 38.0699 | 38.9333 |
| psnr_I_Y | 39.7151 | 40.4396 |
| psnr_Y | 37.409 | 38.2938 |
| ssim_B_Y | 0.919951 | 0.933631 |
| ssim_Y | 0.922409 | 0.935771 |
| lpips_B | 0.0775072 | 0.0600421 |
| lpips_all | 0.074667 | 0.0577964 |
| ib_gap_db_Y | 0.995317 | 0.966675 |
| detail_pulse_Y | 0.173895 | 0.173225 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.0146226 | -0.0108145 |
| grain_keep_vs_input_Y | 1 | 1.00841 |
| grain_lag1_vs_input_Y | 0 | -0.000241517 |
| grain_keep_vs_input_band1_Y | 1 | 1.00821 |
| grain_keep_vs_input_band2_Y | 1 | 1.009 |
| grain_keep_vs_input_band3_Y | 1 | 1.00917 |
| grain_keep_vs_input_band4_Y | 1 | 1.00876 |
| grain_probe_keep_edge_vs_input_Y | 1 | 1.01729 |
| grain_probe_lag1_edge_vs_input_Y | 0 | 0.0012412 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1.00601 |
| grain_probe_lag1_flat_vs_input_Y | 0 | 4.45042e-05 |
| ghost_coeff_Y | 0.0579582 | 0.042982 |
| smear_Y | 0.824461 | 0.863771 |
| halo_overshoot_Y | 0.000185865 | 0.000208812 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 1 |
| wtw_kept_vs_input_Y | 1 | 1 |
| cost_ms_per_frame_pyref | 0.000387501 | 1287 |

## Per case

| content_type | clip | case | metric | default | td_b0085 | flags |
|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | psnr_Y | 48.0976 | 48.0929 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | ssim_Y | 0.989694 | 0.98969 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1.00028 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Y | 0 | -2.20564e-06 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_2160p_12k | flicker_excess_Y | -0.000143152 | -0.000127815 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| film24 | tos | x265_2160p_12k | ghost_coeff_Y | 0.00218632 | 0.00205002 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | smear_Y | 0.902687 | 0.906685 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | halo_overshoot_Y | 8.25973e-06 | 9.41108e-06 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | lpips_all | 0.0415157 | 0.0414538 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | psnr_I_Y | 48.7149 | 48.7109 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | psnr_P_Y | 48.7726 | 48.768 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | psnr_B_Y | 47.8654 | 47.8606 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | ssim_B_Y | 0.989256 | 0.989251 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | lpips_B | 0.0436596 | 0.0435951 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | ib_gap_db_Y | 0.90131 | 0.901687 | td_b0085:INFO |
| film24 | tos | x265_2160p_12k | detail_pulse_Y | 0.168458 | 0.174156 | td_b0085:INFO |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1.00027 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1.00026 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1.0003 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1.00029 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01587 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1 | 1.0003 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 0 | -2.68218e-06 | td_b0085:PASS |
| film24 | tos | x265_2160p_12k | cost_ms_per_frame_pyref | 0.000400003 | 1484.92 | td_b0085:INFO |
| film24 | tos | x265_1080p_4k | psnr_Y | 47.5554 | 47.5366 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | ssim_Y | 0.990652 | 0.990644 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1.00233 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Y | 0 | -6.29331e-05 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_1080p_4k | flicker_excess_Y | -0.000480672 | -0.000274255 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| film24 | tos | x265_1080p_4k | ghost_coeff_Y | 0.00289956 | 0.00176428 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | smear_Y | 0.955935 | 0.969162 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | halo_overshoot_Y | 2.01293e-05 | 2.55302e-05 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | lpips_all | 0.028675 | 0.0286271 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | psnr_I_Y | 46.8961 | 46.8894 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | psnr_P_Y | 48.2199 | 48.1968 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | psnr_B_Y | 47.3734 | 47.3553 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | ssim_B_Y | 0.990408 | 0.990402 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | lpips_B | 0.030132 | 0.0300749 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | ib_gap_db_Y | 0.694451 | 0.69157 | td_b0085:INFO |
| film24 | tos | x265_1080p_4k | detail_pulse_Y | 0.0585832 | 0.0635404 | td_b0085:INFO |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1.00229 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1.00226 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1.00239 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1 | 1.00238 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01359 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | tos | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00226 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 0 | 3.88664e-06 | td_b0085:PASS |
| film24 | tos | x265_1080p_4k | cost_ms_per_frame_pyref | 0.000399996 | 1627.75 | td_b0085:INFO |
| film24 | sol | x265_2160p_12k | psnr_Y | 39.4187 | 39.4175 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | ssim_Y | 0.956944 | 0.956914 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1.00536 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Y | 0 | -1.71765e-05 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | flicker_excess_Y | -0.00370042 | -0.00365432 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| film24 | sol | x265_2160p_12k | ghost_coeff_Y | 0.0116768 | 0.0110982 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | smear_Y | 0.853971 | 0.861774 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | halo_overshoot_Y | 0.000144943 | 0.000152897 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | lpips_all | 0.05574 | 0.0556859 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | psnr_I_Y | 41.1011 | 41.1014 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | psnr_P_Y | 39.933 | 39.9296 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | psnr_B_Y | 39.2027 | 39.202 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | ssim_B_Y | 0.956301 | 0.956275 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | lpips_B | 0.0569565 | 0.0568925 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | ib_gap_db_Y | 0.833961 | 0.831572 | td_b0085:INFO |
| film24 | sol | x265_2160p_12k | detail_pulse_Y | 0.4089 | 0.410142 | td_b0085:INFO |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1.0055 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1.00532 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1.0053 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1.00529 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1 | 1.009 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | sol | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00127 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 0 | 4.102e-05 | td_b0085:PASS |
| film24 | sol | x265_2160p_12k | cost_ms_per_frame_pyref | 0.000400003 | 1082.13 | td_b0085:INFO |
| film24 | sol | x265_1080p_4k | psnr_Y | 36.0479 | 36.0422 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | ssim_Y | 0.930355 | 0.930347 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1.01121 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Y | 0 | -0.000279359 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | flicker_excess_Y | -0.00783003 | -0.00776334 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| film24 | sol | x265_1080p_4k | ghost_coeff_Y | 0.0225282 | 0.0202235 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | smear_Y | 0.813301 | 0.836208 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | halo_overshoot_Y | 0.000277539 | 0.00030792 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | lpips_all | 0.0665992 | 0.0663992 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | psnr_I_Y | 39.1247 | 39.1261 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | psnr_P_Y | 36.8291 | 36.8211 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | psnr_B_Y | 35.7277 | 35.7225 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | ssim_B_Y | 0.927703 | 0.927705 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | lpips_B | 0.0681223 | 0.0678938 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | ib_gap_db_Y | 1.28348 | 1.28127 | td_b0085:INFO |
| film24 | sol | x265_1080p_4k | detail_pulse_Y | 0.301997 | 0.305348 | td_b0085:INFO |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1.0109 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1.01091 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1.01122 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1 | 1.01176 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01769 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| film24 | sol | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00363 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 0 | -0.00119119 | td_b0085:PASS |
| film24 | sol | x265_1080p_4k | cost_ms_per_frame_pyref | 0.000399999 | 1193.22 | td_b0085:INFO |
| nature30 | boat | x265_2160p_14k | psnr_Y | 36.6265 | 36.6139 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | ssim_Y | 0.954663 | 0.954728 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Y | 1 | 1.0065 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Y | 0 | -0.000477062 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_Y | NA | NA | td_b0085:NA |
| nature30 | boat | x265_2160p_14k | flicker_excess_Y | 0.000279709 | 0.000873817 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| nature30 | boat | x265_2160p_14k | ghost_coeff_Y | 0.0335583 | 0.0279559 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | smear_Y | 0.891075 | 0.912548 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | halo_overshoot_Y | 0.000132016 | 0.000157703 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | lpips_all | 0.030902 | 0.0307962 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | psnr_I_Y | 39.641 | 39.6488 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | psnr_P_Y | 37.0888 | 37.069 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | psnr_B_Y | 36.41 | 36.3992 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | ssim_B_Y | 0.953512 | 0.953589 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | lpips_B | 0.0313815 | 0.031264 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | ib_gap_db_Y | 0.836041 | 0.828303 | td_b0085:INFO |
| nature30 | boat | x265_2160p_14k | detail_pulse_Y | 0.0354826 | 0.0312948 | td_b0085:INFO |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band1_Y | 1 | 1.00594 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band2_Y | 1 | 1.00616 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band3_Y | 1 | 1.00692 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band4_Y | 1 | 1.00694 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01927 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_flat_vs_input_Y | 1 | 1.01065 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000521004 | td_b0085:PASS |
| nature30 | boat | x265_2160p_14k | cost_ms_per_frame_pyref | 0.000499997 | 1165.54 | td_b0085:INFO |
| nature30 | boat | x265_1080p_5k | psnr_Y | 31.8428 | 31.8359 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | ssim_Y | 0.920042 | 0.920395 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Y | 1 | 1.01481 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Y | 0 | -5.62726e-05 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | flicker_excess_Y | -0.00688464 | -0.00616216 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| nature30 | boat | x265_1080p_5k | ghost_coeff_Y | 0.0668913 | 0.0582574 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | smear_Y | 0.836716 | 0.868151 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | halo_overshoot_Y | 0.000299464 | 0.000366766 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | lpips_all | 0.0440061 | 0.0437015 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | psnr_I_Y | 35.6116 | 35.6162 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | psnr_P_Y | 32.5329 | 32.5129 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | psnr_B_Y | 31.5496 | 31.5462 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | ssim_B_Y | 0.916379 | 0.916785 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | lpips_B | 0.0454936 | 0.0451883 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | ib_gap_db_Y | 1.16339 | 1.14779 | td_b0085:INFO |
| nature30 | boat | x265_1080p_5k | detail_pulse_Y | 0.0589656 | 0.0572465 | td_b0085:INFO |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band1_Y | 1 | 1.01498 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band2_Y | 1 | 1.01459 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band3_Y | 1 | 1.01474 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band4_Y | 1 | 1.01499 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01792 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_edge_vs_input_Y | 0 | 0.0012412 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00999 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000820246 | td_b0085:PASS |
| nature30 | boat | x265_1080p_5k | cost_ms_per_frame_pyref | 0.000200002 | 1522.99 | td_b0085:INFO |
| sports50_60 | crowd | x265_2160p_16k | psnr_Y | 31.1665 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | ssim_Y | 0.83002 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Y | 0 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | flicker_excess_Y | -0.0364489 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | ghost_coeff_Y | 0.137376 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | smear_Y | 0.684746 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | halo_overshoot_Y | 0.000237639 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | lc_boost_fraction_Y | 0 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | lpips_all | 0.190543 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | psnr_I_Y | 34.6409 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | psnr_P_Y | 31.9202 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | psnr_B_Y | 30.8928 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | ssim_B_Y | 0.825603 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | lpips_B | 0.197307 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | ib_gap_db_Y | 1.12972 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | detail_pulse_Y | 0.186542 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band1_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band2_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band3_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band4_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_edge_vs_input_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_flat_vs_input_Y | 1 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_flat_vs_input_Y | 0 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_2160p_16k | cost_ms_per_frame_pyref | 0.000400003 | NA | td_b0085:NA |
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | 28.5169 | 28.5177 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | 0.806901 | 0.80768 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | 1 | 1.01838 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | 0 | -0.000795613 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | 1 | 1 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | -0.0617728 | -0.0585937 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | td_b0085:NA |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | 0.186549 | 0.179525 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | 0.657254 | 0.691868 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | 0.000366931 | 0.000441454 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | 0 | 0 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | 0.139355 | 0.137911 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | 31.9908 | 31.9847 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | 29.263 | 29.2354 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | 28.2453 | 28.254 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | 0.800445 | 0.801412 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | 0.147005 | 0.145386 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | 1.12018 | 1.08453 | td_b0085:INFO |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | 0.172233 | 0.170846 | td_b0085:INFO |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | 1 | 1.01757 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | 1 | 1.02349 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | 1 | 1.02329 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | 1 | 1.01966 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_edge_vs_input_Y | 1 | 1.02772 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b0085:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_flat_vs_input_Y | 1 | 1.01396 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000119245 | td_b0085:PASS |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | 0.000400003 | 932.426 | td_b0085:INFO |
