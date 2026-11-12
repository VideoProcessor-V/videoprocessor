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

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `ycbcr_12bit_capture` | decklink (startup) | enum | v210, rgb12, off (auto is accepted and means v210) | v210 | Config > VP-V page > Input (both pathways) > 12-bit YCbCr signals (DeckLink) | n/a (VP-only) | DeckLink only, for a source sending 12-bit YCbCr 4:2:2: v210 = 10-bit 4:2:2 keeping below-black/above-white; rgb12 = the card's 12-bit RGB (clips BTB/WTW); off = refuse the signal. | not tiered |
| `magewell_recover_faults` | general (startup) | bool | true, false | true | Config > General page > General behavior > Magewell: recover from capture faults | n/a (VP-only) | fix under test (proposed retirement, group A); on = a fault the card recovers from restarts the device by itself, off = capture stops and needs a manual Restart. | not tiered |
| `capture_display_fixes` | general (startup) | bool | true, false | true | Config > General page > General behavior > Capture / display fixes in Bill's code | off = Bill's earlier behaviour | fix under test (proposed retirement, group A); on = target-only full-screen takes the largest mode first and never an interlaced one, and a DeckLink Rec.601 source is recognised by its active lines (486/480 NTSC, 576 PAL); off = Bill's earlier behaviour. | not tiered |
| `capture_path` | profile ([vprenderer.Default] and matching profiles) | enum | standard, gpu | gpu | VP-V > Input > Main > Capture; also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `standard` (ADR-0019) | gpu = V210 / UYVY / HDYC unpacked on the GPU from the raw frame (byte for byte the same planes, about half the time at 4K, no CPU conversion); standard = the CPU formatters. status line: capture GPU unpack: CPU formatters for the session: <error> | same on all (not tiered) |
| `capture_upload` | profile ([vprenderer.Default] and matching profiles) | enum | auto, render, early | auto | VP-V > Input > Advanced > Capture plumbing (expert); also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `render` (ADR-0019) | early = each raw frame uploaded on the conversion thread as it arrives (a second device and shared textures); render = the render thread uploads it; auto = early in every mode (VRAM-guarded; low-latency path only with efficiency_fixes off). status line: capture GPU unpack: CPU formatters for the session: <error> | same on all (not tiered) |
| `efficiency_fixes` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Proposed retirement > Proven fixes (all on by default) | Original pathway sets `off` (ADR-0019) | Work done twice is done once, same picture: the frame shown takes the planes already uploaded for the motion estimate, and the look-ahead measures a frame's active picture only while an observer still wants it; it also controls capture_upload auto -> early, the sigma-delta grids and the extended-range one-pass/idle path; off = the earlier behaviour. proposed for retirement (group A) | same on all (not tiered) |
| `native_rgb_ingest` | profile ([vprenderer.Default] and matching profiles) | bool | on, off | on | VP-V > Input > Main > Capture; also VP Renderer > Rendering > Fixes under test (upstream copy greyed while VP-V is on) | Original pathway sets `off` (ADR-0019) | DeckLink R12B / R12L / r210 reach the renderer as native RGB unpacked on the GPU instead of squeezed to 10-bit 4:2:0 on the CPU. status line: capture GPU unpack: CPU formatters for the session: <error> | same on all (not tiered) |


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
  `Used` (render thread), counters, `CapBytes`. refs/105 5a (owner O3): an optional second reader on another
  device of the same adapter (the optical-flow producer's) - `AttachReader` / `DetachReader` / `HasReader`,
  `AcquireReader` / `UsedReader` with its own "released" fence; a slot is reused only when the payload, the render
  side and the reader have all released it (`SlotReusable`); a reader that never released (stopped, dead) holds
  its slot until `DetachReader` (bounded ~100 ms GPU wait, `readerDetachTimeouts`); `noFreeReader` counts the slots
  it held at a no-free-slot. Not wired to the producer yet (5b).
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
| `CaptureRouteFacts.h` | `FactsOf`: the route's facts from the state, the override and the switches (pinned on real states) |
| `CaptureRoutes.h` | `ChooseRoute`: the conversion route as a pure decision (pinned by `CaptureRouteTableTests`, 32768 rows against a copy of the old expressions) |
| `CapturePayload.h` | the R18 slot's content (`PayloadExtension`: the raw frame and its layout, the early hand-off, the DV tunnel flags; r2/p10) and the free readers (`RawOf`, `IsRaw`, `BytesOf`, `RawEncodingOf`, `DvTunnelRawOf`, `RawFrameOf`, `HasEarlyUpload`, `EarlyUploadDue`) |

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

R18 (r2/p10, zone CAP): ALL capture data of the converted payload is one generic extension, `Vpv::Capture::PayloadExtension` in
`QueuedFrame::ConvertedPayload::extension` (the one `// VPV-HOOK` slot): `rawFrame`, `rawRowBytes`, `rawBytes`,
`rawEncoding`, `early`, `earlyHandoff`, `earlyHeight`, `dvTunnelRaw`, `dvTunnelHighAligned`. Bill's struct keeps his members
(pixels, contract, dolby, conversionCpuMs, completedQpc) and VP's `captureEncoding` (set on EVERY payload, incl. the CPU
formatter's, so it stays a member: moving it would cost one extension allocation per CPU frame). `Raw()` / `Bytes()` are
the free readers `IsRaw` / `BytesOf`. The extension is built on the conversion thread before the publish and never reassigned
after; a payload with no slot (CPU formatter, native-upload marker) answers as the former defaults. The earlier
`earlyFrame` / `earlyRowBytes` are the same shared frame and row bytes as `rawFrame` / `rawRowBytes` (one pointer, not two).
The host hands the slot to the R16 `AfterPublish` dispatch only when `EarlyUploadDue` says it carries a hand-off and its
frame (`LibplaceboVideoRenderer.cpp`, the `publishedExtension` line), so the per-frame cost is as before the move: a raw
payload without an early hand-off adds the slot's one `make_shared` on the conversion thread and no hub dispatch.

Design note (C9, doc 93, no code here): the payload's raw luma is the DECODE POINT that C9 will share. The raw frame the
slot holds is unpacked once, on the GPU, by `UnpackRawPayload` / `UnpackRgbPayload` / the DV unpack; a later consumer of the
picture's luma (source analysis) reads that unpack's luma output through the same slot and must not decode the capture frame
a second time (compute once / skip if present). The slot is input.capture's; a consumer that needs more payload data of its
own gets an interface, not a field here.

Not done yet (P10 open items): the upload branches via R17 `TryUpload`
(item g, DEFERRED until after P5b by the M2 lead: R17 stays unwired and `UploadContext` is not widened;
the branches are interleaved with Bill's plane upload and the de-judder's `UpdateDejudderEstimate` / `TakeNextPlanes`,
P5b's; `UploadContext` lacks the frame facts they read), the data members of item a, the init log (e), and the queue
/ cadence / file-frame items (j-q, P10-5..P10-8).

Tests: `CaptureRouteTableTests` (the recorded route table), `CaptureExtensionTests` (routing per host, a retired
renderer, lookups during binding churn), plus the existing EarlyUpload* (with `EarlyUploadSecondReaderTests`, refs/105 5a), CaptureUnpackHookTests, DvTunnelUnpackHookTests,
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
