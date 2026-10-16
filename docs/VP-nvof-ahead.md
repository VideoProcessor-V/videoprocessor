# Optical flow ahead of display: a vector store (design)

Owner, 2026-10-16:
- "it actually might be a lot of savings ultimately if we get it to start working on the frame 4 or so ahead and save the vector sets for us to use for the 2 frame look ahead. this would free us up to do other things. obviously always have to default back to the original gpu path and keep that updated/happy. but this potentially resolves our look ahead problem for detail"

Status (2026-10-16): built and on by default (`optical_flow: on`). The engine seeds the matcher's refinement; the producer computes pairs ahead on its own device; the matcher is the fallback. See "Built" at the end. Design approved by the owner (2026-10-16: "agree and like your GPU compute if missing fallback in keeping with the design philosophy").

## Idea

Today the de-judder's block matcher runs on the render thread's GPU budget, between the current frame A and the next frame B, when they are rendered.

The NVIDIA optical-flow engine (NVOFA) is separate hardware. On the 3090 and the laptop's 3050 Ti it runs alongside the shader cores, with almost no render-budget cost. If it works on queued frames as they arrive, about 4 source frames ahead of display, every consumer finds its vectors already waiting:

| Consumer | Pairs it needs | Today |
|---|---|---|
| De-judder (warps, extras, trust layers) | (A, B) | Matcher on the render thread |
| Detail accumulation v1 (causal) | (prev, cur) | The de-judder's (A, B) |
| Detail accumulation v2 (two frames ahead) | (cur, cur+1), (cur, cur+2) | Not possible (would need several matcher runs) |
| Look-ahead verification at cuts | none (luma only) | n/a |
| Low-latency 50/60p (backward vectors) | (prev, cur) | Planned |

## Architecture

1. **Upload.** EarlyUpload (`capture_upload: early`) already uploads each frame on the conversion thread, as it arrives, on a second D3D11 device on the same adapter. The frames are in shared textures well before display.
2. **Produce.** An optical-flow worker on that second device takes each newly uploaded frame f and computes flow (f−1 → f). When v2 runs it also computes (f−2 → f). It uses the D3D11 interface (`nvOpticalFlowD3D11.h`, SDK 5.0.7), grid 4, with the cost buffer.
3. **Convert.** A small compute pass on the second device turns the 4×4 flow into:
   - the de-judder's block field: one texel per 16×16 block, `rgba = vx, vy, trust, error`, with the median of the 16 vectors and trust from the cost and the spread;
   - the fine field (4×4) for detail accumulation.

   Both go into shared textures.
4. **Store.** The vector store (`VectorStore.h`) records each pair (from, to) under the continuity generation:
   - **Discontinuities clear it:** resync, seek, format change, drop ([[vp-vector-validity]]).
   - **Pruning:** pairs older than the frame being rendered are dropped.
   - **Bounded:** about 16 pairs.
5. **Consume.** At render time each consumer asks the store for its pair:
   - **Hit:** it uses the waiting field. It waits on that slot's fence and runs the same trust layers as today: consistency, landing check, M6 pixel trust.
   - **Miss** (no engine, an error, not ready yet, a non-NVIDIA GPU): the matcher runs exactly as today. It stays the default path and is tested on every build.

## Why this pays

- **Shader time.** The matcher's render-thread cost disappears when the store hits. That budget can go to detail accumulation, the debander, and so on.
- **Detail look-ahead becomes possible.** Pairs two frames ahead are cheap because the engine is otherwise idle.
- **Latency is unchanged.** Only frames already queued are used. A shallow queue simply means fewer pairs ahead, falling back to the causal path.

## Rules (from earlier owner decisions)

- **The matcher is never removed.** Both paths are kept updated and tested ("keep that updated/happy").
- **Switching is smooth.** Per pair, hit or miss, with no visible change. The same trust layers apply to both sources.
- **Startup self-check.** A known pair is run within a time limit; any error falls back for the session. `auto` becomes the default only after the real-clip corpus gate ([[vp-nvof-parallel-path]]):
  - removal at least the matcher's;
  - damage at most the matcher's;
  - faster on the laptop and the 3090;
  - the stream-fault harness passes.
- **Licence.** Review the SDK licence before shipping. The runtime `nvofapi64.dll` ships with the driver.

## Steps

1. **VectorStore (done here).** Keying, generations, pruning, the bound, statistics, and the "which pairs next" plan. CPU tests.
2. **Optical-flow worker on the EarlyUpload device:** create, a self-check on a synthetic pair, execute, the cost buffer, fences. GPU (on mains).
3. **Field conversion pass**, plus an equivalence test against the matcher on synthetic pans and objects.
4. **Consumer wiring:** the de-judder asks the store first. Statistics: hit rate and matcher runs saved.
5. **Corpus gate** (removal, damage, cost) on the laptop, then the 3090.
6. **Detail accumulation v2** uses the pairs two frames ahead.

## Vector math (owner, 2026-10-16: "Can it do vector math too")

The engine computes motion only. It is not a general compute unit. What it offers beyond the raw flow:
- **Cost.** A per-vector cost buffer (confidence).
- **Hints.** External hint vectors: ours, or the previous flow carried forward, to steady and speed the search.
- **Region of interest.** An ROI (the active picture inside the bars).
- **Grid size.** Output grids of 1, 2 or 4 px.
- **Both directions.** Forward and backward flow, as two calls.

The vector math is a small compute pass on the second device. It costs a fraction of a millisecond, because the fields are tiny (one texel per 4×4 or 16×16 block):
- **Consistency.** Forward and backward flow should cancel, as the matcher's consistency layer already checks.
- **Composition.** (f→f+1) chained with (f+1→f+2) is compared with the direct (f→f+2). Disagreement marks occlusions and unreliable areas.
- **Reduction.** Median and spread to the 16×16 block field.
- **Global and camera motion.** A robust fit (pan, zoom, rotation) over the field. This gives the camera-only part used by `object_strength`, and a cut/fade check.
- **Temporal steadiness.** A block's vector against its own history along the motion, for the trust layers.
- **Acceleration.** Changes in velocity between successive pairs (for pacing and judder analysis).

The SDK also ships NVIDIA's frame-interpolation library (NvOFFRUC, CUDA). Our warp stays our own, because its taste semantics (displacement, the even cadence, the object/camera split) are the owner's. The library is only a possible comparison reference.

## Depth and the queue (owner, 2026-10-16)

"I would by default accumulate the optical flow vectors to 4 frame max depth. Low latency disables. Would ensure baseline queue defaults allow for at least a 4 frame lookahead and adjusts when set lower."

- **Depth.** Up to 4 source frames ahead by default. This is our setting (a slider/box with a range and a reset, per the settings scheme), with a maximum of 4.
- **Low latency switches it off.** That covers the Low Latency queue profile and anything the de-judder already treats as low latency (`DejudderPolicy::lowLatency`). The matcher path is unchanged.
- **The baseline queue must hold the depth.** In the shipped Normal profile, `target_frames` today is 3 (`lead_frames` 4, `queue_size` 32), which gives about 3 future frames at most. It will be raised so that 4 future frames are queued in steady state. The exact value will be measured from `availableFutureFrames` on mains, not guessed. This changes the shipped default only: deployed configurations are user data and are not edited.
- **It adjusts when the queue is set lower.** The depth used is min(4, the future frames actually queued) per frame. `VectorStore::Plan` already takes only what is queued: no waiting, no added latency. A queue set to 1 or 2 simply means fewer pairs ahead, with the matcher covering the rest.
- **Reporting.** The stats line and the log show the configured depth, the frames actually available, the store's hit rate, and the reason when it is off (low latency, no engine, a shallow queue).

## Owner additions (2026-10-16, later)

1. **Empty or partly filled queue.** "It also needs to adapt if the queue is unfilled or partially filled and not just crash." The depth is min(configured, frames actually queued) on every frame. Zero queued means no pairs ahead (NoFramesAhead): the matcher runs, with no waiting and no failure. Tests must cover an empty queue, a partly filled one, a queue filling at startup, a reset, and frames dropping out of the queue mid-plan.
2. **The engine is the established default.** A separate setting, `optical_flow: on|off`, defaults to on. Setting it off disables the engine (the matcher only). It sits alongside `motion_ahead_frames` (the depth).
3. **The engine's extra information feeds our path.** "take and vector differences etc that it allows that fed into our path." The cost buffer, forward/backward consistency, chained-versus-direct differences, the camera-motion fit, steadiness, and acceleration all go into the de-judder's trust layers and detail accumulation, not only the raw vectors.

## Conflicts between the motion look-ahead and the queue (owner, 2026-10-16: "if motion look ahead and global queue disagree we'll need to sort out a conflict resolution")

**Rule: the queue always wins, and the motion look-ahead adapts.** Latency is the operator's explicit choice; vectors are an optimisation. The motion side never changes the queue at run time. Only the shipped Normal default is raised to hold 4 ahead.

| Conflict | Resolution |
|---|---|
| Depth above the frames the queue profile holds ahead | Use what is queued. Config shows an inline note beside the box ("the queue holds N ahead: N will be used"). Logged once. |
| A Low Latency profile, directly or chosen by a frame-rate rule (e.g. 50/60 sports) | Off; the matcher only. |
| Queue draining, refilling, resetting, or filling at startup | The depth follows it per frame. Never wait for vectors; never stall. |
| The raised default target tripping `reset_queue_too_large_percent` | Must not happen. Check the threshold against the new target when the default is measured. |
| Other readers of the queued frames (active-picture look-ahead, LED predictions, the cut look-ahead) | No conflict. They read queued frames only and never change the depth. |
| `optical_flow: off`, or no engine | The matcher, as today. |

## NVIDIA's own frame interpolation (owner, 2026-10-16: "does nvidia have an example frame interpolation setup using optical flow ... potentially include it as the interpolation option we offer. I don't want to necessarily go down that rabbit hole too much though")

**What it is.** The Optical Flow SDK 5.0.7 zip (C:\dev\Optical_Flow_SDK_5.0.7.zip) includes the FRUC library: `NvFRUC.dll` (a closed binary), `NvFRUC.h`, `NvFRUCSample`, and the guide `Doc/NVOFA_FRUC.pdf`.

**From the guide:**
- **Hardware and dependencies:** Turing and later (the 3090 and the 3050 Ti). It uses NVOFA plus CUDA, with D3D11 through CUDA–DirectX interop, synchronised by ID3D11Fence.
- **Timing:** it interpolates a frame at a requested timestamp between two inputs.
- **Method:**
  1. optical flow;
  2. a forward/backward consistency check that rejects vectors;
  3. infilling of the rejected vectors;
  4. warping;
  5. image-domain hole filling.
- **Inputs: ARGB and NV12 only** (8-bit).

**Fit for VP.**
- **Precision.** 8-bit input only: no 10-bit or PQ HDR source precision. VP renders at 10 to 16 bits before the 8-bit FL35 link, so frames would be quantised before our processing.
- **A black box.** None of the owner's taste semantics (displacement scale, the even cadence, the object/camera split, roll-off) and none of our trust layers or fallbacks.
- **A whole-frame interpolator.** It replaces the de-judder's warp; it cannot feed it.

**HDR** (owner, 2026-10-16: "why not hdr"). `NvFRUC.h` confirms NV12Surface and ARGBSurface only (8-bit). HDR10 must be cut to 8-bit PQ, where one step is 4 codes:
- banding in smooth and dark areas;
- tone mapping to the FL35 stretches the steps further;
- only the interpolated frames would be 8-bit, so banding alternates frame to frame as a shimmer.

Not ruled out. Run at the end of the chain (the FL35 link is 8-bit anyway), but the interpolated frames would miss our dither. Offer HDR as a labelled "8-bit interpolation, may band" mode, and measure banding and shimmer on the HDR clips.

**Proposal (kept small).** Only after the engine-ahead work, offer it as a clearly labelled alternative motion engine ("NVIDIA interpolation") behind Fixes under test:
- SDR first; HDR as the labelled 8-bit mode above, after measurement;
- exclusive with ours, like libplacebo smooth motion ([[vp-motion-engine-exclusive]]);
- compared with ours on the real-clip corpus.

It would be a thin wrapper of about a day of work, with no tuning. Its pipeline (consistency, infill, hole filling) is a useful reference for ours.

### The HDR-compatible version (owner, 2026-10-16: "need an hdr compatible version to make it at all usable for people")

The 8-bit limit belongs to NVIDIA's packaged FRUC library, not to the engine's role.

- **Motion.** The engine gets an 8-bit (perceptual) luma copy of the frame for motion only. Vectors depend on structure, not on tonal precision, so HDR10 motion estimation is unaffected.
- **Picture.** VP's own interpolator builds the in-between frames from the real 10-bit (HDR or SDR) frames at full precision, following FRUC's published method:
  1. flow;
  2. the forward/backward consistency check;
  3. vector infill;
  4. a bidirectional warp and blend;
  5. hole filling.

  Our temporal dither then runs on every frame alike, so there is no banding shimmer.
- **Product.** A full-frame "smooth motion (optical flow)" mode, beside the de-judder's taste-controlled warp. It is exclusive with the other engines by profile rules ([[vp-motion-engine-exclusive]]).
- **The NVIDIA library** stays optional: an SDR-only reference for comparison on the corpus.
- **Builds on** the engine-ahead work (vectors waiting in the store, the matcher as fallback). The new parts are the bidirectional full-frame warp and the hole filling.

## Measured: the motion corpus (2026-10-16, laptop RTX 3050 Ti, 36 synthetic scenes with exact motion)

| Mode | Judder removed (benefit) | Trusted-vector error (half px) | Worst wrongly trusted | Cut |
|---|---|---|---|---|
| Matcher (block searches), deployed | 1471.6 | 0.13 | 1.9% | held |
| Engine instead of the searches (medium / slow) | 1291.5 / 1399.4 | 0.44 / 0.39 | 100% (the cut) | **warped** (before the reach limit) |
| Matcher + engine as a refinement candidate | 1474.7 | 0.14 | 2.8% (fence alias) | held |
| **Engine seed + matcher refinement (medium / slow)** | **1468.7 / 1471.5** | 0.26 / 0.24 | 3.6% (fence alias) | held |

**Findings:**
1. **A precedence bug.** The experimental replace path ran the backward search even when the engine was used. Fixed.
2. **Engine alone is unsafe at cuts.** With the reach limit, the cut scene still reads 100% wrongly trusted in replace mode. It is never the default.
3. **The reach limit.** The engine matched a far, similar texture across a cut. Vectors beyond the searches' own reach (coarse range × 16 + 8 half-size px) are now no candidate.
4. **The seed mode equals the matcher (99.8–100%)** while skipping the searches (the saving; timing to follow), with the matcher's refinement, half-pixel step and trust tests unchanged.
5. **The fence scene's wrong trust is a one-period alias**, the same picture. It was already known for the matcher.

**Next:** measure the timing (live matcher vs seed vs candidate), then choose the default (seed if it saves time). Then the producer ahead and the store.


## Built (2026-10-16)

**What runs:**
- **Defaults.** `optical_flow: on` (Config: Optical-flow engine) and `motion_ahead_frames: 4` (0–4). Off in a low-latency queue profile.
- **Seed mode by default.** The engine's block vectors seed the matcher's refinement in place of the block search (the 6.3 ms pass on the laptop at 4K). The refinement, the half-pixel step and the trust tests are unchanged. Engine vectors beyond the searches' reach are rejected (the cut case).
- **The producer** (`MotionAhead::Producer`) runs on its own D3D11 device and worker:
  1. the render loop submits the current and queued frames once each (hand-off 0.010 ms mean on the render loop);
  2. the worker makes the half-size luma (halved again for the half-size inner matcher at 4K);
  3. it runs the engine per consecutive pair, both ways, with costs;
  4. the result goes into the VectorStore under the continuity generation.
- **The de-judder** takes its pair from the store (no engine work at render time). A miss runs the engine at render time; no engine means the block searches.
- **The queue.** The shipped Normal profile and Config's Reliable preset are target_frames 5 (4 ahead). A time-based target holds at least 5 frames (low latency exempt). A shallower queue gives a smaller depth.
- **The log** reports the motion source ("optical-flow engine (computed ahead)" / "optical-flow engine" / "block searches (…)"), the depth decision, and the producer statistics.

**Measured (laptop RTX 3050 Ti):**
- **Corpus.** Seed 99.8–100% of the matcher's removal, the cut held.
- **Live 4K matcher.** 7.5 → 5.5 ms with the engine at render time; with the producer the engine time leaves the render path too.
- **Producer.** About 11 ms per pair on its worker. The renderer took 20 of the first 23 pairs from the store; the misses were before it started.

**The producer's luma, by format** (owner: compare 4:4:4 / 4:2:2 / 4:2:0):
- Direct decoders for P010/P210, v210, UYVY/HDYC, R12B, R12L, r210/R10b/R10l and 8-bit RGB, 2 × 2 averaged, rows on up to 4 threads (4K P010 2.3 ms).
- The same picture gives the same luma: v210 = P010 exactly; R12B = R12L exactly; UYVY and r210 within one step.

**Tests:**
- full unit suite 2795/2795;
- Config 112 + 1 known skip (off screen);
- the Beta suite;
- 998 black-bar / active-picture / crop / look-ahead tests.

**Fixed on the way:**
- a fail-fast at teardown on NVIDIA (the engine's textures released before it; the driver DLL unloaded);
- the replace path's precedence bug;
- the inner matcher's reporting.

**Not built yet:**
- the engine's global flow in place of the D1 global search (about 0.4 ms);
- the cost buffer and the intra-block spread as trust inputs;
- the multi-pair vector math (chained against direct, acceleration);
- the 3090 measurements.

## Measured: the 7 real clips (2026-10-16; UVG 120p at 24p spacing and Xiph 50p, laptop RTX 3050 Ti)

| Mode | Judder removed | vs matcher | Damage (harm) |
|---|---|---|---|
| Matcher (block searches) | 41.3% | — | 0.40 |
| Engine instead (medium / slow) | 39.1% / 40.3% | −5.3% / −2.6% | 0.33 |
| Matcher + engine candidate | 41.4–41.5% | +0.3–0.4% | 0.40–0.41 |
| Engine seed + matcher refinement, medium | 41.2% | −0.2% | 0.37 |
| **Engine seed + matcher refinement, slow** | **41.9%** | **+1.3%** | **0.35** |

**Per clip,** the slow seed beats the matcher on crowd_run, Jockey, park_joy, ShakeNDry and YachtRide, and ties on Bosphorus and ReadySetGo.

**Defaults:**
- The producer uses the slow preset. It is off the render path, at the matcher's engine size (a quarter at 4K).
- The render-time fallback uses medium.
- Owner gate ([[vp-nvof-parallel-path]]): removal at least the matcher's (+1.3%), damage at most the matcher's (0.35 vs 0.40), faster (the block search skipped). Met on the laptop; the 3090 is to be confirmed.

## Camera motion from the engine (planner Q1, 2026-10-16 night)

The whole frame's motion (D1, read by the refinement's start, the roll-off and the camera) now comes from the engine's field when its pair came ahead: `MotionAhead::FlowGlobal` on the producer's worker (no render-thread cost) - the most common vector of the confident cells (a 3 x 3 vote on a half-texel grid, refined by the mean within a texel), its share as the ambiguity on D1's scale, and a second layer. The global search stays the fallback (no pair ahead, no engine). Switch `optical_flow_global` (Fixes under test, default on).

Measured: corpus (36 scenes) and 7 real clips **identical without the taste** (100.0% of the matcher; 41.9% removed both). With the taste (D2/D4/R-A) the results differ on 7 scenes; the block vectors are identical (same vector error) - only the speed the roll-off reads differs. On known pans (`EngineGlobalMatchesTheSearchOnKnownPans`, 4-px steps): true 6 / 12 / 20 / 32 px -> the search 8 / 16 (+4 vertical) / 24 (+4 vertical) / 32, the engine 4 / 12 / 20 / 32. The engine reads the speed exactly where the search overshot medium pans by a step, so the taste now follows the true speed (real clips with the taste 35.3% vs 33.9% removed). Open: the search's overshoot on the fallback path (no engine).
