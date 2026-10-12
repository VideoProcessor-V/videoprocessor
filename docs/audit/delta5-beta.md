# Audit — delta 5 (D5-1..D5-4) and the Beta feature GPU tests

Range d306240d..ee607a7a (d306240d brief, 88680958 D5-1/2/3, 847f6a55 D5-4, ee607a7a Beta tests).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at ee607a7a, 0 errors, no warnings in changed code.
VideoProcessor-BetaTests.vcxproj built separately: OK.
Tests:
- native 2526 total, 2524 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). All sigma-delta, occlusion-mask and
  de-judder GPU tests pass, including SigmaDeltaKeepsCodesExactOnATrueTenBitOutput,
  OcclusionMaskWorksWithFullRangeOutput and DejudderToleranceFollowsTheSourceRange;
- VideoProcessorConfigTests 103 passed, 2 failed (the two known RPC tests);
- VideoProcessorBetaTests: all PASS, same numbers as yours (8-in-10 white 940 / black 64 / grey mean
  502, libplacebo alone 504; true 10-bit limited 940/64/501 and full 1023/0/501; LED code 40 → 28,
  expected 27.89; mask 500/504 kept, 500 stopped; de-judder 47 of 48 blocks).

## Blockers
- None. For the FL35 set-up (limited, 10-bit swapchain, colour depth 8) the domain D5-1 now sets is
  exactly what the cinema rule typed by hand, so nothing changes on the projector.

## Should fix
- (D5-1) **A float swapchain gets a 10-bit quantiser.**
  - VP can negotiate R16G16B16A16_FLOAT. Its target is full range with
    sample and colour depth 16.
  - `SetHookParameter` clamps the parameters to their limits (8–10), so the
    sigma-delta then quantises a float output to 10-bit codes 0–1023. Any
    value above 1.0 (scRGB HDR highlights) is clipped, and the float
    precision is lost.
  - This was already true with the hand-typed values (worse: 8-bit), but
    D5-1 is where it should be handled. For a float target (or a depth
    above 10), switch the sigma-delta off for the frame (`enabled` 0) and
    log it once.
  - The log line should also show the values actually set, not the
    requested 16. FL35's SDR/LLDV output is not affected.

## Notes
- **D5-2 snap.** Half-float step at value v is 2^(floor(log2 v) − 10); in
  10-bit codes that is about 0.5 near white, 0.25 for codes 256–511, 0.125
  for 128–255, and so on. With the 4% margin the snap is:
  - Codes 512–1023: snap 0.52, so **every** value snaps to its nearest
    code. At true 10 bits the sigma-delta is plain rounding in the upper
    half; nothing below a whole code is carried there. That is
    unavoidable, because rgba16f cannot store finer than about half a code
    there, and its grid is offset from the code grid (940 sits between
    939.58 and 940.08). It does hide half-code gradients, but nothing finer
    could be trusted.
  - Codes 256–511: snap 0.26, so the quarter-code values next to a code
    snap and the half-code value is carried. Right.
  - Below code 256: the fixed 0.13 floor governs. It is wider than the
    half-float step there (0.125 and finer), so 1/8-code detail next to a
    code is also snapped. Harmless, but not needed: at 10 bits the step
    term alone would do.
  - The step changes at the powers of two (codes 256 and 512), which is
    correct. Each channel is computed separately (vector ops). Negative
    (below-black) values use the 0.13 floor.
  - No test shows the true-10-bit sigma-delta dithering a sub-code value
    (for example a grey at k + 0.5 below code 512). The tests prove only
    that exact codes stay exact.
- **exact_half().** `packHalf2x16` rounds to the nearest half (HLSL
  f32tof16 on D3D11). The stored value is then exactly representable, so
  storing it cannot round again. It is within a quarter code of k, so
  libplacebo's conversion to the 10-bit swapchain lands on k. It applies
  only on the 10-bit path; the 8-bit paths (scale, snap, step, motion
  units, store) are untouched.
- **Brief vs code.** The brief keeps `noise_amplitude` in 8-bit-code units,
  scaled internally. The shader instead documents it as "in output codes"
  and does not scale it, so at 10 bits the dither is a quarter as large.
  That is a defensible choice (limit-cycle breaking scales with the
  quantiser step), but the brief, or the shader, should agree.
- **D5-1 shared parameter names.** The renderer sets `levels_full`,
  `swapchain_bits` and `color_bits` on every hook that has them. Today
  only the sigma-delta does; a future user shader with the same names
  would be overwritten too.
- **D5-1 untested.** The renderer's derivation from `target.repr` has no
  test; only the shader's behaviour for given values is tested.
- **D5-3.** Full-range Y (0–255 per unit) is measured as 219 per unit:
  limited-equivalent code differences, which is all the matcher uses (mean
  absolute differences). The tolerance and the frame-trust threshold now
  mean the same for both ranges. The test's (8, 4) shift is whole pixels
  at the matcher's half resolution (4, 2), so there is no interpolation
  error. 3.2 codes against a tolerance of 3 (2.75 when full range) is a
  narrow margin, but deterministic.
- **Beta tests vs VP:**
  - The shaders are parsed directly, with only `{{output_width}}` /
    `{{output_height}}` replaced. Parameters are set by a copy of
    `SetHookParameter` that does not clamp to the shader limits; harmless
    here, because all test values are within them.
  - Stages match VP: the sigma-delta at OUTPUT behind a texture-input
    PREOUTPUT hook, as VP's LED hooks are; the LED compensation at
    PREOUTPUT; the occlusion mask's own hook ahead of the sigma-delta.
  - The LED test reads a true 10-bit target, because 377 rounds an 8-in-10
    output to the 8-bit grid. The 8-in-10 LED path on 377 is therefore
    covered only through the sigma-delta test.
- Nothing left marked unbuilt in the changed files. The only "untested"
  wording left is in the older plan (VP-dither-judder-plan.md), about
  earlier stages.

## Verified OK
- **D5-1:**
  - Per frame: levels from `pl_color_levels_guess(target.repr)`, sample
    depth (8 if unset), colour depth (else the sample depth). That covers
    8-bit, 10-bit, 8-in-10, full and limited correctly.
  - The cost is three parameter look-ups per hook per frame, which is
    negligible.
  - Logged once per domain change.
  - Without the sigma-delta in the chain nothing is set and nothing is
    logged, so "off = Bill's VP" holds.
- **D5-2:**
  - At colour depth 10: in_scale and max_code 1023, step 1/1023.
  - The luma passes stay in 8-bit units (1023/4), and the motion gate is
    ×4.
  - The test ramp is scaled to match, and the occlusion gate is unchanged.
  - Exact white, black and an off-grid grey are held in both ranges on 7.360
    and on 377.
- **D5-3:** the range comes from the source contract (`sampleRange ==
  FULL`). Both A and B get it.
- **D5-4:** the occlusion mask works with full-range output (mid grey 508/512
  carried, stopped with mask 0, alpha back to 1).
- **Beta tests:**
  - Sigma-delta 8-in-10 and true 10-bit, LED compensation (below black
    kept, light within 1% with the boost on and off), occlusion mask and
    de-judder all run on libplacebo 377 with no repair.
  - The libplacebo log is clean for every test.
  - The stub pch.h only lets the shared sources compile.
