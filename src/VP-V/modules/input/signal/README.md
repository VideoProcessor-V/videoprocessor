# Signal (`input.signal`)

Unit: **input** | side: input | order: 20 | status: **built** | hook point: host

## Purpose / why

Identify the signal (SDR / HDR10 / LLDV / DV tunnel), its metadata and its colorimetry, and unpack the DV tunnel, so every later stage knows what the pixels mean.

## Contract

- Input: capture state + InfoFrames / DV RPU
- Output: signal descriptor (transfer, primaries, range, HDR metadata, DV state) attached to each frame
- Geometry: n/a
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `newlldv` (Bill's key, enables VP's inference), `newlldv_sdr_veto`, `dv_tunnel_handoff` (all `[general]`), `dv_tunnel_gpu_unpack`, `dv_tunnel_fixes`, `dv_compare_capture_folder` (renderer profile)
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entries: `Dolby Vision GPU unpack: CPU unpack for the session: <error>`, `DV metadata: <held | static>`
- Behaviour: a failed DV tunnel GPU unpack switches the session to the CPU unpack (visible); a rejected DM is held briefly, then static metadata, then black.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `input.capture`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- DV tunnel unpack reference tests
- HdrInference tests

Contract tests:
- nets never run on raw DV IPT (net slots bypass on DV IPT input)

Chain tests:
- chain gate: SDR / HDR10 / LLDV / tunnel

## DV metadata additions (feat/dv-l8, 2026-10-24)

- `DvSignalLevels.h`: L9 (source primary index) and L10 (custom target displays) parsed into the
  DM, so L8 trims can be resolved (`colour/sdr_tone_shape/DvTrimSelection.h`).
- `DvMetadataHold.h` (no silent fallbacks): a picture whose DM is rejected keeps the previous DM
  only for a bounded hold (0.25 s at the measured rate). After it: TRANSPORT damage (CRC / length
  / continuation failures, truncation) shows static/base metadata (matrices, source range,
  L5/L6/L9/L10/L11 kept; L1 and trims dropped), also past the ~1 s loss limit while headers stay
  CRC-valid, capped at the SDR-exit window (2 s), then black; an UNSUPPORTED / MALFORMED body
  (e.g. a same-mode title change VP cannot decode) is black once the hold expires - never the
  old title's matrices; no valid header: black at the loss limit. Every rejected picture is counted ("DV metadata: N rejected pictures" on the DV status
  line); "Fallbacks engaged: DV metadata" while held or static; behind `dv_tunnel_fixes`.
- `DvCompareCapture.h` (diagnostic, off unless `VP_DV_COMPARE_PATH` or `dv_compare_capture_folder`; the
  `dv_compare_capture` shortcut toggles a recording): a RECORD file in that folder
  records a short labelled clip (raw frames + dv-frames.jsonl: per-picture DM, hold phase, the
  trim level / parameters, target nits) for `tools/dv_compare/dv_pair_compare.py`, which pairs an
  LLDV clip with a tunnel clip by picture fingerprint and scores PSNR / Delta E ITP / stops.
  Captured film frames are never committed.
- `DvTunnelPolicy.h` (WIP until its GPU check): no libplacebo plane deband on tunnel pictures (it
  ran on the 12-bit IPT planes before the decode and moved saturated near-zero channels by up to
  3.6 codes), and no VP deband there either (deband_vp also works on the source planes before
  the decode; "Fallbacks engaged: VP deband" when it is on). KNOWN GAP / queued: a post-decode DV
  deband (intensity in PQ before tone mapping).
  The libplacebo skip runs only with `dv_tunnel_fixes` on, so the Original pathway keeps Bill's
  libplacebo deband (integ/batch1 merge audit). Its DV ingress status suffix is a policy line, not
  a downgrade, so it is not listed under "Fallbacks engaged".

## How the code works

Checked against the code 2026-10-25 (WP-14 / M2 step 14, `feat/m2-dv`).

**Public API**
- `HdrInference.h/.cpp` - `HdrInferenceTracker` (newlldv: BT.2020, 10/12-bit (8-bit with an LLDV
  indication), no HDR InfoFrame -> PQ after a confirmation time; the SDR veto armed by
  per-shot evidence and committed at a cut), `EvaluateHdrInferenceEligibility`,
  `MeasureHdrVetoGrid`, `SampleHdrVetoGrid`, `HdrVetoCutDetector`.
- `HdrInferenceSignalBuilder.h` - `BuildHdrInferenceSignalFromState` (the GUI's signal builder,
  shared with the tests).
- `HdrVetoCaptureGate.h` - moves the veto switch onto the capture thread and the cut frame.
- `DolbyVisionTunnelGate.h`, `DolbyVisionTunnelHandoff.h` - which captured signals the DV tunnel
  formatter may claim (`dv_tunnel_handoff`), and the hand-off of an RGB LLDV signal to the tunnel.
- `GpuDolbyVisionTunnelUnpack.h/.cpp` - the tunnel's RGB8-in-r210 container unpacked to 12-bit
  4:2:2 planes on the GPU (one compute pass); `DolbyVisionTunnelUnpackReference.h` is its exact CPU
  transcription (also used by Bill's `AnalysisFrame.cpp` / `AnalysisLumaSource.cpp` through the
  forwarding header).
- `DvGpuUnpackSession.h` (WP-14) - the renderer's glue around that unpack (per-device object,
  logs, the one-time session fallback).
- `DvSignalLevels.h` (L9 / L10 parsing), `DvMetadataHold.h` (bounded hold, then static
  metadata), `DvCompareCapture.h` (diagnostic recorder), `DvTunnelPolicy.h` (which plane stages
  may touch the raw tunnel planes).

**Data flow.** In: the capture `VideoState` (colorimetry, EOTF, InfoFrames, bit depth, DV VSIF) and,
for the tunnel, the raw r210 / R10b / R10l frame. Out: the signal decision the GUI pushes to the
renderer (PQ inferred or not; tunnel claimed or handed off), the tunnel's DM per picture
(parsed by Bill's `CR210DolbyVisionTunnelFormatter`, which calls `DvSignalLevels` and
`DvMetadataHold::Tracker` from inside his file) and, with `dv_tunnel_gpu_unpack`, the tunnel planes
unpacked on the GPU instead of by the CPU formatter.

**Threading.** `HdrInferenceTracker`, the tunnel gate and hand-off: the GUI thread
(`VideoProcessorDlg`). `HdrVetoCaptureGate`: armed on the GUI thread, observed on the capture
thread (pure apart from the state pointer). DV metadata parsing and the hold: the conversion
thread (inside the formatter). `GpuDolbyVisionTunnelUnpack` / `DvGpuUnpackSession`: the render
thread; the session-wide "failed" and "wanted" flags are atomics in the host, read by the frame
preparation on the conversion thread. `DvCompareCapture::Recorder`: render thread plus a mutex
for the GUI toggle.

**Config keys the code reads**
- `[general]` (passed to the GUI as switches, `ConfigBoolSwitches.h`): `newlldv` (Bill's key; it
  enables VP's `HdrInferenceTracker`, which replaced his heuristic in his dialog - see the
  findings), `newlldv_sdr_veto`, `dv_tunnel_handoff`.
  These GUI-side switches are outside the renderer's Original pathway (`OriginalPathway.h`
  lists renderer keys only); they get overrides when WP-16/18 move the signal decisions into VP-V.
- Renderer profile sections (`[vprenderer.*]`, host settings reader until WP-18):
  `dv_tunnel_gpu_unpack` (default on), `dv_tunnel_fixes` (default on; switches the formatter's
  bug-hunt fixes and the metadata hold), `dv_compare_capture_folder` ("" = `VP_DV_COMPARE_PATH`
  or off).
- Environment (diagnostics only): `VP_DV_COMPARE_PATH`, `VP_TEST_DV_UNPACK_FAIL`.

**Fallbacks a user sees**
- `Dolby Vision GPU unpack: CPU unpack for the session: <error>` (the GPU unpack failed on a
  rendered frame; released when the device is recreated).
- `DV metadata: packet rejected: the previous picture's DM held` / `...: static metadata (no
  scene L1, no trims)`.
- `Dolby Vision metadata`, `Dolby Vision source range`, `Dolby Vision scene metadata` (host,
  tunnel pictures with invalid DM / source range / no L1); all released outside a tunnel.
- The SDR veto and the inference itself are decisions, not fallbacks (shown on the HDR status).

**To lift the module:** the DV glue patch applied (`DvHostStage`, `DvGpuUnpackSession`); the DV
code inside Bill's formatter (L9/L10 parse, the hold tracker) turned into a hook his formatter
calls (a patch-set item to offer upstream); the HDR inference and tunnel decisions out of
`VideoProcessorDlg.cpp` into a VP-V signal adapter (WP-16/18) with an either/or setting against
Bill's own newlldv heuristic; the settings readers in VP-V's config (WP-18).

**Test it standalone** (CPU):

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Logger:trx /Tests:HdrInferenceTests,DvIncomingHdrModeTests,DvTunnelHandoffTests,DvTunnelLoopbackTests,DolbyVisionTunnelGpuUnpackReferenceTests,DvSignalL8HoldTests,DvSyntheticCorpusTests,DvHostStageTests
```

GPU (not run by agents): `GpuDolbyVisionUnpack*` in `LibplaceboLutParserTests`, the DV autotest /
loopback (`test/dv-tunnel-auto`), the renderer harness with `VP_TEST_DV_UNPACK_FAIL=1`.

## Known issues / open items

- internal software DV pathway for synthetic RPU tests (item 60)

## Where the code lives (live file list)

Code location: **partly migrated** (M2 step 14). Bill's files (`HDRData.cpp`, `ColorSpace.cpp`, `EOTF.cpp`, `vprenderer/Bt2020SignalController.h`; present in origin/v1.4.02-beta) are code this slot uses, not owned, and never move. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

VP files (in this folder):

- `src/VP-V/modules/input/signal/HdrInference.h`
- `src/VP-V/modules/input/signal/HdrInference.cpp`
- `src/VP-V/modules/input/signal/HdrInferenceSignalBuilder.h`
- `src/VP-V/modules/input/signal/HdrVetoCaptureGate.h`
- `src/VP-V/modules/input/signal/DolbyVisionTunnelGate.h`
- `src/VP-V/modules/input/signal/DolbyVisionTunnelHandoff.h`
- `src/VP-V/modules/input/signal/GpuDolbyVisionTunnelUnpack.h`
- `src/VP-V/modules/input/signal/GpuDolbyVisionTunnelUnpack.cpp`
- `src/VP-V/modules/input/signal/DolbyVisionTunnelUnpackReference.h`
- `src/VP-V/modules/input/signal/DvSignalLevels.h`
- `src/VP-V/modules/input/signal/DvMetadataHold.h`
- `src/VP-V/modules/input/signal/DvCompareCapture.h`
- `src/VP-V/modules/input/signal/DvTunnelPolicy.h`

Bill's files used (stay):

- `src/VideoProcessor-Lib/HDRData.cpp`
- `src/VideoProcessor-Lib/ColorSpace.cpp`
- `src/VideoProcessor-Lib/EOTF.cpp`
- `src/VideoProcessor-Lib/vprenderer/Bt2020SignalController.h`

Forwarding headers (removed in M2 step 0) stay at the old paths of the seven moved headers for the renderer host and Bill's files (`VideoProcessorDlg.cpp`/`.h`, `ConfigFileTests.cpp`, `AnalysisFrame.cpp`, `AnalysisLumaSource.cpp`, `LibplaceboLutParserTests.cpp`, `DolbyVisionTunnelFormatterTests.cpp`); listed in the MANIFEST.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `newlldv`, `newlldv_sdr_veto`, `dv_tunnel_handoff`, `dv_tunnel_gpu_unpack`, `dv_tunnel_fixes`, `dv_compare_capture_folder`.
- Tiers: not tiered.
- VP-V page, Input tab: not tiered (Always on: capture and timing have no quality steps, only paths (the toggles below).).
- Listed under the tier: `dv_tunnel_gpu_unpack`, `source_range`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `Dolby Vision GPU unpack: CPU unpack for the session: <error> | DV metadata: <held | static>`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): DV tunnel unpack reference tests; HdrInference tests.
- Contract (formats, geometry, fallback entry): nets never run on raw DV IPT (net slots bypass on DV IPT input).
- Chain (total delay at the 4/3/2-frame budgets): chain gate: SDR / HDR10 / LLDV / tunnel.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
