<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Full gates (probe, LPIPS): td_real_low 72 frames, letterbox (active picture, mirror padding), no-harm

One row per candidate; means over cases of candidate - default (td_off). `w` = mean block weight, `act` = share of blocks with weight > 0.05 (scored run). Worst = the case value farthest from neutral. Gates = non-PASS flags among the grain/ghost/smear/flicker/halo/BTB-WTW/GOP rows (0 = all PASS).

| candidate | n | dPSNR B | dPSNR all | dSSIM B | dLPIPS | d gap | w | act | worst grain keep | worst lag-1 | worst band | worst probe edge | d ghost | d smear | gates non-PASS | errors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| td_b008_boost0 | 8 | +0.000 | +0.000 | +0.00002 | -0.0000 | -0.001 | 0.0020 | 0.013 | 1.0008 | -0.0000 | 1.0010 | 1.0003 | -0.0001 | +0.0003 | 0 | 0 |
| td_b008 | 8 | -0.004 | -0.006 | +0.00017 | -0.0005 | -0.012 | 0.0020 | 0.013 | 1.0203 | -0.0009 | 1.0211 | 1.0250 | -0.0030 | +0.0165 | 0 | 0 |
| td_b008 | 2 | -0.008 | -0.007 | -0.00000 | -0.0000 | +0.002 | 0.0001 | 0.001 | 1.0025 | 0.0000 | 1.0029 | 1.0117 | -0.0005 | +0.0064 | 0 | 0 |
| td_b008_boost0 | 2 | -0.000 | -0.000 | +0.00000 | -0.0000 | +0.000 | 0.0001 | 0.001 | 1.0000 | 0.0000 | 1.0000 | 1.0000 | -0.0000 | +0.0000 | 0 | 0 |
| td_b008_boost0 | 5 | NA | NA | NA | NA | NA | 0.0014 | 0.007 | 1.0007 | 0.0000 | 1.0008 | 1.0046 | -0.0001 | +0.0011 | 0 | 0 |
