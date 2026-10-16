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
