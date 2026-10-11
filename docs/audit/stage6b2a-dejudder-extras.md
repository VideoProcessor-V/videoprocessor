# Audit — Stage 6b-2a: de-judder extra presents

Diff audited: 2ce7f764..HEAD on stage7-rc2, source files only (be719dfc, bd414002;
HEAD 8a2c7e2d is docs). The implementer session was busy in its own clone. This
checkout stayed clean at 8a2c7e2d throughout (checked at the start and the end), and
no other changes appeared.
Build: OK — full x64 Release /t:Rebuild at 8a2c7e2d, 0 errors, no new warnings.
Tests: native 2475 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known); all 8 de-judder tests pass.
VideoProcessorConfigTests first run 96 passed, 3 failed: the two known RPC tests plus
"remote target switch keeps window responsive". On a re-run, 97 passed with only the two
known failures. The extra one is intermittent; no Config code changed in this diff.

Spec used: VP-stage6b2-design.md, VP-stage6b2-answers.md, VP-stage6b2a-answers.md,
brief "6b-2a status".

## Blockers
- None that block a first projector test, provided the vp.log checks below are made.

## Should fix
- LibplaceboVideoRenderer.cpp `PresentDejudderExtras` / RenderLoop — **B can
  be delayed by a refresh** (check (2)).
  - The extras loop only returns at the vblank of the last extra. Only then
    does RenderLoop start B's whole pipeline:
    - dequeue and look-ahead walk;
    - B's upload and the next frame's (C's) upload;
    - the motion estimate for B→C;
    - the analysis passes, B's render and its present.
  - "B could still make it" is predicted with A's callback-to-present time
    plus `renderMs` + 4 ms. But `renderMs` covers only the final render call
    (from `renderStart` before `pl_render_image`), not the work before it.
  - Without extras, B starts as soon as it is dequeued; with extras it starts
    at the last extra's vblank, so all of that work must fit in one refresh.
    With a 4K source (two 4K uploads plus a 2–5 ms estimate plus the render)
    it may not, and B then appears one refresh late — a judder step. The
    "render > 0.6 refresh" hold looks at `renderMs` only and does not catch
    it.
  - Fix: base the allowance and the hold on A's whole dequeue-to-present
    time. Also count "B presented later than predicted", so it shows in
    6b-2b's stats line.
  - Observable now: the existing "Alpha frame pacing trace … reason=submit_gap"
    lines in vp.log (see the projector checks below).
- `PresentDejudderExtras`, hook list — **an extra can be encoded twice.** The
  extras drop the LED measurement hook but keep `OutputEncodingRepair`, which
  was inserted for A's list. If the LED drive is on and `LED compensation.glsl`
  is not running (load failure — the "WARNING gamma boost NOT applied" case),
  the measurement hook was A's only PRE_OUTPUT texture hook. The extras then
  carry the repair without the bug it repairs, so they are encoded twice
  (limited range compressed again) while A is not. The result would be a
  brightness flicker on every extra. Rare, but recompute
  `OutputEncodingRepair::Needed` for the extras' list and drop the repair when
  it is no longer needed.
- (B) **A's presentation record and the pacing trace are not fully identical
  with extras on.** Telemetry samples the frame statistics at each of A's
  presents and marks A as presented using the *latest* displayed present,
  which is now an extra.
  - A's `presentedRefresh`/`presentedQpc`, the snapshot's `lastPresentId` and
    `lastPresentRefresh` take the extra's values, 1–2 refreshes later.
  - The pacing trace's "display refresh gap" check needs exactly one new
    present between A's samples (`statsPresentDelta == 1`), so it never
    fires while extras run.
  - Drift correction is not affected: it uses only A's records not yet on
    screen (debt) and whether `lastPresentId` changed, and both behave as
    before. The other fields are diagnostics.
  - Either subtract the extras from the present counts used there, or record
    this as an accepted deviation from the planner's requirement.

## Notes
- (2) Other ways an extra can take B's refresh:
  - an extra whose Present returns after its vsync shows one refresh later
    (counted as "late");
  - an extra whose Present returns in time but whose GPU work finishes after
    the vsync is not detected at all.
  Both are bounded by the "too late" pre-check (`renderAllowance`), which has
  the same `renderMs`-only weakness.
- (3) While waiting, B's arrival is only checked between vblank waits. If the
  previous picture needs more than one vblank to appear (up to 3), B's
  arrival goes unseen for that long. The capture-clock prediction still
  protects B's refresh, so this only matters together with the first
  should-fix.
- (4) The "De-judder phase" line is written on the first frame after a reset,
  when the latency floor has only that frame's own sample, so its φ0 always
  reads 0.000. The cycling φ0 (0 / 0.2) is visible in the per-extra lines
  (first 200), not in the phase line.
- (1) libplacebo-owned path: if both the extra's render and the fallback
  re-render of A fail, the acquired frame is submitted cleared, so one
  black refresh. Practically unreachable (A rendered moments earlier).
- (8) `dejudderOutputSwapchain` is compared by address only. A new swapchain
  at the same address would keep the old output, which only matters if the
  window moved to another monitor. The output itself is a counted reference,
  so nothing dangles.
- No GPU wait or present is exercised by the tests (the live loop needs a
  display). The harness tests cover the warp → `pl_render_image` path and the
  pure policy.

## Verified OK
- (A) Off (`dejudder_strength` 0): reason ≠ Active, so the extras block never
  runs.
  - `dejudderExtrasSpentTime` stays false, so RenderLoop's render cycle,
    queue ages, capture-to-submit and capture-to-target use `now` exactly
    as before.
  - No new present, wait, upload, dispatch or log. The only addition is
    building two small callback objects per frame (no allocation, no lock,
    no timing effect).
- (B) RenderLoop measurements: with extras the render cycle time, queue
  ages/backlog recovery, capture-to-submit and capture-to-target all use the
  instant captured right after A's present and telemetry (end of RenderLocked,
  before the extras). Nothing else in RenderLoop after RenderLocked reads the
  clock except coarse log/status ticks. A's AlphaPresentationRecord fields set
  at submission (ids, QPCs, render/swap ms, queue depth) are unchanged.
- (1) Extras reuse A's `renderImage` (planes swapped for the warp output),
  `target` (only the FBO swapped; overlays, crop, LUT kept) and `frameParams`
  (minus the LED measurement hook).
  - VP-owned path: the extra wraps backbuffer 0, checks it is renderable,
    clears the whole surface (keeps the blanking matte black), renders,
    flushes, releases the wrap and the backbuffer, then calls Present(1,0)
    once — the same order as A.
  - libplacebo path: every acquired frame is submitted (A re-rendered on a
    failed extra) and swapped.
- (2) The stops:
  - B has arrived: the queue is back at its dequeue depth, a new
    generation, or a stop. This is the same condition that makes RenderLoop
    dequeue B.
  - B could make the refresh: A's callback + P_s + smoothed
    callback-to-present + `renderMs` + 4 ms < target vsync.
  - Too late to render: `now` + allowance > target.
  - Finished after its refresh: counted as "late".
  - A cap of 3 extras.
  `PlanExtra` is unit-tested for 24p/60 phases, B near/arrived, past the
  next frame and the cap.
- (3) Wait and vsync: `WaitForVBlank` up to 3 times until
  `GetFrameStatistics().PresentCount ≥` the last present id. The vsync the
  picture appeared on is `SyncQPCTime − (SyncRefreshCount −
  PresentRefreshCount) × P_d`, with a signed 32-bit difference that survives
  wrap; this is right per DXGI.
  - Nothing B needs to arrive takes `renderMutex`. `OnVideoFrame` (enqueue)
    uses `m_queueMutex`/`m_stateMutex`; `ConversionLoop` uses
    `m_queueMutex`, `formatterMutex` and its timing lock. No holder of
    `m_stateMutex` waits on `renderMutex`, and the render thread holds
    neither the queue lock nor the state lock during RenderLocked.
  - Other blocking users of `renderMutex`:
    - the render thread itself (cadence bookkeeping, the look-ahead walk,
      backlog recovery, `IsGpuFailed`);
    - `Retire` and `FinalizeRetirementForShutdown` (shutdown; at most ~2
      refreshes longer).
    UI status getters use try-lock. Config and LED changes are posted and
    applied on the render thread. None of this is time-critical at that
    granularity.
  - Lock order: the new callback takes `m_queueMutex` inside `renderMutex`,
    as RenderLoop's existing backlog code already does (lines ~19811,
    ~19930). Nothing takes them the other way round; the look-ahead walk
    releases the queue lock before taking `renderMutex`.
- (4) Phase: L = minimum of (first vsync − callback) over 120 frames, and
  δ = (V − callback − L)/P_s, clamped at 0, stop at ≥ 1.
  - 24p on 60 Hz: A's first vsync alternates 0 and 0.2 P_s after its true
    time, so the extras get δ = 0.4, 0.8 or 0.6, which is the 0/.4/.8/.2/.6
    cycle.
  - 23.976 on 59.94 is the same. 23.976 on 60 drifts slowly and the 5 s
    floor follows it; drops and repeats are held.
  - Callback jitter biases L low by at most the jitter, ≈0.024 δ per ms:
    negligible.
- (5) Cadence: ratio = display/source; extras only if the ratio is ≥ 1.95
  and more than 0.05 from a whole number.
  - Included: 24/60 and 23.976/59.94 (2.5); 23.976/60 (2.5025); 24/59.94
    (2.4975); 25/60 (2.4).
  - Excluded: 50/60 (1.2), 30/60, 29.97/60, 25/50, 24/48 and 24/120.
  Unit-tested.
- (6) Frame trust: one 16×16 group (dispatch size 16×16 with group 16, so
  exactly 256 threads). Each thread strides over all blocks (`k += 256`), so
  any block count works. Tree reduction in `shared` memory with `barrier()`
  in uniform control flow; the share is written to a 1×1 rgba16f texture.
  - The warp's `field_vector` returns "hold" for every pixel when the share
    < `dejudder_frame_trust` (default 0.25), so the whole frame is held.
  - Bindings are one per texture per pass (`field` + `dst`; `src`, `field`,
    `frame_trust`, `dst`).
  - Tested: the cut output equals A exactly.
- (7) Logging:
  - holds: at most one line per 5 s, with counts;
  - extras: the first 200, then 1 in 240;
  - phase: one line per generation or cadence-ratio change;
  - latency floor: a line only when it moves more than one refresh from the
    last logged value.
  Nothing per frame in steady state.
- (8) Teardown: `dejudderOutput` is released and `dejudderOutputSwapchain`
  cleared in `RetireOutput`, with the matcher, before the device is
  destroyed.
- `dejudder_frame_trust` is validated (0–1), parsed, in the fingerprint and
  live-applied (tested).

## Not verifiable here — check in vp.log on the projector PC
Play a 24p pan with `dejudder_strength: 0.25`, then compare with 0:
1. "De-judder phase: cadence 2.500 …" appears once (its φ0 reads 0.000, see
   Notes); the first "De-judder extra:" lines show φ0 0.000/0.200 and δ
   0.4/0.8/0.6 cycling.
2. "Alpha frame pacing trace … reason=submit_gap" lines: they should not
   appear more often with the de-judder on than off. More of them means B
   is being delayed (first should-fix).
3. "De-judder: hold (…)" lines: few, and none saying "extra finished after
   its refresh" or "too late for the refresh" in steady playback.
4. The extra lines' `block_ms`: well under 8 ms. Long blocks mean the
   swapchain was full.
5. No "latency floor moved" lines during steady playback.
6. With a 4K source as well as 1080p (the first should-fix is
   load-dependent).
Then judge by eye.

## Implementer responses

- **Should fix 1 — B delayed by a refresh: fixed.**
  - The allowance and the hold now use A's whole processing time:
    RenderLoop's pre-render active time (look-ahead and conversion work,
    waits excluded) plus RenderLocked from entry to A's Present returning,
    not `renderMs`.
  - Extras are held when that time plus 4 ms would not fit in one refresh
    ("frame work too long for extras").
  - "B presented later than predicted" is counted (`nextFrameLate`). When
    the extras stop because B is due, the expected vsync is stored. On B's
    own first observed vsync, a refresh late is counted and logged as a
    hold reason; it is for the 6b-2b stats line.
- **Should fix 2 — an extra encoded twice: fixed.** After removing the LED
  measurement hook, `OutputEncodingRepair::Needed` is re-evaluated on the
  extras' own list (with the renderer's disabled hooks and the target LUT
  type). The repair is dropped when nothing in that list triggers the bug.
- **Should fix (B) — A's record and the pacing trace: fixed.** VP remembers
  the present ids of recent extras, and the refresh each main present (A)
  appeared on, as observed in the extras' first wait.
  - When the latest present on screen is an extra, the frame-statistics
    sample given to presentation telemetry and the pacing trace reports the
    main present before it, and that present's refresh. So
    `presentedRefresh`, `lastPresentId` and `lastPresentRefresh` read as
    they would without extras.
  - The pacing trace's present delta subtracts the extras in between, so
    the "display refresh gap" check works again.
  - With no extras the adjustment is an identity (empty list), so off stays
    today's path.
  - Ids are per swapchain: cleared when the swapchain changes and at
    teardown.
- **Note (4) — phase line φ0 0.000: fixed.** The phase line is written once
  the latency floor has 24 frames (about a second at 24p).
- **Note (2), GPU work after an in-time Present:** covered by the same
  whole-work allowance; not otherwise detectable without GPU queries.
  Accepted.
- **Note (3), B's arrival only checked between vblank waits:** accepted. The
  capture-clock prediction protects B's refresh; more than one wait only
  happens when a picture is late anyway.
- **Note (1), double render failure on the libplacebo path:** accepted
  (practically unreachable).
- **Note (8), output compared by address:** accepted. The output is a
  counted reference; a monitor move recreates the swapchain through
  `RetireOutput`, which clears it.
