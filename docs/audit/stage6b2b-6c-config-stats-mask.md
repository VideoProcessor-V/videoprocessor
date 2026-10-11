# Audit — Stage 6b-2b + 6c: de-judder Config box, stats line, occlusion mask

Diff audited: 063342f2..e4429613 (742607ed, aefd4654, e4429613), source files only.
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at e4429613, 0 errors, no warnings in changed files.
Tests: native 2480 total, 2478 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). All new tests pass:
OcclusionMaskReachesTheSigmaDeltaThroughAlpha, OcclusionMaskAlignsWithThePictureAfterScalingAndBlanking,
DejudderTelemetrySeesTheMainPresentNotTheExtras, the policy and settings tests, the API version test (25).
VideoProcessorConfigTests 98 passed, 2 failed (the two known RPC tests); "de-judder box presets
and mask" passes.

Spec used: VP-dither-judder-plan.md "6b-2b + 6c status", VP-stage6-dejudder-answers.md (Q2: M1′,
mask-only mode), stage6b2a-dejudder-extras.md (re-audit and responses).

## 6b-2a re-audit 2

- Blocker, B allowance counted twice — **resolved.** The allowance is now B's
  render time + 4 ms only (the after-call time is counted once, in the
  measured call latency).
- Should fix, stale "next frame late" prediction — **resolved.** The
  prediction is stored with its queue generation, read once and cleared at the
  start of the next extras block, and cleared when the extras block is
  skipped. Small remaining gap (Note): a frame dropped by drift correction in
  the same generation never reaches RenderLocked, so the prediction can be
  applied to the frame after it. At worst one false "next frame late" count.
- New unit test (DejudderTelemetrySeesTheMainPresentNotTheExtras) — **good.**
  It checks that extras are skipped when finding the main present, that the
  in-between extras are counted, and a present-id wrap (0xFFFFFFFF → 0).

## Blockers
- None.

## Should fix
- Stats line can say **"mask on" when the mask is not used.**
  `LibplaceboVideoRenderer.cpp` (mask block in RenderLocked): `dejudderMaskParamSet = install`
  is set *before* the "no sigma-delta takes it" check, so with no sigma-delta
  in the chain the flag is true and the stats line shows "mask on". Also,
  once any frame has used the mask, `dejudderMaskFrames > 0` keeps "mask on"
  for good, even when NLS later blocks it. Set the flag only when the
  parameter was really written, and base "mask on" on recent use (e.g. the
  last source frame), so the stats line can be trusted for tally item checks.

## Notes
- Mask-only mode: the stats line shows "cadence 0.00". The cadence ratio is
  only computed inside the extras code, which does not run in mask-only
  mode. Cosmetic.
- Mask-only mode costs a B upload and a motion estimate on every source
  frame, at any cadence (60p too). Expected by design; worth watching the
  render time on the projector PC at 60p.
- Status line is published only when a source frame is rendered (at most
  every 500 ms). If frames stop, the line keeps its last text until frames
  resume or the feature is switched off. Harmless.
- The new alignment test covers one output-texture layout (whichever
  libplacebo gives with blanking). The other layout is handled by the same
  formula and is safe by reasoning (see Verified OK 4), but not tested.
- Config box: picking "Not set (off)" clears the strength field (key removed
  on save); "Off" writes `dejudder_strength: 0`. Both mean off. Fine, just
  two ways to say it.

## Verified OK
- (1) **Off is today's path.** With strength 0 and occlusion_mask off,
  `UpdateDejudderEstimate` returns first (no telemetry, upload, estimate or
  log); the mask block in RenderLocked does not run (no hook, no parameter
  write); extras need `dejudderExtrasAllowed`. The stats line is cleared
  once when it goes off and stays empty. The sigma-delta's `occlusion_mask`
  keeps its default 0, so alpha is ignored and its output is unchanged.
- (2) **Mask-only.** `Decide` skips the cadence check when occlusion_mask is
  on and returns Active with strength 0 on any cadence (Low_Latency,
  interlaced, RGB etc. still switch it off). `ExtrasAllowed` needs strength
  > 0 and an uneven cadence, so it is false in mask-only mode, and the extras
  block (including `dejudderMainPresented`) is gated on it. The extras never
  run in mask-only mode. Unit-tested.
- (3) **Matcher B-side occlusion.** For each block of B: follows B→A, looks up
  A→B at the landing block (clamped), "seen" = the two vectors cancel within
  the consistency limit and B's match is good. Right side and right test.
  Slots: the estimate for (A, B) writes into the slot *not* holding A's mask
  (A's is used later in the same refresh), and tags it with B's sequence.
  The mask is consumed on use (tag set to 0), so never applied twice; tags
  are cleared in `ReleaseDejudder`. `nextSourceSequence` is taken from the
  same queue entry as B's planes, under the same lock, and reset per frame.
- (4) **OcclusionMask hook.** OUTPUT stage, SIG_TEX, compute, own signature.
  RGB copied with a nearest texel fetch (bit-exact; test 1 confirms the
  picture is unchanged). Pixel → block: offset from the destination
  rectangle, scaled to source pixels, divided by 16, clamped; pixels outside
  the picture get alpha 1. Works whether the texture is the destination
  rectangle (origin 0,0) or the full target (origin at the rectangle);
  the test with 1.5× scaling and 16 px blanking lands on x 40..63 as
  expected. A non-storable texture counts as a failure and returns safely.
  One binding per texture.
- (5) **Installation.** Inserted directly before the first hook that took
  the parameter; the encoding repair is then inserted at the front, so it
  runs first. Only on a source frame's first refresh with its mask ready;
  not on cadence repeats, not with NLS (logged once), not without a
  sigma-delta (logged once). Every other refresh writes 0; the extras drop
  the hook and write 0. **Alpha 0 cannot reach the screen:** the sigma-delta
  writes alpha 1 when it takes the mask, and the swapchain uses
  `DXGI_ALPHA_MODE_IGNORE` anyway.
- (6) **Sigma-delta.** New parameter, default 0. `occluded` (param on and
  alpha < 0.5) stops both the motion-compensated carry and the plain
  carry. Alpha out is 1 only when the parameter is 1, otherwise unchanged.
  No `//!` in the new comments. Tests: mask 1 keeps the carry, mask 0 stops
  it, parameter 0 ignores alpha, alpha written back as 1.
- (7) **Stats line.** `GetDejudderStatus` is on IVideoRenderer with a safe
  default, forwarded by the plugin wrapper; API version 25 and the version
  test updated on purpose. The render thread writes the text under its own
  small mutex at most twice a second (forced on a reason change); the UI
  reads under the same mutex. Cleared when the feature goes off.
- (8) **Config box.** Preset ↔ strength binding both ways (0 / 0.25 / 0.35,
  anything else = Custom); "Auto" on the default profile counts as not set;
  the preset box itself is not a saved field. occlusion_mask saved as on/off;
  validator accepts only on/off; strength 0–1, tolerance 0.5–16, cut
  threshold 0–1. Loading syncs the preset with signals blocked, so nothing
  is written for untouched fields. The renderer parses `occlusion_mask`
  (bad value logged, previous kept); it is in the fingerprint, transport
  copy and live-change list.
- (9) **Tests.** M1′ proof (picture unchanged, carry gating, alignment after
  scaling and blanking), B-side mask (uncovered background new, square and
  still background seen, invalid slot null), policy (mask-only, extras
  allowed) and settings (occlusion_mask on/off/invalid) — all pass.

## To check on the projector PC (vp.log and screen; tally items 32–38)
- vp.log reason line now reads "De-judder: active (strength …, extras on/off,
  occlusion mask on/off, …)". On 24p/60 with Gentle: extras on. With strength 0
  + mask on: active, extras off.
- vp.log "De-judder occlusion mask: used on N source frames (hook applied …,
  failed 0)" — failed must stay 0. With NLS on: the one "not used while NLS
  is active" line. Without the sigma-delta: the one "no sigma-delta" line.
- Ctrl+I: a "De-judder:" line. Off → no line. Gentle on 24p/60 → s=0.25,
  cadence 2.50, +1–2 extras, small late/full/held counts. Mask-only → "mask
  only …, mask on" (cadence shows 0.00 — known).
- Screen: mask on vs off on a slow pan with dark gradients — no change in
  picture or colours, no flashing blocks or edges, no dither "swimming"
  at uncovered edges; no black or transparent patches anywhere.
- Mask-only at 60p: render time in Ctrl+I still well inside the frame
  period, no new dropped frames.
- Config: set Gentle, Apply, reopen — preset shows Gentle; set mask on,
  Apply — `occlusion_mask: on` in the cinema profile, nothing else changed.
- Switch both off live: the Ctrl+I line disappears; vp.log shows "off".

## Implementer responses

- **Should fix — "mask on" when the mask is not used: fixed.**
  `dejudderMaskParamSet` is set only when the parameter was really written
  as 1 (after the "no sigma-delta" check). The stats line shows "mask on"
  only if the mask was used within the last second. Otherwise it shows the
  reason: "mask off (NLS)", "mask off (no sigma-delta)" or "mask waiting".
- **Note — "cadence 0.00" in mask-only mode: fixed.** The cadence ratio is
  computed in the estimate path whenever both rates are known.
- **Note — a drift-correction drop could cause one false "next frame
  late": fixed.** The prediction now also carries the source sequence it is
  for (B's, from the queue). It counts only when that frame is the one
  shown, so a dropped frame in between no longer counts.
- **Planner (6b-2b/6c answers, item 3) — mask-only limited to film rates:
  done.** Without extras, a source above 30 fps gets the new reason
  `VideoRateMaskOnly`. The matcher is released, and it is logged once. With
  extras (24p etc.) the mask rides along as before. Unit-tested (50, 59.94
  refused; 23.976 and 30 allowed).
- **Note — mask-only cost at 60p:** moot now (refused above 30 fps).
- **Note — status line frozen while no frames are rendered:** accepted.
- **Note — the second output-texture layout untested:** accepted; the same
  formula covers it (your Verified OK 4).
- **Note — "Not set" vs "Off" in the Config box:** accepted. Both mean off;
  "Not set" removes the key.
- **Planner item 2 — NLS mapping (6c-2):** next, after this audit, with a
  GPU test at the owner's NLS settings.
