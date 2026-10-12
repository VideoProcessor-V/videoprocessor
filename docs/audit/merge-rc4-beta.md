# Audit — 7c fixes (65696c65), merge of 1.4-beta-rc4 (25bc4c4a), patched libplacebo for the Beta (ae560a13)

Reviewed: `git show 65696c65`; `git diff 65696c65 25bc4c4a` and `git show --cc 25bc4c4a`;
`git show ae560a13`; 3rdparty\libplacebo-beta\README.txt and source\ (both archives extracted and
compared, patches re-applied).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at ae560a13, 0 errors, no warnings in changed code; the
Beta renderer DLL is built by the solution. VideoProcessor-BetaTests.vcxproj built separately: OK.
Tests:
- native 2523 total, 2521 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known);
- VideoProcessorConfigTests 103 passed, 2 failed (the two known RPC tests);
- VideoProcessorBetaTests: both PASS lines (code 40 → 40/40/40, 980 → 980/980/980, white 940; PRE_OUTPUT
  probe 283 alone, 306 with the repair, Needed() = 0);
- tools\test_libplacebo_variants.py, both load orders: PASS (API 26 for both renderers, shared runtimes
  identical).
No Device Guard block. Same as your results.

## Blockers
- None.

## Should fix
- (5) **None of our GPU features is tested on the Beta's libplacebo.**
  - The 85 GPU tests in LibplaceboLutParserTests link the stable 7.360
    library. They cover the sigma-delta's 10-bit scale, the LED
    compensation (sign, E 2.41), the occlusion mask and its NLS map, the
    de-judder warp and the encoding repair.
  - On 377 only two things are proven: the output clamp and the
    PRE_OUTPUT decision. 377 changed the output path (it encodes after
    PRE_OUTPUT itself).
  - Before the owner uses the VP Renderer Beta on the projector, run at
    least the sigma-delta, LED-compensation, occlusion-mask and de-judder
    GPU tests against 377 (e.g. build them into VideoProcessor-BetaTests).

## Notes
- (0) The cut's CNTR now always goes first and the LED's jump follows one
  command gap (≈ 200 ms) later. That is what the 7c-2 answers (Q4) specify.
  The fade-up follows the LED as it really lands.
- (1) docs\libplacebo-beta.md still says "Plugin API 25". That is a
  document; the code and tests say 26.
- (2) Deploying: AGENTS.md's runtime pair names only
  `vprenderer\VideoProcessorVPRenderer.dll`. If the Beta is to be used,
  `vprenderer-beta\VideoProcessorVPRendererBeta.dll` and `libplacebo-377.dll`
  must come from the same build. API 26 refuses a mismatched DLL rather
  than running it.
- (4) Patch 0002 widens the clamp for every limited-range system,
  including YCbCr chroma (now 4–1019 too), not only RGB. That is legal per
  BT.709 / BT.2100, and `pl_color_repr_limits` is used only by the output
  clamp. VP outputs RGB.
- (4) The FL35 DLL was built with GCC 16.2 / Meson 1.12.1; Bill's was built
  with 16.1 / 1.11.2. Both the Beta tests and the variants test load it
  against the unchanged bundled runtimes, so all its imports resolve.
- (6) The 8-in-10 check asserts "below 64 and on the 8-bit grid", not the
  exact 40, and the reserved-code check looks at R only (R = G = B is
  asserted elsewhere). It is enough to tell the patched DLL from the
  unpatched one.

## Verified OK
- (0) **65696c65:**
  - The cut's (and the pre-position's) CNTR is queued before the LED
    command. The driver sends a waiting CNTR first, so the order no longer
    depends on thread timing.
  - Pre-lift during a fade's gain hold happens only when the frame is truly
    black (APL < 17 and highlight < 20, the custom dim's own test).
  - The pairing comment now says "the modelled light now".
- (1) **Conflicts:**
  - rc4's `dynamic_target` / `custom_tone_mapping` and our saturation,
    de-judder (strength, tolerance, cut threshold) and occlusion mask are
    each present in all of: the settings, the validation, the parsing,
    the fingerprint, the copy to the render parameters, the transport
    copy and the live-change list.
  - Plugin API 26 is in the header, ConfigFileTests and
    `test_libplacebo_variants.py`. Nothing else checks the number: the
    loader and the proxy test compare against the constant, and the
    exports return it.
- (2) **Beta project:**
  - Its source list is now identical to the stable renderer project; the
    only differences are `VP_LIBPLACEBO_BETA` and libplacebo-377.
  - Shaders are found next to VideoProcessor.exe, so both renderers use the
    same `shaders` folder.
  - The Beta-only code (shader-cache folder, library name, renderer name,
    rc4's tone fields) does not touch our features.
  - The LED drive and the de-judder live inside the renderer DLL, so the
    Beta has them too. No Config page assumes the stable renderer.
- (3) **The rest of rc3..rc4 came in unchanged.** I compared each of the 114
  files' changes with rc4's own:
  - 106 are identical;
  - the 8 conflicted files differ only by the resolutions above (and, in the
    renderer, indentation).
  - That covers subtitle script modes, the `~=` operator and the packaging.
  - Config pages: LED drive 20, Updates 21; the page-count test (22) passes.
- (4) **Patched libplacebo:**
  - Applying 0001 + 0002 to Bill's 8bccd657 archive gives exactly the
    source in the FL35 archive. Only colorspace.c and two test files differ.
  - The complete patched source is present (LGPL), with the patches and
    FL35-README.
  - Hashes match the README: DLL 8f63be99…e653, FL35 source zip
    92ffa9c0…d5ea. The built output folder has the same DLL.
  - The code does what it says. Limited range: codes 1–254 (8-bit), 4–1019
    (10-bit), the same on all channels; 8-in-10 gives 4–1016. Full range is
    untouched, float outputs are never clamped (`swizzle_color`), and YCgCo
    keeps its own override.
- (5) **Encoding repair:**
  - `Needed()` returns false when compiled against API ≥ 377. Each renderer
    DLL compiles OutputEncodingRepair.cpp itself (it is not in the shared
    library), so the stable DLL keeps the repair and the Beta drops it.
  - A compile-time switch is the right mechanism: each DLL links exactly
    one libplacebo, and the evidence is clear (283 correct; the repair
    gives 306, a double encode).
  - The only other 7.360 assumption in our code is the extras' repair
    decision, which calls the same `Needed()`.
- (6) **BetaGpuTests (WARP):**
  - 40 → 40 and 980 → 980 on R, G and B (Bill's binary gives 64 and
    940/960/960); reserved codes 4 / 1019; the 8-in-10 grid.
  - The PRE_OUTPUT probe proves the 377 encoding order, and the test asserts
    `Needed()` is false.
