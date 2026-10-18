# Bug hunt round 3, hunter B (picture): fixes

Branch `fix/h3-picture` from `integ/2026-10-17` (17e534c3). Findings:
`D:/codex/hunt3-scratch/b/findings.md`. Local only (no remote calls).

Commits: tests first with API stubs (fb064a38), then the fixes (9a36cfbb), then this report together with a shader macro rename (`DP_RGB`).

## Fixes

| Finding | Fix | Switch |
|---|---|---|
| F6 (M/H) pulse ghost on repeats of another picture | `DetailPulse.cpp`: the apply pass computes the low band of the picture it draws (`load_low`, shared with the measure pass, lines 131/171/365); the stored half band is gone. The CPU model already did this. | detail_pulse_stabilise |
| F1 (M) crossfade vs Extended Range | New `CrossfadeMix.{h,cpp}`. The path being left is rendered with the hooks before PRE_OUTPUT plus the extended-range late restore (`FromPathHooks`). A capture hook keeps its PRE_OUTPUT picture in rgba32f (`RenderFrom`, line 209). The new path is rendered with a mix hook straight after the late restore: `from*(1-w)+to*w` in the output signal domain (`RunMix`, line 147). Wired in LVR `RenderCrossfadeMix` (5408, used at 23040), which adds the 7.360 output-encoding repair when the mix needs it. libplacebo's mixer is only the fallback. | source_resolution_latch_fixes |
| Lead: the old path skipped the debander, pulse and texture restore | LVR `CrossfadeFromStages` (5371): debander slot `kSlotCrossfade` (new, `VpDebander::kSlots` 4), `DetailPulse::ApplyForeign` (902) and `TextureRestore::ApplyForeign` (586). These apply the frame's decision with the picture's own bands at its own geometry, read the tile weights/gains at the same picture position, and never advance the history. `SourceRebase` slot `kSlotCrossfade`. | latch fixes + each stage's switch |
| F3 (L) source-warp extras skip the fade | LVR 15932 / 16102 / 16128: each extra builds its own path being left from the native warped planes and goes through `RenderCrossfadeMix`. | latch fixes |
| F2 (M) ambient inert for SDR | `LibplaceboRenderParameters::SdrAmbientLift` (151) and `ApplySdrAmbientLift`. The BT.1886 black lift `V' = sV + o` (white fixed, code order and BTB/WTW kept) is applied through the frame's `pl_color_adjustment` (LVR 18087, 22732). Off, strength 0 and a dark room are bit-identical. sdr_black_nits for SDR stays unapplied by design; documented in CONFIGURATION.html and docs/VP-intent-interpreter-design.md section 4. | ambient_adapt (correctness) |
| F7 (L/M) texture restore repeat overwrites A's low band | `TextureRestore.cpp` 746: a repeat writes `lowRepeat`. | texture_restore |
| F4 (L/M) look-ahead domain | New `MeasureAhead(frame, ..., debander, params)` overload (DetailPulse.cpp 938) debands B in the look-ahead slot first; LVR 22982. | detail_pulse_stabilise |
| F9 (L/M) pulse skipped under subtitle move | The pulse runs on the composed one-plane RGB picture (F8 below), so its history runs on through cues. LVR 21396 passes the source's YCbCr system and levels. | detail_pulse_stabilise |
| F5 (L) Stage B pulses A undebanded | LVR 22948: the history advances on the debander's own draw of A. | detail_pulse_stabilise |
| F8 (L) silent no-op on RGB | One-plane RGB(A) is stabilised on Y = KrR+KgG+KbB in the source's codes, with the same delta on R, G and B (`PlaneKind`, 557; `code_at`). Unreadable planes are counted, logged once per reason, and the status reads "inactive (reason)" (LVR 21402; texture restore likewise). | detail_pulse_stabilise |

Precision: fp32 band math. The pulse output keeps the source plane format (at least 16 bits). The crossfade capture and mix are rgba32f. The source-resolution monitor still reads renderImage before any of these stages (DetailRestoreSourceResolutionInputOrderInTheRenderer passes).

## Tests (CPU, single process, TRX)

- Before the fixes (`D:/codex/hunt3-scratch/h3p/trx1`): 5 of 7 new or guard tests failed. Two of the five failed against the stubs as intended (crossfade mix and hook selection, SDR lift). The detail pulse ghost test also failed then, but on its own odd shift (fixed to (6, 4) before the stub commit), and it was not re-run before the fixes.
- After the fixes (`trx2`, rerun on the final build in `trx3`): 55/55 passed. This covers DetailPulse and TextureRestore (CPU), CrossfadeMix (CPU), CrossfadeMixIsMonotonicAndExactAtTheEnds, ExtendedRangeCarry (all), Ambient (all CPU, including AmbientSdrStrengthZeroChangesNothing and AmbientSdrLiftReachesTheCodes) and DetailRestoreConfig (including the renderer-order source check).
- Second run, same TRX folder: the SubtitleCutPaste, SourceResolutionLatch and LibplaceboRenderParameters classes, 121/121 passed.
- Final x64 Release solution build rc=0, stable and Beta renderers included (`D:/codex/hunt3-scratch/h3p/build5.log`).
- New CPU tests: DetailPulseRepeatOfAShiftedPictureHasNoGhost (stale-low ghost RMS about 1.07 codes; the correct repeat follows the moved picture exactly), the F7 block in TextureRestoreRestartsAtDiscontinuities, CrossfadeSignalMixIsExactMonotonicAndKeepsBtbWtw, CrossfadeRampThroughAHalfWeightFadeKeepsBtbWtw (libplacebo's mixer clips 60/60 BTB codes; VP's mix is exact with and without a LUT), CrossfadeFromPathTakesThePictureHooksOnly, AmbientSdrStrengthZeroChangesNothing and AmbientSdrLiftReachesTheCodes.

## GPU tests for Claude (not run here)

`/TestCaseFilter:"FullyQualifiedName~DetailPulseGpu|FullyQualifiedName~TextureRestoreGpu|FullyQualifiedName~CrossfadeGpu|FullyQualifiedName~AmbientSdrGpu"`

New:
- DetailPulseGpuRepeatOfAShiftedPictureMatchesModel (F6)
- DetailPulseGpuForeignGeometryKeepsTheHistory
- DetailPulseGpuRgbPictureMatchesLumaAndKeepsHistory (F8/F9)
- DetailPulseGpuNonLumaPlaneIsCountedAsInactive
- DetailPulseGpuLookAheadInTheDebandedDomain (F4)
- TextureRestoreGpuRepeatOfAnotherPictureKeepsTheReferences (F7)
- TextureRestoreGpuForeignGeometryKeepsTheReferences
- CrossfadeGpuKeepsBtbWtwOnARamp (F1, with and without LUT)
- AmbientSdrGpuLiftReachesTheOutputCodes (F2)

Regression: DetailPulseGpuMatchesModel and TextureRestoreGpuMatchesModel. The shaders compiled only as C++ strings here: the first GPU run is their first real compile.

## Not covered / open

- The LVR wiring (crossfade on A and on extras, look-ahead slot, Stage B domain, status) has no unit seam beyond the module tests. Check in the real app: a source-resolution change with Extended Range and a calibration LUT, subtitles with the move on, and the log lines "Source resolution crossfade: mixed in the output signal domain" and "Ambient adaptation (SDR input): BT.1886 black lift".
- Texture restore stays luma-plane only: under the subtitle move it reports "inactive" and its references restart at cue boundaries (texture_restore is off by default).
- The crossfade's old-path texture restore uses the new path's per-tile grain level N1 on a different-resolution picture (an approximation, fading out over the 1.5 s).
- The look-ahead debands both of B's planes (it only needs luma). Cost: one debander pass per new source frame, only while VP deband runs.
