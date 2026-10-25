# Fixed-constant inventory (2026-10-18)

Owner rule ([[vp-general-not-fixed]]): never hard-code refresh rates, frame rates, cadences or
resolutions; derive them from the measured rates. This inventory is taken at integ/2026-10-17
74340728 (timing merge) on branch feat/no-fixed-constants. It follows
docs/VP-fixed-constant-sweep-2026-10-17.md (the 25-item ranked list and batches B1-B7) and
records what this batch converted, what was already converted by earlier work, and what remains.

## Method

- A scan of every product source (src/VideoProcessor-Lib, -GUI, -Config, -ConfigCore, -VPRenderer,
  -StreamRecorder, -Update, -OutputProbe; tests excluded), with comments and string literals stripped,
  for rate literals (24, 25, 30, 48, 50, 60, 100, 120, 144, 240), the 1000/1001 family
  (23.976, 29.97, 59.94, 47.952, 119.88, 1001) and resolution literals (720, 1080, 1440, 1920, 2160,
  3840, 4096, ...). 856 code lines matched. A second scan listed frame-count constants
  (`*_FRAMES`, `*Samples`, `% 120` log throttles): 25 constants and 26 throttles.
- Each matched line was classified by rule and the rate- or resolution-bearing ones were then read
  in context. The counts are approximate where a rule classified a group (the UI and level tables);
  every rate-, cadence- and resolution-bearing site is listed below by name.

## Counts by class (856 scanned code lines)

| Class | Lines | Action |
|---|---:|---|
| Not a rate or resolution (UI geometry, LED levels, code values, percentages, byte/bit widths, array sizes, buffer counts, ms timeouts) | ~650 | none |
| Real standard / physical constants | 138 | keep, commented |
| Reference geometry / rate for relative scaling (2160 / 1080 / 23.976 references, already proportional) | 13 + ~15 relative steps | keep |
| Unmeasured-rate fallbacks (60 Hz / 24p values used only until a rate is measured) | 26 | keep (logging deferred, B7) |
| Already expressed in seconds / ms (windows, holds, back-offs) | ~20 | none |
| Rate bounds | 2 + 2 escapes | **converted (B1)** |
| Fixed film / video bands and rate bands | 3 sites | **2 converted (B2)**, 1 deferred |
| Frame-count windows that should be time | 13 scanned + 25 constants | **1 converted** (gap-fill span); 4 already converted by the timing merge; rest deferred (B3-B7) |
| Frame-count log throttles (`% 120`, ...) | 11 code sites (26 incl. GUI/DS) | deferred (B7, log only) |

## Converted in this batch

Behaviour changes are behind one renderer-profile switch, **`general_rate_fixes`** (on | off,
default **on**; Config: Fixes under test, "General frame-rate rules"). Pure correctness / refactor
changes have no switch.

1. **Film / video class without fixed bands** (sweep #3, B2) - behaviour, switched.
   - `DejudderPolicy::IsFilmLike(hz, generalRates)` (VP-V/modules/motion/vector_delivery/DejudderPolicy.h:107) replaces the
     fixed bands `<= 30.5` and `47.5-48.5` (kept as the legacy `IsFilmLike(hz)`, :88) with one
     standard constant: video starts at the lowest broadcast video picture rate, 50 Hz (the 625/50
     family), less a 2% relative measurement tolerance (`VideoRateFloorHz()` = 49 Hz, :101-106).
   - `DejudderPolicy::Input::generalRates` (:69) feeds `EffectiveStrength` (:164) and `Decide` (:204);
     `InterpolationPolicy::Input::generalRates` and `IsFilmLike(..., generalRates)`
     (vprenderer/InterpolationPolicy.h:106-112, `DejudderTakes`) use the same rule.
   - Renderer: LibplaceboVideoRenderer.cpp:14129 (de-judder) and :14159 (interpolator).
   - Unchanged: every standard rate (23.976 ... 48 film; 50, 59.94, 60, 100, 119.88, 120, 144, 240
     video). Changed: odd rates 30.5-47.5 and 48.5-49 become film-like (were video).
2. **Gap-fill span as a time** (owner 2026-10-18) - behaviour, switched.
   - `InterpolationTimeline::kMaxSpanSeconds` = the legacy 4 periods at the owner's 23.976p
     reference = 166.8 ms; `MaxSpanFrames(rate, inTime)` = whole periods that fit
     (vprenderer/InterpolationTimeline.h:44-58): 4 at 23.976/24/25 (unchanged), 5 at 29.97/30,
     8 at 48/50, 10 at 59.94/60, 20 at 119.88/120, 40 at 240; 4 (legacy) when off or the rate is
     unknown.
   - `MaxPairSpanFrames(rate, displayHz)` (:63) additionally caps a gap PAIR to the periods that
     `kMaxOutputs` (16) refreshes cover, so a high-rate pair (120p on 120 Hz: 20 periods) is not
     offered only to fall back to a single present; the owner's paths are unaffected (23.976p on
     60 Hz: 4; 60p on 60 Hz: 10). The timeline's restart rule keeps the uncapped time span.
   - `GapFillPair(..., maxSpanFrames)` (:78), `Timeline::Plan` restart and span (:121) and
     `SetSpanInTime`. Renderer: the dequeue's span check and gap pair
     (LibplaceboVideoRenderer.cpp:27584-27612, `MaxPairSpanFrames` with the timeline display rate), the estimate's gap-pair check (:14254, the pair's own
     span kept in `vpFillMaxSpan`, :5832), the timeline (:27981).
   - Config: the Fill dropped frames choice and tooltip (ConfigEditorWindow.cpp:8515-8522) now say
     "gaps up to about 1/6 s (1-3 frames at 24p, up to 4 at 30p, up to 9 at 60p)" instead of the
     fixed "1-3 missed frames".
3. **Low-latency "50/60 Hz" queue rule** (sweep #12, B2) - Config only, no switch.
   - The queue button now writes the class rule `${cadence} >= 49` (the same 49 Hz video floor)
     instead of `${cadence} == 49-61`, which missed 100/120/144/240 Hz video
     (ConfigEditorWindow.cpp:6157-6215). A saved `49-61` rule still shows as this button and is kept
     as written until the button is clicked. Label: "50 Hz and faster sources".
4. **Display-rate bound escapes** (sweep #4, B1) - correctness, no switch.
   - CBufferedLiveSourceVideoOutputPin.cpp:1018: the scene-timing phase accepted 10-240 Hz
     (`qpcFrequency / 240`); now `DisplayRefreshBounds::AcceptsPeriod` (DisplayRefreshRatePolicy.h:117,
     10-500 Hz) so 360/480 Hz panels keep their phase. Changes nothing at or below 240 Hz.
   - AlphaPresentationTelemetry.cpp:19-21: copied 10/500 literals replaced by DisplayRefreshBounds
     (numerically identical, NaN semantics preserved).
   - The `NoCopiedRateBoundLiterals` test (QueueTimingFixesTests.cpp) now also catches both shapes
     (a clock divided by a rate bound; expected/observed compared with a bound).

## Already converted before this batch (verified at 74340728)

- #1 DWM refresh 20-120 Hz -> `DisplayRefreshBounds::Accepts` (render-ahead, 2026-10-17).
- #2 Display-ratio film class -> source-only class (`cadence_class_fixes`, hunt 2).
- #4 ~39 copies of 10-240 Hz -> `DisplayRefreshBounds` / `CaptureRateBounds` (hunt 2 C3), except the
  two escapes above.
- #8 EarlyUpload slots `8 + 3` -> `AlphaQueuePolicy::EarlyUploadSlots(target)`.
- #13 AlphaCadenceCorrectionPolicy frame counts -> seconds (`queue_timing_fixes`, timing merge).
- #23 SourceResolutionMonitor `kTile 256` -> `SourceResolution::TileSize(height)` (latch fixes).
- 15 fps pulldown floor -> exclusive with a relative margin (`cadence_class_fixes` H1).
- LED look-ahead, LED fade (ms), de-judder fade/strength rate (23.976 reference), pixel settings
  (2160 reference), deband radius (1080 reference), pulldown lock (seconds + cycles), the latency
  floor window (5 s), CR210 DV packet-loss / SDR-fallback limits (derived from the frame rate).

## Real constants kept (standard or physical; most already commented in place)

- DeckLink display-mode tables (BlackMagicDeckLinkTranslate.cpp, ~104 lines): SMPTE/ITU modes.
- The 1000/1001 family: `DejudderPolicy::IsRate1001`, DisplayTopologySession, the 23.976 rate
  references (`referenceRateHz`, `kSpanReferenceRateHz`): NTSC-derived standard ratio.
- `DejudderPolicy::kVideoRateFloorHz` = 50: the 625/50 television family (new, this batch).
- `RateIs625Family` (ColorSpace.cpp) and BT.601 525/625 line counts; Magewell `height >= 720` HD
  colourimetry: ITU-R BT.601 / BT.709 rules.
- PQ (SMPTE ST 2084) constants `2610/16384`, `2523/4096`, ... (SceneDescriptor.h, DolbyVisionTrims.h,
  LedRawTransfer.cpp), DV trim code scales `/4096`, 12-to-10-bit tables.
- `DisplayMode::ToString` / VideoProcessorDlg mode names (720p HD, 1080 Full HD, 2K/4K DCI): labels.
- Bounds as guards (not lists): `CaptureRateBounds` 10-240, `DisplayRefreshBounds` 10-500,
  `PulldownDetector::kMinFilmHz` 15, `SourceRebase::kMinimumHeight` 144.
- NVOF 4x4 hardware grid, shader workgroup and 12x12 block sizes (`/144.0` = a 12x12 SAD), DV
  packet layout bit shifts.
- References for relative scaling (proportional, the reference only fixes the unit):
  `referenceHeight` 2160 (DejudderWarp settings, RenderedWarp feather, overlay), the `/1080` and
  `/1920` sampling steps, `ScaledSearchRange` (`/1080`), GrainPreserve `planeLines / 2160`,
  VpDebander `1080 * radiusScale`, SourceResolutionDetector `height * 256 / 2160`.

## Remaining derive / time items (deferred, by batch)

None of these is a list of rates that silently falls through; each is a taste value or a unit to
change. They touch the queue/timing code that just landed, files being merged (intent, detail, MPC),
or need an owner decision, so they are left for B3-B7.

| Sweep # | Site | Class | Why deferred |
|---|---|---|---|
| 5 | DisplayRefreshRatePolicy.cpp:163 doubled-rate fallback only for 24.1-31 Hz input | derive (integer multiples of the measured rate) | display mode switching; needs a GUI-level switch and owner sign-off |
| 6 | QueueLatency.h:13 `MaxTargetFrames 16` caps `target_latency_ms` | time (cap in ms + queue_size) | B3, queue code |
| 7 | MotionAhead.h:19 `kDefaultDepth 4`, `motion_ahead_frames` | time (ms at the measured period) | B3, queue/producer |
| 14 | CBufferedLiveSourceVideoOutputPin.cpp:7643-7650 startup target `fps < 30`, 60 fps fallback | time (lead in ms) | B3, DirectShow path |
| 15 | CBufferedLiveSourceVideoOutputPin.cpp:254 50 ms delivery hold | derive (max(50 ms, 1.2 periods)) | B3 |
| 16 | LiveEpochConvergenceController.h frame-count priming / warm-up | time | B3 |
| 9 | DejudderWarp.h:522 `HalvesForSearch` step at sqrt(2) x 1080 lines | resolution step (continuous windows) | B4, motion geometry |
| 10 | DejudderWarp.h:83 `consistencyPixels` not in `Scaled()` | resolution (reference height) | B4 |
| 11 | MotionAheadProducer.h:62/78 `kBlockPx 8`, `kRange 40` | resolution (fraction of height) | B4 |
| 19 | DejudderWarp `holdHysteresis` in frames | time (ms key) | B4 |
| 17 | GrainPreserve.h sigma / block sizes in source px | resolution (native height / 1080) | B6, grain files being merged |
| 18 | VpDebander.h `holdAlpha 0.5` per frame | rate (1-(1-a)^(ref/src)) | B6, detail work |
| 21 | Temporal sigma-delta search +-64 px, 16x16 blocks | resolution (output height) | B7 |
| 22 | Three refresh-key conventions (floor / round / truncate) | consistency | B7, cfg migration (owner approved) |
| 24 | Subtitle `offset_pixels` 15, `gray_blur_px` 30 | resolution (fraction of height) | B7, low priority |
| 25 | 26 unmeasured-rate fallbacks (60 Hz in AlphaQueuePolicy, ActivePicture*, AlphaSourceCropPolicy, BufferedPictureExpansion, LedDriveModule; 1/24 s in AdaptiveDownscale, SourceResolutionMonitor, customHook; 23.976 in CR210) | keep + log when used | B7 |
| - | 11 frame-count log throttles (`% 120`, `% 2400`, `% 240`) in the renderer and DS pin | time (seconds) | B7, log only, renderer conflicts |
| - | `InterpolationTimeline::kMaxOutputs 16` refreshes per source frame | budget (live GPU budget per source frame) | keep: a cost bound, not a rate |
| - | ContentClassifier.h (intent) | uses `InterpolationPolicy::IsFilmLike` defaults | intent files excluded from this batch; it takes the derived class (generalRates default) |

## Tests

src/VideoProcessor-Test/GeneralRateMatrixTests.cpp: classifier, de-judder and interpolation decisions,
gap span, timeline and bounds at 23.976, 24, 25, 29.97, 30, 47.952, 48, 50, 59.94, 60, 71.928, 100,
119.88, 120, 144 and 240 Hz plus odd rates, on displays 24-360 Hz, with the switch on and off; the
height-relative helpers at 720, 1080, 1440, 2160, DCI 4096x2160 and 4096x1716, 3996x2160 and the
anamorphic 1440x1080 / 720x576 / 720x480 rasters. Totals in docs/agent-reports/no-fixed-constants.md.

## B3-B7 status (2026-10-18, branch feat/no-fixed-constants-2)

Done behind `general_rate_fixes`: #6 queue time-target ceiling, #7 motion-ahead depth, #14/#15
DirectShow startup buffering and delivery hold, #16 convergence evidence, #9/#10/#11 de-judder geometry,
#18 debander hold, #22 one refresh-key convention with migration, #25 fallback logging, the frame-count
log throttles. Not done: #5 (proposal only: GUI switch + owner OK), #17 guard workgroup, #21, #24
(reasons in the report). Shared rules: src/VideoProcessor-Lib/GeneralRate.h, RefreshKey.h. Report:
docs/agent-reports/no-fixed-constants-2.md.
