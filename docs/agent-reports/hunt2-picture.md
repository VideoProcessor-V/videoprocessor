# Hunt 2 picture group: lead report (2026-10-17)

Branch `fix/hunt2-picture` on the fork (vmcpherson2012-dev/videoprocessor). It is based on
merge-v1.4.02 @ cd544707, plus `ci/github-actions`. Sub-branches merged: `fix/hunt2-picture-a`
(1b9a92fe), `-b` (b340a2fb) and `-c` (05dd922a, a698015a). The sub-agents were stopped before
they wrote their own reports, so this file covers all three.

## Fixes

### A: debander and grain (07 H1, L7; 15-intent C14, C15)
- `deband_vp_grain_guard on` (the default) is now the grain split. Each pixel is split into a
  low band (Gaussian sigma 6 px at half resolution) plus a residual. Only the low band is
  debanded. A band edge's sharp part is put back where the residual is coherent and
  consistent along the contour (structure tensor). Grain and coding noise pass untouched.
  - Code: `VpDebander.cpp:36-60`, `:110`, `:177`; `GrainPreserve.h:120-180`.
  - `veto` restores the 2026-10-17 spatial-share veto; `off` turns the guard off.
- The grain-scale pixel constants grow with plane height above 2160 lines
  (`GrainPreserve::GrainScale`; interpolator low band and split constants).
- `deband_vp_exclusive on` (new, default on): libplacebo's automatic deband is off while VP's
  grain-guarded debander runs (C14). An explicit `deband_strength` is kept.

### B: colour and Extended Range (08 #1-#4, #9; 15-intent C6)
- **08 #1:** history frames are relabelled with the prepared colour, so every temporal frame is
  converted once (`TemporalRenderer.h:145`; call at `LibplaceboVideoRenderer.cpp` around 21575).
  - Under libplacebo frame mixing only, `ExtendedRange::ForTemporal` turns the late restore
    (LUT / gamut mode) off (`ExtendedRange.cpp:515`, `Reason::TemporalMix`).
  - BTB is kept with deinterlacing. Under mixing, BTB is lost to libplacebo's mixer (a
    libplacebo limitation) but WTW is kept.
- **08 #2:** the smooth-motion and frame-mixer tooltips say VP's own motion is the BTB-safe
  path. This is logged once per session.
- **08 #3:** `inputIsSdr` (SDR EOTF or the DV-tunnel RGB fallback) is used at every SDR gate
  (`LibplaceboVideoRenderer.cpp:15510`).
- **08 #4:** DeckLink Rec.601 at any height now maps to BT.601 instead of throwing
  (`BlackMagicDeckLinkTranslate.cpp:463`). Magewell YUV601 above 576 lines follows the AVI
  InfoFrame colorimetry (`MagewellCaptureDevice.cpp:674`, `ColorSpace.cpp:237`).
  - **Capture overlap:** these two capture files were touched for the colour mapping only.
    Tell the capture group.
- C6 (HDR BTB/WTW) is recorded as a known gap in `docs/BUILD-STATUS.md` and
  `docs/VP-btb-wtw-answers.md`.

### C: sigma-delta, NLS, readings, re-base (09 L1/L2/L4; 07 M1/M2/M3/L4; C15, C19)
- **Residual bound:** now 0.5 + noise_amplitude, so the mean bias is gone
  (`Temporal sigma-delta.glsl:808`).
- **k clamp:** k is clamped to the codes that survive libplacebo-beta's output clamp
  (`:863`): limited range 2^(n-8) .. 255 x 2^(n-8) - 1, which is 1-254 at 8-bit, 4-1016 for
  8-in-10 and 4-1019 at 10-bit. Reserved codes are never emitted; BTB/WTW codes are kept.
  CPU model: `SigmaDeltaDomain.h`.
- No TO_NEXT vectors after a failed extra (L4).
- `nls_sampling catmull` (new, default): separable Catmull-Rom, widened by the local
  minification. `bilinear` restores the earlier sampling (`shaders/NLS.glsl:31`, `:84`;
  `NLSPlus.glsl`).
- `source_coding_fixes on` (new, default):
  - Effective bits are read by occupancy (an order-4 Hill number) on smooth-envelope windows
    (`SourceResolutionDetector.cpp:547`).
  - Blocking is detected at any grid period and phase.
  - Bands are found as two dominant levels, with noise or a ramp allowed.
- The re-base float fallback tries 32-bit before 16-bit and logs the fallback (`SourceRebase.cpp`).

### Lead sweep (cross-area)
1. **effectiveBits to auto deband.** The new occupancy reading puts 8-bit content in a 10-bit
   signal at about 8.8 bits. `VpDebander::Strength` then gave 3.4 codes, which is below the real
   4-code step.
   - Fix: with `source_coding_fixes on`, the step is taken at floor(bits + 0.1), and readings
     within 0.5 bit of the signal depth count as that depth (`VpDebander.cpp:327`).
   - Test: `VpDebanderStrengthSnapsOccupancyReadingToContentLevels`.
2. **deband_vp_exclusive vs temporal paths.** VP's debander is idle while smooth motion or
   deinterlacing runs, so with libplacebo's deband yielded those frames had no deband at all.
   They now take libplacebo's deband as configured (`LibplaceboVideoRenderer.cpp:21404`).
3. **Grain split on noisy bands.** A's own test failed: soft 2-code noise kept 37 % of the band
   fundamental. The consistency gate (|mean| / RMS along the contour) closed once noise reached
   half a step, so the steps' sharp part passed through with the noise.
   - Fix: the gate is now the mean's significance, |mean| / standard error at 1-2 SE
     (`GrainPreserve::SplitConsistency`). The shader and the CPU model are changed together.
   - Result: noisy bands keep 3-18 % of the band fundamental. Grain keeps 0.96-1.01x of its RMS.
4. **Coding grids.** C's last step used min(gridH, gridV), which missed 2x-upscaled grids
   because a coded gradient blocks only across the gradient. C's test failed at ratio 0.5.
   - Fix: blockiness is now the stronger axis. A quantisation-staircase axis (median step about
     0, every step the same sign) counts as bands. The side-bin tolerance is now a half (was a
     quarter), to allow for an upscaler's lobes.
   - `SourceResolutionDetectorTests`: 34/34 pass.
5. **ER BTB vs the sigma-delta clamp.** No conflict: limited range keeps footroom and headroom
   codes, and only the timing codes are reserved.
6. **Deband order vs ER hooks.** No conflict: VP deband runs on the source planes before
   libplacebo's render, with no value clamps, and ER's carry/restore run inside the render.
7. **Build errors:**
   - `std::clamp` in a C++14 test project.
   - A raw newline inside B's tooltip string literal (`ConfigEditorWindow.cpp`, C2001).
   - Both are fixed.
8. **Shared parser, Config and fingerprint conflicts:** resolved as a union. All four new keys
   have parser, validation, fingerprint, change-log, dump and Config controls.

## Switches (all have Config controls)
| Key | Values | Default | Off / old behaviour |
|---|---|---|---|
| `deband_vp_grain_guard` | on, veto, off | on (split) | `veto` |
| `deband_vp_exclusive` | on, off | on | `off` |
| `nls_sampling` | catmull, bilinear | catmull | `bilinear` |
| `source_coding_fixes` | on, off | on | `off` (it also selects the unsnapped Strength) |

## Tests
- **CPU (A):** VpDebanderSplitKeepsGrain, VpDebanderSplitDebandsNoisyBands,
  VpDebanderSplitKeepsStructureAndDebandsCleanBands, GrainSplitConstantsScaleWithHeight.
- **CPU (B):** ForTemporalKeepsPerFrameModesAndDropsLateRestoreUnderMixing,
  HistoryFramesTakeThePreparedColourWhenGiven,
  DeinterlacedHistoryIsConvertedOnceWithThePreparedColour,
  Rec601FlagAndAviColorimetryDecideTheMatrix.
- **CPU (C):** SigmaDeltaCodeLimitsAreThePermissibleCodes,
  SigmaDeltaMeanIsUnbiasedAtAnyDitherAmplitude, SigmaDeltaNeverEmitsReservedCodes,
  CodingFixes* (4), plus ConfigFileTests cases.
- **CPU (lead):** VpDebanderStrengthSnapsOccupancyReadingToContentLevels.
- **Renderer-DLL test:** Hunt2PictureKeysReachTheSettings.
- **Local run** (VpCiNoMagewell=true): 236 targeted CPU tests.
  - All pass after the sweep fixes.
  - Exception: Hunt2PictureKeysReachTheSettings, which needs the renderer DLL (not built locally).

## GPU tests for Claude
Not run here. `/TestCaseFilter:"Name=VpDebanderGrainSplitDebandsNoisyBands|Name=VpDebanderSmoothsBandsKeepsTextureAndHoldsAlongTheVectors|Name=DeinterlacedExtendedRangeConvertsEachFrameOnce|Name=Hunt2PictureKeysReachTheSettings"`

Also run the sigma-delta and NLS shader passes in the chain gate (Beta 377). The split
shader's consistency gate changed in the sweep, so run VpDebanderGrainSplitDebandsNoisyBands
on the GPU.

## Deferred
- 09 L3: the Stable-only 0.13 snap bias stays parked (the owner focuses on Beta 377).
- 09 L5, 08 #5/#6/#7/#8, and 07 L1-L3/L5-L8 except L4/L7.
- C6: design a VP-owned sign-preserving PQ-to-SDR hook. Recorded as a gap only.
- C5 (LED compensation gain clips BTB) belongs to the LED group.
- The DV-tunnel and capture overlap above should be checked with the capture group.

## CI
- **Run 39068902169** (https://github.com/vmcpherson2012-dev/videoprocessor/actions/runs/39068902169)
  at 14e6dac5:
  - Checkout, Qt, toolchain and the **x64 Release build (whole solution) passed**.
  - The CPU tests were running when GitHub returned "Your account was suspended" (HTTP 403)
    for the fork account. No result could be read, and nothing more can be pushed.
- **Run 39066138152** (93467197) failed to build. Both causes are fixed:
  - `std::clamp` in the C++14 test project;
  - the C2001 raw newline in the tooltip string.
- Local verification after the suspension (x64 Release, VpCiNoMagewell=true, VPRenderer + Test built):
  245/245 targeted CPU tests pass, including Hunt2PictureKeysReachTheSettings. Config editor
  (Qt) tests were not run locally (no Qt on this laptop); the Config project compiled on CI.
- Commits after 14e6dac5 (this report) are local only, in `D:/codex/videoprocessor/hunt2-picture`,
  until the account is restored.
