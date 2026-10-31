<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Adaptive ringing repair: owner use cases (see ringing_usecases.py)

ring_ratio = ringing energy with / without the repair in the viewed space; ring_rms = without (10-bit codes, 4 = one 8-bit step); keep_edge = near-edge grain gate of the repair (+-5 %).

| usecase | case | chain | k | ring_ratio | ring_rms | dpsnr | keep_edge |
|---|---|---|---|---|---|---|---|
| (4) text 1080p | credits x264 crf28 | native | 0 | 0.9411 | 2.834 | 0.006947 | 0.9502 |
| (1)(2) x2 upscale + sharpen | credits x264 crf28 | up2 | 0 | 0.9167 | 2.07 | 0.007543 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf28 | up2 | 0.5 | 0.916 | 2.601 | 0.006573 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf28 | up2 | 1 | 0.9162 | 3.129 | 0.005916 | - |
| (4) text 1080p | credits x264 crf35 | native | 0 | 0.9307 | 3.216 | 0.002479 | 0.9517 |
| (1)(2) x2 upscale + sharpen | credits x264 crf35 | up2 | 0 | 0.9516 | 2.79 | 0.002788 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf35 | up2 | 0.5 | 0.9501 | 3.438 | 0.002334 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf35 | up2 | 1 | 0.9501 | 4.099 | 0.00203 | - |
| (4) text 1080p | credits x264 crf40 | native | 0 | 0.9594 | 5.341 | 0.001574 | 0.955 |
| (1)(2) x2 upscale + sharpen | credits x264 crf40 | up2 | 0 | 0.9768 | 4.426 | 0.001733 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf40 | up2 | 0.5 | 0.976 | 5.369 | 0.001507 | - |
| (1)(2) x2 upscale + sharpen | credits x264 crf40 | up2 | 1 | 0.9755 | 6.32 | 0.001345 | - |
| (1) film/text 1080p | tos4k_text x264 crf35 | native | 0 | 0.935 | 4.988 | 0.006325 | 0.9763 |
| (1)(2) x2 upscale + sharpen | tos4k_text x264 crf35 | up2 | 0 | 0.8955 | 2.518 | 0.00652 | - |
| (1)(2) x2 upscale + sharpen | tos4k_text x264 crf35 | up2 | 0.5 | 0.9058 | 2.955 | 0.006659 | - |
| (1)(2) x2 upscale + sharpen | tos4k_text x264 crf35 | up2 | 1 | 0.9154 | 3.48 | 0.006734 | - |
| (5) 720p rung x2 | credits x264 crf38 | up2 | 0 | 0.9318 | 5.074 | 0.00364 | 0.9329 |
| (5) 720p rung x2 | credits x264 crf38 | up2 | 1 | 0.93 | 7.593 | 0.003204 | 0.9329 |
| (5) 720p rung x2 | tos4k_text x264 crf38 | up2 | 0 | 0.9523 | 4.063 | 0.003257 | 0.9785 |
| (5) 720p rung x2 | tos4k_text x264 crf38 | up2 | 1 | 0.958 | 5.409 | 0.003299 | 0.9785 |
| (3) chroma | sintel_sky x265-8_crf30 Cb | native | 0 | - | - | 0 | - |
| (3) chroma | sintel_sky x265-8_crf30 Cr | native | 0 | - | - | 0 | - |
| (3) chroma | sol_sky x265-10_crf30 Cb | native | 0 | - | - | 0 | - |
| (3) chroma | sol_sky x265-10_crf30 Cr | native | 0 | 0.3389 | 1.252 | 0.03159 | 0.9376 |
| (3) chroma | sol_dark aom-10_crf40 Cb | native | 0 | - | - | -0.0006055 | - |
| (3) chroma | sol_dark aom-10_crf40 Cr | native | 0 | - | - | 0 | - |
