# Audit scope - running list since the last audit

The last code and options audit was stage7 2026-10-14c (docs/audit/stage7-2026-10-14c-config.md). This
list grows with each change so the gate's code audit and options audit start from it. Each entry: what
changed, the commits, its switch and default, and the tests that cover it.

## Stream robustness, pulldown, black bars, generality (2026-10-14)

- 427e2c1e: stream robustness, general pulldown detection, black bars, no fixed rates or resolutions.
- 8d42db13: the Config section "Fixes under test" (each fix on by default with a switch):
  `fix_scene_cooldown`, `dejudder_picture_area`.
- b801e935: `RecordedStreamReplaysThroughTheRenderer` (stream replay, synthetic or VP_TEST_STREAM_DIR).

## Efficiency, exact (2026-10-14 / 15)

Every change below is byte-identical by test (Beta "parallel block search byte-identical to the serial
search", sigma-delta serial reference, "reused half-size planes byte-identical", 6d 0.161845).

- 6a371d83: frame trust histogram in one sweep.
- 904debe6: B's first present takes the sigma-delta vectors from A's field - switch
  `dither_vectors_new_frame` (Fixes under test, on).
- 19c8cc7f: sigma-delta quarter-resolution tile load and ping-pong carry.
- 15236dbf, e85d62df, ebfaeeb5, c32caccf, ed0f67d7: block search picks folded into the SAD loops,
  candidates from their index, 128 threads per block, the 1/8 sample table.
- 43787371: second sweep, two blocks per 32-thread group.
- 8724191f: the lowRes outer builds only the pyramid levels it reads.
- ce795406: reuse of the last pair's B planes (keyed by frame number; gap forgets, resync destroys);
  the fade-corrected B copies the stored value where no fade is active.
- 792c7261: merged small passes (global motion both directions, the kept searches, the grid-up).
- Rejected and reverted (documented in docs/VP-matcher-search-restructure.md): flat-pass search,
  shared `to` window, tree-reduction picks, spread second-sweep reads, half-size row sharing.

## Experimental, off (2026-10-14 / 15)

- c1ca7aeb, fee16b8d: NVIDIA optical flow wrapper and `Settings::opticalFlow` motion source (off,
  parked; 3rdparty/nvidia-optical-flow MIT headers).

## Native RGB ingest and capture fixes (2026-10-15, roadmap item 2)

- 818a2ae9, f44b20f2, 7134e035: `GpuRgbUnpack` - DeckLink R12B/R12L/r210/R10b/R10l/ARGB/BGRA handed on
  raw and unpacked on the GPU to native RGB 4:4:4 (rgba16) plus the matcher's luma (the CPU formatters'
  Y exactly). Switch `native_rgb_ingest` (Fixes under test, on); off = the CPU conversion to 10-bit
  4:2:0 P010, or libplacebo's native upload for ARGB/BGRA. Both capture paths.
  - Fix: R10b/R10l never showed a picture on libplacebo's native upload (no D3D11 plane format
    matches their bit order); now GpuRgbUnpack, or the P010 formatter with the switch off.
  - ARGB/BGRA with the switch off: an empty prepared payload so the queue (which now sees a
    formatter) does not drop the frames.
  - Tests: GpuRgbUnpackMatchesTheCpuDecode (52 cases byte-exact: 7 formats, BT.709/2020, padded
    rows; 4K timing report); NativeRgbIngestRunsThroughTheRenderer (all 7 variants, both capture
    paths, switch on and off, de-judder held on RGB).
- `[decklink] ycbcr_12bit_capture` (v210 default since bug hunt 2, keeps BTB/WTW / rgb12 earlier default / off): a signal the driver reports as 12-bit
  YCbCr 4:2:2 was refused; now 12-bit RGB from the card when offered for the mode, else 10-bit 4:2:2.
  Test: DeckLinkYCbCr12CaptureChoice. **No Config control yet (options audit).**
- f9369400: `capture_path` default `gpu` on both pathways (moved from "Low latency (experimental)" to
  Fixes under test); de-judder next/ahead-frame unpack failures skip that frame instead of turning the
  session to the CPU; route and encoding parity in the de-judder layout checks; "GPU unpack" in the
  ingress status; a log of the most capture buffers held at once; the 12-bit unpack reads each block
  once. Audit of CPU consumers on the raw route: none breaks (analysis reads the raw frame);
  SubtitleBoxDetector falls back to per-sample decode on raw V210 (optional fast path). Full suite
  2610/2611.
- De-judder on RGB (roadmap item 2): the matcher accepts luma-only planes (no source warp for them);
  the renderer feeds it the unpack's luma (the CPU formatter's Y exactly), unpacks the next / ahead
  frames luma-only (GpuRgbUnpack lumaOnly), runs it with the rendered warp and holds ("RGB capture")
  with the source warp, or ("RGB source without the rendered picture") when the picture is not
  cached. Tests: NativeRgbIngestRunsThroughTheRenderer (24p R12B and ARGB estimate; source warp
  holds), luma-only luma identical in GpuRgbUnpackMatchesTheCpuDecode; Beta byte-identity; quick tier.
- 61668ab0: test DLL built without whole-program optimisation (LNK1103 corrupt intermediates);
  quick test tier (heavy tests excluded) for routine runs, the full suite at milestones.

## Early upload (2026-10-15, the pinned-memory work, option A)

- Probe (PinnedUploadCapabilitiesAndTiming): libplacebo's D3D11 backend has no host-mapped buffers and no
  buffer transfers (max_mapped_size 0, buf_transfer 0), so the planned pinned upload buffers (and the
  earlier package's PinnedUploadStaging / NativeAnalysisReadback) cannot engage on D3D11. A 4K 12-bit
  frame's pointer upload: the call blocks the render thread 2.4 ms, 5.5 ms with the transfer (6.8 GB/s).
- `EarlyUpload`: a second D3D11 device on the render device's adapter, a ring of shared textures and two
  shared fences (uploaded / released). The conversion thread uploads each raw frame as it arrives; the
  render thread waits on the uploaded fence, unpacks from the shared texture (wrapped for libplacebo),
  signals released. A slot is reused only when its payload is gone (CPU readers done) and every GPU use
  is released; never blocks (no free slot: the render upload). Pool: 11 slots, capped at 1 GB and a
  quarter of the adapter's video memory budget (an integrated GPU's dedicated size is tiny). Lifecycle
  lock (no Close mid-upload), pool id (a reopened pool refuses old slots).
- Switch `capture_upload` (render default / early), experimental, in Fixes under test's area of the
  Config. GpuCaptureUnpack / GpuRgbUnpack read an already-uploaded frame (`uploaded`).
- Fix found on the way: the RGB unpack's textures and dispatch were not released before the device at
  teardown (rgbUnpack, rgbIngestLuma).
- Tests: EarlyUploadMatchesTheRenderUploadAndNeverReusesBusySlots (V210 and R12B byte-identical to the
  render upload, a held slot never handed out, a released slot back, the cap); EarlyUploadRunsThrough
  TheRenderer (V210 / R12B, 24p de-judder: every frame after the first read early, no fallbacks);
  quick tier 2191/2192.

## Known test notes

- WindowResizeLayoutTests::ShellStylePlacementWithoutSizingHintsIsNotReverted and
  ConfigurationReadCacheTests::SameSizeEditWithRestoredWriteTimeInvalidatesCache fail on this laptop
  in every full run (unrelated to the above).
- LedFadeTests::SwitchFromScriptToCompensateResetsContrastFirst failed once in a full run and passes
  alone (timing-sensitive).
