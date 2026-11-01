| metric | NR vs FR | frames | pooled Spearman | within-clip Spearman |
|---|---|---|---|---|
| band | nr_band vs fr_band | 822 | -0.061 | 0.028 |
| block | nr_block vs fr_block | 870 | 0.460 | 0.290 |
| ring | nr_ring vs fr_ring | 822 | 0.441 | 0.032 |
| noise | nr_noise vs fr_noise_loss | 687 | 0.030 | -0.603 |
| detail | nr_detail vs fr_detail_kept | 822 | 0.048 | 0.304 |
| poor_index | poor_index vs fr_ssim_loss | 870 | -0.060 | -0.211 |

AUC = P(the lowest rung looks worse than the top rung of the same raster): 0.5 = no signal.

| metric | AUC lowest vs top rung | median top | median low | n low / top |
|---|---|---|---|---|
| band | 0.510 | 0.000 | 0.000 | 222 / 222 |
| block | 0.526 | 0.051 | 0.058 | 222 / 222 |
| ring | 0.491 | 1.280 | 1.244 | 222 / 222 |
| noise | 0.583 | 0.115 | 0.098 | 222 / 222 |
| detail | 0.520 | 0.603 | 0.593 | 222 / 222 |
| poor_index | 0.519 | NA | NA | 222 / 222 |

poor thresholds (absolute artefact level): {"band": 0.0, "block": 0.057923, "ring": 1.244217}
relative-drop thresholds (value / the stream's own baseline): {"detail": 0.9972, "noise": 0.9217}
