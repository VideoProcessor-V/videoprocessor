# Audit — stage8-rs232 fixes, Stage 8-3, Stage 9, whole-chain tests + Stage 10 crash fixes, Stage 10b

Range 0439818f..9002d72c: 187fa60e (notes), def1577c, 09e27760, 6ec8fb00, 9dbec896, 9002d72c.
Context read: C:\dev\handoff\planner-2026-10-12-findings.txt, docs\VP-stage10b-side-texture.md.
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 9002d72c, 0 errors. VideoProcessor-BetaTests built: OK.
Tests:
- native 2548 total, 2546 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). All four WholeHookChain tests (incl. the quick
  matrix), every ExtendedRange test (incl. …OnSaturatedColourWithSsim, …OnASinglePlaneRgbSource, …AfterPlaneHooks,
  …KeepsBtbThroughNlsAndSsim), AdaptiveDownscale* (incl. …GrainMapFromTheVectors), AmbientRoomLights…,
  AmbientSensor… pass.
- VideoProcessorConfigTests 103 passed, 2 failed (the two known RPC tests).
- VideoProcessorBetaTests (377): 5 of 5 PASS lines. It ran without a block here.
- Not re-run by me: the full 160-combination matrix (VP_TEST_MATRIX=full) and the GeForce mode.

## Blockers
- None.

## Should fix
1. **Stage 9: any [ambient] change resets the sensed room, so a Config Apply
   can switch profiles twice.**
   - `PublishStagedRuntimeSettings` calls `m_ambientState.Configure` when any
     [ambient] key changes, even only `hold_s` or `lux_lit`. `Configure`
     sets `sensed = false`, so `Current()` falls back to the saved `room`
     until the next reading (up to 1 s), which then decides at once.
   - If the sensed room is lit and the saved room is dark, an Apply switches
     to the dark profiles and, a second later, back.
   - Fix: keep the sensed state when only the thresholds or the hold
     change; reset on a change of mode or port.
2. **Stale comments about the alpha carry (10b no longer uses alpha):**
   - AdaptiveDownscale.h line 26 ("The Stage 10 carry's offset in alpha is
     blended…") and the blend's `o.a = fa.a; // the carry's offset`;
   - OcclusionMaskHook.h line 17 ("The Stage 10 range carry owns…");
   - "Temporal sigma-delta.glsl" line 53 ("extended-range carry (Stage 10)
     owns it only from the RGB stage to the SCALED stage").
   - Behaviour is fine (alpha is 1 throughout). The notes now describe a
     design that no longer exists, and the next reader could reintroduce
     the alpha assumptions.

## For the planner (precision, as the implementer asked)
- **10b always uses the texture-form restore.** libplacebo stores the
  scaled picture in a half-float texture first, so in-range edges move by at
  most 1 code but the mean rises to 0.03–0.083 (criterion ≤ 0.03; the tests
  now allow ≤ 0.1).
- The cause is measured: after the carry's texture output, libplacebo
  rejects an inline hook at SCALED. I see no cheaper route inside libplacebo
  7.360 / 377 without patching it.
- The alternative is a libplacebo patch letting the scaled picture stay in
  a full-float FBO (the Beta already carries FL35 patches). Otherwise the
  ≤ 0.1 mean is the price of 10b. The decision is the planner's.

## Notes
- **10b restore mapping.**
  - Output pixel → normalised position in the scaled picture's rect
    (`params->rect` at SCALED) → the same position in the carry's rect
    (`params->rect` at RGB). That rect is libplacebo's source crop, already
    adjusted for its destination rounding, so crop, blanking (a smaller
    destination) and anamorphic layouts (separate x / y scale) map
    correctly.
  - The carry runs after NLS (last at RGB), so the remainder is in post-NLS
    geometry, which the later linear scaling preserves. That is right.
  - The subtitle move's composed frame and RGB captures are single-plane
    RGB, now handled by the texture carry (tested). 6d rendered extras use
    the cache (already restored). Source-path extras re-run the carry and the
    restore in the same render.
  - The footprint average (4×4 bilinear, at least one source pixel) gives
    exactly 0 away from out-of-range content and cannot ring. Next to it, the
    result is scaled(clipped) + box(remainder), not scaled(original). That is
    the documented 9–16-code difference near BTB/WTW, and it is legitimate.
- **10b textures.** `clipped`, `remainder` and `restoreTex` are all
  rgba32f.
  - The carry releases everything on a GPU change. The restore resizes only
    its own texture (the fixed access violation). `Release()` frees all
    three and is called in `RetireOutput` before the device.
  - The carry hands libplacebo VP's own `clipped` texture as the RGB image.
    It is reused next frame, which is safe on one GPU queue.
- **The carry goes last in the list.** After `Prepare` only the 6d cache
  (PRE_OUTPUT) and the repair (OUTPUT, at the front) are added, so nothing
  can follow it at RGB. The restore is first at SCALED.
  `singlePlaneRgb` is now unused, because the texture form is always used.
- **Stage 9:**
  - `SensorReader` keeps its state under its own mutex; the GUI polls the
    newest reading once a second on the UI timer, as are `UpdateAmbient` and
    every source-lookup creation. The lookup captures the room by value, so
    the rules never read the state from another thread.
  - While there is no capture signal, a room change waits like every rules
    change (by design).
  - The room-lights key flips dark / lit until restart or an [ambient]
    change, and a second press ends it. Its "Key override" entry follows
    the same flag.
  - The port-clash check compares `[ambient] port` with `[projector] port`
    even when the LED drive is off. It rejects the configuration then too.
    Harmless, but strict.
  - Port names are validated as COMn only.
- **8-3, matcher and map:**
  - `adaptiveGrainMapWanted` is set from the last measured frame's
    `Active()`. So the matcher starts one source frame after the first
    downscaled frame, and keeps running one frame after the last.
  - The policy reuses the mask-only path, so the matcher is limited to
    film rates (above 30 fps: `VideoRateMaskOnly`, scalar grain).
  - The map advects with A→B vectors used backwards (the content's
    position in the previous map ≈ centre − vector), which assumes constant
    motion over one frame. Reasonable for a slowly smoothed grain measure.
  - Untrusted blocks keep their value and are marked untrusted, so the
    blend uses the scalar there. A frame below the trust share, or the
    scalar's cut flag, resets the map; a missing field restarts it.
  - The blend's NLS lookup goes from the PRE_KERNEL (post-NLS) position
    through NLS's own map to source blocks. That is the right direction.
    An unknown NLS shader keeps the scalar (logged).
  - Map textures are freed in `Release`, and the matcher's grain-error
    texture with the matcher.
- **Chain probe vs the renderer.**
  - The same order inside each stage: measure first at PRE_KERNEL; SSIM
    then the blend at POST_KERNEL; NLS then the carry at RGB; the restore
    first at SCALED; the mask before the sigma-delta at OUTPUT; the repair
    first.
  - Differences:
    - the repair is added by the caller's flag, not by
      `OutputEncodingRepair::Needed()`. That is equivalent in the shipped
      tests, which always include the LED hook, but a diagnostic run
      without the LED hook would add a repair the renderer would not;
    - the LED is one PRE_OUTPUT hook (the renderer has measurement +
      compensation);
    - there is no 6d cache;
    - the sigma-delta is given `color_bits 10` (true 10-bit). The owner's
      8-in-10 domain (colour depth 8), which `SigmaDeltaDomain` would
      choose, is not in the chain runs, only in the stage tests.
- **8-rs232 fixes.** The local-VP check is by process name
  (`VideoProcessor.exe`). If the snapshot fails it errs on the side of not
  opening the port. The resume after a test restarts any drive a renderer
  started during it, at the cost of a full-light reset right after a test.

## Verified OK
- **def1577c:**
  - `ResumeLedDriveAfterTest` is called after every test and restarts a
    drive that a renderer created during the test (stopped first, so the
    port is free).
  - Config opens the port itself only when no local VideoProcessor.exe
    runs, otherwise VP's error is shown.
  - The adaptive gate uses the source crop height against the destination
    height (SSIM's own condition). Tested.
- **9dbec896:** the inline-carry assertion for single-plane RGB sources and
  after plane hooks (CfL) is gone (texture carry, both tested), and so is
  the restore's resize freeing the carry's texture.
- **9002d72c:**
  - Primaries are converted before scaling, where libplacebo does it.
  - Clipped picture to libplacebo plus a per-channel remainder kept by VP;
    alpha untouched.
  - Away from out-of-range content, on = off within 1 code (3 with SSIM on
    P3), against 46 in 10a.
  - BTB / WTW are kept through NLS, SSIM, the blend, CfL, prescalers,
    SSimSuperRes, the subtitle move, blanking and bar detection (whole-chain
    tests).
- **Stage 9:**
  - The `ambient` variable is empty when off, so no rule matches it.
  - Hysteresis between `lux_dark` and `lux_lit` (dark above lit is
    corrected), a hold, and a lost detector falling back to the saved room
    (the next reading then decides at once).
  - The reader reopens after an error or 10 s of silence. Lux-line parsing
    is unit-tested.
  - Schema, Config card, the room-lights key and the overlay "Room:" line
    are all in place.
- **8-3:** `adaptive_grain_map` / `adaptive_grain_map_smooth_ms` are
  validated, fingerprinted and live-applied. The matcher runs only when the
  map wants it (or the de-judder / mask does). The grain-error texture is
  created on first use. The map is updated once per source frame.
