# Agent report: DV tunnel automated test system (2026-10-17/18)

Branch `test/dv-tunnel-auto` from local `integ/2026-10-17` (ca7e3670), worktree
`D:\codex\videoprocessor\dv-autotest`. Local only (no remote calls). Full design and
owner steps: `docs/VP-dv-tunnel-autotest.md`; owner page for the Apple TV check:
`docs/dv/LLDV-vs-tunnel-check.md`.

## What was built

- **Loopback harness (primary)**: VPV packer (`dv_loopback/DvTunnelPacker.h`), capture
  containers, deterministic stamped pattern + metadata sequence, an analyzer running VP's
  own tunnel formatter and the renderer's presentation rule, a simulated loop (in-memory
  capture card with latency/miss/repeat/bit-error/range faults), latency/drop/repeat/
  metadata/pixel reports; hardware sender + read-only probe tool
  (`VideoProcessorDvLoopback.exe --probe | --send | --restore-sdr`).
- **Independent reference encoder** (tests) - catches the common-mode trap (demonstrated).
- **Perturbation tests**: every DM field alone (extremes, +-1, just out of range), and
  single-sample / single-bit picture changes at edges and inside/outside the metadata area.
- **Incoming HDR modes**: tunnel, LLDV, HDR10 with/without InfoFrame, SDR and transitions,
  through VP's own gate / newlldv inference / LLDV metadata resolution (moved to Lib
  headers unchanged).
- **Tier 2**: dovi_tool 2.3.4 (`D:\tools\dovi_tool`), 37 MIT RPUs (`D:\testclips\dv\`,
  README), converter `tools/dv_rpu_to_dm.py`, fixtures `test-fixtures/dv-rpu/`; anchors:
  dovi_tool's reading through the loop, RPU CRC-32.
- **Tier 3**: `RawCaptureRecorder` (env `VP_RAW_CAPTURE_PATH`, shortcut Ctrl+Alt+D or
  RECORD/STOP files, burst mode), corpus reader (VP and StreamRecorder formats), replay
  test with regression digests and an optional dovi_tool anchor.
- **Apple TV reference**: `tools/dv_lldv_vs_tunnel.ps1` (CPU replay -> GPU render at the
  LLDV target -> `dv_compare/dv_lldv_vs_tunnel.py`: content alignment robust to
  start/pause/seek, Delta E ITP per frame/scene, L1 tracking); Python self-test passes.

## Behaviour-preserving moves (no maths changed)

`DolbyVisionTunnelGate.h` (Dlg x2), `DolbyVisionTunnelPresentation.h` (renderer),
`HdrInferenceSignalBuilder.h` (Dlg), `LldvStaticMetadata.h` (Dlg). New GUI hooks only for
the opt-in recorder (no effect unless `VP_RAW_CAPTURE_PATH` is set) and the
`raw_capture_toggle` shortcut (Config > Shortcuts).

## Results

- Build: full solution x64 Release, rc=0 (GUI, renderer, Config, tests, `VideoProcessorDvLoopback.exe`).
- New CPU tests: 33, all pass (`DvTunnelLoopbackTests` 18, `DvIncomingHdrModeTests` 8,
  `DvRawCorpusTests` 5, `DvRpuAnchorTests` 2). 113 metadata perturbations and 254 picture
  perturbations checked; 1374 real RPU frames through the loop (1368 multi-packet DMs,
  1 profile-4 14-bit DM rejected as VP documents), 0 mismatches against dovi_tool;
  1251 RPU CRC-32s checked, the only mismatch is dovi_tool's malformed `p8_001_end_crc32`.
- Full CPU suite (single-threaded, TRX): 3380 tests, 3378 passed, 2 failed - both DeckLink
  converter performance tests (code untouched). `CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest`
  passes on rerun; `DeckLinkConvertersSustained4K60PerformanceComparison` still exceeds a
  60 fps period while three other agents' vstest processes were running (timing under load).
- Config tests (off screen, one shard): 143 passed, 0 failed, 1 skipped (needs a real window); gate passed.
- Python comparison self-test: aligner exact with a late start, a 5-frame pause and a skip
  (49/49 frames, 0 wrong); BT.2124 dE ITP sanity passes.
- GPU tests: written, not run (Claude).

## Decoder discrepancies / findings

- No decoding bug found: unpack, CRC, scrambling, packet syntax and every ETSI-defined
  DM field agree byte for byte with the independent encoder; all perturbations exact.
- Spec note: ETSI's default YCCtoRGB offsets imply a 2^30 scale; VP uses 2^28 (right for
  real P5/P8 RPUs and the live Apple TV picture). A YCbCr-signalled tunnel with ETSI's
  defaults would be offset 4x. To settle with the first real capture's DM.
- Precision note: offsets go through float (changes below ~2^-24 relative vanish) - far
  below a 12-bit step.
- Finding A: 10-bit RGB LLDV/HDR10 captured as r210 with the tunnel switch on stays black
  and suppresses newlldv. Not fixed (a bit-depth gate would break padded 8-bit tunnels).
- Finding B: 8-bit RGB LLDV with the switch on is shown through the SDR passthrough.
- Finding C: a truncated frame with intact metadata is decoded (no completeness check).

## Hardware feasibility (laptop, read-only probe)

Dell XPS 15 9510: NVIDIA RTX 3050 Ti has 0 display connectors (NVAPI) and no DXGI
outputs; every port incl. USB-C/Thunderbolt DP alt mode is scanned out by the Intel GPU.
VPV cannot run on this laptop. Recommended loop: cinema PC RTX 3090 HDMI -> Integral 2
(TV-led DV EDID; NVIDIA engages DV mode only when the sink EDID has a Dolby VSVDB) ->
DeckLink Quad input (same PC, QPC latency), or -> UltraStudio 4K Mini on the laptop.

## GPU tests for Claude

`VP_TEST_GPU=hardware`, filter
`FullyQualifiedName~DvAutotestGpu|FullyQualifiedName~DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer`.
`DvAutotestGpuRendersTunnelCorpusForLldvCompare` needs `VP_TEST_DV_RENDER_CORPUS` /
`_OUT` (skips otherwise). Not run by the agent.
