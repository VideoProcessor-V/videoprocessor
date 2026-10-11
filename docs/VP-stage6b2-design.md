# Stage 6b-2 — extra presents: design note for review

2026-10-11. Implementer → planner. 6b-1 is done and audited (`c4cd0d01`).
Settings, B on the GPU, one motion estimate per source frame, no visible
change. This note is about the step that changes the picture, written
before the code because it touches the presentation path. File references
are to `LibplaceboVideoRenderer.cpp` (LVR).

## 1. What I found in the render path

- `RenderLocked` is one ~4,700-line function. Between acquiring the
  backbuffer (LVR ~11850) and `pl_render_image` (via
  `NlsHookRecovery::Render`, ~15046), it does a lot of once-per-source-frame
  work:
  - active-picture and black-bar analysis;
  - scene detection;
  - subtitle evidence and composition;
  - the HDR peak decision;
  - the LED measurement and controller;
  - profile-boundary resets;
  - telemetry and pacing traces.
  Calling it again per refresh would re-run all of that. **So the extra
  presents cannot simply call `RenderLocked` again.**
- Everything an extra present needs is still alive at the end of
  `RenderLocked`, right after A's present:
  - the source frame (`renderImage`), the target (`target`), the final hook
    list and parameters (`frameParams`), the overlays and the crop.
  - Only three things differ per present: the backbuffer, the source
    planes and the sigma-delta's per-refresh carry. The carry is already
    per render.
- Present is `Present(1, 0)` on the VP-owned flip swapchain (3 buffers,
  maximum frame latency 2), or `pl_swapchain_submit_frame` on the
  libplacebo-owned one. Presents queue in order and each is shown for at
  least one refresh. So A plus N extras submitted back to back show on N+1
  consecutive refreshes, and CPU back-pressure comes from the swapchain
  itself.

## 2. Proposed mechanism

**One block at the end of `RenderLocked`, after A's present and A's
telemetry.** It runs only when the 6b-1 reason is Active and A rendered and
presented. For each extra refresh k = 1 … N:

1. Warp A by `s·δ_k` (`MotionWarp::Warp`, 6a).
2. Copy `renderImage` and replace its two plane textures with the warped
   planes.
3. Acquire the next backbuffer the same way A did, clear it, and copy
   `target` with only the FBO swapped.
4. Call `pl_render_image` with A's `frameParams`, minus the LED measurement
   hook (see 4.1). Use no NLS recovery: if the render fails, stop the extras
   and log once.
5. Present. Count it in its own counters: extra presents, holds, skipped
   extras, warp+render ms. Do not count it in A's presentation record, so
   drift correction and the pacing traces still see one present per source
   frame.

Off (any reason other than Active) means the block does not run.

### 2.1 How many extras, and the phase

The extras must exactly fill the refreshes A owns, so B is never queued
behind them.

- **Reference times.** Present calls jitter with OS scheduling, but the
  capture callback runs on the capture card's clock. Per source frame:
  - `T_A = callbackQpc_A + L`;
  - `L` is a smoothed average of (present call − callback);
  - `T_B = T_A + P_s`.
  The same `L` applies to both, so the queue delay (~200 ms) cancels.
- **Vsync grid** from the frame statistics VP already reads after each
  present: `SyncQPCTime` plus n·`P_d` (measured display Hz).
- **A's refreshes:** the grid vsyncs `V_1 … V_{N+1}` in `(T_A, T_B]`. That
  is 2 or 3 at 24p on 60 Hz.
  - `φ0 = (V_1 − T_A)/P_s`;
  - `δ_k = φ0 + k·P_d/P_s` for extra k.
  At 24p on 60 Hz the cycle is 0, .4, .8, .2, .6 up to the constant shift.
  1 ms of jitter moves δ by 0.024 frames, about 0.006 at s = 0.25, which
  is invisible.
- **Never delay B:**
  - The last extra is skipped if its vsync is within one refresh of `T_B`
    (your rule).
  - All extras stop at once if B (or anything newer) has already been
    queued.
- A's own present stays unwarped. Its ideal position would be `s·φ0`,
  which is ≤ 0.25·0.4 = 0.1 frames. Warping A's own present too is
  possible later, but it would put the warp on today's main path, so I
  propose not to.

### 2.2 Holds (no extras; DXGI shows A again, exactly as today)

- cadence-repeat frames (drift correction presenting a frame twice);
- an A→B cut (2.3);
- subtitle cut-and-paste composition active this frame. Its composed
  source is not A's planes. That needs its own rule and is out of scope
  for 6b, so it is reported as a reason;
- the frame statistics are unavailable or disjoint (no vsync grid);
- the reason is anything but Active.

### 2.3 Cut between A and B: a GPU-only frame trust

The per-block hold already turns a cut into ≈A, but the few spuriously
trusted blocks (≤2% in the 6a cut test) would still move.
- I propose a small pass in `MotionWarp` that counts trusted blocks.
- `Warp` holds the whole frame when fewer than a fixed share is trusted
  (proposed 25%).
- It stays on the GPU with no read-back, so there is no stall.
- A GPU test covers it: the cut test asserts that the output equals A
  exactly.

## 3. Points needing a decision

1. **The mechanism in §2** (extras at the end of `RenderLocked`, reusing
   A's frame, target and hooks). OK?
2. **LED measurement.** Extras leave out the measurement hook, so each
   source frame gets exactly one reading, from its first present. That
   pairs by source identity by construction. The compensation hook stays
   on every present.
3. **libplacebo peak detection** (HDR) smooths per *render*. Extras make it
   adapt 2–2.5× faster in wall time at 24p. I propose to accept this and
   note it. Freezing it per extra would need parameters I have not
   verified in 7.360.1.
4. **25p on 50 Hz and 30p on 60 Hz.** With the current rule (source <
   display/1.5) they get one extra at δ = 0.5. There is no 3:2 there, only
   sample-and-hold steps. Keep it, or limit to source < display/2.2 (24p on
   60 only)? Owner question too.
5. **The Config box and stats line** come in this step, as planned:
   - the box: switch, Gentle 0.25 / Medium 0.35 / custom; cinema profile
     only;
   - the stats line: "De-judder: s=0.25 active, 3/2 extras, x.x ms",
     shown with Ctrl+I like the others.
6. **Logging while new:** φ0 and δ_k per present, as you asked. Log the
   first 200 extras, then 1 in 240, plus every hold reason (rate-limited).
   Logging every present for hours would fill vp.log.

## 4. Build and tests

- **6b-2a:** the frame trust (§2.3), the extras block and the phase.
  - GPU tests: frame trust; the extras' render path on the harness (warp →
    `pl_render_image`), checking that the output moves by s·δ and that A's
    own present is unchanged.
  - Policy tests: the refresh count and phase.
- **6b-2b:** the Config box and the stats line.
- Each step is committed and audited. The auditor diffs the present
  sequence with the flag off, as you required.
