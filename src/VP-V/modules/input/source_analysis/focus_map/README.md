# input.source_analysis / focus_map (P17, job #1 L-FOCUS)

The per-region defocus level and the focus-change flag: one skeleton product (`kFocusMap`, token `focus_map`, unit
`input/source_analysis/focus_map`, fixed id 32 of the fixed id table, owner R14-95: 30 band_memo, 31 edge_map, 32 focus_map,
33 room_light, 34 composed_vectors reserved, 35 grain.level, 36 p7f_camera_fit_field, End 37), a cheap screen pass that runs before
temporal detail. Owner rule (2026-11-12 22:16 / 22:18): built ONLY from shared units; nothing is measured here.

## Inputs (declared edges; every one read through `Acquire`, which publishes a missing one)
| edge | product | used for | when absent |
|---|---|---|---|
| lag 0 and lag 1 | kEdgeMap (31, regions unit) | the SHARPNESS input (edge law, unit v2): per-block sums of the Sobel energy; lag 1 = the change reference under the same law | not served on the CPU (C3: GPU-only owner fill) or another size: the interim law for both frames, `kSharpnessInterim` on every block, Fallbacks line "sharpness: P11 interim, edge map pending"; lag 1 absent under the edge law: `kNoPrevious` (released: + `kNoPreviousRetained`) |
| lag 0 and lag 1 | P11 region block statistics | band energies E1 / E2 / E3, grain level N, coverage; lag 1 = the focus-change reference | no P11: the fill declines (visible); no lag 1: `kNoPrevious`; lag 1 RELEASED (retention missing): `kNoPrevious` + `kNoPreviousRetained`, counted, `product.focus_map` Fallbacks line |
| (none) | P10 grain split | not an edge (audit SHOULD 4; a declared read the code never makes is a false declaration, check_private_copies X8): its N and bands reach the map inside P11, and N = -1 there means no split | `kNoSplit` on that block (fine = E1) |
| lag 0 | P2 vector pair | backward vector per cell (the cell edge is derived from the payload size), mean per block | P7 below |
| lag 0 | P7 camera fit | translation stand-in | zero motion + `kNoMotion` (visible), never absent |

## Law (unit v2)
One law per frame pair; the delta never compares two laws.
- EDGE LAW (the edge map is served on the CPU for the frame): per P11 block `S2 = sum |G|^2`, `S4 = sum |G|^4` over the block's pixels
  of kEdgeMap; `peak = S4 / S2` (the energy-weighted |G|^2, about 0.7 x the edge-crest |G|^2 whatever share of the block the edge
  covers, so it does not depend on the block size); `sharp = max(peak - 48 N, 0)` (48 N = the white-noise grain floor of `peak`);
  `ratio = sharp / (48 total + eps)`; `defocus = 1 - Ramp(ratio, 0.05, 0.6)`. A Gaussian-blurred step edge of sigma b gives
  ratio ~ 0.6 / b^2. The 0.05 / 0.6 ramp is PROVISIONAL (that model, not yet fitted on real clips: 102b FOCUS-EDGE-LAW-CALIBRATE).
- INTERIM LAW (v1, the edge map not served): `fine = max(E1 - N, 0)`, `ratio = fine / (fine + E2 + E3 + eps)`,
  `defocus = 1 - Ramp(ratio, 0.35, 0.75)`.
- Both: `total = fine + E2 + E3`, `confidence = Ramp(total, 2, 30)`, coverage >= 0.5.
`delta = defocus - defocus(source block in the previous frame, found by the motion)`; `kChanged` when `|delta| >= 0.25` and both
confidences >= 0.3; `kSharper` when `delta < 0`. Packed 8 B per P11 block: u16 defocus, s16 delta (s1.15), u16 confidence, u16 flags.

## Registered fix FOCUS-EDGEMAP-SWITCH (owner-decisions 11-14 05:25; 102b FOCUS-EDGEMAP-SWITCH)
| | before (r2/job1-focus e4b184625, unit v1) | after (this lane, unit v2) |
|---|---|---|
| sharpness input | P11 band energies E1 / E2 / E3 (fine-band share of the band energy) | kEdgeMap through `Acquire(kEdgeMap, seq, Cpu)` (and seq - 1): the peak Sobel energy over the P11 band energy |
| declared edges | P11 (0, 1), P10, P2, P7 | kEdgeMap (0, 1), P11 (0, 1), P2, P7 (P10 dropped: never read, SHOULD 4 / X8); catalog row 30 reads kEdgeMap (the focus map is its first reader) |
| edge map not served on the CPU | n/a | the v1 law, bit for bit, plus `kSharpnessInterim` on every block and the Fallbacks line |
| confidence, change flag, motion, packing | as v1 | unchanged |

## HARD TD RULE (owner 11-12 22:16)
Never import detail sharper than the current frame's focus state into a region whose focus changed. The temporal-detail consumer row
(`ProductCatalog.h` consumer 31, repair 24) is DECLARED only; there is no TD wiring in this module.

## STATUS: INERT UNTIL OWNERS (register 102b FOCUS-INERT-UNTIL-OWNERS)
The CPU owner fill declines (visibly: no map, `product.focus_map` fallback) until P11, P2 and P7 have CPU owner fills or a GPU-form
path; trigger: L-B5 P11 v2 + L-B6 P7 CPU forms. The P11 CPU bytes are read as doubles (the reference `Evidence::data` layout) or floats
(a read-back), by byte count. The P2 / P7 payload layouts in `FocusMap.h` (`kP2BytesPerCell`, `kP7Bytes`) are local definitions that move to
their owners when they publish. Only the CPU form is registered; a GPU form (the GLSL mirror) is the variant a later screen-pass consumer
asks for. The declaration is `Backend::GpuCompute` and `refRenderMs = 0.05` is an unmeasured estimate.

P11 at seq - 1 must be RETAINED: the P11 owner fill must register `keepFrames >= 1` (102b FOCUS-P11-LAG1-RETENTION). Until then a
released lag-1 read is told apart from an absent one: the fill sets `kNoPreviousRetained` on every block, counts it
(`Stats::noPreviousRetained`) and engages the `product.focus_map` Fallbacks line (`FocusMap::StatusDecision`, shown by the skeleton module),
so an un-retained P11 is visible and never reads as "no focus change". It is never re-filled (the memo refuses a released frame: no second
P11 computation).

The skeleton module REPLACES the fill on every bundle rebuild (the memo outlives a bundle) and shows a refused registration on the same
Fallbacks line.

## Settings
No setting is added: the thresholds are `FocusMap::Params` constants of the law (no user-visible knob, nothing in Config).

## Design goal and provenance
Owner 2026-11-12 22:16 / 22:18: know per region how defocused the source is and when the focus changes (rack focus), so temporal detail
never imports sharper detail into a region whose focus changed. By owner ruling (owner-decisions 11-14 05:25, "focus map built ON the
edge map"; 102b FOCUS-EDGEMAP-SWITCH) the sharpness input IS `kEdgeMap` (id 31), read through Acquire.

## Files
`FocusMap.h` CPU reference (the tolerance oracle) | `FocusMapGlsl.h` the GPU mirror | `FocusMapFill.h` the owner fill (CPU form).
