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

- Existing keys: `dv_tunnel_handoff`, `dv_tunnel_fixes`, `dv_trims`
- Planned keys: (none)
- Tiers: not tiered

## Fallbacks

- "Fallbacks engaged" entry: Signal: DV tunnel unpack on the CPU (<reason>)
- Behaviour: DV tunnel GPU unpack failure falls back to the CPU (today log-only).

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
- `DvCompareCapture.h` (diagnostic, off unless `VP_DV_COMPARE_PATH`): a RECORD file in that folder
  records a short labelled clip (raw frames + dv-frames.jsonl: per-picture DM, hold phase, the
  trim level / parameters, target nits) for `tools/dv_compare/dv_pair_compare.py`, which pairs an
  LLDV clip with a tunnel clip by picture fingerprint and scores PSNR / Delta E ITP / stops.
  Captured film frames are never committed.
  `DvCompareCaptureSink.h` wraps the recorder as a `VpDiagnostics::RenderedFrameCapture` (WP-17): the renderer
  calls it through its `RenderedFrameCaptures` (frame rendered, and `ToggleDvCompareCapture` -> `Invoke("dv_compare_capture")`).
- `DvTunnelPolicy.h` (WIP until its GPU check): no libplacebo plane deband on tunnel pictures (it
  ran on the 12-bit IPT planes before the decode and moved saturated near-zero channels by up to
  3.6 codes), and no VP deband there either (deband_vp also works on the source planes before
  the decode; "Fallbacks engaged: VP deband" when it is on). KNOWN GAP / queued: a post-decode DV
  deband (intensity in PQ before tone mapping).
  The libplacebo skip runs only with `dv_tunnel_fixes` on, so the Original pathway keeps Bill's
  libplacebo deband (integ/batch1 merge audit). Its DV ingress status suffix is a policy line, not
  a downgrade, so it is not listed under "Fallbacks engaged".

## Known issues / open items

- internal software DV pathway for synthetic RPU tests (item 60)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/HdrInference.cpp`
- `src/VideoProcessor-Lib/HdrInference.h`
- `src/VideoProcessor-Lib/HDRData.cpp`
- `src/VideoProcessor-Lib/ColorSpace.cpp`
- `src/VideoProcessor-Lib/EOTF.cpp`
- `src/VideoProcessor-Lib/DolbyVisionTunnelGate.h`
- `src/VideoProcessor-Lib/DolbyVisionTunnelHandoff.h`
- `src/VideoProcessor-Lib/vprenderer/GpuDolbyVisionTunnelUnpack.cpp`
- `src/VideoProcessor-Lib/vprenderer/GpuDolbyVisionTunnelUnpack.h`
- `src/VideoProcessor-Lib/vprenderer/Bt2020SignalController.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: (none) in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `dv_tunnel_handoff`, `dv_tunnel_fixes`, `dv_trims`, `dv_compare_capture_folder`.
- Tiers: not tiered.
- VP-V page, Input tab: not tiered (Always on: capture and timing have no quality steps, only paths (the toggles below).).
- Listed under the tier: `dv_tunnel_gpu_unpack`, `source_range`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `Signal: DV tunnel unpack on the CPU (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): DV tunnel unpack reference tests; HdrInference tests.
- Contract (formats, geometry, fallback entry): nets never run on raw DV IPT (net slots bypass on DV IPT input).
- Chain (total delay at the 4/3/2-frame budgets): chain gate: SDR / HDR10 / LLDV / tunnel.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
