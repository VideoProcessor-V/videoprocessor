# input.source_analysis / focus_map (P17, job #1 L-FOCUS)

The per-region defocus level and the focus-change flag: one skeleton product (`kFocusMap`, token `focus_map`, unit
`input/source_analysis/focus_map`, id 30 here = lead 8 allocation 32 after kBandMemo / kEdgeMap), a cheap screen pass that runs before temporal detail. Owner rule (2026-11-12 22:16 / 22:18):
built ONLY from shared units; nothing is measured here.

## Inputs (declared edges; every one read through `Acquire`, which publishes a missing one)
| edge | product | used for | when absent |
|---|---|---|---|
| lag 0 and lag 1 | P11 region block statistics | band energies E1 / E2 / E3, grain level N, coverage; lag 1 = the focus-change reference | no P11: the fill declines (visible); no lag 1: `kNoPrevious` |
| lag 0 | P10 grain split | settles that the band energies are real | `kNoSplit` on every block (fine = E1) |
| lag 0 | P2 vector pair | backward vector per cell (the cell edge is derived from the payload size), mean per block | P7 below |
| lag 0 | P7 camera fit | translation stand-in | zero motion + `kNoMotion` (visible), never absent |
The band memo (iface-L-B3, `grain.band`) is not an edge yet: P11 already carries the P10 band energies, and the memo has no id in this tree
(rule-33 flag to lead 7; the edge is added when the id lands).

## Law
`fine = max(E1 - N, 0)`, `ratio = fine / (fine + E2 + E3 + eps)`, `defocus = 1 - Ramp(ratio, 0.35, 0.75)`, `confidence = Ramp(total, 2, 30)`.
`delta = defocus - defocus(source block in the previous frame, found by the motion)`; `kChanged` when `|delta| >= 0.25` and both
confidences >= 0.3; `kSharper` when `delta < 0`. Packed 8 B per P11 block: u16 defocus, s16 delta (s1.15), u16 confidence, u16 flags.

## HARD TD RULE (owner 11-12 22:16)
Never import detail sharper than the current frame's focus state into a region whose focus changed. The temporal-detail consumer row
(`ProductCatalog.h` consumer 29) is DECLARED only; there is no TD wiring in this module.

## STATUS: INERT UNTIL OWNERS (register 102b FOCUS-INERT-UNTIL-OWNERS)
The CPU owner fill declines (visibly: no map, `product.focus_map` fallback) until P11, P10, P2 and P7 have CPU owner fills or a GPU-form
path; trigger: L-B5 P11 v2 + L-B6 P7 CPU forms. The P11 CPU bytes are read as doubles (the reference `Evidence::data` layout) or floats
(a read-back), by byte count. The P2 / P7 payload layouts in `FocusMap.h` (`kP2BytesPerCell`, `kP7Bytes`) are local definitions that move to
their owners when they publish. P11 at seq - 1 must be retained: the P11 owner must declare keepFrames >= 1 (rebase onto r2/job1-core
ff54bfb2a is pending). Only the CPU form is registered; a GPU form (the GLSL mirror) is the variant a later screen-pass consumer asks for.

## Settings
No setting is added: the thresholds are `FocusMap::Params` constants of the law (no user-visible knob, nothing in Config).

## Design goal and provenance
Owner 2026-11-12 22:16 / 22:18: know per region how defocused the source is and when the focus changes (rack focus), so temporal detail
never imports sharper detail into a region whose focus changed. Interim sharpness measure = the shared P10 band energies carried by P11;
by owner ruling (lead 8, 2026-11-14) the sharpness input becomes `kEdgeMap` (id 31) as a registered FIX with a before/after table when
L-B5 lands it. Never a private gradient.

## Files
`FocusMap.h` CPU reference (the tolerance oracle) | `FocusMapGlsl.h` the GPU mirror | `FocusMapFill.h` the owner fill (CPU form) |
`tests/FocusMapTests.cpp` (ramp, rack focus, motion, Acquire single-build) | `tests/FocusMapWarpTests.cpp` (CPU vs GPU on the WARP device).
