<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Round-2 local tone mapping (HDR -> SDR 100 nit), Python reference

Means over 6 clips (whole frames at 1280 wide, 16 frames). See ltm_eval.py / tonemap_ref.py.

| candidate | lc ratio (global = lost) | restore | boost | halo | pump (PQ codes) | flicker | grain keep vs global | BTB / WTW kept (card) | py ms/frame |
|---|---|---|---|---|---|---|---|---|---|
| global | 0.681 | 0.00 | 0.000 | 0.00000 | 0.000 | 0.000 | 1.000 | 1.000 / 1.000 | 210 |
| dynamic | 0.703 | 0.04 | 0.000 | 0.00303 | 1.110 | 0.086 | 1.016 | 1.000 / 1.000 | 215 |
| cr | 0.721 | 0.13 | 0.000 | 0.00016 | 0.089 | 0.003 | 1.082 | 1.000 / 1.000 | 290 |
| ltm_gauss | 0.808 | 0.39 | 0.000 | 0.00171 | 0.936 | 0.039 | 1.126 | 1.000 / 1.000 | 521 |
| ltm_nograin | 0.750 | 0.22 | 0.000 | 0.00046 | 0.727 | 0.036 | 1.078 | 0.992 / 1.000 | 372 |
| ltm | 0.749 | 0.22 | 0.000 | 0.00046 | 0.737 | 0.037 | 1.068 | 0.992 / 1.000 | 564 |
| ltm_half | 0.714 | 0.11 | 0.000 | 0.00014 | 0.380 | 0.019 | 1.032 | 0.996 / 1.000 | 532 |
| ltm_mc | 0.751 | 0.22 | 0.000 | 0.00047 | 0.837 | 0.050 | 1.081 | 0.992 / 1.000 | 1071 |

## Codec artefacts after tone mapping, candidate vs global (all masters x encodes, 1080p raster)

step p99 / false-flat: mean of per-case ratios; band / dark MSE: ratio of summed MSE; blockiness: mean
difference candidate - global (BI is itself a difference, so a ratio is meaningless).

| candidate | step p99 ratio | false-flat ratio | band MSE ratio | dark MSE ratio | blockiness diff |
|---|---|---|---|---|---|
| global | 1.000 | 1.000 | 1.000 | 1.000 | +0.0000 |
| cr | 1.016 | 0.970 | 1.052 | 1.021 | +0.0003 |
| ltm | 1.148 | 1.096 | 1.314 | 1.300 | -0.0005 |
| ltm_mc | 1.165 | 1.114 | 1.351 | 1.323 | -0.0007 |

## Synthetic excursion card

| candidate | BTB kept | WTW kept | clip at limit | out min | out max |
|---|---|---|---|---|---|
| global | 1.000 | 1.000 | False | 20.2 | 1019.0 |
| dynamic | 1.000 | 1.000 | False | 20.2 | 1019.0 |
| cr | 1.000 | 1.000 | False | 20.2 | 1019.0 |
| ltm_gauss | 1.000 | 1.000 | False | 20.2 | 1019.0 |
| ltm_nograin | 0.992 | 1.000 | False | 20.2 | 1019.0 |
| ltm | 0.992 | 1.000 | False | 20.2 | 1019.0 |
| ltm_half | 0.996 | 1.000 | False | 20.2 | 1019.0 |
| ltm_mc | 0.992 | 1.000 | False | 20.2 | 1019.0 |

## Per clip

| clip | candidate | motion_px | src_cuts | lc_ratio | lc_s1 | lc_s3 | boost | restore | halo | pump | flicker | grain_keep | btb | wtw | mc_resets | py_ms | note |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sol_a | global | 8.168 | 2 | 0.7372 | 0.7424 | 0.7328 | 0 | 0 | 0 | 0 | 0 | 1 | - | - | - | 305.6 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | dynamic | 8.168 | 2 | 0.7824 | 0.787 | 0.7785 | 0 | 0.172 | 0.0004411 | 1.198 | 0.0638 | 1.022 | - | - | - | 261.7 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | cr | 8.168 | 2 | 0.7696 | 0.785 | 0.7551 | 0 | 0.123 | 0.0001467 | 0.08097 | 0.001016 | 1.063 | - | - | - | 331.1 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | ltm_gauss | 8.168 | 2 | 0.8581 | 0.8583 | 0.8519 | 0 | 0.4599 | 0.001547 | 1.17 | 0.01464 | 1.096 | - | - | - | 606.1 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | ltm_nograin | 8.168 | 2 | 0.7932 | 0.7832 | 0.7997 | 0 | 0.2129 | 0.0005065 | 1.005 | 0.0169 | 1.022 | - | - | - | 368.3 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | ltm | 8.168 | 2 | 0.7942 | 0.7849 | 0.8004 | 0 | 0.2169 | 0.000508 | 1.014 | 0.01699 | 1.029 | - | - | - | 547.5 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | ltm_half | 8.168 | 2 | 0.7651 | 0.7629 | 0.7661 | 0 | 0.106 | 0.000159 | 0.508 | 0.008477 | 1.013 | - | - | - | 570.9 | Sol Levante HDR10 4:4:4, bright animation |
| sol_a | ltm_mc | 8.168 | 2 | 0.7958 | 0.7875 | 0.8012 | 0 | 0.223 | 0.0005156 | 1.128 | 0.01729 | 1.044 | - | - | 2 | 1525 | Sol Levante HDR10 4:4:4, bright animation |
| sol_b | global | 3.497 | 0 | 0.6909 | 0.6888 | 0.6903 | 0 | 0 | 0 | 0 | 0 | 1 | - | - | - | 244.7 | Sol Levante, sky gradients |
| sol_b | dynamic | 3.497 | 0 | 0.7359 | 0.7346 | 0.7346 | 0 | 0.1455 | 0.003582 | 0.3657 | 0.1312 | 1.034 | - | - | - | 224.5 | Sol Levante, sky gradients |
| sol_b | cr | 3.497 | 0 | 0.7328 | 0.7431 | 0.7202 | 0 | 0.1355 | 0.0001538 | 0.02901 | 0.005078 | 1.082 | - | - | - | 236.9 | Sol Levante, sky gradients |
| sol_b | ltm_gauss | 3.497 | 0 | 0.8263 | 0.8235 | 0.8204 | 0 | 0.4381 | 0.001428 | 0.3536 | 0.06646 | 1.126 | - | - | - | 417.4 | Sol Levante, sky gradients |
| sol_b | ltm_nograin | 3.497 | 0 | 0.7766 | 0.7617 | 0.7841 | 0 | 0.2773 | 0.0006483 | 0.3542 | 0.06289 | 1.107 | - | - | - | 466.8 | Sol Levante, sky gradients |
| sol_b | ltm | 3.497 | 0 | 0.7752 | 0.7598 | 0.7832 | 0 | 0.2727 | 0.0006419 | 0.3558 | 0.06322 | 1.087 | - | - | - | 805.9 | Sol Levante, sky gradients |
| sol_b | ltm_half | 3.497 | 0 | 0.7321 | 0.7231 | 0.7361 | 0 | 0.1334 | 0.0002056 | 0.1786 | 0.03221 | 1.04 | - | - | - | 693.7 | Sol Levante, sky gradients |
| sol_b | ltm_mc | 3.497 | 0 | 0.7772 | 0.763 | 0.7842 | 0 | 0.2792 | 0.0006571 | 0.4254 | 0.07636 | 1.103 | - | - | 0 | 1632 | Sol Levante, sky gradients |
| noc_low | global | 1.004 | 0 | 0.7632 | 0.772 | 0.7541 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | - | - | 243.9 | Nocturne low-key, film grain |
| noc_low | dynamic | 1.004 | 0 | 0.6507 | 0.6523 | 0.6488 | 0 | -0.475 | 0.003791 | 0.2221 | -0.2187 | 0.823 | 1 | - | - | 268.9 | Nocturne low-key, film grain |
| noc_low | cr | 1.004 | 0 | 0.7956 | 0.8151 | 0.7758 | 0 | 0.1367 | 9.191e-05 | 0.014 | 0.005256 | 1.057 | 1 | - | - | 393.2 | Nocturne low-key, film grain |
| noc_low | ltm_gauss | 1.004 | 0 | 0.8305 | 0.8296 | 0.8257 | 0 | 0.2842 | 0.000493 | 0.09636 | 0.0932 | 1.08 | 1 | - | - | 794.2 | Nocturne low-key, film grain |
| noc_low | ltm_nograin | 1.004 | 0 | 0.8036 | 0.802 | 0.8016 | 0 | 0.1705 | 0.0001266 | 0.1037 | 0.04165 | 1.071 | 0.9969 | - | - | 397.4 | Nocturne low-key, film grain |
| noc_low | ltm | 1.004 | 0 | 0.8019 | 0.8009 | 0.8 | 0 | 0.1636 | 0.0001257 | 0.1031 | 0.04308 | 1.058 | 0.9984 | - | - | 604.5 | Nocturne low-key, film grain |
| noc_low | ltm_half | 1.004 | 0 | 0.7825 | 0.7862 | 0.7771 | 0 | 0.08149 | 4.637e-05 | 0.05181 | 0.02148 | 1.028 | 0.9984 | - | - | 543.4 | Nocturne low-key, film grain |
| noc_low | ltm_mc | 1.004 | 0 | 0.804 | 0.8037 | 0.8013 | 0 | 0.1723 | 0.0001358 | 0.1805 | 0.09944 | 1.07 | 0.9984 | - | 0 | 994 | Nocturne low-key, film grain |
| noc_bright | global | 19.95 | 0 | 0.7111 | 0.7285 | 0.6935 | 0 | 0 | 0 | 0 | 0 | 1 | - | - | - | 177.8 | Nocturne bright, film grain |
| noc_bright | dynamic | 19.95 | 0 | 0.731 | 0.7509 | 0.7108 | 0 | 0.06883 | 0.0002564 | 0.8263 | 0.05492 | 1.055 | - | - | - | 204.2 | Nocturne bright, film grain |
| noc_bright | cr | 19.95 | 0 | 0.7473 | 0.7783 | 0.7164 | 0 | 0.1252 | 5.123e-05 | 0.05561 | 0.001155 | 1.066 | - | - | - | 310.8 | Nocturne bright, film grain |
| noc_bright | ltm_gauss | 19.95 | 0 | 0.8054 | 0.8053 | 0.7978 | 0 | 0.3264 | 0.0008303 | 0.8267 | 0.0426 | 1.095 | - | - | - | 524.3 | Nocturne bright, film grain |
| noc_bright | ltm_nograin | 19.95 | 0 | 0.783 | 0.7825 | 0.7777 | 0 | 0.2487 | 0.0002207 | 0.8846 | 0.03885 | 1.09 | - | - | - | 403.4 | Nocturne bright, film grain |
| noc_bright | ltm | 19.95 | 0 | 0.7795 | 0.7798 | 0.7743 | 0 | 0.2366 | 0.0002144 | 0.8878 | 0.03956 | 1.071 | - | - | - | 499.1 | Nocturne bright, film grain |
| noc_bright | ltm_half | 19.95 | 0 | 0.7445 | 0.7534 | 0.7331 | 0 | 0.1156 | 5.988e-05 | 0.4452 | 0.01957 | 1.034 | - | - | - | 513.1 | Nocturne bright, film grain |
| noc_bright | ltm_mc | 19.95 | 0 | 0.7805 | 0.7817 | 0.7745 | 0 | 0.2401 | 0.0002266 | 0.9679 | 0.04552 | 1.081 | - | - | 0 | 844.9 | Nocturne bright, film grain |
| chimera | global | 12.26 | 0 | 0.5072 | 0.504 | 0.5107 | 0 | 0 | 0 | 0 | 0 | 1 | - | 1 | - | 143.2 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | dynamic | 12.26 | 0 | 0.5944 | 0.5899 | 0.5987 | 0 | 0.177 | 0.008015 | 2.631 | 0.289 | 1.06 | - | 1 | - | 142.6 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | cr | 12.26 | 0 | 0.5634 | 0.5787 | 0.5492 | 0 | 0.1141 | 0.0003942 | 0.3024 | 0.003647 | 1.131 | - | 1 | - | 199.1 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | ltm_gauss | 12.26 | 0 | 0.7196 | 0.7108 | 0.7171 | 0 | 0.431 | 0.004456 | 2.707 | 0.008525 | 1.211 | - | 1 | - | 388.2 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | ltm_nograin | 12.26 | 0 | 0.5783 | 0.5571 | 0.5947 | 0 | 0.1441 | 0.0007531 | 1.572 | 0.04659 | 1.044 | - | 0.3871 | - | 289.6 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | ltm | 12.26 | 0 | 0.5793 | 0.5582 | 0.5959 | 0 | 0.1462 | 0.0007678 | 1.615 | 0.04918 | 1.054 | - | 0.4516 | - | 396.6 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | ltm_half | 12.26 | 0 | 0.5433 | 0.5298 | 0.5544 | 0 | 0.07321 | 0.0002298 | 0.875 | 0.02765 | 1.021 | - | 0.6129 | - | 373.7 | Chimera PQ (8-bit master; 4000 nit assumed) |
| chimera | ltm_mc | 12.26 | 0 | 0.5812 | 0.5612 | 0.5968 | 0 | 0.1501 | 0.0007833 | 1.705 | 0.05306 | 1.078 | - | 0.4516 | 0 | 695.2 | Chimera PQ (8-bit master; 4000 nit assumed) |
| splice_cut | global | 7.629 | 1 | 0.6748 | 0.6826 | 0.667 | 0 | 0 | 0 | 0 | 0 | 1 | - | - | - | 145 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | dynamic | 7.629 | 1 | 0.725 | 0.7279 | 0.7221 | 0 | 0.1545 | 0.002109 | 1.416 | 0.1946 | 1.099 | - | - | - | 190.3 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | cr | 7.629 | 1 | 0.7203 | 0.7428 | 0.698 | 0 | 0.1399 | 0.0001317 | 0.05254 | 0.0007116 | 1.097 | - | - | - | 266.5 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | ltm_gauss | 7.629 | 1 | 0.8071 | 0.8028 | 0.8021 | 0 | 0.4069 | 0.001488 | 0.4592 | 0.01121 | 1.15 | - | - | - | 396.3 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | ltm_nograin | 7.629 | 1 | 0.7671 | 0.7586 | 0.7694 | 0 | 0.284 | 0.0005301 | 0.4415 | 0.01036 | 1.138 | - | - | - | 305.6 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | ltm | 7.629 | 1 | 0.7637 | 0.7548 | 0.7667 | 0 | 0.2733 | 0.0005218 | 0.4442 | 0.01072 | 1.111 | - | - | - | 527.8 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | ltm_half | 7.629 | 1 | 0.718 | 0.7173 | 0.7158 | 0 | 0.1328 | 0.0001589 | 0.2226 | 0.00543 | 1.052 | - | - | - | 499.2 | cut: Sol -> Nocturne at frame 8 |
| splice_cut | ltm_mc | 7.629 | 1 | 0.7651 | 0.7574 | 0.7671 | 0 | 0.2778 | 0.0005307 | 0.6174 | 0.008982 | 1.108 | - | - | 1 | 734.1 | cut: Sol -> Nocturne at frame 8 |

## Amplification per case

| master | raster | encode | candidate | step_p99 | false_flat | blockiness | band_mse | dark_mse | psnr |
|---|---|---|---|---|---|---|---|---|---|
| sol_sky | 1080p | x265-10_crf30 | global | 10.57 | 0.02963 | -0.004498 | 3.946 | 0.2346 | 38.09 |
| sol_sky | 1080p | x265-10_crf30 | cr | 11.02 | 0.0286 | -0.004069 | 4.495 | 0.2469 | 37.24 |
| sol_sky | 1080p | x265-10_crf30 | ltm | 13.54 | 0.02171 | -0.00523 | 6.044 | 0.2678 | 36.67 |
| sol_sky | 1080p | x265-10_crf30 | ltm_mc | 13.97 | 0.02194 | -0.005681 | 6.374 | 0.2725 | 36.49 |
| sol_sky | 1080p | aom-10_crf40 | global | 11.21 | 0.03085 | -0.01029 | 4.183 | 0.2846 | 38.52 |
| sol_sky | 1080p | aom-10_crf40 | cr | 11.67 | 0.03022 | -0.009023 | 4.785 | 0.2986 | 37.7 |
| sol_sky | 1080p | aom-10_crf40 | ltm | 13.63 | 0.02589 | -0.01225 | 5.899 | 0.3205 | 37.07 |
| sol_sky | 1080p | aom-10_crf40 | ltm_mc | 13.94 | 0.02657 | -0.01271 | 6.156 | 0.3237 | 36.91 |
| sol_dark | 1080p | x265-10_crf30 | global | 8.324 | 0.01026 | 0.01259 | 1.312 | 1.259 | 34.45 |
| sol_dark | 1080p | x265-10_crf30 | cr | 8.374 | 0.01038 | 0.01237 | 1.328 | 1.277 | 33.82 |
| sol_dark | 1080p | x265-10_crf30 | ltm | 8.796 | 0.01018 | 0.01172 | 1.417 | 1.438 | 34.78 |
| sol_dark | 1080p | x265-10_crf30 | ltm_mc | 8.929 | 0.01038 | 0.01191 | 1.457 | 1.476 | 34.69 |
| sol_dark | 1080p | aom-10_crf40 | global | 6.391 | 0.007686 | -0.01683 | 0.7714 | 0.6906 | 36.13 |
| sol_dark | 1080p | aom-10_crf40 | cr | 6.428 | 0.007766 | -0.0169 | 0.781 | 0.7003 | 35.4 |
| sol_dark | 1080p | aom-10_crf40 | ltm | 6.706 | 0.007643 | -0.01586 | 0.8264 | 0.7812 | 36.59 |
| sol_dark | 1080p | aom-10_crf40 | ltm_mc | 6.789 | 0.007921 | -0.01608 | 0.8459 | 0.7956 | 36.52 |
| nocturne_dark | 1080p | x265-10_crf30 | global | 8.458 | 0.1637 | 0.005372 | 3.728 | 4.525 | 37.74 |
| nocturne_dark | 1080p | x265-10_crf30 | cr | 8.527 | 0.1569 | 0.005436 | 3.777 | 4.633 | 37.21 |
| nocturne_dark | 1080p | x265-10_crf30 | ltm | 9.7 | 0.2 | 0.004439 | 4.733 | 5.937 | 37.32 |
| nocturne_dark | 1080p | x265-10_crf30 | ltm_mc | 9.734 | 0.202 | 0.00428 | 4.751 | 5.959 | 37.23 |
| nocturne_dark | 1080p | aom-10_crf40 | global | 6.564 | 0.2044 | 0.016 | 2.464 | 2.847 | 38.5 |
| nocturne_dark | 1080p | aom-10_crf40 | cr | 6.611 | 0.198 | 0.01685 | 2.494 | 2.912 | 37.94 |
| nocturne_dark | 1080p | aom-10_crf40 | ltm | 7.651 | 0.254 | 0.01645 | 3.202 | 3.816 | 38.11 |
| nocturne_dark | 1080p | aom-10_crf40 | ltm_mc | 7.7 | 0.2564 | 0.01642 | 3.246 | 3.867 | 38.03 |
| nocturne_mid | 1080p | x265-10_crf30 | global | 9.03 | 0.07275 | 0.03257 | 7.448 | 8.63 | 37.76 |
| nocturne_mid | 1080p | x265-10_crf30 | cr | 9.101 | 0.06862 | 0.0325 | 7.564 | 8.792 | 37.13 |
| nocturne_mid | 1080p | x265-10_crf30 | ltm | 10.19 | 0.09539 | 0.03118 | 9.372 | 11.37 | 37.4 |
| nocturne_mid | 1080p | x265-10_crf30 | ltm_mc | 10.31 | 0.09629 | 0.0311 | 9.549 | 11.59 | 37.26 |
| nocturne_mid | 1080p | aom-10_crf40 | global | 6.363 | 0.05765 | 0.01765 | 3.896 | 4.522 | 38.79 |
| nocturne_mid | 1080p | aom-10_crf40 | cr | 6.412 | 0.05337 | 0.01775 | 3.96 | 4.617 | 38.14 |
| nocturne_mid | 1080p | aom-10_crf40 | ltm | 7.266 | 0.08265 | 0.01814 | 4.98 | 5.971 | 38.52 |
| nocturne_mid | 1080p | aom-10_crf40 | ltm_mc | 7.366 | 0.08397 | 0.01799 | 5.115 | 6.146 | 38.37 |
