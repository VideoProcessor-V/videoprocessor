# SDR tone shape + DV trims (`colour.sdr_tone_shape`)

Unit: **colour** | side: output | order: 200 | status: **built** | hook point: PRE_OUTPUT

## Purpose / why

Delta 13's SDR tone shape and headroom, and the (non-certified) DV trims; first at PRE_OUTPUT.

## Contract

- Input: target-light picture at output size
- Output: same domain
- Geometry: output size
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `sdr_tone_shape`, `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, `sdr_contrast_recovery`, `dv_trims`, `dv_tone_mapping` (feat/dv-l8: `libplacebo` | `dolby_style`, default `libplacebo`)
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entries: `SDR tone shape: not applied (<why>)` (call site in WP-4's patch), `DV tone mapping: <why>: bt2390[ + trims]`, `Dolby Vision trims: <state>`
- Behaviour: a shader failure skips the stage; the Dolby-style curve falls back to bt2390 (+ the luminance trims).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.tone_gamut`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- SdrToneShape tests
- DolbyVisionTrims tests
- DvTrimSelectionTests, DvDolbyStyleTests, DvHostStageTests

Contract tests:
- output encoding repair present when a PRE_OUTPUT hook runs

Chain tests:
- (none yet)

## DV trims with L8 and the Dolby-style DV tone mapping (feat/dv-l8, 2026-10-24)

Both are NON-CERTIFIED APPROXIMATIONS (not Dolby display management); the certified reference is
LLDV from the Apple TV, compared with `tools/dv_compare/dv_pair_compare.py`.

- `DvTrimSelection.h` (behind `dv_trims`): the trim level is **L8** when the DM carries usable L8
  trims whose known targets cover the display target at least as well as L2's (bracketed beats
  one-sided, else the nearer target), else **L2**, else none ("DV trims: L8 / L2 / none" on the DV
  status line; "Fallbacks engaged: Dolby Vision trims" when L8 is present but every L8 target is
  unknown and there is no L2). L8 targets come from L10 (custom targets, parsed in
  `input/signal/DvSignalLevels.h`) or the predefined CM v4.0 list: libdovi names the preset ids
  that carry no L10 (1, 16, 18, 21, 27, 28, 37, 38, 42, 48, 49) but publishes no luminance for
  them, so only index 1 = 100 nits is assumed (unverified from public sources). Selection / interpolation by
  the tone-mapping target is exactly L2's; below every target the lowest-target trim is used
  unchanged. L8 extras: mid contrast and clip trim (bends on the trimmed PQ value: black and peak
  fixed, monotonic), per-hue saturation and hue vectors (R/Y/G/C/B/M centres). Neutral extras are
  the L2 arithmetic bit for bit (`dv_ext = 0` in `shaders/DV trims.glsl`).
- `DvDolbyStyle.h` (`dv_tone_mapping: dolby_style`): for DV pictures, a VP tone curve set as a
  custom libplacebo tone-mapping function (PQ scaling): the three-anchor rational curve of
  ST 2094-10 Annex B, in PQ, through (scene min -> target min), (scene mid -> target mid),
  (scene max -> target max) from L1 and the user's target; no expansion (a scene that fits is
  untouched). The trims then run in ICtCp (`dv_space = 1`). Status: "DV tone mapping: Dolby-style
  (approx.)". When it cannot run (calibration LUT, unsupported output transfer, the slot off):
  "Fallbacks engaged: DV tone mapping" and bt2390 + the luminance trims.
- `DvDolbyStyle::ReferenceConditions`: per DV picture, the trim level, the selected /
  interpolated trim target (min / max nits), the mastering display (L6, else the DM source range),
  L9 primaries index, L11 content type + reference mode, and which curve ran - the input a later
  CAM16 stage needs so it compensates only the difference to the real screen and room.

Public sources (structure only; no Dolby code or confidential material):
- SMPTE ST 2094-1 / ST 2094-10 (Dynamic Metadata for Color Volume Transform, Application #1):
  the L1 / L2 metadata semantics and the Annex B tone curve form; its open implementation in
  libplacebo (`src/tone_mapping.c`, `st2094_10`, `st2094_pick_knee`) for structure.
- ITU-R BT.2100 (ICtCp, PQ) and ITU-R BT.2124 (Delta E ITP).
- ETSI GS CCM 001 (the HDMI DV tunnel's dm_metadata() and extension blocks).
- Dolby's public "Dolby Vision" white papers (display management in IPT-PQ / ICtCp, intensity
  tone mapping with separate chroma handling, trims per target display).
- libdovi / dovi_tool (quietvoid, MIT): the RPU field definitions of L1, L2, L8, L9, L10, L11.
- libplacebo's DV / IPT colour code (`src/shaders/colorspace.c`) for the IPT-stage application.
The interpretation of L8's mid contrast, clip trim and vectors, the predefined target list beyond
index 1, and the curve's mid adaptation constants are VP assumptions, to calibrate against LLDV.

## How the code works

Checked against the code 2026-10-25 (WP-14 / M2 step 6, `feat/m2-dv`).

**Public API** (all header-only):
- `SdrToneShape.h` - `SdrToneShape::Settings` (knee on/off, headroom, knee start, knee strength,
  recovery), `Active`, `Map` (the CPU reference of `shaders/SDR tone shape.glsl`),
  `EffectiveStrength`, `EffectiveRecovery`.
- `DolbyVisionTrims.h` - the L2 trim arithmetic (`ApplyPixel`, `PqFromNits`, `LuminanceWeights`),
  the CPU reference of `shaders/DV trims.glsl`.
- `DvTrimSelection.h` - `Select(dm, displayPeakNits)`: L8, else L2, else none; `ShaderParameters`,
  `Describe`, `StatusText`.
- `DvDolbyStyle.h` - the Dolby-style curve (`Build`, `ForDm`, `Function()` as a libplacebo
  tone-mapping function), ICtCp helpers, `CannotRun`, `ShaderParameters`, `ToneSwitch`,
  `StatusText`, `Conditions` (the `ReferenceConditions` record).
- `DvHostStage.h` (WP-14) - the renderer's DV glue in one object: `OnTunnelState`,
  `IngressSuffix`, `SkipVpDeband`, `OnPreOutput`, `SkipPlaneDeband`, `ReleaseHooks`,
  `ToggleCompareCapture`, `ReferenceConditions()`. The host calls it once per hook point
  ([m2-dv-callsites.md](../../../../../docs/design/m2-dv-callsites.md)); until that patch is
  applied the same code still runs inline in `LibplaceboVideoRenderer.cpp`.

**Data flow.** In: the target-light picture at PRE_OUTPUT (after libplacebo's tone and gamut
mapping and the encode to the output signal), the picture's DV DM (`DolbyVisionTunnelFrameMetadata`
from Bill's tunnel formatter), the output target (transfer, primaries, peak, calibration LUT).
Out: up to two PRE_OUTPUT hooks (`SDR tone shape.glsl` for SDR in/out; `DV trims.glsl` for DV
tunnel pictures) with their parameters, and for `dv_tone_mapping: dolby_style` libplacebo's
tone-mapping function swapped for the Dolby-style curve on DV pictures (`ToneSwitch` restores the
configured curve otherwise). The DV hook runs after the SDR shape and before the LED measurement
and compensation; the output encoding repair (`output.dither`) follows any PRE_OUTPUT hook.

**Threading.** Everything runs on the render thread (`RenderLocked`). Settings are copied into the
frame's `activeSettings` before the render; no locks here. The comparison capture recorder that
`DvHostStage` owns has its own mutex (its toggle arrives from the GUI through the plugin API).

**Config keys the code reads** (renderer profile sections, `[vprenderer.*]`, read by the host's
settings reader, `LibplaceboVideoRenderer.cpp`, until WP-18):
`sdr_tone_shape` (off | knee), `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`,
`sdr_contrast_recovery` (auto | off | 0-2), `dv_trims` (on | off, default on), `dv_tone_mapping`
(libplacebo | dolby_style, default libplacebo). The Original pathway sets `sdr_tone_shape off`,
`dv_trims off`, `dv_tone_mapping libplacebo`.

**Fallbacks a user sees** ("Fallbacks engaged", Ctrl+I / Config status / Ctrl+Alt+D, one-time notice):
- `DV tone mapping: <why>: bt2390[ + trims]` - Dolby-style requested on a DV picture but it cannot
  run (calibration LUT, unsupported output transfer, the slot off, the trims shader missing).
- `Dolby Vision trims: <state>` - trims on, a DV picture with DM, but the trim cannot act (a
  calibration LUT, an output transfer other than PQ / gamma / BT.1886 / sRGB, the shader missing,
  or L8 present with every L8 target unknown and no L2). "Neutral" (no trim for this picture) is
  not a fallback.
- `SDR tone shape: not applied (<why>)` - the shader did not load. TODAY LOG-ONLY in the host: the
  builder exists (`FallbackCatalog::SdrToneShape`) and its call site is in WP-4's
  `silent-fallbacks-callsites.patch` (not yet applied on this branch).
- The tunnel deband skips appear as `VP deband: off on the DV tunnel planes ...` (owned by
  `DvHostStage`, entry id `VP deband`) and on the DV ingress line.

**To lift the module** (a stand-alone slot): the host patch applied (all DV calls through
`DvHostStage`); the SDR tone shape block moved into the same adapter (it is still inline host
code); the settings readers moved into VP-V's config section (WP-18); the shaders stay in
`shaders/` (deploy layout).

**Test it standalone** (CPU, single-threaded, from the solution root after a Release build):

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Logger:trx /Tests:SdrToneShapeWtwTests,DolbyVisionTrimsTests,DvTrimSelectionTests,DvDolbyStyleTests,DvHostStageTests
```

GPU (not run by agents): `DolbyVisionTrimShaderMatchesItsReference`,
`DolbyVisionTrimShaderL8AndIctcpPathsMatchTheirReferences`, the DV synthetic suite.

## Known issues / open items

- KNOWN GAP (needs a live capture, after the deploy / with the DeckLink): the L8 / L9 / L10
  container layout in the tunnel is assumed (16-bit containers, like L1/L2) and the predefined
  CM v4.0 target ids beyond index 1 are not assumed; verify both on a real CM v4.0 title (the
  formatter logs the first L8 block per title with its raw bytes; dv-frames.jsonl records L10).
- GPU 2026-10-24: the shader matches its C++ references on the NVIDIA GPU (L2, L8 and ICtCp
  paths: DolbyVisionTrimShaderMatchesItsReference, DolbyVisionTrimShaderL8AndIctcpPathsMatchTheirReferences).
- RESOLVED 2026-10-25 (fix/dv-dolby-style-dark; GPU confirmation on integ/batch1, RTX 3050 Ti:
  DvSynthGpuRendersJobs with the regenerated jobs, trims off = on the curve within 1%: dim scene
  1 -> 1.12, 20 -> 16.6, 200 -> 100 nits; with the corpus L2 the picture is 1.28-1.45x the curve,
  to be judged against LLDV). dv_tone_mapping stays default libplacebo. Diagnosis (CPU and
  the captures): the "one stop dark" Dolby-style report
  (200 -> 98.5, 20 -> 9.7, 5 -> ~2, 1 -> 0.21 nits at 100 nits) was a broken test, not the mode.
  The hand-edited Dolby-style jobs had a REAL new line after `tone_mapping: bt2390`, so the
  harness ran each job without dv_tone_mapping, sdr_target_nits, the P3 target and the 2.2 output:
  every capture says "DV tone mapping: libplacebo (bt2390)", 203 nits, Rec.709, sRGB. bt2390 at
  203 nits encoded sRGB and read as a 100-nit 2.2 picture gives 98.6 / 9.73 / 2.04 / 0.207 -
  the reported numbers to three digits (and why the mid adaptation changed nothing). The ICtCp
  trims are not a second darkening: in ST 2094-10 / CM the trim is the target-specific
  adjustment of the tone-mapped intensity, which is where they act (relative to the target
  peak), and the corpus's L2 brightens slightly (CurveThenIctcpTrimsKeepTheWedgeOnTheCurve).
  run_dv_synth.py now writes the Dolby-style jobs (`jobs <root> <file> -dolby_`) and
  DvSynthGpuRendersJobs rejects a jobs file with non-job lines and fails any capture whose
  reported tone mapping / target nits / primaries / output gamma differ from the job's.

## Where the code lives (live file list)

Code location: **partly migrated** (M2 step 6; the shaders stay in `shaders/`, the runtime layout). The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/colour/sdr_tone_shape/SdrToneShape.h`
- `src/VP-V/modules/colour/sdr_tone_shape/DolbyVisionTrims.h`
- `src/VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h`
- `src/VP-V/modules/colour/sdr_tone_shape/DvDolbyStyle.h`
- `shaders/SDR tone shape.glsl`
- `shaders/DV trims.glsl`

Forwarding headers (removed in M2 step 0): `src/VideoProcessor-Lib/vprenderer/SdrToneShape.h` and `DolbyVisionTrims.h`, kept for the renderer host and Bill's `LibplaceboLutParserTests.cpp`.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0002 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `sdr_tone_shape`, `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, `sdr_contrast_recovery`, `dv_trims`, `dv_tone_mapping`.
- Tiers: not tiered.
- VP-V page, Colour tab: not tiered (libplacebo does the colour in every tier; the cheaper combined HDR->SDR LUT for Classic/Low is not yet implemented.).
- Listed under the tier: `dv_trims`, `sdr_tone_shape`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#6-colour](../../../../../docs/settings/VP-settings-reference.md#6-colour).
- Fallback entry ("Fallbacks engaged"): `DV tone mapping: <why>: bt2390[ + trims] | Dolby Vision trims: <state> | SDR tone shape: not applied (<why>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): SdrToneShape tests; DolbyVisionTrims tests.
- Contract (formats, geometry, fallback entry): output encoding repair present when a PRE_OUTPUT hook runs.
- Chain (total delay at the 4/3/2-frame budgets): MISSING.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
