# Audit — Stage 6c-2: the occlusion mask with NLS (+ re-audit of 6b-2b/6c fixes)

Diff audited: e4429613..f4c4f920 (1c81e81d, f4c4f920), source files only.
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at f4c4f920, 0 errors, no warnings in changed files.
Tests: native 2482 total, 2480 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). New tests pass:
OcclusionMaskFollowsTheNlsMap, NlsMaskMappingMatchesTheShadersCentreScales, the policy test
(VideoRateMaskOnly). VideoProcessorConfigTests 98 passed, 2 failed (the two known RPC tests).

Spec used: VP-stage6b2b-6c-answers.md (items 2, 3), VP-dither-judder-plan.md ("6c-2",
"Audit fixes"), stage6b2b-6c-config-stats-mask.md and its responses.

## 6b-2b/6c re-audit
- Should fix, stats line "mask on" when unused — **resolved.** The flag is set
  only after the "no sigma-delta" check; "mask on" needs a use within the last
  second, otherwise the line gives the reason (waiting / no sigma-delta / NLS
  shader not known).
- Note, "cadence 0.00" in mask-only mode — **resolved.** The ratio is now
  computed in the estimate path whenever both rates are known.
- Note, late prediction after a dropped frame — **resolved.** The prediction
  carries B's source sequence and counts only if that frame is the one shown.
- Planner's 30 fps limit — **done right.** See check (5).

## Blockers
- None.

## Should fix
- None.

## Notes
- (4) **The off-centre test proves the crop domain, but only through two of
  the three profiles.** I recomputed the test case with the whole texture as
  the domain. The mask edge moves by: standard 2 px, NLS+ 3 px, protected
  5 px. So with the wrong domain the test fails (protected and NLS+), but the
  standard profile on its own would still pass at the 2 px tolerance. That is
  fine as long as all three stay in the test.
- (4) Only the horizontal stretch is tested. The vertical profile (NLS-V,
  `warp_axis` 1) uses the same code with the axes swapped, but no test runs
  it.
- (3) The "NLS shader not known" text on the stats line is only cleared when
  the mask is next used. If NLS is switched off at that moment, the line can
  show the old reason until the next masked frame (well under a second at
  24p). Cosmetic.
- (3) The shader is recognised by the end of its file name: any file ending
  in `NLS.glsl` (e.g. a user's `MyNLS.glsl`) would be treated as the bundled
  NLS. Today only the bundled files are named that way.
- (1) `geometry`: the shaders cut a fraction down to a whole number, but the map
  rounds it, so a value like 0.7 would give different results. The loader
  writes only 0 or 1 (classic/protected), so this cannot happen from the
  Config app.
- (5) The "above film rates" line is logged once per VP run, not once per
  source. Fine for diagnosis.

## Verified OK
- (1) **NlsMaskMapping matches the shaders**, line by line:
  - Classic NLS: centre scale = 1 + (ratio − 1) × strength on the warped axis,
    1 on the other.
  - Classic geometry: the power curve. Protected: the same quintic Hermite
    as the shader, with the same edge slope max(0.2, 1 − 0.5 (s − 1) curve).
  - With scale 1 the protected curve is exactly the identity, so classic's
    other axis is untouched.
  - NLS+: balance × strength, limited by log(max zoom) / log(ratio); q and
    both exponents as in the shader; ratio clamped to 1–1.5, curve to 1–2.9,
    protection to 0–0.45.
  - NLS+ is chosen by file name (`NLSPlus.glsl`), not by `axis_balance`.
  - A missing curve, geometry or protection (or NLS+'s axis_balance or
    protections) makes the map invalid, so the mask stays off.
  - strength and max_center_zoom fall back to the loader's own defaults
    (1, 1.08), which the loader always fills in.
- (1) **Binding log unchanged.** It now calls the same CenterScales, using the
  old rule (NLS+ when axis_balance is present), with the same defaults and
  limits. The logged values are the same as before.
- (2) **GLSL map matches** the shaders' functions. Output pixel → linear
  source position → normalised over the source crop → NLS map → back to
  source pixels → block. Domain = source crop: libplacebo's MAIN (= RGB)
  stage texture is the crop rounded out to whole pixels, with the rect at the
  sub-pixel offset. `HOOKED_pos` spans that rect (shaders.c sh_bind,
  renderer.c pass_read_image). So the shader's 0..1 is the crop, exactly for
  whole-pixel crops and within a pixel otherwise.
- (3) **Renderer:**
  - The mask runs with NLS only for NLS.glsl / NLSPlus.glsl with a valid map.
  - Otherwise: mask off, one log line naming the file, stats "mask off (NLS
    shader not known)", and the parameter written as 0.
  - SetFrame clears the NLS map and SetNls is called after it, so a frame
    without NLS never keeps an old map.
  - Domain passed = `renderImage.crop`, same as the source crop.
- (4) **GPU test:**
  - The three profiles match VideoProcessor.cfg after the loader's
    defaults: standard = classic / 0.35 / curve 2; protected = 0.35;
    NLS+ = protected, 0.35 / 0.25, balance 0.25, zoom 1.08.
  - Ratio 1.33 is within the configured 1.4 maximum.
  - 2 px in both axes is fair: a block is about 22 × 17 output pixels here,
    and the square's own edge is softened by the scaler.
- (5) **30 fps limit:**
  - Without extras, a source above 30.5 Hz gives `VideoRateMaskOnly`. The
    matcher and B's textures are released (it is in the release list) and the
    refusal is logged once.
  - With extras (strength > 0, uneven film cadence) the check is skipped, so
    the mask still runs.
  - Unit-tested: 50 and 59.94 are refused; 23.976 and 24p with extras are
    allowed.

## To check on the projector (NLS on, scope profile, mask on)
- vp.log: "De-judder occlusion mask: used on N source frames (… failed 0)".
  There must be **no** "not used with NLS shader '…'" line for the standard,
  protected or NLS+ profiles.
- vp.log: the NLS binding line shows the same centre scales and balance as
  before this change (compare with an older log).
- Ctrl+I: "…, mask on" with NLS active. "mask off (NLS shader not known)"
  means the map was refused, so report it.
- Screen: on a slow pan with dark gradients near the **sides** of the scope
  picture (where NLS stretches most), no dither "swimming" or blocky edges
  at newly uncovered areas. Compare mask on/off: no change in picture, no
  shifted block-shaped patches. Check each NLS profile once.
- A 50/60p source with only the mask on: vp.log "occlusion mask alone is for
  film rates … off for this source"; Ctrl+I "mask only above film rates".

## Implementer responses

No blockers or should-fix. Notes acted on:
- **(4) vertical not tested: fixed.** NLS-V (`[shader.nls.vertical]`,
  NLS.glsl with warp_axis 1) added to OcclusionMaskFollowsTheNlsMap; passes
  in both axes with both crops. All three earlier profiles stay in the test,
  as you advised.
- **(3) file recognised by its ending: fixed.** It is now matched by exact
  file name (`NLS.glsl` / `NLSPlus.glsl`, any folder), so `MyNLS.glsl` is
  not taken for the bundled NLS.
- **(3) stale "NLS shader not known" text: fixed.** It is cleared as soon as
  a frame renders without NLS.
- **(1) geometry rounding: fixed.** The map now cuts the fraction off like
  the shaders (`int(clamp(geometry, 0, 1))`); unit-tested with 0.7.
- **(5) the film-rate line once per run:** accepted.
