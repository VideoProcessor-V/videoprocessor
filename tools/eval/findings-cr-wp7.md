<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# WP7: grain-safe regional contrast recovery, option A (pre-TM) vs B (post-TM) vs libplacebo CR 0.30

HDR -> SDR 100 nit, luma, Python CPU model (`cr_wp7_model.py`), 6 clips x 16 frames at 1280 wide plus 4 codec cases
(x265-10 CRF30, 1080p; VP deband model first, then the candidate). Tables: `prototypes/cr_wp7/cr_wp7.md`. Strength
s(b) = s_max x k_light(J_eff from the light-level LUT, 0.1 to 1 nit ramp) x band gate x (1 - halo guard), tile-smoothed
in time. The grain G rides the curve exactly as in the plain path; s = 0 is byte-identical to the plain curve.

| | restore (CR0.3 = 0.129) | grain all / edge | halo (CR 1.6e-4) | step vis / p99 | pump |
|---|---|---|---|---|---|
| libplacebo CR 0.30 | 0.129 | 1.082 / 1.077 | 1.6e-4 | 0.962 / 1.014 | 0.089 |
| grain-safe CR 0.30 (round 2) | 0.090 | 1.042 / 1.048 | 1.4e-4 | 0.986 / 1.005 | 0.083 |
| A 0.25 | 0.075 (0.58x) | 1.039 / 1.030 | 0.9e-4 | 1.001 / 1.013 | 0.077 |
| B 0.25 | 0.081 (0.63x) | 1.039 / 1.030 | 1.2e-4 | 1.001 / 1.013 | 0.095 |
| A 0.35 | 0.105 (0.81x) | 1.055 / 1.042 | 1.6e-4 | n/a | 0.108 |
| A 0.40 | 0.120 (0.93x) | 1.063 / 1.048 | 1.9e-4 | 1.001 / 1.021 | 0.124 |
| A 0.40, detail band above 1 px | 0.071 | 1.030 / 1.023 | 1.0e-4 | 1.010 / 1.017 | 0.097 |

Verdict (honest): **no setting passes every WP7 gate.** At s <= 0.25 A and B pass grain (+-5 % overall, edge >= 0.90),
boost, halo, step visibility (+0.1 %, p99 +1.3 %, below the 2 % gate) and pump, but restore only 0.58x / 0.63x of CR 0.3
(gate 0.9x). Reaching 0.9x needs s about 0.40, where grain is +6.3 % (fails) and halo exceeds CR 0.3. The shortfall is
largely libplacebo's own grain amplification (+8 %): it counts as "restored" contrast. Per unit of grain excess the
model restores about 30 % more than CR (A 0.40: 0.120 for +6.3 % grain; CR: 0.129 for +8.2 %), and the 1-px detail band
(grain band excluded) restores 0.071 for +3.0 % grain. A vs B: B restores 7 % more at the same s (within the plan's 10 %
rule, so A stays the recommendation), with 25 % more halo and higher pump; A needs no output-size grain map.
Findings: the adaptation low-pass must be about h/240 (3 px at 720p), not libplacebo's 1.75 px (restore doubles);
the 1.75 px version restored only 0.011 to 0.05. The light gate (nit ramp) is what costs restore on the low-key
Nocturne clip (0.081 to 0.032) by design (noise floor); the band gate and halo guard cost 3 to 15 %. Caveats: the grain
probe is synthetic and the worst clip (Chimera, no native grain) is a probe artefact (grain_split mistakes the probe
for structure); restore is the round-2 local-contrast-ratio metric; CPU ms are not GPU-representative.
