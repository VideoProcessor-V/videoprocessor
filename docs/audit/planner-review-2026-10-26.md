# Planner review — 2026-10-26 pack (read-only)

Received from the planning agent via the owner, 2026-10-26. Read in full: 00-README, 01 update and
questions, 02-TODO, the schedule; plans 4–7 by targeted search only.

## Q1 — Order and risk
Ordering right in shape (WP-9 critical path, extraction second lane, Bill merge after the big test). Moves:
1. L0 (frozen r6 / b3base references) after the timing hazard is pinned, or recorded with
   entrance_gpu_drain on; every byte-identity test runs with the drain on (see Q5).
2. N9-P12 (8b either-axis gate, one-axis separable path) before WP-20.
3. Watch the shared-file chain (conflict 9) and the host .cpp writers (conflict 10): lane B touches only
   disjoint host regions; merge in the order the chain lists.
Residual risk: critical path ~120–130 h serial, every package audit-gated → budget one slack day before WP-20.

## Q2 — Bill merge timing
Keep extraction → big test → merge. Re-run the N3-P0 rehearsal merge weekly and record the hunk count.
Trigger: merge first if extraction slips past two weeks OR the rehearsal count rises past ~200 hunks.
Design WP-20 so post-merge qualification (WP-21/WP-20b) is a diff against the frozen references, not a
second full big test.

## Q3 — B-refine on Stage 8b
Right design. Build in: (1) motion engine version in the fingerprint (bypass on mismatch) + train with
grain-map perturbation (dropout/noise); (2) temporal consistency term in the loss (warped previous output,
trusted pixels) or MC stabilisation of B-refine's residual like 8b's; (3) one net with the two scale
factors as conditioning inputs rather than three exports, unless the kit shows a quality gap; (4) train the
default doubler + SuperRes setting first.

## Q4 — Colour repair
A chroma head on A's trunk (switchable, testable alone; head off = byte-identical chroma; own grain gate;
hue preserved). Labels: A's schema + subsampling (4:2:0/4:2:2), chroma siting (left/top-left/centre),
matrix (BT.709/2020), chroma bpp share. Loss in a perceptual chroma space with an explicit hue penalty.
Keep standalone A_chroma as fallback if round 2 drops A.

## Q5 — Timing hazard
Byte-identity tests, frozen references and the chain gate run with the drain ON. Viewing: cost the drain on
the 3090; if < ~0.3 ms/frame default ON in the cinema profile, off in Low_Latency; else ship off and keep
hunting. Debug-layer probe stays the root-cause task. The 10-16 audit "pixel-identity harness — two
identical runs differ" (part 10) is very likely the same fault; close both together.

## Q6 — Missing or contradicted items
1. DeckLink 12-bit: default route v210 (12→10 truncation); DV answers set 12-bit 4:2:2 for PQ as a capture
   criterion → keep the 12-bit route available and verified end to end; decide the default against the
   owner's sources.
2. DV next (10-17): live verification first, the format-neutral scene-descriptor layer, L1 as the LED
   look-ahead's plan are missing → add at least the descriptor interface to M2/intent work.
3. Stage 13 appearance adaptation: reserve its seat in colour.appearance in the M2 tail.
4. Owner measurements missing from owner actions: LED/CNTR step-response (Slo-Mo), no-tile Argyll patch,
   screen-white measurement, Stage 3 ramp test (LED policy's four timing constants + Stage 13 depend on them).
5. Stage 12 review items: bounded auto-disengage on confidence drop (wait for a cut, ≤5 s); structural
   settings-fingerprint test; crash-state redaction (user-name paths, COM transcript) — confirm each landed.
6. Detection stress tests: G1 periodic-tile exclusion, text guard on any band and either axis, G19 (Apple TV
   captures) — confirm status.
7. Polar retirement (N9-P5, optional): keep the "no ringing into blanked bars" test on the no-net path.
8. Fixed delay: E2-02 target_frames 5 — confirm Low_Latency exempt and a delay-gate test for every cinema mode.

## Q7 — Test strategy gaps
1. Determinism: run-twice test (same input → identical output) on WARP and hardware, per slot, drain on.
2. Real capture input: bench rig (DeckLink Mini Recorder 4K in the USB4 enclosure) feeding live HDMI; DV
   tunnel recordings as fixtures.
3. Transitions: mid-stream 24↔60, SDR↔HDR, DV↔LLDV, profile switches, device loss/reset.
4. Soak: multi-hour runs for VRAM growth, pool leaks (D's ~2.2 GB pool), drift, queue depth, nets on.
5. Bypass coverage: every net bypass reason → exact fallback + visible status line.
6. Original pathway = Bill: automated test that VP-V with every VP feature off produces Bill's output
   byte-identically (the final Bill-code gate as a test).
7. Settings coverage: structural fingerprint test (every key enters the fingerprint) + CPU defaults fingerprint.
8. Per-slot timing budget on both GPUs, fullscreen with DXGI frame statistics, as a gate rather than a log.
