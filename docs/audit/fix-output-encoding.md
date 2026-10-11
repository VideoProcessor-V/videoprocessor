# Audit — Fix: output encoding lost after PRE_OUTPUT hooks (libplacebo 7.360.1)

Diff audited: e7b3cceb (inside 5e685c6f..143a55ad). The other Claude session was idle
throughout.
Build: OK — full x64 Release /t:Rebuild at 143a55ad, 0 errors, no new warnings.
Tests: native 2470 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known);
PreOutputHooksKeepTheOutputEncodingWithTheRepair passes. VideoProcessorConfigTests 97 passed,
2 failed (the two known RPC tests).

Spec used: docs/VP-led-drive-module.md, last section; libplacebo 7.360.1
src/renderer.c (`pass_output_target`, `pass_hook`, `img_tex`, `img_sh`).

## Blockers
- None. The fix is right and needed; without it every FL35 build since Stage 7
  step 4 outputs full range on a limited-range chain.

## Should fix
- LibplaceboVideoRenderer.cpp ~14785 + OutputEncodingRepair `Needed` —
  **the repair can encode twice.**
  - `Needed()` looks only at the hook list VP submits. libplacebo skips any
    hook it has disabled after a failure *before* it finishes the shader
    (renderer.c `pass_hook`: the disabled-list check comes before `img_tex`).
    A disabled hook therefore does not trigger the bug, but VP's repair still
    runs.
  - The custom hooks and the compensation shader are dropped from the list
    once libplacebo disables them (`MarkRuntimeFailures`). The LED
    measurement hook is not: after "libplacebo disabled the measurement
    pass" VP keeps adding it every frame.
  - If that happens while no other texture-input PRE_OUTPUT hook is active
    (gamma boost off, LED drive on), the picture is encoded twice: limited
    range applied to already-limited values (black ≈ 30, white ≈ 217).
  - Rare (the measurement hook fails only if its GPU buffer cannot be made),
    but silent on the projector. Fix: stop adding the measurement hook once
    libplacebo has disabled it, or make `Needed()` ignore hooks in
    `pl_renderer_get_errors().disabled_hooks`.

## Notes
- The repair always encodes. libplacebo skips the encode itself when the
  target LUT is of type CONVERSION (`lut_type != PL_LUT_CONVERSION`). VP only
  ever sets NORMALIZED or UNKNOWN (renderer ~7172/7180), so this is safe
  today; mirror the rule if a CONVERSION LUT is ever allowed.
- For 4-component images with a different alpha mode, libplacebo would first
  call `pl_shader_set_alpha(…, INDEPENDENT)`; the repair does not. VP's
  images are 3-component at this point, so unreachable today.
- The vp.log filter matches the exact message "Attempted to modify an
  immutable shader!" from anywhere in libplacebo. A different, genuine cause
  of the same message would also be counted rather than shown. The first
  occurrence is still logged, and a count every 100,000.
  - Frequency: one or two per frame → a count line roughly every 15–30 min
    at 60 Hz.
- Cost: one extra full-picture pass per frame (a sample plus the encode, into
  an rgba16f texture), roughly 0.05–0.1 ms at 2560×1600. Precision is the
  same as libplacebo's own path when a texture hook runs at OUTPUT (one fp16
  intermediate after the encode).
- Consequence noted in the doc and agreed: with the bug, a source alpha plane
  would also lose its background blend, so 6c's M1 (mask in alpha) is
  unlikely to work; M3 is the probable outcome.
- The new test itself does not assert a clean libplacebo log (see the 6a
  re-audit).

## Verified OK
- (a) The bug is real (renderer.c 7.360.1).
  - `pass_output_target` takes `sh = img_sh(pass, img)` at the start. A
    `PL_HOOK_SIG_TEX` hook at PRE_OUTPUT (line 2742) runs `img_tex`, which
    finishes `img->sh` into a texture.
  - Every later step writes to the stale `sh`: the alpha/background blend,
    `pl_shader_set_alpha`, `pl_shader_encode_color` (line 2816), the
    native-LUT step and rotation. All of them are rejected as "immutable",
    and the final draw (`img_sh` on the texture) has no encoding.
  - `PL_HOOK_SIG_COLOR` hooks do not trigger it.
  - VP's real path is affected: `output_range: limited`, and `gamma_boost:
    2.4` puts `LED compensation.glsl` (an mpv texture hook — the only bundled
    PREOUTPUT shader) in every frame, with or without the LED drive. The LED
    measurement hook (`PL_HOOK_SIG_TEX`) triggers it too.
  - The test reproduces it: limited-range red 255 instead of 235.
- (b) The repair:
  - It re-applies exactly the skipped step: `pl_shader_encode_color` with
    `pl_color_repr_normalize(target repr)`, and libplacebo's own alpha rule
    (alpha left alone when it matches or the image has fewer than 4
    components). The output scale is still applied later by libplacebo, on a
    fresh shader, so it is not lost.
  - The steps it leaves out are unused by VP:
    - background/alpha blend: only for 4-component images;
    - XYZ output: VP outputs RGB;
    - native target LUT: VP's calibration LUT is NORMALIZED, applied in
      colour management before PRE_OUTPUT;
    - rotation: VP sets none.
  - It is inserted at the front of the frame's hook list. Hooks run per stage
    in list order, so it is the first OUTPUT hook, before the sigma-delta,
    which now sees properly encoded code values again.
  - It is added only when the list holds a texture-input PRE_OUTPUT hook.
    It uses the same `target.repr` handed to `pl_render_image`.
  - It cannot encode twice on its own (a single insertion per frame), except
    for the disabled-hook case above.
- (c) The GPU test renders through libplacebo on WARP:
  - limited and full range, two colours, the LED measurement hook: with the
    repair, the output matches the no-hook render within 1 code;
  - the LED compensation shader at neutral settings, limited range: the same;
  - it checks the repair actually ran;
  - it asserts that without the repair libplacebo *still* outputs 255 instead
    of 235. A libplacebo that fixes the bug makes this test fail, with a
    message to remove the repair, so it cannot silently start encoding twice;
  - `Needed` is false for an OUTPUT-only list.
- (d) vp.log: the libplacebo message is logged once with an explanation, then
  only a count every 100,000 repeats (atomic counter, thread-safe). Plus one
  line when the repair first becomes active, with the target levels.
- (e) Nothing else between PRE_OUTPUT and OUTPUT matters to VP. The section
  contains only the blend (4-component only), the alpha mode, the encode, the
  XYZ step, the native LUT and rotation. Dithering, the output scale, plane
  sampling and the final draw come after OUTPUT on a new shader and are not
  affected. The LED measurement still reads the picture before the encode,
  as specified.

## Planner follow-ups (c9914404)

Build: OK — full x64 Release /t:Rebuild at c9914404, 0 errors, no new warnings.
Tests: native 2470 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted — known);
PreOutputHooksKeepTheOutputEncodingWithTheRepair and the sigma-delta tests pass.
VideoProcessorConfigTests 97 passed, 2 failed (the two known RPC tests).

Extra measurement, outside the repository: I exported c9914404 to my scratch
folder and added one diagnostic test there (repository untouched). It renders
a constant grey through libplacebo on WARP into a real 10-bit R10G10B10A2
target, set up like VP's default swapchain (limited range, colour depth 8,
bit shift 2). It records the 10-bit red code over 8 refreshes:

| Input | No hooks | Sigma-delta only | LED hook + repair + sigma-delta |
|---|---|---|---|
| white | 940 ×8 | 940 940 940 **944** 940 940 940 940 | 940 940 940 **944** 940 940 940 940 |
| 0.5 | 502 ×8 | 504/500 alternating (mean 502) | 500/504 alternating (mean 502) |
| black | 64 ×8 | 64 ×8 | 64 ×8 |

### Blockers
- None.

### Should fix
- shaders/Temporal sigma-delta.glsl, `in_scale` / `scale` / `full_scale`
  (limited range on the 10-bit swapchain, **the cinema default**) — the
  sigma-delta reads code values ×256. libplacebo actually delivers a code
  *k* there as 4k/1023, i.e. ×255.75: colorspace.c sets limited white at
  235/256 × 2^d/(2^d−1), with d = 10 after normalisation. So every value
  is read 0.1% high: white 235 reads as 235.23.
  - Measured above: peak white comes out as code 236 (944) on about one
    refresh in four instead of a steady 235 (940). It averages 235.23, with
    a slight flicker into whiter-than-white at peak white.
  - Mid-grey is effectively unaffected (≈0.12 code); black is exact.
  - Not caused by this commit: the 10-bit path is byte-for-byte as before,
    so it has been there since Stage 2 and also affects the Stage 3 ramp
    photos.
  - Fix: use 1023/4 (255.75) for limited range on the 10-bit swapchain in all
    three places (the output step 4/1023 is already right).
  - Add a 10-bit target to the domain-contract test: today it only uses an
    8-bit target, where the error cannot show.

### Notes
- The same measurement settles part of an open question in brief §3. It
  says libplacebo's limited 10-bit output may land code k at 4k−1 ("one code
  low above ~128"). In 7.360.1 it lands exactly at 4k (white 940, black 64).
  Only the Barco box's own rounding remains to check (stage 3).
- (f) The new domain-contract test checks white and black only, on an 8-bit
  target with `swapchain_bits 8`. That is the diagnostic path, not the
  cinema path (see above).

### Verified OK
- (f) Domain-contract test: the LED measurement hook, then the repair
  (inserted first, as the renderer does), then the sigma-delta with the
  quantizer on and noise 0. Limited white/black reach 235/16, full reach
  255/0. Libplacebo on WARP, passes.
- (g) Scale change:
  - On the 10-bit swapchain the result is identical to before: limited → 256,
    full → 255 in the quantizer and in both luma passes. So the default path
    is byte-for-byte unchanged.
  - On an 8-bit swapchain, limited is now ×255. That is right per libplacebo:
    with d = 8, limited white = 235/256 × 256/255 = 235/255. The test had
    caught the old ×256 reading 235 as 236.
  - The output step (4/1023 on 10-bit limited, 1/255 otherwise) is unchanged.
- (h) Docs:
  - Brief §3 adds the PRE_OUTPUT bug (every build from Stage 7 step 4 to
    e7b3cceb output full range), the swapchain-dependent limited domain, "a
    `//!` anywhere starts a pass" and the log-only pass failures. All match
    libplacebo's source, apart from the 10-bit "/256" wording above.
  - Audit README §4 item 6 makes the clean libplacebo log a standing rule,
    with the single known PRE_OUTPUT exception.
  - The tally allows only `VideoProcessor-FL35-rangefix` or newer on the
    projector.
  - M1′ is recorded in the Stage 6 answers and the brief (mask written into
    the OUTPUT FBO's alpha after the repair, M3 as the fallback), with three
    harness proofs required. That is consistent with this audit's note that
    M1 through the source alpha would be lost.
