# Audit — Stage 5 step 1: time-based queue (target_latency_ms)

Diff audited: 706f86f0..2d3bf2d3 (c1b46205, d50a990e docs, 2d3bf2d3 config)
Build: OK — full x64 Release /t:Rebuild of VideoProcessor.sln at HEAD 2d3bf2d3, 0 errors, no new warnings (MagewellSdkRoot set to the installed MWCapture SDK 3.3.1.1596).
Tests: native suite (vstest) 2056 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known); VideoProcessorConfigTests 91 passed, 2 failed (the two known RPC tests). One run at HEAD, covering all three units.

Spec used: brief Stage 5 (including the 2026-10-10 status note), guide §9
"Design review 2 responses", README §4, owner check (d).

## Blockers
- None in this unit. Note the queue auto-switch blocker in
  stage7-step5d-config-app.md: a 59.94 Hz source never selects Low_Latency,
  so it would get Normal's 200 ms target instead of 1 frame.

## Should fix
- None.

## Notes
- The rate used is `${cadence}`, the capture card's nominal mode rate
  (StateVariables.cpp ~179), not a measured rate, so it does not jitter.
  Exact multiples (200 ms at 25p = 5.0, 30p, 50p) are handled with a small
  tolerance and stay stable.
- Until the source rate is known the queue uses `target_frames` (5 in the
  test config), which equals the 24p result.
- `std::stoi` on `target_latency_ms` in VideoProcessorApp.cpp is safe only
  because the schema has already validated the key (0–1000).
- `Low_Latency` section: only a comment was added here; it keeps
  `target_frames: 1`.
- Docs: design review 2 SF-1 (`compensation_eotf` 2.2 vs the 2.41 fit) is
  recorded as open pending the owner's direct measurement; the proposed check
  (code 126 vs 235 ratio 0.218 → 2.2, 0.189 → 2.4) is arithmetically right.

## Verified OK
- (d) Profiles without `target_latency_ms`: `ResolveTargetFrames` returns the
  profile's own `target_frames`, else the default queue's — exactly the
  previous expression (`hasTargetFrames ? targetFrames : base`).
- (d) Frame pacing untouched: the only change is which number is handed to
  the existing target-frames setter in `ApplyUnifiedProfileSnapshot`;
  AlphaQueuePolicy and the pacing code are not in the diff.
- (d) The count is recomputed only when a new profile snapshot is applied,
  which happens when the source format/rate (or a profile) changes — on the
  UI thread through the same path a profile switch already used, never inside
  a frame.
- (d) Low_Latency: its own `target_frames: 1` wins over an inherited time
  target (precedence in QueueLatency.h; the profile model also drops an
  inherited `target_latency_ms` from any variant with its own
  `target_frames`/`steady_reserve_frames`), so 1 frame at every rate.
- `target_frames = ceil(ms × rate)`, clamped 1–16; 0 or absent = off; range
  0–1000 in both the main schema and the profile grammar.
- OSD: "Queue delay: N fr = X ms (target T ms)" only when the target comes
  from time. Log line on each change.
- Config app Queue page: "Target delay" (Off/ms) writes `target_latency_ms`,
  with a note listing the real delay at 24/25/30/50/60p; a queue with its own
  Target frames shows Off, matching what VP does. Not keyboard-only.
- Test config: 200 ms on Normal (owner, N-5), `queue_size` 32 is enough.
- Tests in ConfigFileTests cover the conversion, the precedence, Low_Latency
  staying at 1 frame and the off cases.
