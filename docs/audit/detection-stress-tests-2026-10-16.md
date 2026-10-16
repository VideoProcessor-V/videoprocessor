# Stage 12 detection: stress tests run so far (for the planner)

Owner request, 2026-10-16: "create a list of detection stress tests that you've assembled and verified against and get the planner to read and suggest any additional ones we need."

**Scope.** The source-resolution detector (`SourceResolutionDetector`), its tracker, the GPU monitor with auto engage/disengage (`SourceResolutionMonitor`), and the coding readings (bitrate quality, effective bit depth) that the debander and the S8-2 ramp use.

**Status.**
- Every test below passes unless it is marked OPEN.
- Branch merge-v1.4.02, commits bbb04be0 and 7eddcb4e.

## How detection works (one paragraph)

**Per frame.**
- An upscale makes the picture's spectrum mirror-coherent about the source rate r. The content at f and at r−f stays phase-locked.
- Coherence is measured on 256-px tiles, on both axes, over a dense sweep of r. There is no list of fixed heights.
- A lower reading needs both axes.
- The block-coding grids (8, 16 and 32 px) are excluded as coding signatures.

**Over time (the tracker).**
- A lower reading needs at least 3 shots and 20 s of evidence.
- Detail above the declared rate returns to native at once.
- Discontinuities reset the tracker: seek, gap, time running backwards, format change.
- Repeated or paused frames add nothing. Flat or dark frames carry no evidence.

**Auto.**
- Engages at a cut or during a black/flat frame.
- Disengages at once on detail.
- When confidence drops, it waits for a cut or a black frame, for at most 5 s.

**Detection itself uses no motion vectors.** It is a statistical reading. The stages that add detail use the vectors (see the last section).

## A. Single-frame readings: synthetic pictures (`SourceResolutionDetectorTests`)

| # | Test | What it stresses | Result |
|---|---|---|---|
| A1 | RealFourKReadsNative | Native 4K-like pictures: sharp, grainy, defocused, and 8-bit | All read native |
| A2 | FlatAndDarkTilesCarryNoEvidence | Flat, black and near-black tiles | Not informative; no reading |
| A3 | UpscaledSourcesReadTheirHeight | 1440 / 1080 / 900 / 720 lines × Lanczos-3 / Catmull-Rom / Mitchell / bilinear | Height to within 0.01 in every case |
| A4 | OneAxisResampledStaysNative | Native detail on one axis, the other axis upscaled | Native (detail on either axis keeps native) |
| A5 | UpscaledWithCodingAndGrain | 8-bit upscale; 8×8 DCT coding q1–q4 after the upscale; re-grain of 0.5–4 codes | The height or native, never a wrong height |
| A6 | SdrRemappedToPqKeepsItsReading | Apple TV with Match Dynamic Range off and DV active (SDR re-encoded into PQ) | 4K stays native; a 1080 upscale still reads 1080 |
| A7 | BlockedFrameReadsLowQualityAndCleanStaysHigh | A hard-coded sky against a clean frame | Blocked reads below 0.5; clean reads above 0.8 |
| A8 | EffectiveBitDepthReadsEightBitContentInATenBitSignal | 8-bit content in a 10-bit signal, also via SDR→PQ; true 10-bit, with and without grain | 8 reads 8; 10 reads 10 |
| A9 | ShallowEightBitSkyReadsAsBanded | A shallow 8-bit gradient | Banding detected |

## B. Real footage gate (`RealFootageReadings`; needs `VP_TEST_REAL_FOOTAGE`)

**Clips (7).**
- UVG 3840×2160 at 120 fps: Bosphorus, Jockey, ReadySetGo, YachtRide, ShakeNDry. These are 10-bit YUV, and in fact vertically scaled with 8-bit content.
- Xiph park_joy and crowd_run at 2160p50, 8-bit.

Two frames are taken from each clip, read on 12 tiles per frame.

**Cases per frame.**
- Native.
- Native coded with 8×8 DCT at q2 and at q6.
- Upscales: 4 heights × 4 kernels, from a Lanczos master that is quantised and then upscaled.
- Coded upscales at q4 (Lanczos and Catmull-Rom).

**Results.**
- **Native:** 0 of 42 frames read a lower height (no false positives).
- **Uncoded upscales:** exact.
- **Coded 1080:** 26 of 28 read 1080; the other 2 read native, which is the safe direction.
- **Detail above (re-run 2026-10-16):** in every frame, real 4K holds at least 0.18 decades more fine detail above 1080's limit than that frame's own 1080 upscales (the margin is 0.07). Absolute levels overlap across frames (native lowest -0.96, upscales highest -1.04), which is why the comparison is relative. Still 0 of 42 native frames read lower.

## C. Tracker over time (synthetic readings)

| # | Test | What it stresses | Result |
|---|---|---|---|
| C1 | TrackerNeedsShotsAndTimeBeforeALowerReading | Evidence standard (3 shots, 20 s); one frame of detail | Not declared before 20 s; native at once on detail |
| C2 | TrackerKeepsNativeThroughSoftShotsOfARealFilm | 10 minutes of soft shots that read 0.5, with a sharp shot every 90 s | Never leaves native |
| C3 | TrackerDisengagesOnSkipsSourceChangesAndPauses | Reported seek; unreported skip (time backwards, a gap); pause (identical frames, flagged repeats); minutes of flat/dark frames | Native after each skip; a pause builds nothing; dark frames carry no evidence |

## D. Variable-bitrate streaming (new 2026-10-16, synthetic codec model)

**The coder model** (`CodeBlocks`):
- 8 / 16 / 32-px square transforms;
- frequency-weighted quantisation;
- an in-loop deblocking filter.

| # | Test | What it stresses | Result |
|---|---|---|---|
| D1 | DetailAboveSeparatesStarvedFramesFromNativeDetail | Fine detail above the rate for native 4K (sharp/soft, q0–q4, 8/32 transforms) vs upscales (3 rates × 4 kernels × q0/q4/q12) | Sharp 4K stands above every upscale. Gap: 0.69 decades at 1080, 0.19 at 1440, 0.10 at 3/4. Coding moves an upscale's level by ≤0.05. Soft 4K can sit at upscale levels; such a frame then counts as no evidence, never as a lower reading |
| D2 | VariableBitrateStreamsReadTheirSource | 12 shots of 6 s, each with its own bitrate (q 0.5–16) and transform size; I-frames every 4th frame (q×0.6), P/B frames at q×1.3; analysed at 2 Hz | All four pass: native stays native; 1080 Catmull, 1080 Lanczos and 1440 bilinear are declared at 20 s and held through the starved shots. (Fixed on the way: under q~21 on 32-px blocks one axis's peak sat at 29/32, 0.512 against the true rate's 0.509; the axes now agree on a rate one axis holds within 0.05 of its own peak) |
| D3 | StreamSwitchingRenditionsWithoutACut | Adaptive streaming drops from the 4K rendition to 1080 mid-shot, then recovers | Down: declared 120 s after the switch (once the native evidence leaves the rolling window). Up: native on the first 4K frame |

**Found and fixed while building D.**
- **16- and 32-px grids.** HEVC/AV1 16×16 transforms made frames read 15/16 (0.937), which counts as native and would have disengaged auto. The 16- and 32-px grids are now excluded like the 8×8 grid.
- **Starved frames.** A frame whose upscale signature is erased by coding is no longer treated as native detail.

## E. GPU monitor and auto (`LibplaceboLutParserTests`, GPU)

| # | Test | What it stresses | Result |
|---|---|---|---|
| E1 | SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce | Real GPU tile capture at 2 Hz on upscaled shots | Declares 540 on a 1080 frame |
| | | Engage timing | Waits for the next cut |
| | | Detail appears | Disengages at once |
| | | Confidence drop | Native at the next cut or within 5 s |
| | | Fade to black with no cut | Engages at the fade's dark end |
| | | Seek | Discontinuity |
| | | History files | Per session; the newest 5 kept |
| | | Genuine full-resolution content over many shots | Never engages |
| E2 | SourceResolutionRebasesThroughTheRenderer | The whole renderer path: manual, auto, early capture upload | Re-based output matches the native chart |
| E3 | NativeAndNewSettingsOffArePixelIdentical | With the new settings off | Pixel-identical to before |

## F. Not yet covered: please suggest more

1. **Real codec encodes.** Everything in D uses a synthetic DCT model. Real x264 / x265 / AV1 encodes (CRF and capped-VBR ladders, film-grain synthesis, adaptive quantisation, B-pyramids) need ffmpeg, which is not installed. Downloading it needs the owner's permission.
2. **Real streaming captures.** Apple TV app output at known renditions, e.g. a title with a known 1080 master. This also covers the player's own scaling and any sharpening it adds.
3. **Sharpened upscales.** TV-style or streaming-side sharpening/enhancement after the upscale.
4. **Native-resolution overlays on upscaled video.** Subtitles, player UI, logos, menus and scrolling credits.
5. **Crossfades/dissolves** between a native shot and an upscaled one.
6. **Interlaced, telecined and deinterlaced sources**, and 50/60p sports.
7. **Picture geometry.** Letterbox, pillarbox and window-boxed pictures; crops that are not multiples of the block grid; anamorphic ratios.
8. **Starved 4K.** Soft or starved native 4K over long stretches (it must never read lower). The faster-switching Picture-slider positions.
9. **Underlying 8-bit** (owner): most streaming carries 8-bit content in a 10/12-bit signal. A8 covers the reading, and the debander's threshold already scales with the effective bits. There is no test yet on a real 8-bit stream through the full chain.
10. **Variable bitrate and banding** (owner): the debander follows the bitrate-quality reading. That reading is smoothed over about 2 s and resets at cuts, so I/P pumping is averaged and each scene gets its own level. There is no test of debander stability under per-frame I/P quality alternation yet.
11. **Encoders alternating detail between frames** (owner: "encoding alternates detail between one frame and the next to average it in"). The added detail (CNN/SSIM residuals) and the debander's blur are held along the trusted motion vectors, which averages detail along the motion. There is no direct test with I/P detail pumping yet. A motion-compensated temporal accumulation of source detail is a possible later addition: planner's view wanted.
12. **Rendition switch latency.** A down-switch currently takes the 120 s rolling window. Should a confident rendition change (a sudden, sustained loss of detail above at no cut) restart the window?

## G18 result: real codecs (2026-10-16, `RealCodecReadings`, tools/make_codec_corpus.py)

**Corpus.** 126 real encodes of the 7 clips, made with ffmpeg 9.0.2:
- x265 at CRF 20/26/32;
- x264 at CRF 18/23/28;
- SVT-AV1 at CRF 35 with film-grain synthesis.

4 P/B frames of each were decoded: 504 frames in all.

| Case | Frames | Read the height | Native | Wrong |
|---|---|---|---|---|
| Native 4K, all codecs and CRFs | 196 | — | 196 | 0 |
| 1080 rendition, upscaled by the player (bicubic or Lanczos), all codecs incl. AV1 + FGS | 224 | 224 | 0 | 0 |
| 1080 master upscaled, then coded at 4K (x265 CRF 20/26/32) | 84 | 0 | 84 | 0 |

**Invariant held.** 0 native frames read lower, and no upscale read a wrong height.

**New gap.** A "4K" stream of a 1080-mastered title is never detected. It is the safe direction, but a common case (2K-DI films streamed in 4K). The 4K encoder removes the upscaler's faint images above the source Nyquist, and those images carry the mirror coherence.

**Proposed next signal.** A sharp, consistent spectral cliff at a stable rate across frames and shots, gated strictly so soft native 4K never triggers it. To be added after the look-ahead work.

### The G18 gap investigated (2026-10-16): a 1080 master upscaled, then coded at 4K

Two second signals were tried on the real-codec corpus. Neither separates the cases safely.

1. **Spectral cliff** (the log-power drop across a candidate Nyquist, min over the axes).
   - Native: 0.29–0.74 at 1080.
   - 2K master coded at 4K: 0.26–0.82.
   - Overlapping. Bicubic's soft transition plus coding leaves no sharp cliff.
2. **Time-averaged mirror coherence** (the curve summed over frames, the level at 1/2 above the curve's floor).
   - 2K master coded at 4K: +0.018 to +0.044.
   - Native x264: +0.060 to +0.066, because its 8×8 coding grid lights 4/8.
   - Native x265: +0.001 to +0.007.
   - Native AV1 with FGS: +0.046.
   - No separation. The 4K encoder removes the images above the source Nyquist that carry the signature.

**Conclusion.** It stays native: the safe direction, a known limit. The real Apple TV captures (G19) will show how common it is and what survives in practice. **For the planner:** ideas beyond spectral statistics?

## Planner priority items: results (2026-10-16)

| Item | Result |
|---|---|
| **F12** rendition-change event | Built in the tracker. At least 6 consecutive frames read the same lower rate with no cut, and their detail above that rate fell at least 0.3 decades below the same shot's native frames. The older evidence is cleared and 10 s of evidence (time only) declares. D3 (switch mid-shot): declared **9.5 s** after the switch (was 120 s). C2 (soft shots) still passes; synthetic soft shots carry no spectra, so no event. |
| **G1** periodic content | `PeriodicNativeContentReadsNative`: zone plate, fences (7, 10, 16 + grain), grille 12 × 9, bricks 24 × 10, soft blinds, text columns. All native on both axes and on each axis alone, with three guards:<br>• an axis reads lower only with fewer than 2 strong peaks that are neither harmonics of its rate nor coding-grid positions (the comb of exactly periodic content);<br>• no unrelated candidate outranking its peak by more than 0.1;<br>• a single-axis reading at most 0.8 (periodic (P−1)/P, e.g. 8/9 from text).<br>One-dimensional patterns carry no evidence (the other axis has no texture). Unchanged: real footage 0/42 native read lower, real codecs 0 wrong, all upscale tests. |
| **G3** two-step upscales | `TwoStepUpscaleReadsTheLowestRateOrNative`: 720 → 1080 → 2160 reads **0.333 (720)**; 480 → 1080 → 2160 reads native (allowed). Never the intermediate 1080. |
| **G5** scope sources | By design: readings are rates, not heights, measured on tiles inside the active picture (the monitor samples within the crop). An 800-line picture in a 1080 frame upscaled 2× reads 0.5, and the re-base takes the frame back to its 1080 frame. The stats line's "~1080" is the frame-equivalent height. |
| **G8** alternating native / archive shots | `AlternatingNativeAndArchiveShotsDoNotThrash`: **0 changes in 4 minutes**, native on every interview shot. The native frames in the window keep the lower reading from forming. |
| **G12** tile capture with de-judder extras | By design: `sourceMonitor.Frame` is called once per new source frame on the native, unwarped, not re-based `renderImage` (the main path). The extras path warps separately and never feeds the monitor. |

**Also built:**
- **Per-axis re-base:** a one-axis upscale is undone on that axis, the aspect ratio unchanged through the renderer.
- **Kernel-matched re-base** for 1080p: the player's kernel identified and inverted, +8 to +15 dB on real upscales.

### More planner items (2026-10-16 evening)

| Item | Result |
|---|---|
| **G10** long dark film | `LongDarkFilmStaysNative`: 20 minutes of mostly dark frames plus one 10 s bright upscaled shot: native throughout. |
| **G11** freeze frame | `FreezeFrameNeitherDeclaresNorResets`: 30 s frozen on an upscaled shot adds no evidence time, keeps the earlier evidence, and causes no discontinuity. |
| **G16** effective bits with dither | `EffectiveBitsWithDither`: 8-bit content with a full-step dither to 10 bits reads **10.0** (the dither is the extra bit). A quarter-step dither reads **9.0**. A per-frame reading cannot tell a temporal dither; a player's small dither stays near 8. |
| **G17** clean low-detail content | `CleanLowDetailContentIsNotPoor`: flat animation quality 0.94; a clean white studio read **0.16 ("poor") — a real false alarm**. A gentle 10-bit gradient forms two-level 8×8 blocks one code apart, and these were counted as band edges, which would have turned the debander up on a flawless picture. **Fixed:** in a 10-bit or deeper signal a band edge needs at least a 2-code step. The white studio now reads 1.00, while 8-bit-in-10 skies and hard-coded skies still read banded. Detector 26/26, debander 7/7, real footage unchanged. |

### Subtitles and layout (owner 2026-10-16: "what about with the subtitles")

A player that upscales 1080p video draws its own subtitles at 4K: sharp text over a soft picture. Test pattern: `SubtitleTextTestPattern.h` (white strokes, black outline, anti-aliased, limited-range levels; 40/56/80-px text).

| Check | Result |
|---|---|
| Subtitle features vs the re-base (by design) | Subtitle box detection, cut/paste relocation, black-bar detection and NLS geometry all decide in native pixels on the original frame. The relocated subtitle is composed before the re-base, and the re-base only rescales the crop. |
| Detection, one frame | `SubtitleTextOverAnUpscaleNeverReadsAWrongRate`, 8 tiles. **Without a guard:** text in 4/8 tiles read **0.66** (40-px text) instead of 0.5, and text in every tile read native. Never below the true rate. |
| Detection, over time — **a real defect, fixed** | `SubtitlesComingAndGoingDoNotFlipTheDeclaration`: two minutes, subtitles on and off. **Without a guard** 63 % of subtitled frames read native (text is genuine 4K detail), the upscale was **never declared**, and once declared a native frame switches the re-base off at once (bias to native). **Fix:** `source_resolution_text_guard` (Fixes under test, default on). A tile whose sharp steps (neighbours a third of the range apart) sit at least 80 % in one band a quarter of the tile high is left out of that frame's reading; leaving tiles out only loses evidence. **With the guard:** every reading 0.5, declared at 22 s, never thrown back. Real footage and the real-codec corpus: **0 tiles** left out; detector suite 38/38. |
| Text quality through the re-base | `SourceRebasePlayerSubtitleText` (2:1, shown back at 4K with a Lanczos-3 proxy). Both filters soften 4K-drawn text and ring at its edges: kernel inverse 98.8 codes RMS in the text, worst overshoot 258 codes; polar filter 112.8 codes RMS, worst 175. Either way the text is roughly as if the player had drawn it at 1080. The picture itself: inverse 0.25 codes, polar 21. **Owner decision (2026-10-16):** subtitles need only be legible (they are, roughly as text drawn at 1080), fidelity is low priority: no native-text overlay (option 3) or text-area filter (option 2). The overlay route (libplacebo `pl_overlay`, text boxes found on the queued frames ahead) is noted here if ever wanted; it would not follow NLS's warp. |
| Layout with crop, zoom, NLS, edge blanking | `RebaseKeepsTheLayoutWithCropZoomNlsAndBlanking`, 4K output, re-base on and off: scope screen with touch inside; scope screen with NLS (stretch 1.32); letterboxed 2.39 source with auto crop; all three with edge blanking 48/24/16. Picture bounds within 1 px (the softer bottom edge), **0 px shift across**, 1 px down (the test pattern's sawtooth, not a move). NLS: the re-based sides keep 91 % of the centre's relative sharpness (NLS resamples the coarser grid) — small, synthetic pattern. Needed a test-only `VP_TEST_SHADER_DIR` so the runner finds NLS.glsl. |
