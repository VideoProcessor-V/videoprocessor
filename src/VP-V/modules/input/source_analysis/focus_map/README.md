# input.source_analysis / focus_map (P17, job #1 L-FOCUS)

The per-region defocus level and the focus-change flag: one skeleton product (`kFocusMap`, token `focus_map`, unit
`input/source_analysis/focus_map`, id 30), a cheap screen pass that runs before temporal detail. Owner rule (2026-11-12 22:16 / 22:18):
built ONLY from shared units; nothing is measured here.

## Inputs (declared edges; every one read through `Acquire`, which publishes a missing one)
| edge | product | used for | when absent |
|---|---|---|---|
| lag 0 and lag 1 | P11 region block statistics | band energies E1 / E2 / E3, grain level N, coverage; lag 1 = the focus-change reference | no P11: the fill declines (visible); no lag 1: `kNoPrevious` |
| lag 0 | P10 grain split | settles that the band energies are real | `kNoSplit` on every block (fine = E1) |
| lag 0 | P2 vector pair | backward vector per 8 px cell, mean per block | P7 below |
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

## Files
`FocusMap.h` CPU reference (the tolerance oracle) | `FocusMapGlsl.h` the GPU mirror | `FocusMapFill.h` the owner fill (CPU form) |
`tests/FocusMapTests.cpp` (ramp, rack focus, motion, Acquire single-build) | `tests/FocusMapWarpTests.cpp` (CPU vs GPU on the WARP device).
