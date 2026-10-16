# Optical flow ahead of display: a vector store (design)

Owner, 2026-10-16:
- "it actually might be a lot of savings ultimately if we get it to start working on the frame 4 or so ahead and save the vector sets for us to use for the 2 frame look ahead. this would free us up to do other things. obviously always have to default back to the original gpu path and keep that updated/happy. but this potentially resolves our look ahead problem for detail"

Status: design approved by the owner (2026-10-16: "agree and like your GPU compute if missing fallback in keeping with the design philosophy"). The CPU vector store (`VectorStore.h`) is built and tested; no GPU code yet.

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
