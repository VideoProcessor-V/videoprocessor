# Frame detail accumulation (design, for planner review)

Owner, 2026-10-16:
- "especially for the upscale and low bitstream since I'd imagine often encoding alternates detail between one frame and the next to average it in"
- then: "definitely want the frame detail accumulation. that would be extremely extremely cool if could be done efficiently and not just on a 5090".

Status: design only. Nothing is built.

## 1. What it is for

Streaming encoders do not spend the same bits on every frame:

- **I-frames (and reference frames) are coded finely.** The B/P frames between them predict from those frames and code a coarser residual. Fine texture therefore pulses: sharp on an I-frame, washed out over the next frames, sharp again.
- **Low-bitrate streams pulse most.** The encoder drops texture from the B frames almost entirely and relies on the viewer's eye to average it in.
- **Upscaled streams carry the same pulse.** In a 1080 rendition upscaled to 4K, the pulse sits within the 1080 band.

**What the viewer sees is "breathing" texture** (grass, skin, fabric, film grain) that resets at every GOP (group of pictures).

**What we can do.** The de-judder already knows where every 16×16 block moved between consecutive source frames, and how much each vector is trusted. Following the motion back, the detail of earlier, better-coded frames can be carried forward and blended with the current frame's own detail. Where the current frame lost texture the encoder had sent before, it comes back. Where the current frame has it, nothing changes much.

**What it must not do.** It must never invent detail:
- Only detail the stream actually sent, consistent along the motion, is accumulated.
- Anything that disagrees with the current frame is dropped at once: moving objects, occlusions, cuts, flashes.

## 2. The method (one recurrent pass, the existing vectors)

**State.** One history texture at the source signal's resolution, holding a detail band only. Luma first; chroma optional (§6).

**Per source frame** (not per refresh: a repeated refresh adds nothing):

1. **Band split.** Split the current luma into base and detail: `base = blur(cur)`, `detail = cur - base`. The blur is a small separable Gaussian whose radius follows the rate in question: the declared Stage 12 rate, or native.
   - Low frequencies always come from the current frame. Brightness, colour, fades and flashes therefore never lag or ghost.
   - Only the band the encoder pulses is accumulated.
2. **Warp the history.** Move it by the current vector field: bilinear, sub-pixel, per pixel from the 16×16 field with the warp's existing interpolation.
3. **Decide per pixel how much history to keep (`w`).** `w = 0` (current detail only) when any of these hold:
   - the vector is untrusted;
   - the frame trust is below the minimum;
   - vectors are invalid ([[vp-vector-validity]]: cut, resync, drop, repeat, seek);
   - the warped history falls outside the current frame's detail range. The test is the TAA-style neighbourhood clamp: the current 3×3 detail min/max, widened by the expected coding noise from the bitrate reading.

   Otherwise `w` is the confidence-weighted history share. Its ceiling is set by:
   - how starved this frame is relative to the history (per-frame detail energy against the history's energy, so an I-frame overwrites more and a starved B frame keeps more);
   - a strength setting. It is a slider, per the owner's scheme ([[vp-settings-slider-scheme]]).
4. **Blend.** `acc = lerp(detail, clamp(warped_history), w)`, written back as the new history.
5. **Output.** `out = base + acc`. This goes into the re-base (Stage 12) and into the existing downstream chain unchanged.

**Why this is cheap.**
- It costs one blur (2 taps × radius), one warp fetch, one 3×3 min/max, and the arithmetic, all in a single compute pass plus the separable blur.
- The motion vectors are already computed by the de-judder. No new matcher, no optical flow, no neural network.
- At 4K that is about 5 texture reads per pixel in total. Expected cost is 0.3–0.7 ms on the 3090, and less at a 1080 source rate. Budget: 1 ms at 4K on the 3090, measured on the laptop first.
- Memory: one 4K r16f history (16 MB), plus a ping-pong copy.

## 3. Where it sits in the chain

**Placement: source frame → [detail accumulation] → Stage 12 re-base → libplacebo / CNN / SSIM → output.**
- It runs before anything that adds or reshapes detail. The sharpeners therefore see a steady, fuller detail band. They already hold their own residual along the vectors (§2c), which removes their flicker; this removes the input's pulse.
- The debander runs after it. A steadier input lets the debander's band detection settle as well.
- The resolution monitor keeps reading the raw source, so detection is unchanged and independent.

## 4. When it is off or falls back

It follows [[vp-vector-refinements-fallback]]:
- **No de-judder vectors, so off.** It never starts the matcher itself. 50/60p will need [[vp-low-latency-vectors]].
- **The frame, or a block, is not trusted:** current detail only for that frame or block.
- **Discontinuities reset the history:** cut, resync, drop, repeat, seek, format change.
- **A clean, high-bitrate source accumulates almost nothing.** The current frame always matches its history, so `w` stays low and the output equals the input within a code. Clean Blu-ray-class sources are effectively untouched.

## 5. Tests (before any default)

**T1 — Synthetic I/P pulsing (CPU and GPU).**
- Setup:
  - textured, moving content with known truth;
  - the coder model from the VBR tests: I every N frames at q, B frames at 3q;
  - true motion vectors, then the matcher's vectors.
- Gate:
  - the per-frame detail energy's GOP pulse falls by at least half;
  - the error against truth falls on the B frames;
  - it never rises on the I-frames.

**T2 — Faithfulness.**
- Setup:
  - moving objects over a static textured background;
  - occlusion and reveal;
  - a flash;
  - a fade;
  - a cut mid-GOP.
- Gate:
  - no ghost trail beyond one code outside the clamp;
  - full reset at a cut;
  - no lag in base brightness.

**T3 — Clean source.**
- Setup: an uncoded source.
- Gate: output equals input within one code everywhere.

**T4 — Real clips.**
- Setup: the UVG and Xiph clips encoded with real x265 and AV1 (low-bitrate capped VBR, long GOP), via ffmpeg (owner approved the install, 2026-10-16).
- Gate: measured breathing (GOP-periodic detail energy) and SSIM against the uncompressed original, on the B frames.

**T5 — Cost on the laptop, then on the 3090, at 4K and at a 1080 source rate.**

## 6. Open questions for the planner

1. **Chroma too, or luma only?** Encoders starve chroma first, but chroma detail is visually weak. The proposal: luma first, chroma behind a switch.
2. **Accumulate at the declared Stage 12 rate or at the signal rate?** For an upscaled stream, the band above the declared rate holds only upscaler and coding residue, so it is cleaner to accumulate below the declared rate's Nyquist and leave the band above alone.
3. **Weights.** Should the per-frame weighting use a measure of the frame's coding quality (blockiness, detail energy against the history), or an I/B guess from the energy pulse?
4. **Sub-pixel motion across many frames** can recover a little real resolution from aliased sources (multi-frame super-resolution). Streaming masters are normally filtered, so the expected gain is small. Worth a later step, or leave it?
5. **Interaction with grain.** Film grain is new on every frame, so accumulating it averages it away. Proposal: the clamp plus a grain allowance (the de-judder's grain estimate) keep grain from being smoothed. Should grain stay fully per-frame?
6. **Future frames, using optical flow** (owner, 2026-10-16: "if we need vectors from multiple future frames, should we be using optical flow for that part").
   - **v1 (above)** is causal and needs only the consecutive-frame vectors the de-judder already has.
   - **v2** would also pull detail back from the next one or two frames, e.g. an upcoming I-frame lifting the soft B-frames before it. That needs vectors from the current frame to each future frame.
   - Our block matcher is costly per pair, and chaining consecutive vectors accumulates error.
   - NVIDIA's optical-flow engine (NVOFA) is the natural source. It is separate hardware from the shader cores, so it costs almost no render budget on the 3090. It gives 4×4 vectors plus a per-vector cost usable as trust. The SDK is in C:\dev\Optical_Flow_SDK_5.0.7.
   - It needs frames queued ahead (latency): film playback only, never low-latency 50/60p. Fall back to v1 when no frames are ahead or the hardware is absent.
   - Proposal: v1 first, v2 behind a switch afterwards. Planner's view?

## 7. What the optical-flow corpus means for accumulation (2026-10-16)

The motion corpus measured the engine's raw vectors (docs/VP-nvof-ahead.md):
- **Precision.** About 0.4 half-size pixels of error on trusted blocks (the matcher: 0.13), roughly 0.8 px at 4K.
- **Confident wrong matches.** It can confidently match a far, similar texture, as across a cut. The reach limit now rejects vectors beyond the searches' reach.

Owner, 2026-10-16: "are we going to have a problem with detail integration using the optical flow look ahead". The rules for accumulation:
- **v1 (causal)** uses the de-judder's finished field for (prev, cur): refined and trust-tested. It never uses raw engine vectors.
- **v2 (look-ahead).** The engine's vectors for (cur, cur+2) are refined before use: the matcher's half-pixel step (and the quarter-pixel step) around them, with no coarse search. This is done at render time; its cost is to be measured. They also pass the same safeguards:
  - the reach limit;
  - forward/backward consistency;
  - the engine's cost;
  - a reset at cuts and discontinuities.
- **The clamp is the backstop.** History detail is used only within the current frame's local detail range. A misaligned or wrong vector gives history that disagrees and is not used.
- **The per-frame weight** follows how well the warped detail predicts the current frame. A slightly misaligned contribution gets little weight instead of softening texture.
- **Test T1 gains a misalignment case.** Vectors deliberately offset by 0.5 and 1 px must not soften the result below the plain current frame.

## 8. Automatic enable / disable (owner, 2026-10-16: "if the detail recovery requires optical flow then it needs to be automatically disabled when it's off or not detected")

| State | Accumulation |
|---|---|
| Optical flow on, the producer's engine ready (pairs computed ahead) | v2 (look-ahead) and v1 |
| Optical flow off, or no engine (not NVIDIA, self-check failed, the producer stopped), or a low-latency profile | v1 only (causal, the de-judder's own vectors), automatically. Logged once: "detail accumulation: look-ahead off (no optical flow): causal only" |
| No de-judder vectors (the de-judder off, vectors invalid after a discontinuity, interlaced, unknown rates) | Off for those frames, automatically (the current frame unchanged) |

- **One source of truth.** The renderer's motion-ahead decision (`MotionAhead::Decide` and the producer's `EngineReady`) decides. Accumulation never starts the engine or the matcher itself ([[vp-vector-refinements-fallback]]).
- **Switching is per frame and smooth.** v2 to v1 drops the look-ahead term only, with no visible jump: the history stays. Off clears the history.

## 9. Resolution detection is untouched (owner, 2026-10-16: "make sure the detail recovery doesn't break the original resolution detection for upscaling")

- **Read before accumulation.** The source-resolution monitor samples the raw source frame (`sourceMonitor.Frame` on the unwarped, not re-based, not accumulated frame, once per source frame), and its look-ahead reads the queued frames. Accumulation is applied after that sampling and before the re-base, so detection never sees accumulated detail. Accumulated detail could otherwise add energy above the source rate and mask an upscale's signature, or look like native detail.
- **T6 (gate).** Detection readings (ratio, confidence, auto decisions) are identical with accumulation on and off: the renderer harness on the upscaled and native charts, and the real-codec corpus through the renderer.
- **The re-base factor is decided from the raw reading.** Accumulation then runs at the signal resolution (§6 Q2: limited to below the declared rate's Nyquist when re-based).

## 10. Chroma formats (owner, 2026-10-16: "compare the detail recovery too between 4:4:4 and 4:2:2 and 4:2:0")

**T7.** T1–T4 on each capture format: 4:4:4 (RGB R12B / r210, or YCbCr 4:4:4), 4:2:2 (v210, the DeckLink default) and 4:2:0 (P010). For each:
- detail gain on the B frames;
- the GOP pulse reduction;
- ghosting;
- cost.

**Expected:**
- **Luma** behaves the same in every format; it is the same signal.
- **Chroma recovery** (§6 Q1) matters most for 4:2:0, where chroma is a quarter of the resolution and is coded hardest. T7 decides whether chroma accumulation is on, and for which formats.
- **The optical-flow producer's luma** (P010 read directly, v210 and RGB sampled) must give the same vectors for the same picture in each format: same-picture equivalence within 0.1 px. It is part of T7 and of the producer tests, which so far cover P010.

**The capture format is not the coding history** (owner, 2026-10-16: "with 4:4:4 there could still be bitrate artifacts").
- An Apple TV sends 4:4:4 (or RGB) decoded from a 4:2:0 HEVC stream: coding artifacts, and chroma upsampled from 4:2:0.
- So nothing is decided from the capture format. Accumulation (and chroma accumulation) follows the measured coding quality (the bitrate-quality reading) and the chroma's measured effective resolution.
- The source-resolution detector can read the chroma planes as it reads luma: upsampled 4:2:0 chroma shows the mirror signature at half the rate.
- The same reading could let the re-base (Stage 12) treat chroma separately.
- **T7 adds:** 4:4:4 decoded from a coded 4:2:0 stream (ffmpeg: encode 4:2:0, decode, upsample to 4:4:4) alongside a true 4:4:4 source.

## 11. The opportunity, measured (2026-10-16)

**Material.** Bosphorus (UVG), a 1536×768 crop, 48 frames at 24p. x265 medium, keyint 24, 4 B-frames (a streaming-like GOP). Each decode is compared with the uncompressed original.

| Bitrate | I-frames | P-frames | B-frames (37 of 48) |
|---|---|---|---|
| CRF 24 | 75% of the fine detail kept, 43.4 dB | 74%, 42.3 dB | 72%, 41.3 dB |
| CRF 32 (starved) | 68%, 40.3 dB | 68%, 39.1 dB | 65%, 38.8 dB |

"Fine detail" is the RMS of the frame minus a 5×5 blur, as a share of the original's.

**Reading:**
- The I/B pulse with a good encoder is about 3–4 points of fine detail and 1.5–2 dB. Hardware streaming encoders and starved ladders are expected to be larger; to measure on real streams (G19).
- A quarter to a third of the fine detail is gone from every frame: accumulation cannot restore what no frame holds.
- **What accumulation can do:**
  - lift the B-frames (three quarters of all frames) towards the I-frame level;
  - average the independent coding noise across frames.
- **Expected:** about +1 to +2 dB on most frames and steadier texture; not a large sharpness gain.

**Gate for building:** the planner's view (Q4) plus a prototype on this material (scratch: D:\hfr\breathing). The prototype must lift the B-frames by at least 1 dB with no frame worse, before any GPU work.
