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

- Existing keys: `sdr_tone_shape`, `dv_trims`, `dv_tone_mapping` (feat/dv-l8: `libplacebo` | `dolby_style`, default `libplacebo`)
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: SDR tone shape: not applied (shader did not load)
- Behaviour: Shader failure skips it.

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

## Known issues / open items

- KNOWN GAP (needs a live capture, after the deploy / with the DeckLink): the L8 / L9 / L10
  container layout in the tunnel is assumed (16-bit containers, like L1/L2) and the predefined
  CM v4.0 target ids beyond index 1 are not assumed; verify both on a real CM v4.0 title (the
  formatter logs the first L8 block per title with its raw bytes; dv-frames.jsonl records L10).
- GPU 2026-10-24: the shader matches its C++ references on the NVIDIA GPU (L2, L8 and ICtCp
  paths: DolbyVisionTrimShaderMatchesItsReference, DolbyVisionTrimShaderL8AndIctcpPathsMatchTheirReferences).
- OPEN (WIP, dv_tone_mapping stays default libplacebo): on the synthetic policy scenes the
  Dolby-style mode renders about one stop darker than the L1 intent model with crushed shadows
  (wedge: 200 -> 98.5, 20 -> 9.7, 5 -> ~2, 1 -> 0.21 nits at a 100-nit target; bt2390: 100, 20,
  5, 1). libplacebo's own LUT generation reproduces the curve exactly on the CPU
  (LibplaceboGeneratesTheCurve), and changing the mid adaptation 0.7 -> 1.0 did not change the
  render, so the inputs libplacebo hands the function in the renderer (scene min/avg/max, target)
  or the swap itself are the suspects: log the effective tone parameters per frame next.

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/SdrToneShape.h`
- `src/VideoProcessor-Lib/vprenderer/DolbyVisionTrims.h`
- `shaders/SDR tone shape.glsl`
- `shaders/DV trims.glsl`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0002 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `sdr_tone_shape`, `dv_trims`, `dv_tone_mapping`.
- Tiers: not tiered.
- VP-V page, Colour tab: not tiered (libplacebo does the colour in every tier; the cheaper combined HDR->SDR LUT for Classic/Low is not yet implemented.).
- Listed under the tier: `dv_trims`, `sdr_tone_shape`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#6-colour](../../../../../docs/settings/VP-settings-reference.md#6-colour).
- Fallback entry ("Fallbacks engaged"): `SDR tone shape: not applied (shader did not load)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): SdrToneShape tests; DolbyVisionTrims tests.
- Contract (formats, geometry, fallback entry): output encoding repair present when a PRE_OUTPUT hook runs.
- Chain (total delay at the 4/3/2-frame budgets): MISSING.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
