<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TemporalDetail on real streaming-like encodes (suites td_real, td_letterbox, td_noharm)

Default = `td_off` (the decoded input). Deltas = candidate - default; dB for PSNR. `gap` = PSNR(I+P) - PSNR(B) (the I/B pulse), `pulse` = frame-to-frame CV of fine-band energy out/master (breathing). Gates = input-relative grain (RMS, lag-1, four temporal bands), near-edge two-seed probe, ghost, smear, flicker, halo, BTB/WTW, GOP ratios: non-PASS flags listed. CPU model ms = td_cli Process() per 960x544 frame (one thread, double; NOT GPU-representative). Vectors: DIS-flow stand-in. Sections are named after the run directory; `a+b` merges a re-run. ERROR rows = the case ran out of memory while six runs shared the laptop (re-run serially where listed with +).

## run_td_b008 (td_b008)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | -0.005 | -0.005 | -0.004 | -0.005 | -0.00000 | -0.0001 / -0.0001 | 0.90 -> 0.90 | 0.168 -> 0.174 | 0.0003 | 1064 | PASS |
| film24 | tos | x265_1080p_4k | -0.018 | -0.023 | -0.007 | -0.019 | -0.00001 | -0.0001 / -0.0000 | 0.69 -> 0.69 | 0.059 -> 0.064 | 0.0009 | 1175 | PASS |
| film24 | sol | x265_2160p_12k | -0.001 | -0.003 | +0.000 | -0.001 | -0.00003 | -0.0001 / -0.0001 | 0.83 -> 0.83 | 0.409 -> 0.410 | 0.0024 | 695 | PASS |
| film24 | sol | x265_1080p_4k | -0.005 | -0.008 | +0.001 | -0.006 | +0.00000 | -0.0002 / -0.0002 | 1.28 -> 1.28 | 0.302 -> 0.305 | 0.0044 | 811 | PASS |
| nature30 | boat | x265_2160p_14k | -0.011 | -0.020 | +0.008 | -0.013 | +0.00008 | -0.0001 / -0.0001 | 0.84 -> 0.83 | 0.035 -> 0.031 | 0.0009 | 766 | PASS |
| nature30 | boat | x265_1080p_5k | -0.003 | -0.020 | +0.005 | -0.007 | +0.00041 | -0.0003 / -0.0003 | 1.16 -> 1.15 | 0.059 -> 0.057 | 0.0021 | 985 | PASS |
| sports50_60 | crowd | x265_2160p_16k | -0.003 | -0.027 | -0.006 | -0.009 | +0.00012 | -0.0010 / -0.0009 | 1.13 -> 1.11 | 0.187 -> 0.187 | 0.0019 | 803 | PASS |
| sports50_60 | crowd | x265_1080p_6k | +0.009 | -0.028 | -0.006 | +0.001 | +0.00097 | -0.0016 / -0.0014 | 1.12 -> 1.08 | 0.172 -> 0.171 | 0.0040 | 678 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 4 | -0.007 | -0.008 | -0.00001 | -0.0001 / -0.0001 | 0.93 -> 0.93 | 0.234 -> 0.238 | 0.0020 | 936 |
| nature30 | 2 | -0.007 | -0.010 | +0.00024 | -0.0002 / -0.0002 | 1.00 -> 0.99 | 0.047 -> 0.044 | 0.0015 | 875 |
| sports50_60 | 2 | +0.003 | -0.004 | +0.00054 | -0.0013 / -0.0012 | 1.12 -> 1.10 | 0.179 -> 0.179 | 0.0029 | 741 |
| ALL | 8 | -0.005 | -0.007 | +0.00019 | -0.0004 / -0.0004 | 1.00 -> 0.98 | 0.174 -> 0.175 | 0.0021 | 872 |

## run_td_b005 (td_b005)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | -0.005 | -0.005 | -0.004 | -0.005 | -0.00000 | -0.0001 / -0.0001 | 0.90 -> 0.90 | 0.168 -> 0.174 | 0.0003 | 1029 | PASS |
| film24 | tos | x265_1080p_4k | -0.018 | -0.023 | -0.007 | -0.019 | -0.00001 | -0.0001 / -0.0000 | 0.69 -> 0.69 | 0.059 -> 0.064 | 0.0009 | 1163 | PASS |
| film24 | sol | x265_2160p_12k | -0.001 | -0.003 | +0.000 | -0.001 | -0.00003 | -0.0001 / -0.0001 | 0.83 -> 0.83 | 0.409 -> 0.410 | 0.0024 | 688 | PASS |
| film24 | sol | x265_1080p_4k | -0.005 | -0.008 | +0.001 | -0.006 | +0.00000 | -0.0002 / -0.0002 | 1.28 -> 1.28 | 0.302 -> 0.305 | 0.0044 | 789 | PASS |
| nature30 | boat | x265_2160p_14k | -0.011 | -0.020 | +0.008 | -0.013 | +0.00008 | -0.0001 / -0.0001 | 0.84 -> 0.83 | 0.035 -> 0.031 | 0.0009 | 786 | PASS |
| nature30 | boat | x265_1080p_5k | -0.003 | -0.020 | +0.005 | -0.007 | +0.00041 | -0.0003 / -0.0003 | 1.16 -> 1.15 | 0.059 -> 0.057 | 0.0021 | 1008 | PASS |
| sports50_60 | crowd | x265_2160p_16k | NA | NA | NA | NA | NA | NA / NA | 1.13 -> NA | 0.187 -> NA | NA | NA | ERROR error_candidate |
| sports50_60 | crowd | x265_1080p_6k | +0.009 | -0.028 | -0.006 | +0.001 | +0.00097 | -0.0016 / -0.0014 | 1.12 -> 1.08 | 0.172 -> 0.171 | 0.0040 | 680 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 4 | -0.007 | -0.008 | -0.00001 | -0.0001 / -0.0001 | 0.93 -> 0.93 | 0.234 -> 0.238 | 0.0020 | 917 |
| nature30 | 2 | -0.007 | -0.010 | +0.00024 | -0.0002 / -0.0002 | 1.00 -> 0.99 | 0.047 -> 0.044 | 0.0015 | 897 |
| sports50_60 | 1 | +0.009 | +0.001 | +0.00097 | -0.0016 / -0.0014 | 1.12 -> 1.08 | 0.179 -> 0.171 | 0.0040 | 680 |
| ALL | 7 | -0.005 | -0.007 | +0.00020 | -0.0004 / -0.0003 | 1.00 -> 0.97 | 0.174 -> 0.173 | 0.0021 | 878 |

## run_td_b0085 (td_b0085)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | -0.005 | -0.005 | -0.004 | -0.005 | -0.00000 | -0.0001 / -0.0001 | 0.90 -> 0.90 | 0.168 -> 0.174 | 0.0003 | 1043 | PASS |
| film24 | tos | x265_1080p_4k | -0.018 | -0.023 | -0.007 | -0.019 | -0.00001 | -0.0001 / -0.0000 | 0.69 -> 0.69 | 0.059 -> 0.064 | 0.0009 | 1182 | PASS |
| film24 | sol | x265_2160p_12k | -0.001 | -0.003 | +0.000 | -0.001 | -0.00003 | -0.0001 / -0.0001 | 0.83 -> 0.83 | 0.409 -> 0.410 | 0.0024 | 687 | PASS |
| film24 | sol | x265_1080p_4k | -0.005 | -0.008 | +0.001 | -0.006 | +0.00000 | -0.0002 / -0.0002 | 1.28 -> 1.28 | 0.302 -> 0.305 | 0.0044 | 802 | PASS |
| nature30 | boat | x265_2160p_14k | -0.011 | -0.020 | +0.008 | -0.013 | +0.00008 | -0.0001 / -0.0001 | 0.84 -> 0.83 | 0.035 -> 0.031 | 0.0009 | 774 | PASS |
| nature30 | boat | x265_1080p_5k | -0.003 | -0.020 | +0.005 | -0.007 | +0.00041 | -0.0003 / -0.0003 | 1.16 -> 1.15 | 0.059 -> 0.057 | 0.0021 | 1014 | PASS |
| sports50_60 | crowd | x265_2160p_16k | NA | NA | NA | NA | NA | NA / NA | 1.13 -> NA | 0.187 -> NA | NA | NA | ERROR error_candidate |
| sports50_60 | crowd | x265_1080p_6k | +0.009 | -0.028 | -0.006 | +0.001 | +0.00097 | -0.0016 / -0.0014 | 1.12 -> 1.08 | 0.172 -> 0.171 | 0.0040 | 681 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 4 | -0.007 | -0.008 | -0.00001 | -0.0001 / -0.0001 | 0.93 -> 0.93 | 0.234 -> 0.238 | 0.0020 | 929 |
| nature30 | 2 | -0.007 | -0.010 | +0.00024 | -0.0002 / -0.0002 | 1.00 -> 0.99 | 0.047 -> 0.044 | 0.0015 | 894 |
| sports50_60 | 1 | +0.009 | +0.001 | +0.00097 | -0.0016 / -0.0014 | 1.12 -> 1.08 | 0.179 -> 0.171 | 0.0040 | 681 |
| ALL | 7 | -0.005 | -0.007 | +0.00020 | -0.0004 / -0.0003 | 1.00 -> 0.97 | 0.174 -> 0.173 | 0.0021 | 883 |

## run_td_b008_boost0 (td_b008_boost0)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | -0.000 | -0.000 | +0.000 | -0.000 | -0.00000 | NA / NA | 0.90 -> 0.90 | 0.168 -> 0.168 | 0.0003 | 845 | PASS |
| film24 | tos | x265_1080p_4k | +0.000 | +0.000 | +0.000 | +0.000 | +0.00000 | NA / NA | 0.69 -> 0.69 | 0.059 -> 0.059 | 0.0009 | 1123 | PASS |
| film24 | sol | x265_2160p_12k | +0.000 | +0.000 | +0.000 | +0.000 | +0.00000 | NA / NA | 0.83 -> 0.83 | 0.409 -> 0.409 | 0.0024 | 764 | PASS |
| film24 | sol | x265_1080p_4k | +0.000 | +0.000 | +0.001 | +0.000 | +0.00001 | NA / NA | 1.28 -> 1.28 | 0.302 -> 0.302 | 0.0044 | 650 | PASS |
| nature30 | boat | x265_2160p_14k | +0.000 | -0.000 | +0.000 | +0.000 | +0.00000 | NA / NA | 0.84 -> 0.84 | 0.035 -> 0.035 | 0.0009 | 1212 | PASS |
| nature30 | boat | x265_1080p_5k | +0.001 | -0.000 | +0.001 | +0.001 | +0.00001 | NA / NA | 1.16 -> 1.16 | 0.059 -> 0.059 | 0.0021 | 697 | PASS |
| sports50_60 | crowd | x265_2160p_16k | +0.000 | -0.000 | +0.000 | +0.000 | +0.00001 | NA / NA | 1.13 -> 1.13 | 0.187 -> 0.186 | 0.0019 | 832 | PASS |
| sports50_60 | crowd | x265_1080p_6k | +0.001 | -0.000 | +0.000 | +0.001 | +0.00003 | NA / NA | 1.12 -> 1.12 | 0.172 -> 0.172 | 0.0040 | 616 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 4 | +0.000 | +0.000 | +0.00000 | NA / NA | 0.93 -> 0.93 | 0.234 -> 0.234 | 0.0020 | 845 |
| nature30 | 2 | +0.001 | +0.000 | +0.00001 | NA / NA | 1.00 -> 1.00 | 0.047 -> 0.047 | 0.0015 | 955 |
| sports50_60 | 2 | +0.001 | +0.000 | +0.00002 | NA / NA | 1.12 -> 1.12 | 0.179 -> 0.179 | 0.0029 | 724 |
| ALL | 8 | +0.000 | +0.000 | +0.00001 | NA / NA | 1.00 -> 1.00 | 0.174 -> 0.174 | 0.0021 | 842 |

## run_td_b008_trust1 (td_b008_trust1)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | tos | x265_2160p_12k | -0.005 | -0.005 | -0.004 | -0.005 | -0.00000 | -0.0001 / -0.0001 | 0.90 -> 0.90 | 0.168 -> 0.174 | 0.0007 | 1181 | PASS |
| film24 | tos | x265_1080p_4k | -0.019 | -0.025 | -0.006 | -0.020 | -0.00001 | -0.0001 / -0.0000 | 0.69 -> 0.69 | 0.059 -> 0.064 | 0.0035 | 721 | PASS |
| film24 | sol | x265_2160p_12k | -0.000 | -0.003 | +0.001 | -0.001 | -0.00002 | -0.0001 / -0.0001 | 0.83 -> 0.83 | 0.409 -> 0.410 | 0.0041 | 557 | PASS |
| film24 | sol | x265_1080p_4k | -0.005 | -0.008 | +0.001 | -0.006 | +0.00001 | -0.0002 / -0.0002 | 1.28 -> 1.28 | 0.302 -> 0.305 | 0.0070 | 546 | PASS |
| nature30 | boat | x265_2160p_14k | -0.011 | -0.020 | +0.008 | -0.013 | +0.00008 | -0.0001 / -0.0001 | 0.84 -> 0.83 | 0.035 -> 0.031 | 0.0015 | 645 | PASS |
| nature30 | boat | x265_1080p_5k | -0.004 | -0.020 | +0.004 | -0.007 | +0.00041 | -0.0003 / -0.0003 | 1.16 -> 1.15 | 0.059 -> 0.057 | 0.0030 | 511 | PASS |
| sports50_60 | crowd | x265_2160p_16k | -0.003 | -0.027 | -0.005 | -0.008 | +0.00014 | -0.0010 / -0.0009 | 1.13 -> 1.11 | 0.187 -> 0.187 | 0.0059 | 501 | PASS |
| sports50_60 | crowd | x265_1080p_6k | +0.008 | -0.028 | -0.006 | +0.000 | +0.00101 | -0.0016 / -0.0014 | 1.12 -> 1.09 | 0.172 -> 0.171 | 0.0109 | 378 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 4 | -0.007 | -0.008 | -0.00001 | -0.0001 / -0.0001 | 0.93 -> 0.93 | 0.234 -> 0.238 | 0.0038 | 751 |
| nature30 | 2 | -0.007 | -0.010 | +0.00024 | -0.0002 / -0.0002 | 1.00 -> 0.99 | 0.047 -> 0.044 | 0.0022 | 578 |
| sports50_60 | 2 | +0.002 | -0.004 | +0.00058 | -0.0013 / -0.0011 | 1.12 -> 1.10 | 0.179 -> 0.179 | 0.0084 | 440 |
| ALL | 8 | -0.005 | -0.007 | +0.00020 | -0.0004 / -0.0004 | 1.00 -> 0.98 | 0.174 -> 0.175 | 0.0046 | 630 |

## lb_td_b008 (td_b008)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | toslb | x265_2160p_12k | -0.010 | -0.011 | -0.007 | -0.010 | -0.00000 | -0.0000 / -0.0000 | 1.07 -> 1.07 | 0.072 -> 0.073 | 0.0001 | 748 | PASS |
| film24 | toslb | x265_1080p_4k | -0.019 | -0.018 | -0.009 | -0.019 | -0.00001 | -0.0001 / -0.0001 | 0.68 -> 0.68 | 0.038 -> 0.044 | 0.0006 | 734 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| film24 | 2 | -0.015 | -0.014 | -0.00001 | -0.0001 / -0.0001 | 0.87 -> 0.88 | 0.055 -> 0.058 | 0.0003 | 741 |
| ALL | 2 | -0.015 | -0.014 | -0.00001 | -0.0001 / -0.0001 | 0.87 -> 0.88 | 0.055 -> 0.058 | 0.0003 | 741 |

Active-picture rectangle (letterbox; codes vs the input; candidate (default)): bar change max must be 0, band = +-16 rows around the picture/bar boundary, overshoot/undershoot vs the input's 5x5 local range.

| clip | case | lb_bar_change_max_Y | lb_band_change_rms_Y | lb_band_overshoot_mean_Y | lb_band_undershoot_max_Y |
|---|---|---|---|---|---|
| toslb | x265_2160p_12k | 9.2680 (0.0000) | 0.5959 (0.0000) | 0.0213 (0.0000) | 9.1343 (0.0000) |
| toslb | x265_1080p_4k | 10.5725 (0.0000) | 0.7834 (0.0000) | 0.0233 (0.0000) | 8.3870 (0.0000) |

## noharm_td_b008 (td_b008)

| content | clip | case | dPSNR B | dPSNR P | dPSNR I | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms | gates |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| film24 | nh_35mm | noharm_input_ref | NA | NA | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.005 | 0.0001 | 560 | PASS |
| film24 | nh_16mm | noharm_input_ref | NA | NA | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.015 | 0.0023 | 912 | smear_Y:FLAG |
| dark | nh_dark | noharm_input_ref | NA | NA | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.008 | 0.0006 | 998 | PASS |
| nature30 | nh_river | noharm_input_ref | NA | NA | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.018 | 0.0065 | 564 | smear_Y:FLAG |
| sports50_60 | nh_goal | noharm_input_ref | NA | NA | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.013 | 0.0004 | 1095 | PASS |

Means by content type:

| content | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS B / all | gap in -> out | pulse in -> out | weight | model ms |
|---|---|---|---|---|---|---|---|---|---|
| dark | 0 | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.008 | 0.0006 | 998 |
| film24 | 0 | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.010 | 0.0012 | 736 |
| nature30 | 0 | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.018 | 0.0065 | 564 |
| sports50_60 | 0 | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.013 | 0.0004 | 1095 |
| ALL | 0 | NA | NA | NA | NA / NA | NA -> NA | 0.000 -> 0.012 | 0.0020 | 826 |

