# GPU capture path for a rule-selected low-latency mode (plan, not built)

Owner, 2026-10-13: "implement it as widely as possible in a 'low latency
mode' that we can create rules for, i.e. for >59 Hz since those are the
live TV and gaming use cases, while for now at least keeping the original
proven pathway for cinema." The capture card is a **DeckLink**. Scheduled
after the audit of 6f29497d..HEAD, as its own reviewed item.

## Why

Today every DeckLink frame that is not 8/10-bit RGB is converted on the
CPU (V210/UYVY to P210, r210/R12L to P010), then libplacebo copies it
again into its own upload buffer (uploads pass a plain pointer). The
GPU-native package from the owner's earlier work with Bill
(vp_agent_briefing.zip: GpuNativePlaneUpload, V210GpuUnpack,
R12LGpuUnpack, PinnedUploadStaging, NativeAnalysisReadback) was never
integrated - none of it is in any branch - and was written against the
older P010-only renderer and never compiled. Bill's 2026-08-02 change
already uploads ARGB/BGRA/R10b/R10l natively. madVR and MPC-VR need the
CPU P010 converters (DirectShow media type) and keep them unchanged.

## Shape

- **A profile key, rule-selectable:** `capture_path: standard | gpu`
  (input profile; default `standard` = today's proven path). A rule such
  as `when: $source_rate > 59` picks a low-latency profile that sets
  `gpu` together with `[queue.Low_Latency]`. Cinema (23.976/24) keeps
  `standard`.
- **As wide as possible within `gpu`:**
  1. V210 and UYVY/HDYC: raw bytes to the GPU, a compute pass unpacks
     to P210 (keeping 4:2:2, Bill's current quality; the old package
     dropped to 4:2:0).
  2. r210 and R12L/R12B: raw to the GPU, unpacked to RGB (R12 at full
     12 bits).
  3. Every format: pinned, ping-ponged upload buffers (`host_mapped`
     `pl_buf`, upload by `.buf`, `pl_buf_poll`, never blocking; a busy
     slot falls back to the pointer upload for that frame).
  4. Optional last step: a custom DeckLink buffer allocator so the card
     writes straight into pinned memory (removes the final copy). Shared
     capture code: behind the same key, only for the VP renderer.
- **Analysis features keep working from the raw buffer:** active
  picture / NLS, scene detection, subtitle tracking and the LED
  lookahead read sparse luma on the CPU. They read the raw DeckLink buffer
  (already in system memory) through a small V210/UYVY sample reader in
  AnalysisLumaSource - no full-frame conversion, no GPU readback lag.
- **De-judder next frame:** its early upload of the next source frame
  takes the same GPU unpack.
- **Fallback:** any unsupported format, a failed capability check or a
  failed upload falls back to `standard` for the session (logged once).

## Measure first, and after

On the cinema PC: the `libplacebo initialized: D3D11, ... upload` line
(which format the DeckLink delivers) and the `Alpha frame pacing trace`
fields `conversion_ms` and `source_upload_cpu_ms` during a game. Repeat
with `capture_path: gpu`; record both in BUILD-STATUS.

## Tests

Pixel-exact against the CPU converters for V210, UYVY, r210, R12L
(synthetic frames covering every packing phase and odd widths); the
analysis sample reader against the converted planes; the pinned upload's
fallback when a slot is busy; teardown order (GPU resources released
before the device); the chain matrix with `capture_path: gpu`; the
GeForce runs.
