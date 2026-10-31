<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Mosquito headroom by codec x rate (means over 4 masters)

Near-edge mask = eval_metrics.near_edge_zone of the master; luma, 10-bit codes. See mosquito_headroom.py.

| raster | encode | kbps (full raster) | PSNR Y | ring RMS | near-edge HF share of error | smooth near-edge share | headroom dB (mean / max) |
|---|---|---|---|---|---|---|---|
| 2160p | x264 crf23 (H.264) | 46199 | 39.14 | 11.67 | 6.8 % | 0.4 % | 0.02 / 0.06 |
| 2160p | x264 crf28 (H.264) | 20730 | 36.90 | 14.81 | 5.8 % | 0.2 % | 0.01 / 0.04 |
| 2160p | x265 crf24 (HEVC) | 40103 | 39.45 | 10.29 | 6.4 % | 0.4 % | 0.02 / 0.06 |
| 2160p | x265 crf30 (HEVC) | 15457 | 36.97 | 13.12 | 5.2 % | 0.2 % | 0.01 / 0.04 |
| 2160p | aom crf30 (AV1) | 34546 | 39.39 | 9.87 | 6.5 % | 0.4 % | 0.02 / 0.07 |
| 2160p | aom crf40 (AV1) | 15954 | 37.53 | 12.20 | 5.6 % | 0.3 % | 0.01 / 0.05 |
| 1080p | x264 crf23 (H.264) | 14157 | 37.55 | 14.65 | 8.4 % | 0.2 % | 0.01 / 0.02 |
| 1080p | x264 crf28 (H.264) | 6573 | 35.18 | 19.15 | 7.4 % | 0.1 % | 0.01 / 0.01 |
| 1080p | x265 crf24 (HEVC) | 13523 | 37.92 | 12.95 | 7.7 % | 0.2 % | 0.01 / 0.02 |
| 1080p | x265 crf30 (HEVC) | 5448 | 35.28 | 17.54 | 6.7 % | 0.1 % | 0.01 / 0.01 |
| 1080p | aom crf30 (AV1) | 13135 | 38.10 | 11.96 | 8.0 % | 0.2 % | 0.01 / 0.03 |
| 1080p | aom crf40 (AV1) | 6105 | 35.93 | 15.62 | 7.3 % | 0.1 % | 0.01 / 0.01 |

## Per master

| master | raster | codec | encode | kbps_full | psnr | ring_rms | ring_share | smooth_share | headroom_db | zone_share | content |
|---|---|---|---|---|---|---|---|---|---|---|---|
| nocturne | 2160p | H.264 | x264 crf23 | 3.168e+04 | 38.63 | 10.83 | 0.04415 | 0.0008522 | 0.003703 | 0.05398 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 2160p | H.264 | x264 crf28 | 7486 | 37.49 | 12.5 | 0.04524 | 0.000681 | 0.002958 | 0.05398 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 2160p | HEVC | x265 crf24 | 1.952e+04 | 38.81 | 9.235 | 0.03345 | 0.0005471 | 0.002376 | 0.05397 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 2160p | HEVC | x265 crf30 | 4897 | 37.88 | 11.13 | 0.03925 | 0.0005341 | 0.00232 | 0.05397 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 2160p | AV1 | aom crf30 | 1.424e+04 | 39.36 | 7.684 | 0.02625 | 0.0005527 | 0.002401 | 0.05397 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 2160p | AV1 | aom crf40 | 4944 | 38.82 | 8.258 | 0.02682 | 0.0004868 | 0.002115 | 0.05397 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | H.264 | x264 crf23 | 5949 | 38.42 | 10.92 | 0.02314 | 0.00203 | 0.008824 | 0.02925 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | H.264 | x264 crf28 | 2043 | 37.71 | 13.19 | 0.0287 | 0.001919 | 0.008344 | 0.02925 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | HEVC | x265 crf24 | 3978 | 38.68 | 8.968 | 0.01661 | 0.001664 | 0.007235 | 0.02926 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | HEVC | x265 crf30 | 1334 | 38 | 11.39 | 0.0229 | 0.001576 | 0.006849 | 0.02926 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | AV1 | aom crf30 | 2104 | 38.87 | 7.464 | 0.012 | 0.001669 | 0.007255 | 0.02926 | film grain (synthetic Vision3), live action, PQ |
| nocturne | 1080p | AV1 | aom crf40 | 1187 | 38.52 | 8.452 | 0.01422 | 0.001651 | 0.007176 | 0.02926 | film grain (synthetic Vision3), live action, PQ |
| sol_levante | 2160p | H.264 | x264 crf23 | 3.273e+04 | 43.76 | 8.064 | 3.442e-05 | 2.776e-07 | 1.206e-06 | 2.331e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 2160p | H.264 | x264 crf28 | 1.528e+04 | 41.24 | 13.18 | 5.155e-05 | 6.678e-08 | 2.9e-07 | 2.331e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 2160p | HEVC | x265 crf24 | 1.907e+04 | 43.53 | 6.642 | 2.015e-05 | 0 | -0 | 2.119e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 2160p | HEVC | x265 crf30 | 7534 | 40.62 | 10.28 | 2.473e-05 | 0 | -0 | 2.119e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 2160p | AV1 | aom crf30 | 1.023e+04 | 42.16 | 7.823 | 2.037e-05 | 0 | -0 | 2.119e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 2160p | AV1 | aom crf40 | 3982 | 39.82 | 11.85 | 2.733e-05 | 0 | -0 | 2.119e-05 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | H.264 | x264 crf23 | 1.053e+04 | 41.3 | 13.85 | 0.0009152 | 4.606e-07 | 2e-06 | 0.0003699 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | H.264 | x264 crf28 | 4379 | 38.79 | 18.41 | 0.0009066 | 2.477e-07 | 1.076e-06 | 0.0003699 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | HEVC | x265 crf24 | 6679 | 41.08 | 12.57 | 0.0007071 | 2.539e-08 | 1.103e-07 | 0.000365 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | HEVC | x265 crf30 | 2384 | 38.38 | 16.91 | 0.0006867 | 3.924e-08 | 1.704e-07 | 0.000365 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | AV1 | aom crf30 | 3559 | 40.02 | 13.57 | 0.0006449 | 4.374e-08 | 1.899e-07 | 0.000365 | animation: hard edges, flat colour, PQ |
| sol_levante | 1080p | AV1 | aom crf40 | 1283 | 37.85 | 17.34 | 0.0006382 | 7.089e-09 | 3.079e-08 | 0.000365 | animation: hard edges, flat colour, PQ |
| tos4k | 2160p | H.264 | x264 crf23 | 2.666e+04 | 41.16 | 8.72 | 0.03449 | 0.0001306 | 0.0005674 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 2160p | H.264 | x264 crf28 | 1.473e+04 | 38.28 | 10.9 | 0.02778 | 7.768e-05 | 0.0003374 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 2160p | HEVC | x265 crf24 | 2.104e+04 | 41.77 | 7.877 | 0.03241 | 0.0001401 | 0.0006084 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 2160p | HEVC | x265 crf30 | 9350 | 38.51 | 10.09 | 0.02514 | 9.761e-05 | 0.0004239 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 2160p | AV1 | aom crf30 | 1.509e+04 | 41.32 | 7.998 | 0.03017 | 9.055e-05 | 0.0003933 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 2160p | AV1 | aom crf40 | 7873 | 38.87 | 9.868 | 0.0261 | 6.162e-05 | 0.0002676 | 0.03638 | film / CG mix, text-like edges |
| tos4k | 1080p | H.264 | x264 crf23 | 1.006e+04 | 38.28 | 11.09 | 0.08815 | 0.0005382 | 0.002338 | 0.1115 | film / CG mix, text-like edges |
| tos4k | 1080p | H.264 | x264 crf28 | 5374 | 35.21 | 14.46 | 0.07393 | 0.0003285 | 0.001427 | 0.1115 | film / CG mix, text-like edges |
| tos4k | 1080p | HEVC | x265 crf24 | 8966 | 39.1 | 9.605 | 0.07998 | 0.0004791 | 0.002081 | 0.1115 | film / CG mix, text-like edges |
| tos4k | 1080p | HEVC | x265 crf30 | 3998 | 35.51 | 13.18 | 0.06584 | 0.0003015 | 0.00131 | 0.1115 | film / CG mix, text-like edges |
| tos4k | 1080p | AV1 | aom crf30 | 6988 | 38.69 | 9.685 | 0.07385 | 0.000338 | 0.001468 | 0.1115 | film / CG mix, text-like edges |
| tos4k | 1080p | AV1 | aom crf40 | 3470 | 35.93 | 12.54 | 0.06571 | 0.0002188 | 0.0009503 | 0.1115 | film / CG mix, text-like edges |
| park_joy | 2160p | H.264 | x264 crf23 | 9.373e+04 | 33.03 | 19.06 | 0.195 | 0.01321 | 0.05774 | 0.2797 | high detail (trees), camera pan |
| park_joy | 2160p | H.264 | x264 crf28 | 4.542e+04 | 30.6 | 22.64 | 0.1572 | 0.008504 | 0.03709 | 0.2797 | high detail (trees), camera pan |
| park_joy | 2160p | HEVC | x265 crf24 | 1.008e+05 | 33.69 | 17.41 | 0.1894 | 0.01401 | 0.06127 | 0.2797 | high detail (trees), camera pan |
| park_joy | 2160p | HEVC | x265 crf30 | 4.005e+04 | 30.87 | 20.97 | 0.1436 | 0.008704 | 0.03797 | 0.2797 | high detail (trees), camera pan |
| park_joy | 2160p | AV1 | aom crf30 | 9.863e+04 | 34.72 | 15.98 | 0.2025 | 0.01522 | 0.06662 | 0.2797 | high detail (trees), camera pan |
| park_joy | 2160p | AV1 | aom crf40 | 4.702e+04 | 32.59 | 18.8 | 0.1716 | 0.01049 | 0.04578 | 0.2797 | high detail (trees), camera pan |
| park_joy | 1080p | H.264 | x264 crf23 | 3.009e+04 | 32.2 | 22.75 | 0.2231 | 0.004625 | 0.02013 | 0.2717 | high detail (trees), camera pan |
| park_joy | 1080p | H.264 | x264 crf28 | 1.45e+04 | 29 | 30.52 | 0.1922 | 0.002731 | 0.01188 | 0.2717 | high detail (trees), camera pan |
| park_joy | 1080p | HEVC | x265 crf24 | 3.447e+04 | 32.83 | 20.67 | 0.2126 | 0.005255 | 0.02288 | 0.2717 | high detail (trees), camera pan |
| park_joy | 1080p | HEVC | x265 crf30 | 1.407e+04 | 29.21 | 28.68 | 0.1781 | 0.002875 | 0.0125 | 0.2717 | high detail (trees), camera pan |
| park_joy | 1080p | AV1 | aom crf30 | 3.989e+04 | 34.85 | 17.11 | 0.232 | 0.006206 | 0.02704 | 0.2717 | high detail (trees), camera pan |
| park_joy | 1080p | AV1 | aom crf40 | 1.848e+04 | 31.42 | 24.16 | 0.2104 | 0.00332 | 0.01444 | 0.2717 | high detail (trees), camera pan |
