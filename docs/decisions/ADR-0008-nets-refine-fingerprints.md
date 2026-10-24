# ADR-0008: Nets: one refine net at the end, fingerprints, bypass on mismatch, consolidation

- Status: Accepted
- Date: 2026-10-24 (owner decision; recorded 2026-10-24)
- Slots: scaling.refine, repair.encoding, scaling.output, control.net_runtime

## Context

Round 1: per-axis C was harmful (kernel-matched re-base alone ~71.5 dB, +C ~55 dB, ringing 0.22->0.8); B and C were too slow; D gained ~10 dB but failed the grain gate; A collapsed its gate.

## Decision

ONE refine net after the 4K route, trained to the truth, inputs = picture + continuous per-axis scale (sx, sy) + A-ran flag; texture restore/deband as robustness augmentation. Per-axis C dropped. Nets are fingerprinted (label schema, pass order H->V, r16/fp16 intermediates, upstream versions/states, VP commit) and bypassed visibly on mismatch, per combination group if needed. Train upstream -> downstream. If small nets cost ~4 ms+ each on the 3090, consolidate (likely <=2 live nets). Never motion/interpolation nets; never on raw DV IPT.

## Consequences / open

Open: one refine told vs two versions (owner: one told if cost is identical). D: measure the real cost in VP first; harm checks (MTF, HF energy, grain, pan).
