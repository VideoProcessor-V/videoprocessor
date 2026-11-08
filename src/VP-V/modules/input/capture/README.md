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

- Existing keys: `capture_path`, `capture_upload`, `native_rgb_ingest` (`dv_tunnel_gpu_unpack` belongs to `input.signal`, where the DV unpack lives)
- Planned keys: (none)
- Tiers: not tiered (always on); manual paths in Config

## Fallbacks

- "Fallbacks engaged" entries: `capture GPU unpack: CPU formatters ... for the session: <error>`, `native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error>`; `early upload: the render upload (<error>)` (WP-4 / perf's `FallbackCatalog::EarlyUpload`, engaged and released)
- Behaviour: a GPU unpack failure switches to the CPU formatters for the session (visible since r6); early upload unavailable takes the render upload.

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
- capture fallback appears under 'Fallbacks engaged' (renderer harness, GPU)

Chain tests:
- chain gate subset: capture formats x 24/50/60p

## How the code works

Checked against the code 2026-10-25 (M2 step 15, `feat/m2-dv`). Only the VP files are this
module's; the capture back-ends (DeckLink, Magewell, file source), `CaptureInput` and the CPU
formatters are Bill's and are used as they are.

**Public API**
- `GpuCaptureUnpack` - `capture_path: gpu`: raw V210 / UYVY / HDYC uploaded as bytes and unpacked
  by one compute pass into the planes the CPU formatters write (P210: 16-bit luma + interleaved
  CbCr at every row), bit-exact; `Supports`, `Unpack(encoding, data, rowBytes, w, h, planes,
  uploaded)`, `Error`.
- `GpuRgbUnpack` - `native_rgb_ingest`: R12B / R12L / R210 / R10b / R10l / ARGB / BGRA unpacked to
  an rgba16 picture plus the de-judder matcher's r16 luma (exactly the CPU formatter's Y);
  `Supports`, `Is8Bit`, `FullRange`, `ColorDepth`, `BitShift`, `Unpack(..., lumaOnly, uploaded)`.
- `EarlyUpload` - `capture_upload: early` (and `auto`: every mode, VRAM-guarded; low-latency path only with `efficiency_fixes` off): each raw frame
  uploaded on the conversion thread by a second D3D11 device into a ring of shared textures,
  with "uploaded" / "released" fences; `Open`, `Close`, `Upload` (conversion thread), `Acquire` /
  `Used` (render thread), counters, `CapBytes`.
- `EarlyUploadHandoff` - the slot hand-off for `EarlyUpload`: the conversion thread publishes the
  payload first, then `RunUpload` (Pending -> Uploading -> Attached / None); the render's `Take`
  returns the slot, waits only while an upload is running, or marks a not-started upload Abandoned
  and uploads itself (counted as "before the early upload started"). One mutex + condition variable.
- `UploadReuse` - the pure decision whether the de-judder's already uploaded next-frame planes
  can be taken as the shown frame's planes (one upload and unpack per new frame saved).

**Data flow.** In: the raw capture frame handed on by the conversion thread instead of being
converted (`capture_path gpu`, `native_rgb_ingest`, the DV tunnel with `dv_tunnel_gpu_unpack` -
that one lives in `input.signal`), optionally already on the GPU (early upload). Out: the planes
libplacebo renders from (P210 / rgba16) plus the matcher's luma; on a failure, the CPU formatter
path for the rest of the session.

**Threading.** Unpacks: render thread. Early upload: `Upload` on the conversion thread, `Acquire`
/ `Used` / `Open` / `Close` on the render thread; the slot ring is guarded by its own mutex and
never blocks (no free slot: the frame takes the render upload). The host's "wanted" / "failed"
flags are atomics read by the frame preparation.

**Config keys the code reads** (renderer profile sections, host settings reader until WP-18):
`capture_path` (standard | gpu, default gpu), `capture_upload` (auto | render | early, default
auto), `native_rgb_ingest` (default on). Original pathway: `capture_path standard`,
`capture_upload render`, `native_rgb_ingest off`.

**Fallbacks a user sees**
- `capture GPU unpack: CPU formatters (4:2:0) for the session: <error>` - the text says 4:2:0, but
  the CPU formatter for V210 / UYVY / HDYC is P210 4:2:2 unless the P010 conversion is forced
  (finding F-3; fix patch `m2-dv-fixes.patch`).
- `native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error>`.
- Early upload "not available" (the second device or the shared textures cannot be made):
  TODAY LOG-ONLY ("Early upload: not available (...); the render upload") - finding F-4, fix patch
  WP-4 / perf engage `early upload: the render upload (<error>)` and release it when the pool opens
  or early upload is no longer wanted (F-4 resolved there).

**To lift the module:** the host's capture upload glue (`UnpackRawPayload`, the route text, the
early-upload sync and slot handling, the raw/convert decision in the frame preparation) moved
into a VP-V capture adapter, like `DvGpuUnpackSession`; the settings readers in VP-V's config.
The Magewell SDK (`MagewellSdkRoot`, `/p:VpCiNoMagewell=true`) only concerns Bill's `magewell/`
back-end; nothing in this folder includes it.

**Test it standalone** (CPU):

```
vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /Logger:trx /Tests:EfficiencyTests,VideoFrameFormatterTests
```

GPU (not run by agents): `GpuCaptureUnpackMatchesTheCpuFormatters`,
`GpuRgbUnpackMatchesTheCpuDecode`, the early-upload byte-identity and slot tests in
`LibplaceboLutParserTests`, the renderer capture harness.

## M2 P10: the capture extension (hub module `input.capture`)

Doc 59 section 5. The capture code leaves the renderer's file; the renderer keeps generic, inert call sites.

| File | Role |
|---|---|
| `CaptureExtension.h/.cpp` | the hub module: Lifecycle `SettingsApplied` / `GpuReleased`, R16 `OnConvertedFrame` / `AfterPublish`, each routed to the renderer registered under the hook's host; registered in `core/hub/HubBootstrap.cpp` after resync |
| `CaptureHost.h` | `ICaptureHost`, the header-only Impl adapter (`CaptureImplHost<Impl>`) and `HostBinding` (the Impl's LAST member: registered last, removed first; fixed lock-free host-keyed slot table, `core/hub/HostSlots.h`). A failed registration engages the renderer's "capture extension" fallback (`FallbackCatalog::CaptureUnbound`) and logs a line |
| `CaptureImpl.inl` | the moved code, as out-of-line members of the renderer's Impl (the P5a pattern): `CaptureApplySettings` (item f), `CaptureReleaseGpu` (d), `CaptureConvert` (h), `CaptureAfterPublish` (i), the early upload and the unpackers (a functions, b). Included ONCE by the renderer after its Impl |
| `CaptureRoutes.h` | `ChooseRoute`: the conversion route as a pure decision (pinned by `CaptureRouteTableTests`, 32768 rows against a copy of the old expressions) |
| `CapturePayload.h` | the R18 slot's content (`PayloadExtension`; P10-3: the early hand-off) |

Threads and locks (unchanged from before the move):

- conversion thread: `CaptureConvert` (no queue lock; `formatterMutex` inside the Dolby Vision analyse only), then
  `CaptureAfterPublish` after the notify (no lock; the hand-off's own mutex). `m_queueMutex` is never held across either.
- render thread (`renderMutex`): `SyncEarlyUpload`, `EarlyTexture`, `BookUploadTimers`, `Unpack*` (as before).
- Retire path (`renderMutex`): `CaptureReleaseGpu` at the `GpuReleased` notification, still before `pl_d3d11_destroy`;
  it now runs after the renderer's other VP releases there (de-judder, rebase, calibration, ...), not before them.
- settings thread: `CaptureApplySettings` at `SettingsApplied` (raised by `UpdatePipelineSlots`, after
  `activeSettings = settings` at both sites), release stores read by the conversion and render threads.
- The raw frame's deleter captures the shared `rawBuffersHeld` counter, never the extension or the Impl.
- A route's throw is caught in `CaptureConvert` and returned through `ConvertContext::error`; the loop rethrows it, so
  the frame fails with the same text (the hub would otherwise contain the throw and run the host formatter).

Bill's path: with the routes off (Original: capture_path standard, capture_upload render, native_rgb_ingest off,
dv_tunnel_gpu_unpack off) `ChooseRoute` returns the host formatter, except libplacebo's native RGB upload (ARGB /
BGRA), which the renderer took before P10 too. Bill's formatter block and log text in `ConversionLoop` are unchanged.

Not done yet (P10 open items): the R18 header fields (LVRH zone, after P2 lands), the upload branches via R17 `TryUpload`
(item g, DEFERRED until after P5b by the M2 lead: R17 stays unwired and `UploadContext` is not widened;
the branches are interleaved with Bill's plane upload and the de-judder's `UpdateDejudderEstimate` / `TakeNextPlanes`,
P5b's; `UploadContext` lacks the frame facts they read), the data members of item a, the init log (e), and the queue
/ cadence / file-frame items (j-q, P10-5..P10-8).

Tests: `CaptureRouteTableTests` (the recorded route table), `CaptureExtensionTests` (routing per host, a retired
renderer, lookups during binding churn), plus the existing EarlyUpload*, CaptureUnpackHookTests, DvTunnelUnpackHookTests,
HookAmendmentTests and the renderer capture harness.

## Known issues / open items

- CPU fallback keeps 4:2:2 (CV210toP210) instead of 4:2:0 (item 44, low priority)
- RGB capture luma path for net A (item 49)

## Where the code lives (live file list)

Code location: **partly migrated** (M2 step 15, VP files only). The capture back-ends and formatters are Bill's (present in origin/v1.4.02-beta) and never move; this slot uses them. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

VP files (in this folder):

- `src/VP-V/modules/input/capture/GpuCaptureUnpack.h`
- `src/VP-V/modules/input/capture/GpuCaptureUnpack.cpp`
- `src/VP-V/modules/input/capture/GpuRgbUnpack.h`
- `src/VP-V/modules/input/capture/GpuRgbUnpack.cpp`
- `src/VP-V/modules/input/capture/UploadReuse.h`
- `src/VP-V/modules/input/capture/EarlyUpload.h`
- `src/VP-V/modules/input/capture/EarlyUpload.cpp`
- `src/VP-V/modules/input/capture/CTwoPlane420VideoFrameFormatter.h` (M2 item 11: our file-frame NV12/P010/P016 formatter, moved out of Bill's `video_frame_formatter/`; forwarding header kept at the old path for the renderer host `LibplaceboVideoRenderer.cpp`)
- `src/VP-V/modules/input/capture/CTwoPlane420VideoFrameFormatter.cpp`
- `src/VP-V/modules/input/capture/CaptureExtension.h`, `CaptureExtension.cpp`, `CaptureHost.h`, `CaptureImpl.inl`, `CaptureRoutes.h`, `CapturePayload.h` (M2 P10)

Bill's files used (stay):

- `src/VideoProcessor-Lib/vprenderer/AlphaNativeRgbIngress.h`
- `src/VideoProcessor-Lib/CaptureInput.cpp`
- `src/VideoProcessor-Lib/CaptureInput.h`
- `src/VideoProcessor-Lib/blackmagic_decklink`
- `src/VideoProcessor-Lib/magewell`
- `src/VideoProcessor-Lib/video_frame_formatter`
- `src/VideoProcessor-Lib/VideoConversionOverride.cpp`

Forwarding headers (removed in M2 step 0) stay at `src/VideoProcessor-Lib/vprenderer/` for the four moved headers, for the renderer host and Bill's `LibplaceboLutParserTests.cpp`; listed in the MANIFEST. The Magewell SDK (`MagewellSdkRoot`, `/p:VpCiNoMagewell=true`) only concerns Bill's `magewell/` back-end; nothing here includes it.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0010 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `capture_path`, `capture_upload`, `native_rgb_ingest`.
- Tiers: not tiered (always on); manual paths in Config.
- VP-V page, Input tab: not tiered (Always on: capture and timing have no quality steps, only paths (the toggles below).).
- Listed under the tier: `capture_path`, `capture_upload`, `native_rgb_ingest`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#2-input](../../../../../docs/settings/VP-settings-reference.md#2-input).
- Fallback entry ("Fallbacks engaged"): `capture GPU unpack: CPU formatters for the session: <error> | native RGB GPU unpack: CPU conversion to 10-bit 4:2:0 for the session: <error> | early upload: the render upload (<error>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): GpuCaptureUnpack byte checks (VideoProcessor-Test); GpuRgbUnpack native RGB checks.
- Contract (formats, geometry, fallback entry): capture fallback appears under 'Fallbacks engaged'.
- Chain (total delay at the 4/3/2-frame budgets): chain gate subset: capture formats x 24/50/60p.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
