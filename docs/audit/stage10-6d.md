# Audit — delta 5 fix, Stage 10 (BTB/WTW carry), Stage 6d (rendered-warp extras)

Range 38e22458..6d8dd0fd: fff9f3b3, 0f70ca1a, c5b13c2f (docs), d1f5bbf8, 6d8dd0fd (docs).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 6d8dd0fd, 0 errors, no warnings in changed code.
Tests:
- native 2531 total, 2529 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). SigmaDeltaDomainFollowsTheTarget,
  ExtendedRangeDecision, ExtendedRangeKeepsTodaysScalingAndColours, ExtendedRangeKeepsBtbAndWtwThroughScaling
  and DejudderRenderedWarpMatchesTheSourceWarp pass (6d: cache max 1, hold max 0, displacement equal to
  the source path at 1:1 and 2x, interior mean 0.104 / 0.154). RendererMetadataCycles… passed.
- VideoProcessorConfigTests: 105 passed, 0 failed (the two RPC tests passed this time).
- VideoProcessorBetaTests: **not run by me.** Windows' Application Control blocked the freshly built
  exe (0xC0E90002) even after relinking the project twice; no security setting was changed. Your 377
  results stand unverified by this audit.

## Blockers
1. **6d: the cache and the warp outlive the GPU device (crash on renderer teardown).**
   - `RetireOutput()` releases the de-judder's textures (`ReleaseDejudder`,
     "before the device goes") and then destroys the device and the log
     (`pl_d3d11_destroy`, `pl_log_destroy`).
   - The new `dejudderCache` texture and `dejudderRenderedWarp` (its texture
     and its `pl_dispatch`) are not released there. They are freed later by
     the Impl's member destructors, by calling `pl_tex_destroy` /
     `pl_dispatch_destroy` on a device that no longer exists.
   - This happens on every renderer teardown after rendered extras have run
     once: VP exit, and every renderer rebuild (mode or refresh-rate
     changes, profile switches that recreate the renderer). With the
     de-judder on this is the normal case.
   - Fix: destroy both in `RetireOutput` next to `ReleaseDejudder()`, and
     clear the cache's stored `gpu`.

## Should fix
2. **6d: a fractional destination rectangle makes the extra a resample, not 1:1.**
   - libplacebo rounds the destination rectangle to whole pixels
     (renderer.c, `dst_rect`), so A's PRE_OUTPUT texture, and the cache, is
     the rounded rectangle.
   - The extra declares the whole cache texture as its image and renders it
     onto `f.target`'s crop. When that crop is fractional (aspect fits,
     blanking), libplacebo re-derives the source rectangle and resamples by
     a sub-pixel scale and offset with the configured scaler.
   - The extras are then slightly softer or shifted against A, alternating
     at the film rate.
   - The tests use whole-pixel rectangles only. Fix: render the extra onto
     the rounded integer rectangle (or record `params->rect` in the cache
     and use matching crops).
3. **6d: the cache is armed in mask-only mode.**
   - The arming condition is `dejudderReason == Active`, which is also true
     for occlusion-mask-only mode, where extras are never presented.
   - Every source frame then pays a full-frame copy. On 7.360 the
     texture-input PRE_OUTPUT hook also forces the encoding repair onto A's
     chain.
   - Add `dejudderExtrasAllowed` to the condition.

## Notes
- **Stage 10, today's linear light.**
  - libplacebo linearises a pure power as v^γ × (max − min) + min with the
    frame's own min/max (`pl_shader_linearize`).
  - For SDR to SDR, `pl_color_space_infer_map` copies the target's min/max
    luminance to an SDR source whose luminance is unknown. VP sets source
    luminance only for HDR, so today the scaling cancels and the result is
    exactly v^(γs/γo), which is what the carry produces.
  - Equivalence would break only for an SDR source with its own luminance
    metadata. The restore uses libplacebo's own delinearise, so the black
    handling matches.
- **Stage 10, "applying the matrix after scaling is the same picture".**
  That is true for linear-light scaling only. With sigmoid upscaling, or
  gamma-light scaling, a coloured edge comes out slightly different from
  today (the BT.709→P3 matrix is applied after a non-linear filter). The
  test compares flat patches only.
- **Stage 10, alpha through the scalers.**
  - The offset survives every bundled shader between RGB and SCALED: NLS
    and NLS+ return `HOOKED_tex` (alpha kept), SSimDownscaler returns
    `L.a`, SSimSuperRes returns `c0` with its alpha. ArtCNN and FSRCNNX run
    at LUMA, CfL at CHROMA.
  - A user shader between MAIN and POSTKERNEL that writes alpha (e.g.
    `vec4(rgb, 1.0)`) would make the restore add +1 to every channel, giving
    a white picture. A guard (e.g. ignoring alpha above the carried range)
    or a documented rule would close it.
  - Sharpening scalers also see the per-pixel shift as artificial edges
    next to out-of-range pixels. Only ewa_lanczossharp was measured (in-range
    edges max 1); neither SSIM shader was GPU-tested with the carry.
- **Stage 10, a pixel both below 0 and above 1** (saturated colour) stays out
  of range after the shift (hi − lo > 1). This is rare, and libplacebo
  clamps only the negative side when it linearises.
- **Stage 10 hook order:** the carry runs first at RGB (before NLS and any
  MAIN shader), the restore first at SCALED, before the LED and PRE_OUTPUT
  hooks. The old-path de-judder extras reuse the modified `renderImage` and
  A's hooks, so the carry stays consistent there. Overlays are on the
  target, which is untouched. The auto-off list (HDR source or output,
  calibration LUT, non-power transfers, non-BT.709 SDR source, output
  without BT.709, a disabled hook) covers what the design cannot do.
- **6d, cache contents:**
  - It is A at PRE_OUTPUT, in the target colour space and not yet encoded.
    It already includes the Stage 10 restore and the LED compensation; it
    does not include dither, OUTPUT hooks, target overlays or a target LUT
    (libplacebo applies the target LUT after PRE_OUTPUT, so the extra applies
    it once).
  - The LED gain baked into it is A's, the same as the old path, which also
    reused A's parameters.
  - `OutputOnly` drops every hook that is not OUTPUT-only, plus deband,
    colour adjustment, peak detection, cone, distortion and the image LUT.
    Dither stays as in A (suppressed by the sigma-delta). I found nothing
    applied twice.
- **6d, repair:** on 7.360 the cache hook is pushed before the repair
  decision, so A gets the repair. The extras' OUTPUT-only list re-checks
  `Needed()` without it and drops it. On 377 it is never needed.
- **6d, untested and stated:**
  - The NLS case on the GPU. The slope is a central difference of the
    forward map, which is fine for small displacements; it is a linear
    approximation near the edges.
  - Timing. The Config tooltip claims "about a quarter of the GPU time at
    4K", which has not been measured.
  - The cinema checks.
- **6d, other paths:** the vpOwned swapchain path uses the same
  `pl_render_image(image, target, extraParams)` call. An unknown NLS shader
  keeps the source path (logged).
- The new `SigmaDeltaDomainFollowsTheTarget` test sits under the D5-3 test's
  comment block (the comment now precedes the wrong test). Cosmetic.

## Verified OK
- **fff9f3b3:**
  - `SigmaDeltaDomain::FromTarget` supports only sample depth 8–10 with
    colour depth ≤ sample. libplacebo's D3D11 swapchain and VP's own path
    report 16 for R16G16B16A16_FLOAT, so the sigma-delta is removed from
    that frame's list and the real domain is logged.
  - The docs now say `noise_amplitude` is in output codes. Unit-tested.
- **Stage 10:**
  - The carry, sign-preserving |v|^(γs/γo), is exact for pure powers.
  - The offset goes into alpha as stored (float16), with in-range pixels at
    exactly 0.
  - The restore delinearises only when libplacebo hands it linear values,
    then applies the BT.709→output matrix, sign-preserving.
  - Off is today's exact path: no hooks, and the source colour space is
    untouched.
  - A disabled hook switches it off until restart, logged.
  - The key is validated, fingerprinted, live-applied and has a Config
    control ("Keep below-black and above-white (SDR)", saved by the Config
    test).
- **6d:**
  - Warp: backward mapping with the shared vector rule (the same
    `FieldVectorGlsl` as the source warp) and a hard hold of A where the
    rule says hold.
  - Catmull-Rom, edge-clamped, with no value clamp, so BTB/WTW survive.
    The rgba16f cache and warp textures are reused and recreated only on a
    size change.
  - `dejudder_warp_stage` (rendered | source) is validated, fingerprinted,
    live-applied and has a Config control.
- **Settings first:** both new keys have Config controls and validation; no
  key-only behaviour was added.
