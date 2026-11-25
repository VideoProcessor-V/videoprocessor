# input.source_analysis / focus_map (P17, job #1 L-FOCUS)

The per-region defocus level and the focus-change flag: one skeleton product (`kFocusMap`, token `focus_map`, unit
`input/source_analysis/focus_map`, fixed id 32 of the fixed id table, owner R14-95: 30 band_memo, 31 edge_map, 32 focus_map,
33 room_light, 34 composed_vectors reserved, 35 grain.level, 36 p7f_camera_fit_field, End 37), a cheap screen pass that runs before
temporal detail. Owner rule (2026-11-12 22:16 / 22:18): built ONLY from shared units; nothing is measured here.

## Inputs (S18 F1: edge-map-only v2 law at lag >= 1; NEVER lag 0)
| edge | product | used for | when absent |
|---|---|---|---|
| lag 1 (`seq - 1`) and lag 2 (`seq - 2`) | kEdgeMap (31, regions unit, CPU form) | REQUIRED sharpness input (edge law, unit v2): per-block sums of Sobel energy (`EdgeSums`). Read at `lag >= 1` ONLY (never lag 0) so async GPU -> CPU readback never blocks | `seq == 0`: visible decline; `ReadbackPending`: propagated as non-latched `AcquireCause::ReadbackPending` (retries on next `Acquire`); other unavailable cause or wrong size: visible decline on `product.focus_map` (NO v1 interim fallback). Lag 2 absent: `kNoPrevious` (released: + `kNoPreviousRetained`) |
| lag 1 (`seq - 1`) and lag 2 (`seq - 2`) | P11 region block statistics | frame-aligned band energies E1 / E2 / E3, grain level N, coverage; lag 2 = focus-change reference | no P11 at lag 1: fill declines visibly; no lag 2: `kNoPrevious`; lag 2 RELEASED: `kNoPrevious` + `kNoPreviousRetained`, counted, `product.focus_map` Fallbacks line |
| (none) | P10 grain split | not an edge (audit SHOULD 4 / X8): its N and bands reach the map inside P11, and N = -1 there means no split | `kNoSplit` on that block (fine = E1) |
| lag 1 (`seq - 1`) | P2 vector pair | backward vector per cell (the cell edge is derived from the payload size), mean per block | P7 below |
| lag 1 (`seq - 1`) | P7 camera fit | translation stand-in | zero motion + `kNoMotion` (visible), never absent |

## Law (unit v2 — edge-map-only, S18 F1)
The v1 P11-band interim law (`DefocusBlock`, `kSharpnessInterim`, `Params::ratioLo/ratioHi`) and its v1 interim GLSL mirror
(`FocusMapGlsl.h`) were deleted in S18 F1. `FocusMap` runs the edge law exclusively:
- Per P11 block `S2 = sum |G|^2`, `S4 = sum |G|^4` over the block's pixels of `kEdgeMap` (CPU form, `width * height * sizeof(float)`):
  `peak = S4 / S2` (the energy-weighted `|G|^2`, about `0.7 x` the edge-crest `|G|^2` independent of block coverage);
  `sharp = max(peak - 48 N, 0)` (`48 N` = white-noise grain floor of `peak`);
  `fine = max(E1 - N, 0)`, `total = fine + E2 + E3`;
  `ratio = sharp / (48 total + eps)`; `defocus = 1 - Ramp(ratio, 0.05, 0.6)`.
  A Gaussian-blurred step edge of sigma `b` gives `ratio ~ 0.6 / b^2`. The `0.05 / 0.6` ramp is PROVISIONAL (102b FOCUS-EDGE-LAW-CALIBRATE).
- `confidence = Ramp(total, 2, 30)`, coverage `>= 0.5`.
- `delta = defocus(lag 1) - defocus(source block at lag 2, found by motion)`; `kChanged` when `|delta| >= 0.25` and both confidences `>= 0.3`;
  `kSharper` when `delta < 0`. Packed 8 B per P11 block: u16 defocus, s16 delta (s1.15), u16 confidence, u16 flags.
- `ComputeGrid` with `!edge.Served()` leaves all output blocks zeroed/invalid (`kValid` unset) and never runs an interim law.

## HARD TD RULE (owner 11-12 22:16)
Never import detail sharper than the current frame's focus state into a region whose focus changed. The temporal-detail consumer row
(`ProductCatalog.h` consumer 31, repair 24) is DECLARED only (`UnwiredConsumers`); there is no TD wiring in this module.

## Retention
`kEdgeMap` CPU owner fill (`EdgeMapFill.h`, S18 R3) declares `keepFrames = 2` so both lag-1 (`seq - 1`) and lag-2 (`seq - 2`) remain
available after `OnSeal(seq)`. A released lag-2 read sets `kNoPreviousRetained` on every `kNoPrevious` block, counts it
(`Stats::noPreviousRetained`), and engages `product.focus_map` via `FocusMap::StatusDecision`.

## Settings
No setting is added: the thresholds are `FocusMap::Params` constants of the law (no user-visible knob, nothing in Config).

## Files
`FocusMap.h` CPU reference (edge-map-only v2 law) | `FocusMapFill.h` CPU owner fill (`lag >= 1`, non-latched `ReadbackPending`) |
`FocusMapGlsl.h` stub (`kHasGpuMirror = false`; v1 interim mirror deleted in S18 F1).
