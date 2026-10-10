# Stage 6 — gentle de-judder: design note for review

2026-10-10. Implementer → planner. Written before any Stage 6 code, because
the brief assumed things the renderer does not do. File and line references
are to `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp` (LVR)
at `stage7-rc2`.

## 1. What the renderer does today

- **One render per captured frame.**
  - `RenderLoop` (LVR ~18799) dequeues a frame whenever the queue holds at
    least its target depth (`CanDequeueLocked`), so it runs at the capture
    rate.
  - It renders once with `pl_render_image` and presents once.
  - On the extra refreshes DXGI simply shows that buffer again; nothing
    re-renders.
  - With the FL35 held at 60 Hz (`switch_refresh_rate: false`), film's 3:2
    cadence (alternately 2 and 3 refreshes per frame) therefore comes from
    *when each frame arrives* relative to vsync.
- **There is no per-refresh phase anywhere.** The inputs for one exist:
  - capture timestamps and QPC per frame;
  - `GetFrameStatistics` (`SyncQPCTime`, `PresentRefreshCount`);
  - the measured display Hz.
  Nothing combines them into a phase.
- **`cadenceRepeat` is not 3:2 pulldown.** It is the clock-drift correction,
  which drops a frame or presents one twice. It is applied per source
  frame.
- **Only the current frame lives on the GPU.**
  - There are two persistent upload textures (Y and CbCr, P210/P010) and no
    previous or next frame on the GPU.
  - The next frames sit in the queue as raw capture bytes, and as converted
    P210/P010 payloads once converted.
- **libplacebo is a prebuilt DLL** (`3rdparty/libplacebo/bin`).
  - Its mpv user shaders (the sigma-delta, NLS, every `[shader.standard]`
    file) can bind only their own textures. There is no way to hand them a
    texture from the app without rebuilding libplacebo.
  - Native C++ hooks can bind anything; `LedDrive::MeasurementHook` is the
    precedent.
- **No frame mixing is used.** `pl_render_image_mix` blends frames, which
  is ruled out ("never a blend").

## 2. What de-judder needs, and the proposed shape

De-judder needs, for each refresh a source frame A covers, a picture at
`t_render = t_A + s·δ`:
- δ is the refresh's position after A, in source frames (0 … <1);
- `s` is the strength.

So it needs:
- per-refresh presents;
- the refresh phase;
- frame B (and the previous frame, for the backward check) on the GPU.

### 2a. Per-refresh presents inside the existing loop (recommended)

1. After A's normal present, the loop renders and presents one extra image
   for each further vsync before B's first vsync:
   - B is already queued (a 5-frame queue at 24p), and its due time is the
     next capture arrival;
   - the extra images are A warped by `s·δ_k`.
2. The phase comes from:
   - `δ_k = φ0 + k·P_display/P_source`;
   - `φ0` from A's capture QPC against the predicted vsync, both available
     today.
   - The constant pipeline delay only shifts everything uniformly, so it
     does not matter for judder.
3. Drift correction stays as it is. It acts per source frame; the extra
   presents only fill refreshes A already owns. They never delay B: if B's
   due time is near, the last extra image is skipped.
4. **Off means today's path, untouched.** With de-judder off, or in
   `Low_Latency`, or on a non-film cadence (source rate ≥ display rate /
   1.5), no extra presents happen and nothing is uploaded or estimated.
5. Risk: this is the presentation path.
   - Mitigations: a separate code block behind one flag; per-present
     logging; a stats-overlay line ("De-judder: s=0.25, 3/2 extra presents,
     x.x ms").
   - The existing presentation-debt counters will show any disturbance.

The alternative, 2b, is a separate vsync-driven presenter thread. That is a
much larger rewrite of the pacing, so it is not recommended.

### 2c. Frames on the GPU

- When a frame becomes current, upload the next queue entry's converted
  P210/P010 payload into a second texture set, and keep the previous set.
  That makes a ring of three: prev, A, B.
- Cost at 4K P210: one extra upload per source frame, about 0.8 GB/s at
  24p. A later optimisation could reuse B's upload when B becomes current.

### 2d. Motion and warp (native compute, `pl_shader_custom`)

1. **Bidirectional block matching** at half resolution on source luma
   (Y plane):
   - A→B forward and A→prev backward, 8×8-px blocks;
   - hierarchical, as in Stage 4.
2. **Forward/backward consistency:** a block whose A→B vector is not
   matched by B→A (within 1 px) is an occlusion.
3. **Warp A's Y and CbCr planes by `s·δ·v`.**
   - The output is a new source `pl_frame` with the same representation,
     handed to the normal `pl_render_image`. Scaling, colour, NLS, LED,
     shaders and dither all apply unchanged.
   - Precedent: `ComposeSubtitleBeforeCrop` builds a new source frame
     before the main render.
4. **Fallback is a hard hold:**
   - occluded or untrusted blocks take A's pixels unwarped;
   - a cut between A and B (Stage 7's cut test, or the matcher's global
     error) means no warp for the whole frame.
   - Never a blend. `s` is global per frame.
5. Budget about 3 ms per extra present at 2560×1600 output from a 4K
   source: matching once per source frame, warping per present.

## 3. Occlusion mask → sigma-delta (delta 2): needs a decision

Delta 2 asks for a generic app-provided texture input to the hook chain,
with the sigma-delta consuming an `occlusion_mask`. With a prebuilt
libplacebo, an mpv user shader cannot receive an app texture. Options:

- **M1 — mask in the source frame's alpha.**
  - The de-judder's warped source frame carries the mask as an alpha plane.
  - The sigma-delta, at OUTPUT, reads `HOOKED.a` as the mask and writes
    alpha 1.
  - Needs proof that libplacebo leaves RGB untouched with a source alpha
    plane (no premultiply or background blend) at our settings; that can be
    tested on the GPU harness first.
  - Generic in the sense that any OUTPUT shader can read it, but it is a
    convention, not an API.
- **M2 — host the sigma-delta natively.**
  - VP runs the sigma-delta's passes from C++ (a small shader host), so it
    can bind app textures.
  - This is the real generic facility, but large. It also changes how the
    sigma-delta file is loaded.
- **M3 — accept the bounded error.**
  - The de-judder hard-holds occluded blocks, so the output there is A
    itself.
  - The sigma-delta's own gate already limits any wrong carry to ≤0.5 code
    for one refresh (delta 2's own reasoning).
  - This defers the mask until the facility exists.

Recommendation: try M1 on the harness. If libplacebo leaves RGB untouched,
build it; if not, take M3 and record it. M2 only if the owner wants the
generic API itself.

## 4. Settings (owner: optional, gentle default)

- `[vprenderer.*]` rendering profile key `dejudder_strength`, 0–1:
  - 0 or absent means off, today's path;
  - the Config app's default when switched on is 0.25 ("Gentle").
- Config app: a De-judder box with a switch plus Gentle 0.25 / Medium 0.35 /
  custom strength. Cinema profile only, never in `Low_Latency`.
- Off by default in the test config. The tally gets items for judder on
  pans, artefacts at edges, and render time.

## 5. Proposed build order (each step committed, reviewed, audited)

1. **6a** — matcher + warp + consistency as a standalone native component,
   with WARP GPU tests:
   - a moving texture warped by `s·δ` lands where expected;
   - occlusion holds;
   - a cut holds.
   Not wired into rendering, so no behaviour change.
2. **6b** — the three-frame GPU ring, extra presents and phase, behind
   `dejudder_strength`; the stats line; the Config box.
3. **6c** — the occlusion mask to the sigma-delta (M1 or M3, per §3).

Questions for the planner:
1. Is 2a acceptable for the presentation path?
2. M1, M2 or M3?
3. Does a half-resolution 8×8 matcher on source luma suit the budget?
4. Should B's double upload be optimised now or later?
