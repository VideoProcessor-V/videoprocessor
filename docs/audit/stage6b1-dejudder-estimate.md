# Audit — Stage 6b-1: de-judder settings, next frame, motion estimate

Diff audited: 5fbd75dd..6696ed17. The other Claude session was idle throughout.
Build: OK — full x64 Release /t:Rebuild at 6696ed17, 0 errors, no new warnings.
Tests: native 2473 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known);
DejudderPolicyRunsOnlyForFilmCadenceWithANextFrame and DejudderSettingsAreInputProfileNumbersWithLimits
pass. VideoProcessorConfigTests 97 passed, 2 failed (the two known RPC tests).

Spec used: VP-stage6-dejudder-design.md, VP-stage6-dejudder-answers.md (Q1, Q3, Q4),
brief Stage 6 "6b-1 status".

## Blockers
- None. Nothing on screen changes.

## Should fix
- LibplaceboVideoRenderer.cpp `UpdateDejudderEstimate` — **the log can fill at
  frame rate in two situations.**
  - "uploading the next frame failed" and "motion estimate failed" are
    logged on every source frame while the failure lasts (24 lines a
    second at 24p).
  - A reason that flips back and forth (e.g. "active" ↔ "next frame not
    ready" when the queue runs low, or after a seek) logs one line per flip.
  Rate-limit both, like the "1st and every 2400th" estimate line.

## Notes
- (1) The off path is the same apart from two tiny per-frame CPU steps that
  run before the strength check:
  - a presentation-telemetry snapshot (a short walk of recent records);
  - RenderLoop copying a pointer to B under the queue lock it already holds.
  No upload, allocation, dispatch, wait or log. Moving the strength check
  first in `UpdateDejudderEstimate` would make "off" strictly today's path.
- (2) B's match check compares byte count, range, colour depth and bit shift,
  but not B's own video state (e.g. an SDR→HDR switch at the same size). A
  mismatched B would only produce an untrusted estimate today; 6b-2 should
  compare the source format too before warping.
- (3) "Film cadence" is source < display/1.5. That includes 25p on 50 Hz and
  29.97p on 59.97 Hz (2:2 cadence, no 3:2 judder): the de-judder will act on
  30p video too. This follows the planner's rule; the owner may want to know.
- (3) Interlaced inputs: the rule uses the input mode's rate. If a 1080i50
  mode reports 25, 50i video would count as film cadence. Not checked here
  (the fork's display-mode semantics); worth a look in 6b-2.
- No test exercises the renderer path (B upload and Estimate inside
  RenderLocked); only the policy and settings are unit-tested, plus the 6a
  component tests. The first real check is the vp.log line on the projector
  PC. A GPU test in 6b-2 (with the warp) should cover it.

## Verified OK
- (1) Off (strength absent or 0): `Decide` returns Off first, the function
  returns before any upload or estimate, and the matcher and B's textures are
  freed if they exist. The initial reason is Off, so nothing is logged. The
  render and present paths are unchanged (the only new call happens after
  A's upload succeeded, and returns at once).
- (2) Next frame:
  - read in RenderLoop right after `pop_front`, under the same `m_queueMutex`
    lock;
  - taken only when it is the same generation, not a cadence repeat, a
    different source sequence, and converted.
  The payload is a `shared_ptr<const …>` published once by the converter
  under the same lock. RenderLocked holds its own reference, so it cannot
  change or be freed meanwhile. Only the queue front is read: queue depth,
  look-ahead, drift correction and the 200 ms target are untouched.
- (3) `DejudderPolicy::Decide` order:
  1. off;
  2. Low_Latency;
  3. rates unknown;
  4. not film cadence (source ≥ display/1.5);
  5. RGB;
  6. plane caps;
  7. no next frame;
  8. next frame differs.
  Unit-tested, including 40 vs 39.9 Hz on 60 and 25 on 50.
  - Source rate: the input display mode's rate. Display rate: the measured
    refresh, else the monitor refresh used for the hook templates.
  - A zero or unknown rate gives "rates not known", never active. On the
    first frame the fallback is still 0, so the de-judder waits one frame.
- (4) Estimate:
  - B is uploaded with exactly A's plane parameters: 16-bit UNORM Y; CbCr
    interleaved at (width+1)/2 with `p010RowBytes` row stride; chroma height
    = height for 4:2:2 (P210), (height+1)/2 for 4:2:0 (P010);
    `chromaShiftY` follows `lossless422`.
  - It runs once per source sequence, never on cadence repeats.
  - The matcher and B's textures are freed for off, Low_Latency, non-film
    and RGB. At teardown (`RetireOutput`) they are freed before
    `pl_d3d11_destroy` and `pl_log_destroy`; that is the only place the
    device is destroyed.
  - A changed match tolerance is applied to the existing matcher before the
    next Estimate.
  - The native-RGB path only reports "RGB capture" (and frees anything left
    over).
- (5) Settings: `dejudder_strength` 0–1 and `dejudder_match_tolerance` 0.5–16
  are validated in RendererProfileConfig (input profile). A bad value in the
  renderer's parse keeps the previous value and logs it. Both are in the
  fingerprint, in the transport copy and in the live-change list; tested.
- (6) Logging: one line per reason change, and the estimate line on the 1st
  and every 2400th estimate (≈100 s at 24p). Nothing per-frame in the normal
  case (see Should fix for the failure and flip cases).
- (7) Agreed: the Config box and the stats line belong in 6b-2. With nothing
  visible changing, the vp.log reason and estimate lines are enough to check
  6b-1 on the projector PC (set `dejudder_strength` by hand).
- Low_Latency is untouched (it simply reports "low-latency queue"). No hook,
  shader or LED path changed.

## Implementer responses

- **Should fix — log at frame rate: fixed.**
  - Upload and estimate failures share a counter. They are logged on the
    1st failure and every 2400th, with the count.
  - Reason changes log at most one line per 5 s. When the 5 s pass, the
    reason then current is logged with the number of changes not shown, so
    the log never ends on a stale reason.
- **Note (1) — off: done.** With strength 0, an Off reason and nothing
  allocated or pending, `UpdateDejudderEstimate` returns before the
  telemetry snapshot. Off is now strictly today's path, apart from
  RenderLoop copying a pointer under the lock it already holds.
- **Note (2) — B's video state: done in 6b-1.** RenderLoop takes B only when
  `SameDejudderVideoState(A, B)`: the same frame encoding, EOTF, colour
  space, orientation and display mode. HDR metadata may differ.
- **Note (3) — 25p/50 and 30p/60: kept as the planner's rule** (source <
  display/1.5). Raised with the owner; 2:2 has no uneven cadence, but
  sample-and-hold judder is still there.
- **Note (3) — interlaced: done.** A new reason `Interlaced` (checked after
  Low_Latency) keeps the de-judder off for interlaced input modes, which
  frees the matcher. Unit-tested. Revisit in 6b-2 only if wanted.
- **Note — no renderer-path test: agreed.** 6b-2 adds a GPU test with the
  warp.

## Re-audit (7b08b573, diff 6696ed17..HEAD)

Build: OK — full x64 Release /t:Rebuild at 7b08b573, 0 errors, no new warnings.
Tests: native 2473 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known);
the de-judder policy test (now with Interlaced) passes. VideoProcessorConfigTests 97 passed,
2 failed (the two known RPC tests). The other Claude session was idle throughout.

Earlier items:
- Should fix, log at frame rate — **resolved.**
  - Upload and estimate failures share one counter and are logged on the
    1st and every 2400th.
  - Reason changes are limited to one line per 5 s.
- Note (1), off not strictly today's path — **resolved.** With strength 0,
  reason Off, nothing allocated and nothing waiting to be logged, the call
  returns before the telemetry snapshot. Only RenderLoop's pointer copy under
  its existing lock remains, which is negligible.
- Note (2), B's video state not compared — **resolved.** RenderLoop takes B only
  when `SameDejudderVideoState(A, B)` holds.
- Note (3), 25p/50 and 30p/60 count as film — **disputed-and-I-agree.** It is the
  planner's rule, and the implementer has raised it with the owner.
- Note (3), interlaced inputs — **resolved** with the new Interlaced reason.
- Note, no renderer-path test — **disputed-and-I-agree.** Deferred to 6b-2's
  GPU test with the warp.

Owner checks:
- (1) **OK, with one small gap.**
  - A change is counted, and the line is written on the first call after 5 s
    have passed since the last one (the very first change is logged at
    once). It always names the reason current at that moment, with the
    number of changes skipped, so a flip-flop can never leave an old reason
    as the last line.
  - Strength going to 0 while a change is waiting: the early return is
    blocked while anything is waiting, so the next calls log "off" once the
    5 s pass, and only then does the early return take over.
  - Gap: the line is only written from a rendered frame. If rendering stops
    within 5 s of a change (signal lost, playback stopped, VP closed), the
    last line stays stale until frames resume. Harmless for diagnosis;
    flushing it at teardown would close it.
- (2) **OK.** The early return needs strength ≤ 0, reason Off, nothing waiting
  to be logged, no matcher and no B texture. Reason Off can only come from
  strength 0, so it cannot hide a live de-judder.
- (3) **OK.**
  - It compares the frame encoding, EOTF, colour space, orientation and the
    full display mode (size, rate, interlace), plus — in the existing
    check — byte count, range, colour depth and bit shift.
  - Ignoring HDR metadata (e.g. MaxCLL) is right for 6b. B is used only to
    find motion, and A is rendered with its own state. A metadata change
    does not change what the luma codes mean for matching.
- (4) **OK.** `Interlaced` is checked right after Low_Latency (before the rate
  checks), it is unit-tested, it is in the list of reasons that free the
  matcher and B's textures, and both call sites pass the input mode's
  interlace flag.

## Implementer response to the re-audit

- (1) gap — **fixed.** At teardown, after the matcher is released, a reason
  change still waiting to be logged is written as "De-judder: <reason> at
  teardown (N changes not logged)". Signal loss without a teardown still
  waits for the next rendered frame. The next frame's line names the
  reason current then, so nothing is lost.
- Stage 6b-1 closed.
