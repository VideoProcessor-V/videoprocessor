# WP-23: VP frame analysis store + one luma decode per frame

Status: **built, CPU-tested, not GPU/renderer-run** (`feat/wp23-fas` from `chore/early-migrations`
ac49a1de, 2026-10-25). Spec: `integration-batch-plan.md` WP-23 and the "WP-23 addendum".
VP-ONLY except the Bill-file hook (`wp23-bill-analysers.md`, UPSTREAMABLE).

## What was built

| Piece | File | Kind |
|---|---|---|
| Store: record, ring keyed by source sequence, epochs, per-group validity, reader API, fallback entry | `src/VP-V/core/FrameAnalysisStore.h` | VP-only, header-only C++14 |
| Producer: single luma decode, reductions, async worker | `src/VP-V/modules/input/source_analysis/VpFrameAnalysis.{h,cpp}` | VP-only |
| Host adapter (the renderer's one handle) | `src/VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h` | VP-only |
| Shared luma hook in Bill's view + his luma-only reads | `AnalysisLumaSource.{h,cpp}`, `SceneDetector.cpp`, `ActivePictureEvidence.cpp`, `SubtitleBoxDetector.cpp` | UPSTREAMABLE patch (`wp23-bill-analysers*.patch`) |
| Our consumers read the shared decode | `LedLookahead.cpp`, `ContentIntentObserver.h`, `IntentInterpreterObserver.h`, `HdrInference.cpp`, `MotionAheadProducer.cpp` | VP files, edited directly |
| Renderer call sites + the `frame_analysis_store` key | `wp23-callsites.patch` (LibplaceboVideoRenderer.cpp, RendererProfileConfig.h) | patch for the integrator |
| Tests | `src/VP-V/tests/FrameAnalysisStoreTests.cpp`, `FrameAnalysisProducerTests.cpp`, `SharedLumaEquivalenceTests.cpp`, `SyntheticAnalysisFrames.h` | CPU |

## The single decode (addendum)

- `AnalysisLumaSource::sharedLuma` holds `Sample().luma` for every pixel, decoded ONCE per frame by
  the producer's worker (`VpFrameAnalysis::DecodeSharedLuma`, row decoder `SampleLumaRow`, the
  per-pixel sampler only where no row decoder exists). Attached when the frame's analysis view is
  first prepared (`Host::AttachIfEnabled` in `PrepareFrameAnalysis`, before the view is cached and
  copied), so every reader of that frame shares it.
- Readers never wait: before `Publish()` they decode as before. Values are identical either way,
  so every decision is identical (equivalence tests below).
- Formats that are already a readable 10-bit plane (P010 / P210 / decoded DV at limited range) get
  NO plane and no copy: their readers already read in place. The decode exists for v210, UYVY,
  every RGB format, the SDR tunnel, packed DV and full-range YCbCr — where the duplicate decodes cost.
- Readers: Bill's SceneDetector, active picture and subtitle luma reads (patch); our LED look-ahead
  full-coverage peak (row path) and raw measurement, content intent and intent interpreter grids,
  HDR veto grid, source-resolution look-ahead (call-site patch), NVOF half luma.
- **NVOF exception (byte identity):** the NVOF producer's luma for RGB is its own structure luma
  `(R + 2G + B) / 4`, and for full-range 4:2:2 the raw code; the shared plane holds `Sample()`'s
  luma, so those formats keep their own decode (vectors unchanged). It reads the plane for
  limited 4:2:2 and the sampler's formats (packed DV, SDR tunnel). Switching RGB to the shared luma
  would change the vectors: listed as an owner option, not done.
- Memory: 2 bytes x pixels per decoded frame (16.6 MB at 4K). Planes are recycled from a pool of
  at most 12 once nothing holds them (no per-frame allocation or zero-fill, audit M1); the pool is
  emptied when the store is switched off. Off: nothing allocated.
- Decoded only when a full-luma reader RUNS (`Host::FullLumaReaders`: NVOF only with optical flow,
  look-ahead frames and running motion.estimation + motion.consumers; the source-resolution
  look-ahead only in auto with scaling.rebase running; WP-15 adds LED full coverage). Sparse readers
  alone (scene, HDR, intent grids, subtitle probes) keep their cheaper own samples.
- The record is measured only when a running slot reads it (`recordReaders`; none on this branch,
  consumers register in WP-13..16 behind their own slot's `Runs()`). With no reader running, an
  arriving frame costs nothing: no job, no record, no plane (modularity sweep).
- `frame_analysis_store` is in the Original pathway table (`OriginalPathway.h`, slots.json, the
  manifest's config keys). The renderer side of that (`// key=` in ApplyOriginalPathwaySettings and
  the validator line) is in `wp23-callsites.patch`, so the drift check and the
  `OriginalPathwayOverridesAreRealKeysAndValues` test pass only with the patch applied (as for every
  host-delivered key).
- The plane is bound to the view it was decoded from (`Bind`/`Matches`, audit M2).

## Range model (explicit decision)

The forecast's clash: our `Sample()` maps full-range sources to studio codes (`fullRangeYuv`, audit
2026-10-16; LED depends on it) while Bill's v1.4.02 emits delivered codes plus `AnalysisLumaRange`
and adapts his thresholds. One model per side, one conversion:

1. **Bill's analysers use Bill's model.** The shared plane is `Sample()`'s output, so it follows
   whatever the merged `AnalysisLumaSource` emits; his decisions stay his.
2. **VP analysis uses 10-bit studio codes** (black 64, white 940, BTB/WTW kept). The conversion is
   ONE function, `VpFrameAnalysis::SamplerCodesAreFullRange` + `StudioLumaMap`. Today it is the
   identity (our sampler already emits studio codes — `RangeModelIsOneStudioMapping` test).
3. **At the WP-21 merge (recommended):** take Bill's sampler text (his file returns to upstream, the
   patch set shrinks), change that one VP function to `source.range == AnalysisLumaRange::Full`,
   and run LED's existing `!kAnalysisLumaSampleLimitedDomain` branch tests with full-range RGB and
   P010 fixtures. No consumer applies a range conversion of its own, so nothing is converted twice.

## The store

- Record per source frame: identity (seq, capture time, size/format/range/transfer, the picture
  rect measured = Bill's crop decision copied read-only, generations), luma statistics (1024-bin
  histogram, mean/APL, min/p1/p50/p99/max; full coverage of the picture), range occupancy (BTB,
  WTW, near black <= 81, clipped >= 1019), grain/noise (8x8 flat blocks, flattest decile, near-black
  and clipped blocks excluded, confidence), chroma summary and labels (placeholders), motion
  summary + vectors-valid (written by the motion segment, WP-7), cadence (`repeatOf`), scene
  (VP or Bill-copied, labelled).
- Ring depth = queue + look-ahead + 2 from the delay budget (`RingDepth`, set in
  `UpdateQueueDelayHold`); never a fixed count.
- Validity: ONE epoch shared with vector validity — every `RequestMotionInvalidate(cause)` also
  invalidates the store (`Host::FromMotionCause` maps the 11 motion causes). A stale record never
  serves temporal use (`ReadWindow`), single-frame use only with its own valid bits; motion summaries
  are cleared at every discontinuity and a pair landing after one is refused.
- Captured repeats get a record pointing at the original (`MarkRepeat`), no re-measure.
- Missing / late / stale -> the consumer keeps its computation and shows
  `Fallbacks engaged: frame analysis (<consumer>: <why>)` (`FallbackEntry`). The producer itself
  shows `frame analysis (producer behind: N frame(s) not analysed, stages measure themselves)`
  while it drops jobs.
- Threads: `Begin` on the conversion thread, publishes on the worker, reads anywhere; one mutex,
  copies only. The worker queue holds a ring's worth; a full queue drops the OLDEST job (never
  blocks the caller).

## Host call sites (`wp23-callsites.patch`, apply after `wp23-bill-analysers.patch`)

One line each unless noted: include; `RendererSettings::frameAnalysisStore` (default off; Original
pathway forces off) + fingerprint + `[vpv] frame_analysis_store on|off` read (8 lines, the existing
pattern) + validation in RendererProfileConfig.h; `VpFrameAnalysis::Host frameAnalysis` in Impl;
`Configure` in `UpdatePipelineSlots`; `AttachIfEnabled` in `PrepareFrameAnalysis`; `OnDiscontinuity`
in `RequestMotionInvalidate`; `SetRingDepth` in `UpdateQueueDelayHold`; the arrival hook in
`ConversionLoop` (~20 lines: copy the queued frame under the queue lock when enabled, prepare its
analysis view, `OnArrival`, engage/release the fallback); the source-resolution look-ahead lambda
reads `SampleLuma`. With the key off nothing runs and nothing is allocated.

## Deviations from the plan text (with reasons)

- Plan WP-23 said Bill's analysers "keep their own decodes"; the later addendum (owner) makes them
  read the shared decode. Done as the upstreamable hook; decisions unchanged.
- Producer is the CPU path (one worker). GPU reductions on the early-upload planes with an async
  readback ring (1dfcaf21 pattern) are the next step; they need GPU tests (not allowed here).
- Our consumers read the shared DECODE (byte-identical by construction). Moving them to the
  record's STATISTICS (LED peaks/APL from the record, intent from the histogram) changes code paths
  and stays in their WPs (WP-15/16/14/13), behind the same switch, as the plan's table says.
- `HdrInference` is a VP file (not in Bill's beta); migrated directly.
- `frame_analysis_store` is a hidden `[vpv]` key like `pipeline_entrance_once`; the Config control,
  settings reference and slots.json entry come with the settings work (WP-18) or when it becomes
  the default.

## Tests (CPU)

- `FrameAnalysisStoreTests` (12): depth from the budget, late->served, missing vs not measured, every
  discontinuity cause stops temporal use, motion cleared/refused across a discontinuity, windows
  need one epoch, repeats point at the original, slot reuse/release, disabled, fallback text,
  counters, reader/writer threads (20k frames, two readers, no torn records).
- `FrameAnalysisProducerTests` (9): shared decode == `Sample().luma` per pixel for all 16 formats,
  other-size views ignore the plane, one studio mapping, reductions == reference (histogram exact,
  mean 1e-6, BTB/WTW probes kept, with and without the plane), grain on known noise, async producer,
  drop-oldest + visible reason, repeats, the host adapter (switch, repeat, shared epoch, ring depth).
- `SharedLumaEquivalenceTests` (8): plain vs shared on 10-frame sequences with a cut, every decoded
  format: SceneDetector, SubtitleBoxDetector, ActivePictureEvidence + global near-black, LED peak
  (with and without a picture-only mask) + raw measurement (both coverage modes), content-intent
  grid + decisions, HDR veto grid, NVOF half luma, unpublished plane = no change.

## For later (GPU / renderer; not run here)

1. Renderer smoke with `frame_analysis_store = on` vs off: byte-identical output on the
   frozen-b3base captures (all consumers' decisions unchanged), Original pathway unchanged.
2. **CPU saved** (the reason for the addendum): per-frame CPU ms of LED look-ahead, intent, NVOF
   half luma, subtitle worker, active-picture walk, with the key off vs on, on 4K r210 / v210 /
   packed-DV and P010 sources; plus the producer's own ms (`Host::Summary`) and total process CPU
   (ProcessCpuUsageWindow). Expect the saving on RGB/v210/DV; ~0 on P010 (no plane).
3. Producer timing at 4K60 on the laptop: worker keeps up (no "producer behind"); if not, the GPU
   reduction path or row-parallel decode (the NVOF producer uses up to 4 threads).
4. Arrival hook cost on the conversion thread (PrepareFrameAnalysis moved earlier) and the timing
   gate with the key on.
5. GPU reductions == CPU reference on captured frames; <= 0.2 ms 3090-projected per source frame.

## Owner decision points

- `frame_analysis_store` has no Config control yet (hidden `[vpv]` key, like
  `pipeline_entrance_once`). It needs the control, the settings reference and the slots.json entry
  before it becomes default-on (audit L3).
- NVOF on RGB sources: keep its own `(R + 2G + B) / 4` luma (today), or read the shared `Sample()`
  luma (saves a decode, changes the vectors; needs the GPU check).

## Open

- Geometry-generation (crop) invalidation call site: the store records which crop it measured;
  the epoch bump on a crop change belongs with WP-8 (S1 reads the store).
- Diagnostics section (`Host::Summary`) into Ctrl+I / Ctrl+Alt+D: WP-11 / WP-17.
- Consumer-side "record late" fallback entries appear when consumers read records (their WPs).
