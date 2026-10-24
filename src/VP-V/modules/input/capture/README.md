# Capture (`input.capture`)

Unit: **input** | side: input | order: 10 | status: **built** | hook point: capture

## Purpose / why

Bring frames in from DeckLink/Magewell (or a file source) without losing precision: GPU unpack of v210/UYVY to P210 4:2:2, native RGB ingest for r210/R12B/R12L, upload reuse and early upload.

## Contract

- Input: capture-card frames: v210, UYVY, r210, R12B, R12L, P010 (file source), DV tunnel (12-bit RGB container)
- Output: GPU planes: P210 4:2:2 (YCbCr) or RGB 4:4:4 (native RGB); 10-16 bit
- Geometry: delivered size (the HDMI signal size), no crop
- Delay share: 0 frames; upload inside the frame period
- Look-ahead: none

## Settings

- Existing keys: `capture_path`, `capture_upload`, `native_rgb_ingest`, `dv_tunnel_gpu_unpack`
- Planned keys: (none)
- Tiers: not tiered (always on); manual paths in Config

## Fallbacks

- "Fallbacks engaged" entry: Capture: CPU 4:2:0 (GPU unpack failed: <reason>)
- Behaviour: GPU unpack failure switches to the CPU formatters for the session (today log-only; must become visible, queue item 44/45).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- GpuCaptureUnpack byte checks (VideoProcessor-Test)
- GpuRgbUnpack native RGB checks

Contract tests:
- capture fallback appears under 'Fallbacks engaged'

Chain tests:
- chain gate subset: capture formats x 24/50/60p

## Known issues / open items

- CPU fallback keeps 4:2:2 (CV210toP210) instead of 4:2:0 (item 44, low priority)
- RGB capture luma path for net A (item 49)

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VideoProcessor-Lib/vprenderer/GpuCaptureUnpack.cpp`
- `src/VideoProcessor-Lib/vprenderer/GpuCaptureUnpack.h`
- `src/VideoProcessor-Lib/vprenderer/GpuRgbUnpack.cpp`
- `src/VideoProcessor-Lib/vprenderer/GpuRgbUnpack.h`
- `src/VideoProcessor-Lib/vprenderer/AlphaNativeRgbIngress.h`
- `src/VideoProcessor-Lib/vprenderer/UploadReuse.h`
- `src/VideoProcessor-Lib/vprenderer/EarlyUpload.cpp`
- `src/VideoProcessor-Lib/vprenderer/EarlyUpload.h`
- `src/VideoProcessor-Lib/CaptureInput.cpp`
- `src/VideoProcessor-Lib/CaptureInput.h`
- `src/VideoProcessor-Lib/blackmagic_decklink`
- `src/VideoProcessor-Lib/magewell`
- `src/VideoProcessor-Lib/video_frame_formatter`
- `src/VideoProcessor-Lib/VideoConversionOverride.cpp`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0010 in [docs/decisions](../../../../../docs/decisions/README.md)
