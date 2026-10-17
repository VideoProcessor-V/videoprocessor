# Fixed-constant sweep (2026-10-17)

Owner rule (2026-10-14): never hard-code refresh rates, frame rates, cadences or resolutions;
derive them from measured or declared rates and ratios (distances as a share of picture height,
reference 2160; rates per second, reference 23.976p). This is the read-only sweep of the code
against that rule. Line numbers are at ad9d1a5d and have moved since.

## Status (2026-10-17)

- **Owner answers (2026-10-17)** to the questions at the end: (1) D2 motion thresholds stay per
  source frame; (2) 25p and 30p are film-class by default (for now); (3) unify the refresh-key
  convention, with a cfg migration and a backup.
- **Fixed since:** #1, the DWM 20-120 Hz acceptance, now goes through `DisplayRefreshBounds::Accepts`
  (10-500 Hz) in VideoProcessorDlg.cpp. Many literal 240/480/500 Hz display-rate bounds remain
  (hunt 2, intent C3).
- **Scheduled:** batches B1 (rate bounds) and B2 (measured classifier) are work-queue item 4. B3-B7
  are not scheduled.
- The effort figures below are human-developer days. With parallel agents B1 + B2 are about 2-4 h
  of agent wall-clock plus build and GPU-gate time.

## Report (sweep agent, 2026-10-17, read-only, merge-v1.4.02 @ ad9d1a5d)

I swept src/VideoProcessor-Lib, src/VideoProcessor-GUI, src/VideoProcessor-Config and shaders/ (508 source files) for rate, resolution, pixel-size and frame-count literals. Each candidate was then read in context. The counts are approximate, because many hits are comments.

**Summary counts (about 430 hits examined)**
- **(a) Legitimate: about 330.**
  - DeckLink mode tables (69, in BlackMagicDeckLinkTranslate.cpp).
  - 525/625/709 colour mapping (MagewellCaptureDevice.cpp:657-671).
  - PQ constants and 4096-entry LUTs.
  - The NVOF 4x4 hardware grid.
  - The N*1000/1001 family detectors (DisplayTopologySession.h:40, DejudderPolicy.h:113).
  - Scan budgets and log-only text.
  - Already-relative sampling steps (width/480 and height/270 crop tolerance; height/1080 row steps).
  - Retry back-offs already in ms, including the **Magewell back-off**: `RecoverableFaultBackoffMs`, 250 ms doubling to 30 s, MagewellCaptureDevice.cpp:1896. This one is fine.
- **(b) Should be derived: about 45 sites, about 20 distinct rules.** The 10–240 Hz bound alone is copied about 39 times.
- **(c) Taste defaults not yet in time or relative units: about 40.**
  - Frame-count log throttles (`% 120`, `% 300`, `% 600`) make up about 17 of these. They are low risk.

**Already fixed today (no action needed):**
- LED look-ahead now uses the measured rate (LibplaceboVideoRenderer.cpp:19965 → LedDriveModule.cpp:345).
- LED fade is in ms; the legacy `fade_frames` is read at 24p (LedDriveConfig.h:1031).
- The de-judder fade window and `strengthRate` are converted from a 23.976 reference (DejudderWarp.cpp:2365, 3552).
- The pixel settings are scaled from a 2160 reference (DejudderWarp.cpp:2351).
- Deband radius scales from a 1080 reference (LibplaceboVideoRenderer.cpp:19290).
- The pulldown lock is in seconds plus cycles (PulldownDetector.h:158-162).

**Ranked top 25 (most likely to misbehave on odd rasters, 1440p, 48p, 100/120+ Hz, VRR)**

Each entry gives: what to change it to / the risk of changing it / the test that proves the default picture is unchanged.

1. **VideoProcessorDlg.cpp:2003 and :2098 (b).** The DWM measured refresh is accepted only between 20 and 120 Hz, so 144/165/240 Hz displays get no measured rate.
   - Change: use `DisplayMode::Minimum/MaximumRefreshRateHz()` (one shared display policy).
   - Risk: low.
   - Test: a unit test of the estimator at 60, 120, 144 and 240; check the log rate at 60 Hz is unchanged.
2. **InterpolationPolicy.h:98-105 (b).** The "measured" film class is `ratio >= 1.95`, which depends on the display. 60p sport on a 120 Hz display becomes "film-like", against the owner rule that 50/60 is never de-juddered.
   - Change: classify from the measured source rate and the pulldown result only, never from the display ratio.
   - Risk: medium.
   - Test: policy table tests for 24/25/30/48/50/60 × 50/60/100/120/144.
3. **DejudderPolicy.h:79, :134, :174 (b).** `IsFilmLike` uses fixed bands (≤30.5, 47.5–48.5). `EffectiveStrength` and `Decide` call the fixed version even when the `measured` class is selected. Only the interpolator honours the switch.
   - Change: one shared classifier, as in #2.
   - Risk: medium. The default is "legacy", so it is unchanged unless switched.
   - Test: `NativeAndNewSettingsOffArePixelIdentical`, plus a classifier table.
4. **The 10–240 Hz bound copied about 39 times** (CBufferedLiveSourceVideoOutputPin.cpp:901, 1017, 3460, 3787; DirectShowVideoRenderer.cpp:2560; OsdTimingPolicy.h:36; AlphaQueuePolicy.h:115; RationalLiveOutputSequencer.cpp:231; AlphaCadenceCorrectionPolicy.cpp:63; LibplaceboVideoRenderer.cpp:13842, 19965, 20948-20955, 21084; and others). There are two problems:
   - Source and display bounds are conflated.
   - LibplaceboVideoRenderer.cpp:13843 already uses 480 for the display.
   - Change: two named policies (source rate; display rate up to about 500) in DisplayMode.
   - Risk: low.
   - Test: a grep-free compile, plus unit tests at 360 Hz.
5. **DisplayRefreshRatePolicy.cpp:163 (b).** Doubled-rate ranking applies only for input between 24.1 and 31 Hz. 48p or 15p input gets no native-before-multiple ordering.
   - Change: rank by integer multiple of the measured input generally.
   - Risk: medium (mode switching).
   - Test: `RankDisplayRefreshModesForInput` tables for 24/25/30/48/50/60.
6. **QueueLatency.h:13 (b).** `MaxTargetFrames = 16` caps `target_latency_ms`. At 120p, 1000 ms needs 120 frames, so the cap silently wins.
   - Change: cap by ms and queue_size.
   - Risk: medium (memory).
   - Test: a FramesForLatency table; 24/50/60 values unchanged.
7. **The motion-ahead depth is in frames** (MotionAhead.h:19, kDefaultDepth 4; RendererProfileConfig.h:704; UnifiedProfileRuntime.cpp:1218; VideoProcessor.cfg target_frames 5 coupled to it). Four frames is 167 ms at 24p but 33 ms at 120p.
   - Change: ms, converted at the measured period, bounded by the queue.
   - Risk: medium (vector availability).
   - Test: the nvof-ahead tests; a log of the depth at 24/50/60.
8. **LibplaceboVideoRenderer.cpp:4843 (b).** EarlyUpload `slots = 8 + 3` is fixed. A larger target_frames or motion-ahead depth starves it.
   - Change: target + ahead + repeat + margin.
   - Risk: low (VRAM cap already applies).
   - Test: early-upload stats show `noFreeSlot` = 0 at 24/60.
9. **DejudderWarp.h:506 (c).** `HalvesForSearch` flips at 1527 lines (√2×1080). `cameraWindow` (DejudderWarp.cpp:3868), consistency and range change meaning at that step, so 1440p and 1600p behave discontinuously.
   - Change: make the half-texel-derived windows continuous in fraction of height.
   - Risk: medium.
   - Test: de-judder clip metrics at 1080/1440/2160.
10. **`consistencyPixels` (LibplaceboVideoRenderer.cpp:3036, default 2.0 half-res px) is not in `Scaled()`.** It is also reused as the **NVOF FB trust tolerance** (DejudderWarp.cpp:3310). It is fixed in pixels.
    - Change: add it to the referenceHeight scaling.
    - Risk: low at 2160, which is the reference.
    - Test: StreamFaults/NVOF trust tests; matcher dump identical at 2160.
11. **MotionAheadProducer.h:62, :78 (c).** `kBlockPx 8`, `kRange ±40` texels (half-res) are fixed.
    - Change: scale the range by height.
    - Risk: low.
    - Test: producer unit tests at 1080/2160.
12. **ConfigEditorWindow.cpp:5939 (b).** The "${cadence} == 49-61" button misses 100/120p and 48p.
    - Change: write a class-based rule (#2).
    - Risk: low (UI).
    - Test: the config tests.
13. **AlphaCadenceCorrectionPolicy.h:138-141 (b).** These are frame counts:
    - `COOLDOWN_FRAMES 120`;
    - stable/prediction samples 120/600;
    - `RATE_FILTER_ALPHA 1/600` per frame, a 25 s time constant at 24p but 5 s at 120p.
    - Change: seconds via the measured period.
    - Risk: medium.
    - Test: policy unit tests at 24/60/120.
14. **CBufferedLiveSourceVideoOutputPin.cpp:7646-7660 (b).** The startup target uses `fps < 30 ? max(5, …)`, with a 60 fps fallback.
    - Change: lead in ms, divided by the measured period.
    - Risk: medium (DirectShow path).
    - Test: preroll log at 24/50/60.
15. **CBufferedLiveSourceVideoOutputPin.cpp:250 (b).** The 50 ms delivery hold is sized for 23.976. Below about 20 fps it is shorter than one frame.
    - Change: max(50 ms, 1.2 × the measured period).
    - Risk: low.
16. **LiveEpochConvergenceController.h:127-133 (c).** `kMinimumIngressBlockUs` 30 ms, `kMinimumPacedPrimingDepth 8` frames, warm-up deliveries 12 frames.
    - Change: express these in ms.
    - Risk: medium.
    - Test: the convergence unit tests.
17. **GrainPreserve.h:61-62, 77, 91 (c).** Today's grain work: low-band sigma 3 px, half resolution, 16×16 guard blocks, `kGainMinPixels 16`, all in source pixels. Grain scales with the scan, so 4K film grain is about 2× larger in pixels.
    - Change: scale sigma and blocks by native height ÷ a 1080 reference (as the deband radius already does).
    - Risk: medium. It changes the 4K grain split, so put it behind a switch.
    - Test: the grain GPU test at 1080 must be byte-identical.
18. **VpDebander.h:45 (b).** `holdAlpha 0.5` per frame is never rate-converted.
    - Change: use `1-(1-α)^(ref/src)`, like `strengthRate`.
    - Risk: low.
    - Test: debander temporal test at 24 identical.
19. **DejudderWarp.cpp:2371 (c).** `holdHysteresis` is in frames at 23.976 (converted).
    - Change: expose the key in ms.
    - Risk: low.
20. **D2 motion scale and roll-off (LibplaceboVideoRenderer.cpp:977-993) (c).** These are "px per source frame"; the pixels are scaled but the per-frame unit is not. 48p halves the effective speed thresholds.
    - Needs an owner decision: px/frame (judder amplitude) or px/s.
21. **Temporal sigma-delta.glsl:29, :405 (c).** Its own search is ±64 output px with 16×16 output blocks, per refresh. At 4K output or 120 Hz the coverage differs.
    - Change: scale by output height. Only used when VPVEC is absent.
    - Risk: low.
22. **LibplaceboVideoRenderer.cpp:3874 (floor) vs DisplayRuleExpression.h:137 (round) vs PPMCorrectionLoader.cpp:241 (truncate) (b, consistency).** Three refresh-key conventions.
    - Change: one exact-rational matcher.
    - Risk: user cfg keys, so it needs migration.
23. **SourceResolutionMonitor.h:126-131 (c).** `kTile 256` px; 8–16 tiles.
    - Change: tile as a fraction of height.
    - Risk: low.
24. **Subtitle `offset_pixels` 15 and `gray_blur_px` 30 (RendererProfileConfig.h:600, 612) (c).** These are source pixels.
    - Change: fraction of picture height.
    - Risk: low. The owner rates subtitles low priority.
25. **Fallback rates (c):**
    - AlphaQueuePolicy.h:14 (60 Hz);
    - LedDriveModule.h:201 (1000/60);
    - CR210DolbyVisionTunnelFormatter.h:124 (23.976);
    - DejudderWarp.h:254 and LibplaceboVideoRenderer.cpp:3050 (reference 23.976);
    - RenderedWarp feather /2160 (LibplaceboVideoRenderer.cpp:10917).

    These are acceptable only as "not yet measured" values. Log when a fallback is used.

The ~17 frame-count log throttles (e.g. LibplaceboVideoRenderer.cpp:11824, 12175, 14352; CBufferedLiveSourceVideoOutputPin.cpp:5358, 6591) are (c). Convert them to seconds, at negligible risk.

**Common proof tests for every batch**
- `NativeAndNewSettingsOffArePixelIdentical`.
- `StreamFaultsKeepTheDejudderSafeAndRecovering`.
- Dump the matcher/settings (LibplaceboVideoRenderer.cpp:26920 block) at 1080p and 2160p × 24/50/60. Scaled values must be equal at the reference geometry and rate.
- The Beta TRX run.
- One chain-gate matrix pass before deploy.

**Batch plan (ordered by risk to unusual inputs, then by effort)**
- **B1: rate bounds and DWM (#1, #4, #25 logging).** One `RatePolicy` header (source / display / VRR range); replace the ~40 copies. Effort: 0.5–1 day. Lowest risk, biggest win for 144–240 Hz and VRR.
- **B2: film/video classification (#2, #3, #12).** One measured classifier (source rate plus pulldown, display-independent), legacy kept switchable. Effort: 1–1.5 days. It needs the owner to confirm 25p, 30p and 48p classes.
- **B3: queue, latency and look-ahead in time (#6, #7, #8, #14, #15, #16).**
  - `motion_ahead_ms` and a latency cap in ms; EarlyUpload slots derived.
  - The DirectShow preroll and hold in ms.
  - Effort: 2–3 days. Medium risk; test DirectShow and VP-renderer separately, pinned and normal.
- **B4: de-judder geometry (#9, #10, #11, #19, #20, #23).**
  - Add consistency, NVOF FB tolerance, producer range and tiles to the referenceHeight scaling.
  - Make the half-search windows continuous.
  - Hysteresis in ms.
  - Effort: 2 days. Prove the 2160 dump is unchanged, then check 1440 and 1080 metrics on real clips.
- **B5: cadence-correction timing (#13).** Seconds-based constants. Effort: 0.5 day.
- **B6: grain and deband scale (#17, #18).** Grain sigma/blocks scaled by native height behind a switch; deband hold rate-converted. Effort: 1 day, plus a GPU grain run (on mains).
- **B7: output-side pixels and keys (#21, #22, #24, log throttles).** Effort: 1–1.5 days. The refresh-key unification needs cfg migration and owner sign-off.

Total: about 8–11 working days. B1 and B2 first, because they are the ones that produce outright wrong behaviour on 120+ Hz displays and on 48p or 60p-on-120 inputs.

**Owner questions**
1. Should the D2 motion scale and roll-off be px per frame or px per second?
2. Are 25p and 30p film-class by default under the measured classifier?
3. May the refresh-key convention change, given that it affects user cfg?
