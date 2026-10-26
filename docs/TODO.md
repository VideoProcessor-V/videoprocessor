# VP-V to-do (backup of the current plan)

Updated 2026-10-26. The detailed plans are in `docs/plans/2026-10-26/`; the master schedule is
`docs/plans/2026-10-26/schedule-2026-10-31.md`. This file is the short list; keep it current when the
plan changes.

## Where things stand
- Integration branch: `integ/batch1-wip` (d85519e2) holds every package merged so far (groups A-E,
  frame analysis store, M2 module moves, hdr_inference either/or, Config usability, test categories,
  tests moved out of Bill's test file). `integ/batch1` stays at the last full-suite pass (43ebff50).
- Baseline 2026-10-26 on d85519e2: CPU 4283/4284 (one stale source pin, fixed on
  `fix/n1-entrance-pin`), Config suite passed, GPU hardware 19/19, WARP graphics tests on the GPU 219/219.
- Drafted, waiting for audit + merge: `fix/n1-entrance-pin`, `feat/m2-item11-rest`,
  `feat/test-filters-wiring`.

## Standing rules
- Audit before every verification build and every merge; cheaper-model drafts get an Opus audit.
- Bill's code: every behaviour change must be a PR-worthy bug fix (registered), or an either/or with the
  Original pathway = Bill's behaviour, or it reverts. Host = call sites only; Bill's files never moved.
- Every slot switchable off with byte-identical output; no silent fallbacks; grain faithful; BTB/WTW kept;
  no hard-coded rates; full precision by default.
- 3090 measurements confirm and tune defaults; no package waits on them.
- Big testing (full suites, GPU) waits for the complete merge; baselines may run any time.

## Next (order from the schedule)
1. Audit + merge the three drafted branches.
2. WP-9 chain (B3 closures: 9.0, 9a, 9b, 9c, 9.6 pre-B3 deletion); record the frozen r6 / b3base sets.
3. WP-10 net wiring (A, then D) with a generic NetHost and cost gate; net runner V2 (aliasing, fused
   leftovers, fp16 only as a labelled manual switch).
4. Stage 8b upgrade: one-axis upscales (separable on the upscaled axis, CNN off), motion-compensated
   detail stabilisation; B-refine net on top of 8b (net off = exactly 8b); 8K rule uses source size.
5. F-11 DV formatter: keep the generic fixes in place (register, PR candidates), extract VP-only code.
6. N3 Bill-divergence programme: Original-pathway entries for the kept defaults, Config never rewrites the
   file on open, restore Bill's Config pages (VP pages separate), extract VP feature code from Bill's
   renderer / Config window / dialog, undo the motion profile rename, report-a-problem in global actions.
7. WP-16 intent glue; WP-18 settings readers + control.config (golden fixtures: madVR-derived owner
   settings in Bill's context, then VP-V defaults); step 0 commit B (Bill include patch, delete forwarders).
8. N8 chroma: measure first; VP chroma upscale option (separable / guided) once at slot 120;
   colour repair (net A colour branch) in TrainKit round 3.
9. Tier sliders: Apply button (preview without writing).
10. N6 intent audits + N7 efficiency sweeps of every module; final Bill-code gate.
11. WP-20 big test (CPU + Config suites, GPU legs, slot on/off matrix, GPU tests on WARP and hardware).
12. After WP-20: Bill merge (WP-21), M1 deploy prep.

## TrainKit round 3 requests
A colour branch; B-refine on Stage 8b output (per-axis variants); half-resolution-trunk D and D_small
(only if quality holds); exporter fields; fp32 by default; D fine-tune after the timing numbers.

## Open checks
- Timing hazard (in-between frames +-1/16-bit): debug-layer probe once Graphics Tools is installed;
  `entrance_gpu_drain` costing on an idle machine and the 3090.
- Dolby-style tone mapping vs LLDV by eye.
- DeckLink 12-bit: no precision misreport (low priority).
