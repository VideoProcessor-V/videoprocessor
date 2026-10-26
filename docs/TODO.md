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

## Planner review 2026-10-26 (docs/audit/planner-review-2026-10-26.md) — adopted
- Frozen r6/b3base references, byte-identity tests and the chain gate run with `entrance_gpu_drain` ON;
  drain default on in cinema profiles if the 3090 cost is < ~0.3 ms/frame (off in Low_Latency).
- Stage 8b one-axis package before WP-20; lane B only in disjoint host regions; one slack day before WP-20.
- Weekly rehearsal Bill merge (record hunks); merge first if extraction slips past two weeks or > ~200 hunks;
  post-merge qualification = diff against frozen references.
- B-refine: motion-engine version in the fingerprint, grain-map perturbation, temporal consistency term,
  one net conditioned on the per-axis factors. Colour repair = chroma head on A's trunk (siting/matrix labels,
  hue penalty).
- New tests: run-twice determinism (WARP + hardware, per slot), live capture via the DeckLink bench rig,
  mid-stream transitions, multi-hour soak, net bypass coverage, Original = Bill byte-identity test,
  structural settings fingerprint, per-slot timing budget gate (fullscreen, DXGI stats).
- Re-check / add: DeckLink 12-bit route verified end to end; DV scene-descriptor interface; Stage 13 seat in
  colour.appearance; Stage 12 items (bounded auto-disengage, structural fingerprint, crash-state redaction);
  detection stress items (G1, text guard, G19); delay gate per cinema mode with Low_Latency exempt.
- Owner measurements (when at the cinema PC; none blocks): LED/CNTR step response (Slo-Mo), no-tile Argyll
  patch, screen-white, Stage 3 ramp test.

## Near-term additions (owner 2026-10-26)
- HDR10+ (SMPTE ST 2094-40): full parse + per-scene curve and target-display selection on the DV trim
  framework (metadata hold, scene descriptor, Dolby-style curve, trim selection); synthetic fixtures from
  the published spec. Delivery: check a newer DeckLink SDK for dynamic HDR metadata; ready for file
  playback / other capture paths. Fallback when no metadata: VP dynamic tone mapping from the frame
  analysis store (either/or with libplacebo peak detection and static; Original = Bill).
- First deploy (M1) after the Bill merge (WP-21) and after the DeckLink Mini Recorder bench tests.
- Argyll patch: skip the white-tile check (keep dark calibration) for the i1Display Pro / ES-1000.
- Offer Bill the registered fixes that help him (standing owner OK; list shown when opened) — AFTER our Bill merge (WP-21).

## Planner design notes (2026-10-26)
- HDR10+ / VP dynamic tone mapping design, Argyll no-tile patch plan, test-suite tiers (per merge / nightly /
  WP-20): docs/plans/2026-10-26/planner-notes-pack3.md.

## Long-term design basis (owner 2026-10-26)
- Build toward an 8K basis (years away: 8K capture + GPUs able to process it). Keep every module
  resolution-general now: derived processing size, VRAM-aware pools, nets/shaders with tile + cost gates,
  motion at reduced resolution (same engine input as 4K), budgets scaling with pixels, synthetic 8K tests.
  The intent audits check for hidden 4K assumptions; the 8K bypass is a performance fallback only.

## TrainKit round 3 requests
A colour branch; B-refine on Stage 8b output (per-axis variants); half-resolution-trunk D and D_small
(only if quality holds); exporter fields; fp32 by default; D fine-tune after the timing numbers.

## Open checks
- Timing hazard (in-between frames +-1/16-bit): debug-layer probe once Graphics Tools is installed;
  `entrance_gpu_drain` costing on an idle machine and the 3090.
- Dolby-style tone mapping vs LLDV by eye.
- DeckLink 12-bit: no precision misreport (low priority).
