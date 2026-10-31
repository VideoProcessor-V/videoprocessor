<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Ringing repair in the stack (post-TM SDR output; 4 HDR masters, x265-10 CRF 30, 1080p)

ring = near-edge smooth-band ringing energy, summed over cases relative to (same stage, same texture, repair off); also relative to plain/none/off in brackets. repair py ms/frame mean: 3339. See ringing_stack.py.

| stage | texture | ringing repair | ring vs off (vs plain/none/off) | halo | lc | keep near-edge / flat vs off | PSNR |
|---|---|---|---|---|---|---|---|
| plain | none | off | 1.000 (1.000) | 0.00000 | 0.550 | 1.000 / 1.000 | 37.49 |
| plain | none | early | 0.999 (0.999) | 0.00000 | 0.550 | 1.000 / 1.000 | 37.49 |
| plain | none | late | 0.999 (0.999) | 0.00000 | 0.550 | 1.000 / 1.000 | 37.49 |
| plain | none | both | 0.998 (0.998) | 0.00000 | 0.550 | 1.000 / 1.000 | 37.49 |
| plain | oracle | off | 1.000 (0.088) | 0.00000 | 0.690 | 1.000 / 1.000 | 42.04 |
| plain | oracle | early | 1.000 (0.088) | 0.00000 | 0.690 | 0.996 / 1.000 | 42.04 |
| plain | oracle | late | 1.000 (0.088) | 0.00000 | 0.690 | 0.998 / 1.000 | 42.04 |
| plain | oracle | both | 1.000 (0.088) | 0.00000 | 0.689 | 0.994 / 1.000 | 42.04 |
| crgs 0.30 | none | off | 1.000 (1.101) | 0.00006 | 0.561 | 1.000 / 1.000 | 36.99 |
| crgs 0.30 | none | early | 0.999 (1.100) | 0.00006 | 0.561 | 1.000 / 1.000 | 36.99 |
| crgs 0.30 | none | late | 0.999 (1.100) | 0.00006 | 0.561 | 1.000 / 1.000 | 36.99 |
| crgs 0.30 | none | both | 0.998 (1.099) | 0.00006 | 0.561 | 1.000 / 1.000 | 36.99 |
| crgs 0.30 | oracle | off | 1.000 (0.100) | 0.00005 | 0.705 | 1.000 / 1.000 | 41.68 |
| crgs 0.30 | oracle | early | 1.000 (0.100) | 0.00005 | 0.705 | 0.996 / 1.000 | 41.68 |
| crgs 0.30 | oracle | late | 0.999 (0.100) | 0.00005 | 0.705 | 0.998 / 1.000 | 41.68 |
| crgs 0.30 | oracle | both | 0.999 (0.100) | 0.00005 | 0.705 | 0.994 / 1.000 | 41.68 |
| pe | none | off | 1.000 (0.913) | 0.00041 | 0.598 | 1.000 / 1.000 | 37.11 |
| pe | none | early | 0.999 (0.913) | 0.00041 | 0.598 | 1.000 / 1.000 | 37.11 |
| pe | none | late | 0.999 (0.912) | 0.00041 | 0.598 | 1.000 / 1.000 | 37.11 |
| pe | none | both | 0.999 (0.912) | 0.00041 | 0.598 | 1.000 / 1.000 | 37.11 |
| pe | oracle | off | 1.000 (0.086) | 0.00027 | 0.732 | 1.000 / 1.000 | 41.13 |
| pe | oracle | early | 1.000 (0.086) | 0.00027 | 0.732 | 0.997 / 1.000 | 41.13 |
| pe | oracle | late | 1.000 (0.086) | 0.00027 | 0.732 | 0.998 / 1.000 | 41.13 |
| pe | oracle | both | 1.000 (0.086) | 0.00027 | 0.732 | 0.995 / 1.000 | 41.13 |

## All rows

| case | stage | texture | ringing_repair | ring | halo | lc | keep_in_edge | keep_in_flat | psnr |
|---|---|---|---|---|---|---|---|---|---|
| sol_sky | plain | none | off | 4.9863 | 0 | 0.64687 | 1 | 1 | 38.942 |
| sol_sky | plain | none | early | 4.9788 | 2.1847e-11 | 0.64686 | 0.99987 | 0.99998 | 38.942 |
| sol_sky | plain | none | late | 4.9758 | 2.0212e-11 | 0.64685 | 0.99996 | 0.99995 | 38.942 |
| sol_sky | plain | none | both | 4.9686 | 4.1724e-11 | 0.64685 | 0.99983 | 0.99994 | 38.942 |
| sol_sky | plain | oracle | off | 1.0251 | 0 | 0.65288 | 1 | 1 | 41.659 |
| sol_sky | plain | oracle | early | 1.0237 | 0 | 0.65288 | 0.99992 | 0.99997 | 41.659 |
| sol_sky | plain | oracle | late | 1.024 | 0 | 0.65287 | 0.99997 | 0.99999 | 41.66 |
| sol_sky | plain | oracle | both | 1.0227 | 0 | 0.65287 | 0.99989 | 0.99996 | 41.66 |
| sol_sky | crgs 0.30 | none | off | 5.6901 | 6.8756e-05 | 0.66602 | 1 | 1 | 38.151 |
| sol_sky | crgs 0.30 | none | early | 5.6787 | 6.8653e-05 | 0.66601 | 0.99989 | 0.99998 | 38.151 |
| sol_sky | crgs 0.30 | none | late | 5.6688 | 6.8461e-05 | 0.666 | 0.99997 | 0.99994 | 38.152 |
| sol_sky | crgs 0.30 | none | both | 5.6578 | 6.8359e-05 | 0.66599 | 0.99987 | 0.99992 | 38.152 |
| sol_sky | crgs 0.30 | oracle | off | 1.0874 | 7.642e-05 | 0.67404 | 1 | 1 | 41.165 |
| sol_sky | crgs 0.30 | oracle | early | 1.0857 | 7.6332e-05 | 0.67403 | 0.99993 | 0.99996 | 41.165 |
| sol_sky | crgs 0.30 | oracle | late | 1.085 | 7.6156e-05 | 0.67402 | 0.99998 | 0.99997 | 41.166 |
| sol_sky | crgs 0.30 | oracle | both | 1.0834 | 7.607e-05 | 0.67402 | 0.99991 | 0.99993 | 41.166 |
| sol_sky | pe | none | off | 4.0445 | 0.00068833 | 0.74959 | 1 | 1 | 37.781 |
| sol_sky | pe | none | early | 4.0421 | 0.00068826 | 0.74959 | 0.99988 | 1 | 37.781 |
| sol_sky | pe | none | late | 4.0319 | 0.00068796 | 0.74957 | 0.99997 | 0.99999 | 37.781 |
| sol_sky | pe | none | both | 4.0296 | 0.00068789 | 0.74957 | 0.99985 | 0.99999 | 37.782 |
| sol_sky | pe | oracle | off | 1.0433 | 0.00074899 | 0.75342 | 1 | 1 | 39.976 |
| sol_sky | pe | oracle | early | 1.0425 | 0.00074897 | 0.75341 | 0.99994 | 1 | 39.976 |
| sol_sky | pe | oracle | late | 1.0406 | 0.0007486 | 0.7534 | 0.99997 | 1 | 39.976 |
| sol_sky | pe | oracle | both | 1.0398 | 0.00074857 | 0.7534 | 0.99991 | 1 | 39.976 |
| sol_dark | plain | none | off | 7.5027 | 0 | 0.74869 | 1 | 1 | 35.256 |
| sol_dark | plain | none | early | 7.4617 | 1.1638e-11 | 0.74868 | 0.99982 | 0.99989 | 35.256 |
| sol_dark | plain | none | late | 7.4557 | 1.128e-11 | 0.74868 | 0.99991 | 0.99991 | 35.256 |
| sol_dark | plain | none | both | 7.4159 | 2.2674e-11 | 0.74866 | 0.99973 | 0.9998 | 35.256 |
| sol_dark | plain | oracle | off | 0.91186 | 0 | 0.79796 | 1 | 1 | 41.205 |
| sol_dark | plain | oracle | early | 0.90817 | 0 | 0.79794 | 0.99966 | 0.99979 | 41.205 |
| sol_dark | plain | oracle | late | 0.90732 | 0 | 0.79793 | 0.99985 | 0.99979 | 41.205 |
| sol_dark | plain | oracle | both | 0.90386 | 0 | 0.79791 | 0.99951 | 0.99959 | 41.205 |
| sol_dark | crgs 0.30 | none | off | 8.7228 | 5.8679e-05 | 0.76192 | 1 | 1 | 34.675 |
| sol_dark | crgs 0.30 | none | early | 8.6751 | 5.8528e-05 | 0.7619 | 0.99982 | 0.99989 | 34.675 |
| sol_dark | crgs 0.30 | none | late | 8.6693 | 5.8517e-05 | 0.7619 | 0.99994 | 0.99991 | 34.675 |
| sol_dark | crgs 0.30 | none | both | 8.6227 | 5.8369e-05 | 0.76188 | 0.99975 | 0.9998 | 34.675 |
| sol_dark | crgs 0.30 | oracle | off | 0.98227 | 6.377e-05 | 0.81401 | 1 | 1 | 40.839 |
| sol_dark | crgs 0.30 | oracle | early | 0.97799 | 6.3583e-05 | 0.81398 | 0.99966 | 0.99979 | 40.84 |
| sol_dark | crgs 0.30 | oracle | late | 0.97687 | 6.3579e-05 | 0.81398 | 0.99989 | 0.99979 | 40.84 |
| sol_dark | crgs 0.30 | oracle | both | 0.97281 | 6.3397e-05 | 0.81395 | 0.99955 | 0.99959 | 40.84 |
| sol_dark | pe | none | off | 5.5139 | 0.00015146 | 0.77723 | 1 | 1 | 35.59 |
| sol_dark | pe | none | early | 5.4872 | 0.0001513 | 0.77722 | 0.99984 | 0.9999 | 35.591 |
| sol_dark | pe | none | late | 5.4814 | 0.00015124 | 0.77721 | 0.99992 | 0.99992 | 35.591 |
| sol_dark | pe | none | both | 5.4555 | 0.00015108 | 0.7772 | 0.99976 | 0.99982 | 35.591 |
| sol_dark | pe | oracle | off | 0.7906 | 0.00012631 | 0.82223 | 1 | 1 | 40.947 |
| sol_dark | pe | oracle | early | 0.78792 | 0.00012611 | 0.82221 | 0.99969 | 0.9998 | 40.948 |
| sol_dark | pe | oracle | late | 0.78753 | 0.00012603 | 0.8222 | 0.99984 | 0.99981 | 40.948 |
| sol_dark | pe | oracle | both | 0.78502 | 0.00012583 | 0.82218 | 0.99954 | 0.99961 | 40.948 |
| nocturne_dark | plain | none | off | 41.443 | 0 | 0.37627 | 1 | 1 | 37.913 |
| nocturne_dark | plain | none | early | 41.409 | 9.3064e-13 | 0.37626 | 0.99984 | 0.99993 | 37.914 |
| nocturne_dark | plain | none | late | 41.397 | 7.0652e-12 | 0.37626 | 0.99988 | 0.99993 | 37.914 |
| nocturne_dark | plain | none | both | 41.365 | 7.7762e-12 | 0.37626 | 0.99973 | 0.99987 | 37.914 |
| nocturne_dark | plain | oracle | off | 3.213 | 0 | 0.65007 | 1 | 1 | 42.894 |
| nocturne_dark | plain | oracle | early | 3.2176 | 0 | 0.64997 | 0.99347 | 0.99947 | 42.894 |
| nocturne_dark | plain | oracle | late | 3.22 | 0 | 0.65 | 0.99531 | 0.99948 | 42.894 |
| nocturne_dark | plain | oracle | both | 3.2251 | 0 | 0.6499 | 0.98889 | 0.99896 | 42.893 |
| nocturne_dark | crgs 0.30 | none | off | 44.465 | 4.5957e-05 | 0.38254 | 1 | 1 | 37.6 |
| nocturne_dark | crgs 0.30 | none | early | 44.422 | 4.5701e-05 | 0.38253 | 0.99983 | 0.99993 | 37.6 |
| nocturne_dark | crgs 0.30 | none | late | 44.415 | 4.5626e-05 | 0.38253 | 0.99987 | 0.99992 | 37.6 |
| nocturne_dark | crgs 0.30 | none | both | 44.373 | 4.5376e-05 | 0.38253 | 0.99971 | 0.99986 | 37.601 |
| nocturne_dark | crgs 0.30 | oracle | off | 3.6266 | 4.0317e-05 | 0.66513 | 1 | 1 | 42.645 |
| nocturne_dark | crgs 0.30 | oracle | early | 3.6299 | 3.9693e-05 | 0.66503 | 0.99322 | 0.99946 | 42.644 |
| nocturne_dark | crgs 0.30 | oracle | late | 3.6304 | 3.983e-05 | 0.66505 | 0.9955 | 0.99945 | 42.644 |
| nocturne_dark | crgs 0.30 | oracle | both | 3.6343 | 3.9235e-05 | 0.66496 | 0.98882 | 0.99891 | 42.643 |
| nocturne_dark | pe | none | off | 41.622 | 0.00040156 | 0.40783 | 1 | 1 | 37.483 |
| nocturne_dark | pe | none | early | 41.604 | 0.00040123 | 0.40783 | 0.99986 | 0.99994 | 37.483 |
| nocturne_dark | pe | none | late | 41.596 | 0.00040105 | 0.40783 | 0.99993 | 0.99995 | 37.483 |
| nocturne_dark | pe | none | both | 41.578 | 0.00040074 | 0.40783 | 0.9998 | 0.99989 | 37.483 |
| nocturne_dark | pe | oracle | off | 3.4704 | 6.8279e-05 | 0.6729 | 1 | 1 | 41.915 |
| nocturne_dark | pe | oracle | early | 3.4732 | 6.7634e-05 | 0.67281 | 0.9944 | 0.99951 | 41.914 |
| nocturne_dark | pe | oracle | late | 3.4768 | 6.7684e-05 | 0.67283 | 0.99592 | 0.99948 | 41.914 |
| nocturne_dark | pe | oracle | both | 3.4802 | 6.7071e-05 | 0.67274 | 0.99042 | 0.999 | 41.914 |
| nocturne_mid | plain | none | off | 56.292 | 0 | 0.42734 | 1 | 1 | 37.84 |
| nocturne_mid | plain | none | early | 56.282 | 0 | 0.42733 | 0.99994 | 0.99998 | 37.84 |
| nocturne_mid | plain | none | late | 56.274 | 1.1193e-12 | 0.42733 | 0.99995 | 0.99999 | 37.84 |
| nocturne_mid | plain | none | both | 56.264 | 0 | 0.42733 | 0.99989 | 0.99997 | 37.84 |
| nocturne_mid | plain | oracle | off | 4.54 | 0 | 0.65733 | 1 | 1 | 42.393 |
| nocturne_mid | plain | oracle | early | 4.5398 | 0 | 0.65726 | 0.99206 | 0.99981 | 42.391 |
| nocturne_mid | plain | oracle | late | 4.5376 | 0 | 0.65729 | 0.99601 | 0.99982 | 42.394 |
| nocturne_mid | plain | oracle | both | 4.5381 | 0 | 0.65721 | 0.98818 | 0.99963 | 42.391 |
| nocturne_mid | crgs 0.30 | none | off | 62.47 | 6.8074e-05 | 0.43293 | 1 | 1 | 37.515 |
| nocturne_mid | crgs 0.30 | none | early | 62.458 | 6.7721e-05 | 0.43293 | 0.99993 | 0.99998 | 37.516 |
| nocturne_mid | crgs 0.30 | none | late | 62.443 | 6.7541e-05 | 0.43293 | 0.9999 | 0.99998 | 37.516 |
| nocturne_mid | crgs 0.30 | none | both | 62.432 | 6.7201e-05 | 0.43293 | 0.99984 | 0.99996 | 37.516 |
| nocturne_mid | crgs 0.30 | oracle | off | 5.3356 | 3.3522e-05 | 0.66728 | 1 | 1 | 42.086 |
| nocturne_mid | crgs 0.30 | oracle | early | 5.3344 | 3.2882e-05 | 0.66721 | 0.99178 | 0.9998 | 42.084 |
| nocturne_mid | crgs 0.30 | oracle | late | 5.3321 | 3.2966e-05 | 0.66723 | 0.99588 | 0.99981 | 42.087 |
| nocturne_mid | crgs 0.30 | oracle | both | 5.3315 | 3.2366e-05 | 0.66715 | 0.98778 | 0.99961 | 42.084 |
| nocturne_mid | pe | none | off | 49.474 | 0.00039437 | 0.4578 | 1 | 1 | 37.569 |
| nocturne_mid | pe | none | early | 49.465 | 0.00039399 | 0.4578 | 0.99995 | 0.99999 | 37.569 |
| nocturne_mid | pe | none | late | 49.459 | 0.00039384 | 0.4578 | 0.99998 | 1 | 37.569 |
| nocturne_mid | pe | none | both | 49.45 | 0.00039348 | 0.4578 | 0.99993 | 0.99998 | 37.569 |
| nocturne_mid | pe | oracle | off | 4.1702 | 0.00014951 | 0.68105 | 1 | 1 | 41.673 |
| nocturne_mid | pe | oracle | early | 4.1701 | 0.00014867 | 0.68098 | 0.99268 | 0.9998 | 41.671 |
| nocturne_mid | pe | oracle | late | 4.173 | 0.00014911 | 0.68101 | 0.99675 | 0.99983 | 41.674 |
| nocturne_mid | pe | oracle | both | 4.1735 | 0.00014828 | 0.68094 | 0.98952 | 0.99964 | 41.672 |
