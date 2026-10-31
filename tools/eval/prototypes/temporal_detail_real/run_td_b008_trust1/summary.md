<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Prototype summary: temporal_detail family, default `td_off`

Suite `td_real`. Key metrics per case; flags are the harness verdicts vs the default (full rows in `<candidate>.md`).

## Means over cases

| metric | default | td_b008_trust1 |
|---|---|---|
| psnr_B_Y | 37.1584 | 38.4249 |
| psnr_P_Y | 38.0699 | 39.3128 |
| psnr_I_Y | 39.7151 | 40.8183 |
| psnr_Y | 37.409 | 38.6709 |
| ssim_B_Y | 0.919951 | 0.937109 |
| ssim_Y | 0.922409 | 0.938972 |
| lpips_B | 0.0775072 | 0.0673155 |
| lpips_all | 0.074667 | 0.065191 |
| ib_gap_db_Y | 0.995317 | 0.96978 |
| detail_pulse_Y | 0.173895 | 0.175501 |
| gop_ratio_minigop | NA | NA |
| flicker_excess_Y | -0.0146226 | -0.00733504 |
| grain_keep_vs_input_Y | 1 | 1.00808 |
| grain_lag1_vs_input_Y | 0 | -0.000273979 |
| grain_keep_vs_input_band1_Y | 1 | 1.00782 |
| grain_keep_vs_input_band2_Y | 1 | 1.00829 |
| grain_keep_vs_input_band3_Y | 1 | 1.00862 |
| grain_keep_vs_input_band4_Y | 1 | 1.00843 |
| grain_probe_keep_edge_vs_input_Y | 1 | 1.01711 |
| grain_probe_lag1_edge_vs_input_Y | 0 | 0.00103023 |
| grain_probe_keep_flat_vs_input_Y | 1 | 1.00531 |
| grain_probe_lag1_flat_vs_input_Y | 0 | 0.000138198 |
| ghost_coeff_Y | 0.0579582 | 0.0361722 |
| smear_Y | 0.824461 | 0.866564 |
| halo_overshoot_Y | 0.000185865 | 0.000184091 |
| lc_boost_fraction_Y | 0 | 0 |
| btb_kept_vs_input_Y | 1 | 0.999999 |
| wtw_kept_vs_input_Y | 1 | 1 |
| cost_ms_per_frame_pyref | 0.00035625 | 762.147 |

## Per case

| content_type | clip | case | metric | default | td_b008_trust1 | flags |
|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | psnr_Y | 48.0976 | 48.0929 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | ssim_Y | 0.989694 | 0.98969 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1.00028 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_lag1_vs_input_Y | 0 | -2.08413e-06 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_2160p_12k | wtw_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_2160p_12k | flicker_excess_Y | -0.000143152 | -0.0001271 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_2160p_12k | ghost_coeff_Y | 0.00218632 | 0.00204793 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | smear_Y | 0.902687 | 0.906703 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | halo_overshoot_Y | 8.25973e-06 | 9.41841e-06 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | lpips_all | 0.0415157 | 0.041457 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | psnr_I_Y | 48.7149 | 48.7109 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | psnr_P_Y | 48.7726 | 48.768 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | psnr_B_Y | 47.8654 | 47.8606 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | ssim_B_Y | 0.989256 | 0.989252 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | lpips_B | 0.0436596 | 0.0435994 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | ib_gap_db_Y | 0.90131 | 0.901641 | td_b008_trust1:INFO |
| film24 | tos | x265_2160p_12k | detail_pulse_Y | 0.168458 | 0.174178 | td_b008_trust1:INFO |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1.00027 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1.00026 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1.0003 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1.00029 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01587 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1 | 1.0003 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 0 | -2.67976e-06 | td_b008_trust1:PASS |
| film24 | tos | x265_2160p_12k | cost_ms_per_frame_pyref | 0.000249998 | 1459.9 | td_b008_trust1:INFO |
| film24 | tos | x265_1080p_4k | psnr_Y | 47.5554 | 47.5355 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | ssim_Y | 0.990652 | 0.990644 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1.00232 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_lag1_vs_input_Y | 0 | -5.71513e-05 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | btb_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_1080p_4k | wtw_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_1080p_4k | flicker_excess_Y | -0.000480672 | -0.000269241 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_1080p_4k | ghost_coeff_Y | 0.00289956 | 0.00173779 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | smear_Y | 0.955935 | 0.969358 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | halo_overshoot_Y | 2.01293e-05 | 2.56124e-05 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | lpips_all | 0.028675 | 0.0286299 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | psnr_I_Y | 46.8961 | 46.8896 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | psnr_P_Y | 48.2199 | 48.1951 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | psnr_B_Y | 47.3734 | 47.3542 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | ssim_B_Y | 0.990408 | 0.990402 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | lpips_B | 0.030132 | 0.0300775 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | ib_gap_db_Y | 0.694451 | 0.69119 | td_b008_trust1:INFO |
| film24 | tos | x265_1080p_4k | detail_pulse_Y | 0.0585832 | 0.0636011 | td_b008_trust1:INFO |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1.00228 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1.00226 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1.00237 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1 | 1.00235 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1 | 1.0136 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | tos | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00226 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 0 | 4.21748e-06 | td_b008_trust1:PASS |
| film24 | tos | x265_1080p_4k | cost_ms_per_frame_pyref | 0.000400003 | 654.912 | td_b008_trust1:INFO |
| film24 | sol | x265_2160p_12k | psnr_Y | 39.4187 | 39.4177 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | ssim_Y | 0.956944 | 0.956917 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_Y | 1 | 1.00535 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_lag1_vs_input_Y | 0 | -1.16832e-05 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | btb_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | sol | x265_2160p_12k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | flicker_excess_Y | -0.00370042 | -0.00365407 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| film24 | sol | x265_2160p_12k | ghost_coeff_Y | 0.0116768 | 0.0110944 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | smear_Y | 0.853971 | 0.861801 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | halo_overshoot_Y | 0.000144943 | 0.000152934 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | lpips_all | 0.05574 | 0.0556796 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | psnr_I_Y | 41.1011 | 41.1019 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | psnr_P_Y | 39.933 | 39.9297 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | psnr_B_Y | 39.2027 | 39.2023 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | ssim_B_Y | 0.956301 | 0.956277 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | lpips_B | 0.0569565 | 0.056884 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | ib_gap_db_Y | 0.833961 | 0.831355 | td_b008_trust1:INFO |
| film24 | sol | x265_2160p_12k | detail_pulse_Y | 0.4089 | 0.410154 | td_b008_trust1:INFO |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band1_Y | 1 | 1.00549 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band2_Y | 1 | 1.00532 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band3_Y | 1 | 1.0053 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_keep_vs_input_band4_Y | 1 | 1.00526 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_keep_edge_vs_input_Y | 1 | 1.009 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | sol | x265_2160p_12k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00127 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | grain_probe_lag1_flat_vs_input_Y | 0 | 4.20779e-05 | td_b008_trust1:PASS |
| film24 | sol | x265_2160p_12k | cost_ms_per_frame_pyref | 0.000399996 | 756.397 | td_b008_trust1:INFO |
| film24 | sol | x265_1080p_4k | psnr_Y | 36.0479 | 36.0423 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | ssim_Y | 0.930355 | 0.930353 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_Y | 1 | 1.01128 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_lag1_vs_input_Y | 0 | -0.000293956 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | btb_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | flicker_excess_Y | -0.00783003 | -0.00776493 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| film24 | sol | x265_1080p_4k | ghost_coeff_Y | 0.0225282 | 0.0202166 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | smear_Y | 0.813301 | 0.83628 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | halo_overshoot_Y | 0.000277539 | 0.00030808 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | lpips_all | 0.0665992 | 0.0664046 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | psnr_I_Y | 39.1247 | 39.1254 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | psnr_P_Y | 36.8291 | 36.8213 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | psnr_B_Y | 35.7277 | 35.7226 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | ssim_B_Y | 0.927703 | 0.927712 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | lpips_B | 0.0681223 | 0.067901 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | ib_gap_db_Y | 1.28348 | 1.28126 | td_b008_trust1:INFO |
| film24 | sol | x265_1080p_4k | detail_pulse_Y | 0.301997 | 0.305353 | td_b008_trust1:INFO |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band1_Y | 1 | 1.01093 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band2_Y | 1 | 1.01094 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band3_Y | 1 | 1.01131 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_keep_vs_input_band4_Y | 1 | 1.01189 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01779 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| film24 | sol | x265_1080p_4k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00361 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | grain_probe_lag1_flat_vs_input_Y | 0 | -0.000545705 | td_b008_trust1:PASS |
| film24 | sol | x265_1080p_4k | cost_ms_per_frame_pyref | 0.000599997 | 603.661 | td_b008_trust1:INFO |
| nature30 | boat | x265_2160p_14k | psnr_Y | 36.6265 | 36.614 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | ssim_Y | 0.954663 | 0.954729 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_Y | 1 | 1.00655 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_lag1_vs_input_Y | 0 | -0.000461038 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | btb_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | wtw_kept_vs_input_Y | NA | NA | td_b008_trust1:NA |
| nature30 | boat | x265_2160p_14k | flicker_excess_Y | 0.000279709 | 0.000885213 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| nature30 | boat | x265_2160p_14k | ghost_coeff_Y | 0.0335583 | 0.0279552 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | smear_Y | 0.891075 | 0.912548 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | halo_overshoot_Y | 0.000132016 | 0.000157716 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | lpips_all | 0.030902 | 0.0307965 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | psnr_I_Y | 39.641 | 39.6488 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | psnr_P_Y | 37.0888 | 37.0691 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | psnr_B_Y | 36.41 | 36.3993 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | ssim_B_Y | 0.953512 | 0.95359 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | lpips_B | 0.0313815 | 0.0312637 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | ib_gap_db_Y | 0.836041 | 0.828256 | td_b008_trust1:INFO |
| nature30 | boat | x265_2160p_14k | detail_pulse_Y | 0.0354826 | 0.0313049 | td_b008_trust1:INFO |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band1_Y | 1 | 1.00599 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band2_Y | 1 | 1.00623 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band3_Y | 1 | 1.00697 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_keep_vs_input_band4_Y | 1 | 1.00696 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01936 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| nature30 | boat | x265_2160p_14k | grain_probe_keep_flat_vs_input_Y | 1 | 1.01065 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000524634 | td_b008_trust1:PASS |
| nature30 | boat | x265_2160p_14k | cost_ms_per_frame_pyref | 0.000300002 | 590.386 | td_b008_trust1:INFO |
| nature30 | boat | x265_1080p_5k | psnr_Y | 31.8428 | 31.8356 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | ssim_Y | 0.920042 | 0.920397 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_Y | 1 | 1.01477 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_lag1_vs_input_Y | 0 | -4.76404e-05 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | btb_kept_vs_input_Y | 1 | 0.999996 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | flicker_excess_Y | -0.00688464 | -0.00614612 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| nature30 | boat | x265_1080p_5k | ghost_coeff_Y | 0.0668913 | 0.0582615 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | smear_Y | 0.836716 | 0.868174 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | halo_overshoot_Y | 0.000299464 | 0.000366991 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | lpips_all | 0.0440061 | 0.0436995 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | psnr_I_Y | 35.6116 | 35.6161 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | psnr_P_Y | 32.5329 | 32.5127 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | psnr_B_Y | 31.5496 | 31.5459 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | ssim_B_Y | 0.916379 | 0.916787 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | lpips_B | 0.0454936 | 0.0451852 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | ib_gap_db_Y | 1.16339 | 1.14785 | td_b008_trust1:INFO |
| nature30 | boat | x265_1080p_5k | detail_pulse_Y | 0.0589656 | 0.0572966 | td_b008_trust1:INFO |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band1_Y | 1 | 1.01495 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band2_Y | 1 | 1.01456 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band3_Y | 1 | 1.01471 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_keep_vs_input_band4_Y | 1 | 1.01494 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_edge_vs_input_Y | 1 | 1.01825 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_edge_vs_input_Y | 0 | 0.00103023 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_keep_flat_vs_input_Y | 1 | 1.01 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.00084084 | td_b008_trust1:PASS |
| nature30 | boat | x265_1080p_5k | cost_ms_per_frame_pyref | 0.000300002 | 581.702 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_2160p_16k | psnr_Y | 31.1665 | 31.1581 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | ssim_Y | 0.83002 | 0.830076 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_Y | 1 | 1.01599 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_lag1_vs_input_Y | 0 | -0.0010443 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | btb_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | wtw_kept_vs_input_Y | 1 | 1 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | flicker_excess_Y | -0.0364489 | -0.034269 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_2160p_16k | ghost_coeff_Y | 0.137376 | 0.131892 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | smear_Y | 0.684746 | 0.711087 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | halo_overshoot_Y | 0.000237639 | 0.000267885 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | lc_boost_fraction_Y | 0 | 0 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | lpips_all | 0.190543 | 0.18967 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | psnr_I_Y | 34.6409 | 34.6355 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | psnr_P_Y | 31.9202 | 31.8934 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | psnr_B_Y | 30.8928 | 30.8894 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | ssim_B_Y | 0.825603 | 0.825744 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | lpips_B | 0.197307 | 0.196298 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | ib_gap_db_Y | 1.12972 | 1.10691 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_2160p_16k | detail_pulse_Y | 0.186542 | 0.186619 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band1_Y | 1 | 1.0148 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band2_Y | 1 | 1.01847 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band3_Y | 1 | 1.01936 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_keep_vs_input_band4_Y | 1 | 1.01729 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_edge_vs_input_Y | 1 | 1.02591 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_keep_flat_vs_input_Y | 1 | 1.00905 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | grain_probe_lag1_flat_vs_input_Y | 0 | 0.000104002 | td_b008_trust1:PASS |
| sports50_60 | crowd | x265_2160p_16k | cost_ms_per_frame_pyref | 0.000400003 | 688.072 | td_b008_trust1:INFO |
| sports50_60 | crowd | x265_1080p_6k | psnr_Y | 28.5169 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | ssim_Y | 0.806901 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_lag1_vs_input_Y | 0 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | btb_kept_vs_input_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | wtw_kept_vs_input_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | flicker_excess_Y | -0.0617728 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | gop_ratio_minigop | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | ghost_coeff_Y | 0.186549 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | smear_Y | 0.657254 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | halo_overshoot_Y | 0.000366931 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | lc_boost_fraction_Y | 0 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | lpips_all | 0.139355 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | psnr_I_Y | 31.9908 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | psnr_P_Y | 29.263 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | psnr_B_Y | 28.2453 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | ssim_B_Y | 0.800445 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | lpips_B | 0.147005 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | ib_gap_db_Y | 1.12018 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | detail_pulse_Y | 0.172233 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band1_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band2_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band3_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_keep_vs_input_band4_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_edge_vs_input_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_edge_vs_input_Y | NA | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_keep_flat_vs_input_Y | 1 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | grain_probe_lag1_flat_vs_input_Y | 0 | NA | td_b008_trust1:NA |
| sports50_60 | crowd | x265_1080p_6k | cost_ms_per_frame_pyref | 0.000200002 | NA | td_b008_trust1:NA |
