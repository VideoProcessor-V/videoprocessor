# Upstream divergence register

Every place where VP-V behaves differently from Bill's (upstream) code. Each entry is either a
PR-worthy bug fix or an either/or option whose Original setting is Bill's behaviour.

Note (feat/resync-core, 2026-10-31): this file did not exist on this branch; if another branch
already holds a register, merge these rows into it.

| Id | Area | Divergence | Scope | Original / off | Kind |
|---|---|---|---|---|---|
| DR-L1 | libplacebo renderer cache (`LibplaceboVideoRenderer.cpp`, `Impl::ApplyResyncLibplaceboFlush`) | On the frame that carries a FULL resync (source change in place, renderer/queue reset, seek, calibration patterns, file discontinuity, format change, large timestamp break), `pl_renderer_flush_cache` drops libplacebo's frame-mix textures and resets its detected peak, and the temporal mixer's retained frames are cleared. No shader rebuild: the fork's body (vp-fl35, `src/renderer.c` 197-204, same in the bundled 7.377.0-vp-fl35 source; no fl35 patch touches it) destroys only `rr->frames` textures and calls `pl_reset_detected_peak`. Bill's `Reset()` keeps the cache (its comment concerns compiled programs, which this does not touch). | VP pathway only, `resync_event = on` | Original pathway, `resync_event` off/shadow: Bill's behaviour (no flush) | VP-only option (either/or) |
| DR-R3-scene | Scene detector (Bill's `SceneDetector`) | `resync_event = on`: the detector is also `Reset` (same generation) on the first frame of every FULL resync, including FULL causes Bill's `m_sceneDetectorGeneration` does not see (seek without a reset, calibration patterns, in-place format change, counter backwards). A continuity slip changes nothing (the comparison across the slip already reports a real change as a cut). | `resync_event = on` | off/shadow/Original: Bill's generation triggers only | Either/or option |
