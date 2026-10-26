# Plan N9: net runner efficiency, and the upscale/downscale net plan

Read-only needs assessment and design plan, 2026-10-26. Tree read: integ-batch1 (d85519e2). No builds, no code edits.
Sources read: `src/VP-V/nets/` (PlNetRunner, NetFusion, README, NetRunner.h), the smoke export shaders in
`D:\nn-corpus\VP-NN-TrainKit\smoke\staging`, `vp_arch_cost.json`, the owner memory notes, `vp-post-reset-queue.md`
(items 30, 104-119, 206-208), `plan-review-consolidated.md` (items 6, 10, N9), `plan-wp16-wp9-wp10.md` (WP-10),
`modularity`/`route_4k`/`refine` READMEs, `efficiency.md`, `VP-render-ahead-design.md`.
All GPU times below are ESTIMATES unless marked "measured". Every estimate is derived from counted multiply-adds
(MACs) and a stated efficiency, so it can be checked (section A2).

## 0. Findings that change the plan (read this first)

1. **The "D 3.7 ms / D_small 2.47 ms on the 3090" numbers cannot be right, and the "fused D ~1.7 ms / D_small ~1.2 ms"
   estimates are below the hardware floor.**
   - Counted from the exported shader: D = about 15,000 MAC per output pixel (trunk 13.2k at the output grid, plus a
     4K source branch of 1.75k per output pixel). At 2560x1600 that is **61 GMAC = 122 GFLOP**.
   - The 3090's fp32 peak is 35.6 TFLOPS. The floor for D is therefore 3.4 ms at 100% of peak. No unfused fragment
     export reaches that. D_small (33.8 GMAC) has a floor of 1.9 ms.
   - The laptop agrees with the arithmetic: exported D 99-101 ms (measured, pl_timer) = 0.6 TMAC/s = 16% of the
     3050 Ti's peak; fused D 43-50 ms = 1.3 TMAC/s = about 34%. The A net on the laptop (measured 9.5 ms at 4K,
     14.3 GMAC) gives the same 1.5 TMAC/s. These three agree with each other.
   - The 3090 figures came from `nvidia-smi` utilisation differences (item 111, "coarse"). They imply 93% of peak for
     an unfused export. They are the outlier (the owner's own open question, item 206). Expect on the 3090:
     **D fused 6-9 ms, D_small fused 3.5-5 ms** (at 40-55% efficiency), not 1.7 and 1.2.
   - Consequence: whether D / D_small fit 60p depends on a number nobody has measured yet. Package P0 measures it in
     about an hour on the 3090 and decides most of the rest.
2. **B costs more than C and D.** B (one pass per axis, 17.3k MAC per low-resolution pixel) is **107 GMAC for 1080p to
   4K both axes**. It is the most expensive net by 2x. Floor on the 3090: 6.1 ms; fused fp32: 11-15 ms.
3. **fp16 storage and Winograd: the earlier verdict is confirmed, with a reason.** Both machines have the same
   bytes-per-MAC ratio (3090: 936 GB/s for about 17.8 TMAC/s; laptop: 192 GB/s for about 3.8 TMAC/s, about 0.05 B/MAC
   both). The laptop measured 4% for fp16 storage, so the 3090 should too. On GeForce Ampere, fp16 on the normal shader
   cores runs at the same rate as fp32 (no doubling; from my knowledge of the architecture, not measured here).
4. **VRAM is the hidden limit on the laptop.** The runner pool is keyed by each pass's SAVE name and never aliases
   textures across layers. D at rgba32f therefore holds about 2.2 GB (18 trunk group textures of 65 MB, 3 regroup
   textures, 4 source-branch textures of 133 MB at 4K, plus S0 and the feature textures). That is my count from the
   shader, not a measurement (verify in P0). It does not fit beside the 4K chain on a 4 GB card. Item 207 estimated
   0.8 GB assuming ping-pong; the code does not ping-pong.
5. **C is dropped in the shipped design** (round 1: harmful, 71.5 dB to 55 dB; helps only 1.5x and 4/3 non-2x cases).
   `net-runner-wiring`/README say "leave C bypassed". The N9 text still lists C. This plan keeps C wired only as an
   optional, default-off, evidence-gated package (P7).
6. **What already exists for N9**: the 8K rule in the runner (`NeuralBypassForSize`, tested; shows as a designed
   bypass), B and C as `NetRole`s with a `scaling.route_4k` slot, the slot contracts (`ScalingRoute4k` gives
   `Res::Processing`), per-axis shape of the exports (B_h/B_v, C_h/C_v), r16 hand-off quantisation inside the
   shader, upstream-net fingerprints. **Missing**: any processing-resolution function, the route_4k stage, the
   two-axis 1-D luma re-base, wiring of B, the cost gate, the 8K motion 1/4 plumbing. `route_4k` and `scaling.refine`
   are placeholders with no code.
7. **D runs inside `pl_render`, so it runs per refresh, not per source frame**, unless the rendered-warp or WP-24
   repeat-reuse cache hides it. A, B and C run in the entrance, once per source frame. See A5.

## A. Net runner efficiency design

### A1. Current architecture (verified in code)

| Aspect | Today |
|---|---|
| Shader generation | The exporter writes mpv GLSL: one fragment pass per group of 4 output channels; each pass re-reads every input group at 9 taps (D: 30 passes, 442 KB of GLSL with the weights as literals). `PlNetRunner` runs each through `pl_shader_custom` + `pl_dispatch_finish`, or compute through `pl_dispatch_compute`, with the mpv macros built as strings each pass, each frame |
| Fusion (optional, `SetFusion`) | `NetFusion`: regroups the conv passes into layers; one compute pass per layer; input tile + 1-px halo in groupshared (mirrored edges); each thread computes all output groups of its pixel; fp32 accumulate; bias, ReLU, skip, tile gate kept. Layers whose tile exceeds 32 KB, whose outputs exceed 8 UAVs, or whose input sizes differ stay exported. Feature, pooling, regroup, bounded-output passes and A's conditioned first layer stay exported |
| Layers per net | A_small: 3 trunk layers + output, at half resolution (pixel-unshuffle), gate branch at 1/8. D/D_small: 3 trunk layers at the output grid + 2 source layers (1 to 8 to 8 channels) at the 4K source. B: 4 trunk layers 24 ch + pixel-shuffle output, at the pre-doubling resolution |
| Memory traffic | Every layer output is a full-size texture (rgba32f or rgba16f per fingerprint). D at 2560x1600 fp32: 393 MB written and read per 24-channel layer = about 0.8 ms of bus time per layer at 936 GB/s; roughly 2.5 ms for the three trunk layers, partly hidden by compute |
| Precision | fp32 accumulate always; intermediates rgba32f (round-1 D/C drift 0.2-0.45 code at fp16) or rgba16f where the export's fingerprint allows; net output r32f (or r16 for the B hand-off) |
| Dispatch count | D exported: 30 passes. Fused: about 9-10 dispatches (5 conv layers + the passes that stay exported). A_small exported: 24 |
| Sync / readbacks | None on the data path. Per pass, one `pl_timer`; at each Run the previous frames' timer results are polled (never waited on). No `pl_tex_download`. Good |
| Per-frame allocations | Textures pooled; recreated only on size/format change (good). Per frame it still builds the header/macro strings and the descriptor vectors for every pass and hands libplacebo the pass body to hash. For D that is several hundred KB of GLSL text per Run. This is CPU on the render thread; never measured |
| First-use cost | libplacebo compiles each distinct shader on first use (GLSL, SPIR-V, HLSL by FXC). A 442 KB weights-as-literals shader will stall the first frame, probably for seconds. Not measured; no pre-warm exists |
| Where it sits | Front end (A, B, C): called by the entrance stage list, once per source frame, on VP's own dispatch and VP-owned textures. D: `RunInPostKernelHook` inside the hybrid scaler's POST_KERNEL hook, i.e. inside `pl_render`, i.e. every render of the picture |
| Crop/copy | A picture that fills part of a pooled texture is blitted to an exact-size texture first (one extra copy per frame for D's hook input and a cropped A input) |

### A2. Cost per net per frame at the working resolutions

MAC per pixel counted from `mat4(` literals in the smoke export (padded zero blocks included; they are computed).
Time = GMAC / (peak x efficiency). Peaks used: 3090 17.8 TMAC/s (35.6 TFLOPS, boost clock); 3050 Ti laptop 3.8 TMAC/s.
"Fused" = the existing fused-direct path (measured 34% on the laptop; assumed 40% on the 3090). "V2" = target of
package P1 (assumed 55%; 1.9 TMAC/s on the laptop). "Floor" = 100% of peak.

| Net, size | GMAC | 3090 floor | 3090 fused | 3090 V2 | Laptop fused | Laptop V2 |
|---|---|---|---|---|---|---|
| A_single_small, 4K delivered (14.3; 1728 MAC/px; gate may skip clean tiles) | 14.3 | 0.8 | 2.0 | 1.5 | 11 (meas. 9.5) | 7.5 |
| A_single_small, 1080p encoded | 3.6 | 0.2 | 0.5 | 0.4 | 2.8 | 1.9 |
| A_motion_small (A plus about 25%, unverified) | +25% | | | | | |
| B x2 H only, 1080p wide in (2.07 Mpx x 17.3k) | 35.8 | 2.0 | 5.0 | 3.7 | 28 | 19 |
| B x2 both axes, 1080p to 4K (H then V) | 107.5 | 6.1 | 15 | 11 | 83 | 57 |
| C both re-base passes, 4K delivered to 1080 | 32 | 1.8 | 4.5 | 3.3 | 25 | 17 |
| D, output 2560x1600 (15.0k MAC/px) | 61.3 | 3.4 | 8.6 | 6.3 | 47 | 32 |
| D, output 1920x1080 | 34.6 | 1.9 | 4.9 | 3.5 | 27 | 18 |
| D, output 3840x2160 | 116.6 | 6.6 | 16 | 12 | 90 | 61 |
| D_small, 2560x1600 (about 8.2k MAC/px) | 33.8 | 1.9 | 4.8 | 3.5 | 26 | 18 |
| D_small, 1920x1080 | 20.7 | 1.2 | 2.9 | 2.1 | 16 | 11 |

Notes on the table:
- D's 4K source branch (1 to 8 to 8 channels, 7.2 GMAC) is a fixed cost whatever the output size. It is 12% of D at
  1600p and 35% of D_small at 1080p.
- Measured rows: laptop exported D 99-101 ms and fused D 43-50 ms (README, 20 frames, pl_timer, smoke D); laptop A at 4K
  9.5 ms (`vp_arch_cost.json`, utilisation method, coarse); 3090 A +1.83 ms at 2160p and B about 23 ms (owner relayed,
  utilisation method; B 23 ms is consistent with an exported-pass 26% efficiency). Nothing is measured with pl_timer on
  the 3090. The 3090 numbers for D and D_small (3.7 / 2.47) are NOT used.

### A3. Design options, evaluated

| Option | What it buys | Cost / complexity | Verdict |
|---|---|---|---|
| **Fused fp32 compute in D3D11 (today)** | 2.0-2.4x over exported (measured, laptop). 34% of peak | Done. Needs the 3090 measurement | **Baseline.** Keep |
| **V2 of the D3D11 fused runner** (tuning, below) | Plausibly 34% to 50-55% of peak (1.4-1.6x). Assumption, not measured | Medium, contained in `nets/` | **Do (P1).** Includes the VRAM fix |
| Full layer fusion / tile-resident activations | Removes the intermediate texture traffic (about 2.5 ms of bus time for D) and most VRAM | The 5 conv layers of D need a halo of 4 px. With 24 channels the tile does not fit 32 KB fp32; at 16x16 outputs plus halo the redundant compute is 1.3-1.6x for a pair of layers, which costs more than the 0.8 ms per layer it saves while compute-bound. Wins only for 16-channel A-class nets or with fp16 tiles | **Not for D/B.** Two-layer fusion for A (16 ch) is a P1 experiment only. VRAM is fixed by buffer aliasing instead |
| fp16 storage, fp32 accumulate | 4% measured; same bytes-per-MAC on the 3090. Halves VRAM | Already built (`fp16Storage`). Fingerprint gate already enforces it | **Keep dropped as a speed lever.** Keep as the VRAM lever on the 4 GB laptop where the export's own `fp16_within_1_lsb` check passes (owner question 3) |
| Winograd F(2x2,3x3) | None measured (laptop: register/shared-bound) | Built | **Keep dropped** |
| Wave intrinsics | Maybe 5-10% by sharing weights/inputs across lanes | D3D11 shaders are SM5 (FXC); wave ops need SM6/DXC (D3D12). NVAPI intrinsics exist for D3D11 but are NVIDIA-only | **No** |
| int8 (DP4A / tensor int8) | Up to 4x on arithmetic | DP4A needs SM6.4 (D3D12 only). Needs quantisation-aware retraining; error on a grain-sensitive net is hard to bound (output is `base + tanh(x) * bound`, bound up to 32 codes) | **No, not before round 3 evidence.** Revisit only if the DirectML spike succeeds |
| **Tensor cores** via D3D12 + DirectML | Convs as fp16 tensor GEMMs. Estimated 3090: D 2.5-3.5 ms, D_small 1.8-2.5, B both axes 3-4, A about 0.6; laptop D_small 6-8, D 9-12. Estimates are bus-limited (24-channel activations at 2 bytes), not proven | Large. A D3D12 device on the same adapter, shared textures (NT handles, `pl_d3d11_wrap`), a fence handshake each way, texture to buffer copies (256-byte row pitch), DML graph built from the export's weights (conv, ReLU, add, depth-to-space; the tile gate has no DML equivalent), the DirectML library shipped app-local, fp16 numerics (fingerprint rule), vendor driver variance; VRAM for the copies | **Spike, decision-gated (P9).** Do not build before P0/P1 numbers show a wanted net misses its budget |
| NVIDIA CUDA / TensorRT with D3D11 interop | Best raw speed on NVIDIA | Hundreds of MB of runtime DLLs, NVIDIA-only, installer and licence weight | **No** (also against the app-local, small-package rules) |
| Vulkan `VK_KHR_cooperative_matrix` | Tensor cores from a compute shader | Needs a renderer-wide backend change or Vulkan-D3D11 interop (README already concluded this) | **No** |
| Architecture change (retrain, round 3): **half-resolution trunk** for D / D_small | Same trick A already uses (pixel-unshuffle, 4x channels, run at half resolution, pixel-shuffle out): trunk MACs about 4x lower. D about 15 GMAC at 1600p (3090 about 2 ms fused; laptop about 11 ms), D_small about 8 GMAC (about 1.1 ms; laptop 6 ms). The 4K source branch must be unshuffled too or it dominates | Retrain from scratch (about 12 h per net on the 3090, round 3); quality risk small but unproven | **Biggest single lever; owner decision for round 3 (question 1).** Compounds with any runner |
| Depthwise or 1x1-factorised convs | About 5-8x fewer MACs per layer | Retrain, quality unknown, low arithmetic intensity (bus-bound), so the real gain is smaller | Later, only if the half-resolution trunk is not enough |
| Run once per source frame | Removes repeat cost (24p to 60 Hz: 2.5x) | A, B, C already at the entrance. D needs rendered-warp or repeat-reuse (A5) | See A5 |
| Source vs processing resolution | A at the encoded resolution (1080p: 4x cheaper than 4K, already the plan). B runs at the pre-doubling resolution by design | None | Already right. Never run A at 4K when a re-base ran |
| Share passes with scaler / re-base | D's `SCALER_OUT` features are trivial; D's source branch is a pure function of the 4K processing image and could be computed at the end of B's V pass | Small gain, couples two modules | **No** (modularity tenet) |
| Async overlap | D3D11 has one immediate context; libplacebo has one queue. Real overlap needs a D3D12 compute queue and VRAM bandwidth contention remains | Only with the DirectML route | No in D3D11. The render-ahead queue (about 200 ms) is the overlap we use |

**V2 contents (P1), no retraining:**
1. Texture aliasing: a layer's outputs reuse the buffers of the layer two steps back (ping-pong). D drops from about
   2.2 GB to about 0.9 GB at rgba32f (about 0.45 GB with fp16 storage). Free the pool when nets are off (item 207).
2. Fuse the passes that stay exported (features, pools, regroup, bounded output, A's first layer) so a net is 5-9
   dispatches instead of 24-30.
3. Register blocking: 2 outputs per thread along x, to amortise shared-tile reads; test weights as literals vs a
   constant buffer; try workgroup shapes. Targets at least 50% of fp32 peak.
4. Per-Run CPU cost: build headers and descriptor templates once at `Prepare`; hash a stable shader key once. Measure
   the render-thread cost per Run before and after.
5. First-use cost: compile at `Load` with a scratch dispatch on the render thread's idle moment, or rely on a
   persistent libplacebo shader cache; show "Nets: preparing" instead of a frame hitch (no silent stalls).
6. Step API: `RunStep()` runs one layer; the host may spread layers of a source frame across refresh slots. Needed
   because one B dispatch of 5-15 ms exceeds a 16.7 ms refresh when the rest of the frame is busy.
7. Cost gate: each net's measured `pl_timer` cost vs the active profile's allowance. Over budget for 2 s means a visible
   bypass ("nets: bypassed (D_small: 5.1 ms > 4.5 ms allowance at 60p)"), recovery with hysteresis.

### A4. Per-frame GPU budget after the rest of the chain

Rest-of-chain figures come from `efficiency.md` (laptop measured, 3090 = laptop / 4.9, "not measured") and
`vp_cost.json`. Treat every row as plus or minus 40%.

| Case | Source period | Rest of chain (3090 est.) | 70% busy limit | Net allowance (3090) | Laptop equivalent |
|---|---|---|---|---|---|
| 24p to 60 Hz, de-judder, render-ahead, 1.5 extras | 41.7 ms | about 12 ms (new-frame render 2.5, extras 3, matcher 1.2, upload 1.2, re-base 0.7, fp32 FBOs 0.6, texture restore about 2, cache and misc 1) | 29 ms | **about 17 ms** (11-20) | Laptop 4K24: rest about 30 ms of 41.7, allowance about 0 |
| 30p to 60 Hz, 2 refreshes, no extras | 33.3 ms | about 10 ms | 23 ms | **about 13 ms** (9-15) | Laptop 4K30 goal: rest about 24-28 ms of 33.3 with "most features", allowance about 0-5 ms |
| 60p low latency (queue 1, no look-ahead, backward vectors, no extras) | 16.7 ms | about 7 ms | 11.7 ms | **about 4.5 ms** (2.5-5.5) | Laptop: none |
| 24p, 1080p source on a 4K-class output | 41.7 ms | about 10 ms | 29 ms | about 19 ms | Laptop 1080p24: allowance about 12 ms |

Render-ahead makes a once-per-source-frame cost an average, not a per-refresh deadline (the queue is about 200 ms,
three swap buffers). In low-latency 60p there is no such slack: a net must fit in the refresh it was started in.

### A5. Once per source frame

- A, B, C: entrance stages; the entrance already skips a refresh of the same source frame (`EntranceStages.h` line 83
  note). Correct by construction.
- D: inside the hybrid hook in `pl_render`.
  - 24p with rendered-picture de-judder warp: the extras warp the already-rendered picture, so D runs once per source
    frame (item 104). Fine.
  - 24p/30p without the warp (cadence repeats): D runs on every repeat render, 2-2.5x the cost per source frame, unless
    WP-24 repeat-reuse draws the cached PRE_OUTPUT picture. WP-24 refuses on "opaque hooks"; confirm D's hook counts as
    deterministic once per source frame (add to WP-10 D tests). Owner-visible rule: the cost gate counts D per render.
  - 50/60p: once per refresh = once per source frame. The 60p row of A4 applies.

### A6. Conclusion: which nets fit, under which runner

| Net | 24p (17 ms) | 30p (13 ms) | 60p LL (4.5 ms) | Laptop 4K30 | Runner needed |
|---|---|---|---|---|---|
| **A_single_small** (4K 2.0 / 1080p 0.5 ms; V2 1.5 / 0.4) | Fits | Fits | Fits (at 4K delivered, only just; fits at 1080p) | 1080p sources only (2-3 ms) | Fused D3D11 is enough |
| A_motion_small | Fits | Fits | Not available (needs look-ahead) | 1080p only | Fused D3D11 |
| **D_small** at 1600p (4.8 / V2 3.5) | Fits | Fits | Borderline: fits only if P0 shows V2 reaches 3.5 ms or less and the rest is at the low end | No (26 ms fused / 18 ms V2) | Fused D3D11 (V2); DirectML would make 60p and the laptop plausible |
| **D** at 1600p (8.6 / V2 6.3) | Fits | Fits | No | No | Fused D3D11 for 24p/30p; DirectML or the half-resolution trunk for 60p |
| **B x2** both axes, 1080p to 4K (15 / V2 11) | Marginal: eats most of the 17 ms, so no D beside it | No | No | No | **Not viable in D3D11 except 24p alone.** Needs DirectML (3-4 ms est.) or a cheaper architecture |
| C (4.5 / 3.3) | Fits | Fits | No | No | Default off anyway |

Summary: with the D3D11 fused runner (V2) on the 3090, A fits everywhere, D_small and D fit 24p and 30p, B fits only 24p
and then without D. 60p low latency gets A and, possibly, D_small. The laptop at 4K30 gets nothing but A on 1080p
sources, shown visibly as bypassed. **The runner decides the fate of B (and of D at 60p).** B and D at 60p need either
the DirectML tensor path or the half-resolution trunk retrain; the second is cheaper in agent time and independent of
the runner, so ask for it in round 3 regardless.

## B. Upscale/downscale net plan (N9)

### B1. Agreed design (from vp-neural-bounded, as the plan consolidates it)

- Always 4K processing. Per-axis x2 net restore (B) only on axes the re-base found upscaled, plus native sources
  (owner decision: native 1080 also goes to 4K). Display aspect enters only in the separable downscale; never resize to a
  fixed 4K size. Output scaling is always a downscale except 8K outputs.
- B is single-axis, applied H then V, no conditioning inputs; the pipeline chooses which passes run. Hand-off between
  axes is quantised to r16 in the shader. C is dropped (harmful); the re-base keeps the kernel-matched inverse.
- D refines the hybrid output downscale (D_small likely the default). A repairs at the encoded resolution (A is still on
  trial: round-2 A diagnostic needs at least 0.5 dB with grain kept, else A is dropped).
- 8K: source above 3840x2160 bypasses every neural stage; processing = min(source, max(4K, output active size)); 8K
  displays keep native-8K processing with nets bypassed and motion at 1/4 per axis (SearchHalvings 2).
- Everything operates on the cropped active picture with mirrored edges; detection runs on the original.
- Pre-net stages are frozen interfaces. Each net carries a pipeline fingerprint; a mismatch means a visible bypass.

### B2. What exists

| Piece | State |
|---|---|
| Net runner, packages, fingerprints, `NetSlot`, visible bypass registry | Built and CPU-tested; GPU tests never run in the integ tree; PlNetRunner not wired |
| B/C roles, upstream-net fingerprint, r16 hand-off in shader | Built (runner side) |
| 8K bypass | `NeuralBypassForSize` in the runner (3840x2160 limit) with a test. The processing-resolution policy and the motion 1/4 plumbing do not exist |
| `scaling.route_4k`, `scaling.refine` | Placeholders (README only; no key, no code) |
| Kernel-matched 2:1 luma inverse, 1-D Lanczos for chroma and for one-axis luma | Built (`SourceRebase`) |
| Two-axis 1-D luma re-base | **Missing.** Luma two-axis falls to polar EWA on purpose ("a 1-D Lanczos rings into black bars", `SourceRebase.cpp` line 270) |
| Chroma for the 4K route | B and C are luma-only. Chroma to 4K must be classical 1-D (N8 chroma plan) and the output must stay 2-plane luma + full-resolution CbCr. Not specified anywhere |
| Fingerprint context for B/C/D | Needs the round-3 exporter fields (`upstream_nets`, pass order, formats) or `net_stamp.py` sidecars; round 1/2 B and C stay bypassed without them |
| Trained B | None (B was never trained; it is last in round 2's order and optional). Bench numbers for B are architecture-only |

### B3. What is missing, per owner rule

1. **Processing-resolution policy** (pure function, unit-tested, recomputed on output-profile or display-mode change with
   vectors invalidated; never hard-coded sizes):
   - inputs: encoded size per axis (after the re-base), effective output active size (after display mode, A-lens, NLS),
     source size;
   - rule: sources above 4K on an axis: P = min(source, max(4K, output)); sources at or below the doubling range: x2
     once per axis; other cases are an owner decision (question 5);
   - outputs: per-axis B passes to run, the processing size, the bypass reasons.
2. **Two-axis 1-D luma re-base** that matches the kit's Python port within 1 LSB (separable Lanczos-3, r16 quantisation
   after each pass, H then V). VP's chroma and one-axis path already match the port, so the work is enabling the same
   path for two-axis luma behind a switch, plus an answer to black-bar ringing (re-base only the active rect with
   edge handling, or keep polar on bar content). Needed before C can be enabled and also to retire polar; with C
   dropped, it is optional.
3. **`route_4k` stage** (entrance stage 2b): runs B per axis, quantises to r16 between axes, applies the per-axis crop
   and aspect factors, produces the processing planes, hands luma to the classical 1-D chroma x2.
4. **8K placeholder** (owner: code and tests now): the policy's >4K branch, the net bypass flag, and the motion
   `SearchHalvings = 2` plumbing with the NVOF-size fallback to VP's matcher. Unit tests only; no 8K capture exists.
5. **Fingerprint gating** for B and D with upstream nets recorded; labels for A (still no codec/bpp/FGS source in VP).
6. **Visible bypass reasons** (all through `NetBypassSet` to Ctrl+I, the Config status, Ctrl+Alt+D and the one-time
   notice): fingerprint mismatch (named field), 8K rule, cost gate, VRAM guard, no output scaling pass (D), deband or
   texture restore on (D), A running (D), vectors in use (D), low latency (A_motion), format or capability missing,
   compile/preparing, source axis not 2x-eligible.

### B4. How it extends WP-10

WP-10 wires A (front end) and D (POST_KERNEL seat in `AdaptiveDownscale::Blending`, +1 h for the seat) only; B, C,
route_4k, chroma, the 8K placeholder and refine stay held (byte-identical, zero cost). N9 adds B, the 4K route and the
policy on top of WP-10's `NetHost`. To avoid a rewrite, ask WP-10 A to build `NetHost` generic from the start: a list of
net slots (A, B_h, B_v, C_h, C_v, D) each with its own runner, `SlotTiming`, bypass entry, and one shared cost gate.
That is about +1 h on item 6 and saves about 3 h later. WP-10's rules stay: nets off is byte-identical; shaders and
weights never in the repo; keys `nets_enabled`, `nets_repair`, `nets_d`, plus `nets_route_4k` (new), under the
`control.net_runtime` / `scaling.route_4k` slots, with Original-pathway entries set off.

### B5. Dependencies on TrainKit rounds 2 and 3

| Need | From | If not delivered |
|---|---|---|
| Which D (24 ch / D_small / D_motion), per tier | Round-2 comparison with VP-measured cost (needs P0) | Ship D_small; D off |
| A kept or dropped | Round-2 A diagnostic | If dropped: WP-10 A becomes only the label/ID plumbing; N9 loses nothing |
| Trained B | Last in round-2 order, "optional"; plus round 3 | No B: the 4K route runs the classical 1-D x2 (Lanczos) and the "always 4K" rule stays classical; B is never promised |
| Round-3 exporter fields (`upstream_key`, `upstream_nets`, pass order, formats, pipeline version) | Round-3 kit | Stamp sidecars per net; B/D bypassed visibly until stamped |
| D fine-tune on the finished engine's 4K images | Round 3, after the runner, 4K route and policy are in | D stays a strong starting point; no fine-tune |
| Cost-aware D architecture (half-resolution trunk) | Round 3 request (needs the P0 numbers) | D_small at the measured cost |

## C. Ordered packages

Hours = agent wall-clock; tokens approximate. O = opus, S = sonnet. Runner first.

| # | Package | Scope / files | Hours | Tokens | Model | Depends |
|---|---|---|---|---|---|---|
| P0 | **Measure the truth** (before any design is fixed) | A standalone `NetBench` (or `PlNetRunnerGpuTests` cases) in `nets/tests`; arch-only exports (the kit's `arch_shaders`: weights are irrelevant for timing; D_small, B, A, C at the real sizes); per-pass and total `pl_timer`, warm; exported vs fused, rgba32f vs fp16; CPU time per `Run`; first-use compile time; VRAM before and after; GPU vs CPU reference on the 3090 | 3-4 (S writes the harness and scripts; O reviews numbers) | 250k | S + O | none |
| P1 | **Runner V2** | `NetFusion`, `PlNetRunner`: texture aliasing, fuse remaining passes, register blocking, per-Run CPU cost, pre-warm, `RunStep`, cost gate (hooks into `SlotTiming`/`NetBypassSet`) | 8-10 | 700k | O (S unit tests) | P0 |
| P2 | **WP-10 A stage** (plan item 6), with generic `NetHost` slots + cost gate | per plan-wp16-wp9-wp10 | 6-8 plus 1 | 400-500k | O | S1, WP-9.0, P1 gate |
| P3 | **WP-10 D** (item 10): `AdaptiveDownscale::Blending` seat; D cost gate; repeat-reuse determinism check | per plan | 4-6 | 200-300k | O | P2 |
| P4 | **Processing-resolution policy + 8K placeholder** (pure code, tests, motion `SearchHalvings` plumbing, no stage yet) | `src/VP-V/core/` (new policy header), tests; `nets/NetRunner.h` reuse | 3-4 | 250k | O | none (can start now) |
| P5 | **Two-axis 1-D luma re-base** matching the kit port within 1 LSB, black-bar handling, fingerprinted | `modules/scaling/rebase/SourceRebase.*`, tests, switch key | 6-8 | 450k | O | kit port bits (kit read-only) |
| P6 | **route_4k stage + B wiring**: per-axis B, r16 hand-off, aspect/crop, classical chroma x2 (coordinate with N8), `nets_route_4k` key, slot README/MANIFEST | `modules/scaling/route_4k/`, `NetHost`, entrance seat | 8-10 | 600k | O (S settings reference/tests) | P2, P4, P1, trained B or stamped arch export |
| P7 | **C wiring**, default off, only if rounds 2/3 show value on non-2x cases | `NetHost`, re-base seat | 3 | 150k | O | P5, kit evidence |
| P8 | **Visible bypass reasons, Config controls, Ctrl+I lines, docs** for all of the above | `VpStatusText`, `VpvSection`, settings reference, tests | 4 | 300k | S drafts, O reviews | P2, P3, P6 |
| P9 | **DirectML tensor-path spike** (gated): D3D12 device on the same adapter, shared texture handshake, D_small and B as DML graphs from the export weights, fp16 numerics vs the fingerprint rule, timing vs P1 | new `nets/DmlNetRunner.*`, spike only | 10-12 (spike); 25-35 more if it goes | 800k (spike); 1.5-2M if it goes | O | P0, P1 results; decision gate below |
| P10 | **GPU qualification** (both machines): P0 harness re-run on the wired build, chain timing at 24p/30p/60p with nets on, VRAM, hitch on first use, A/B vs nets off | tests, logs | 6 plus owner time on the HTPC | 300k | O | P3, P6 |
| P11 | **Round-3 asks to the kit** (written spec): half-resolution trunk D/D_small, exporter fields, an arch-only cost export per variant | docs only | 2 | 100k | O | P0 |

Totals: P0+P1 (runner) 11-14 h; WP-10 items 6 and 10 as planned 10-14 h; N9-specific P4-P8 24-29 h; spike 10-12 h;
qualification 6 h. About 65-75 agent hours in all, about 4.5-5.5M tokens (go-case of P9 adds 25-35 h). Ordering: P0, then
P1 and P4 in parallel (disjoint files), then P2 (after S1 and WP-9.0, which the batch plan already sequences), P3, P5 in
parallel with P3, P6, P8, P10; P9 only on the gate below; P7 only on kit evidence.

**P9 gate:** go if, after P1 and P0, any of these holds on the 3090: B both axes above 11 ms; D_small or D above the
60p allowance of the case the owner wants; or the laptop 4K30 goal for D_small stays above 12 ms. Otherwise skip the
spike and keep the D3D11 runner. Stop the spike if the DirectML graph is not at least 2x faster than V2 on the same
net, or if outputs differ from the fp32 reference by more than 1 code at 10-bit.

### GPU measurements needed

| What | Machine | Why |
|---|---|---|
| pl_timer per pass and per net, warm, exported vs fused, rgba32f vs fp16, A / B / D / D_small / C at real sizes | **3090** (cinema PC; owner copies a small bench folder) | Settles the 3.7 vs 8.6 ms question; fixes every row of A2 |
| Same set | 3050 Ti laptop (mains, full clocks, NVIDIA adapter asserted) | Confirms efficiency ratios (the README's laptop runs are the only measured data) |
| CPU ms per `Run` on the render thread; first-use compile time; VRAM peak and after-aliasing | both | Hitch and 4 GB budget |
| GPU vs CPU reference equality on the 3090 (gate decisions included) | 3090 | Required by WP-10; never run in the integ tree |
| Chain: whole-frame GPU time at 24p / 30p / 60p LL with the net on, with the real rest of chain (not the table's estimate) | 3090 (and laptop 4K30) | Replaces A4's plus or minus 40% |
| DirectML spike timing | 3090, then laptop | Only if the gate is met |

### Risks

1. The 3090 numbers may still come in at 6-9 ms for D. Then D is a 24p/30p feature, and D_small the only candidate for 60p.
2. VRAM: 2.2 GB for D at fp32 without aliasing. With aliasing about 0.9 GB; with fp16 storage 0.45 GB. 4 GB laptops are tight either way.
3. First-use compile stalls (hundreds of KB of literal weights) look like a hang; needs the pre-warm and a visible state.
4. Time-slicing adds latency (up to a few refreshes); incompatible with the 60p low-latency mode, which must keep the cost gate.
5. B's hand-off quantisation (r16 between axes) must be preserved by any fusion or DirectML port.
6. Two-axis 1-D luma re-base can ring into black bars; the nets assume the cropped active picture.
7. D inside `pl_render` runs per refresh without warp or reuse (2-2.5x at cadence repeats).
8. DirectML spike: driver variance, fp16 numerics, a second Microsoft library, and VRAM for copies; the 1-code rule may fail.
9. A may be dropped by the round-2 diagnostic; B may never be trained; the plan must not depend on either.
10. fp16 in the fingerprint: round-1 D drifted 0.45 code at fp16. The README allows fp16 when within 1 LSB; N6 says "no hidden fp16 truncation". Both can hold only if fp16 use is visible and gated (question 3).

## Owner questions (plain language, with a recommendation)

1. **Make the picture-improving net (D) four times cheaper by changing how it is built?** It would work at half the
   detail grid internally (as the repair net A already does), which cuts its work by about 4x and lets weaker graphics
   cards use it. It means training D again in round 3 (about 12 hours on the 3090). Recommend: yes, ask for it in round 3.
2. **May VP use one extra Microsoft component (DirectML) to run the nets on the graphics card's special AI hardware?**
   It could make the nets 2-4x faster, so the big restore net (B) and 60p become possible. It adds work (roughly a week
   of agent time if it works) and small numerical differences. Recommend: not yet; first measure the 3090 (about an
   hour), then decide from the numbers.
3. **Half-precision numbers.** Rule today: allowed only if the result stays within one 10-bit code of the full-precision
   result, and always shown on screen. It would mainly save memory on the laptop. Keep that rule, or never allow it?
   Recommend: keep the rule (visible, per net).
4. **60p sports with the lowest delay.** Allow only the repair net A (and D_small if it fits), and switch the rest off
   with a visible note? Recommend: yes.
5. **Sources below 1080p-class (for example 1440p, 720p).** Doubling 1440p gives 2880p, twice the cost of 1080p, and
   720p doubled is still below 4K. Recommend: double once per axis only for sources up to 1080-class; treat 1440p as
   already native (straight to the high-quality downscale); never double twice.
6. **Laptop at 4K30.** The nets will simply be off there (shown on screen), except the repair net on 1080p sources. Is
   that acceptable until round 3's cheaper D is measured? Recommend: yes.
7. **If the restore net B proves too slow for 30p and 60p, ship it for 24p film only** (shown as "bypassed" for other
   rates) and keep the classical resize otherwise? Recommend: yes, if P0/P1 do not bring it under about 6 ms.
8. **Round-3 timing.** Measured costs (P0) should come before the final D fine-tune so you choose the right D per
   quality level. OK to delay the fine-tune until P0 and P1 are done? Recommend: yes.

## What I verified vs assumed

Verified in code or files: runner structure, fusion rules and limits, pool keying (no aliasing), timer polling, the
8K rule and its test, route_4k and refine are placeholders, the two-axis luma polar fallback, the MAC counts (from the
smoke export), the measured laptop timings, the plan items 6/10/N9 text. Assumed (labelled estimates): 3090 efficiency
(40% fused, 55% V2), the rest-of-chain figures, DirectML speeds, the 2.2 GB VRAM figure, A_motion +25%, Ampere fp16
rate. Not found: trained B quality numbers (round-1 summary is on the T7 drive, not attached).
