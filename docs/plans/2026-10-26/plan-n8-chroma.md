# N8: Chroma upscale plan (read-only assessment, 2026-10-26)

Tree: integ-batch1 @ d85519e2. Costs are estimates from kernel arithmetic, not measurements. Agent hours = wall clock.

## 1. What exists today

- **Slot.** `scaling.chroma_upscale` (order 120, status placeholder, side input, hook prepass) is in docs/design/slots.json:797 and components-master.json:481. Contract: output 2 planes (luma + full-res CbCr), shift 0, delay 0. Planned key `chroma_upscale_vp` (not in the schema). Existing key `chroma_upscaler`. Fallback text: "Off = libplacebo's chroma upscaler". Live file listed: shaders/CfL_Prediction.glsl (opt-in, graphics/subtitles). No code behind the slot.
- **Decision on record** (VP-pipeline-final.md section 3.1, research/chroma c7564afa, 10-24): slot stays EMPTY for now; libplacebo 'auto' (Catmull-Rom) is the default for every tier; CfL is opt-in; siting fixes applied.
- **How chroma is upscaled now.** libplacebo `plane_upscaler`. Set in LibplaceboRenderParameters.cpp:~458 from `chroma_upscaler` (auto = catmull_rom; `upscaler` = leave unset, so Bill's upscaler is used; also ewa_lanczos(sharp), lanczos, bicubic, bilinear). Chroma is upscaled inside pl_render, at the same time as the main scale, from the chroma plane to the luma size, using `chroma_location`.
- **Siting tags** (LibplaceboVideoRenderer.cpp:~19225). Legacy CPU capture to P010 keeps even chroma lines only, so it is tagged TOP_LEFT. Decoded files (NV12/P010/P016) and 4:2:2 uploads are tagged LEFT. The Original pathway keeps upstream's tag (ADR-0019). RGB uploads are not tagged.
- **Bill's `chroma_downsampling`** (AVERAGE/LEGACY/ADVANCED, MainConfigSchema.h:259) is the opposite direction (RGB to 4:2:0/4:2:2 in the CPU formatter, plus P210 ingress for v210/UYVY so 4:2:2 rows are kept). It is the real source of the "chroma already lost" case on RGB captures.
- **Re-base.** The re-base scales each plane's shift with its factor (VP-V/core/ChromaSiting.h; nets doc says one-axis luma plus chroma 1-D, matches Lanczos-3 within 1 LSB). It works on the planes, so it resamples chroma at its own resolution.
- **Ingest.** Native RGB is one rgba16 plane plus an r16 matcher plane (GpuRgbUnpack): no chroma problem. DV tunnel (R210 formatter, GpuDolbyVisionTunnelUnpack): 12-bit I plus alternating 12-bit chroma, so chroma is half-width inside the tunnel and is expanded by the unpack. P210 (4:2:2) uploads exist (`lossless422Upload`); `chromaShiftY=0` is set for 422 in the dejudder path.

## 2. Where chroma quality is lost

1. **Siting:** a wrong tag shifts colour 0.5 source pixel (the fixed TOP_LEFT/LEFT cases; remaining risk is odd sizes `(w+1)/2`, crop/A-lens/NLS geometry, and any VP stage that resamples chroma without `ShiftAfterResample`).
2. **Double resampling:** with re-base on, chroma is resampled by the re-base and again by the plane scaler in pl_render. The slot would collapse that to one controlled upsample.
3. **Precision:** CPU P010 truncates to 10 bits; 12-bit RGB (R12B/R12L) and RGB 4:4:4 lose chroma at ingest unless native ingest is on.
4. **Codec:** the real loss is upstream (4:2:0 streaming codecs, Apple TV chroma upsample). No upscaler recovers it, only plausibility.
5. **BTB/WTW:** libplacebo clips in conversions; any VP chroma pass must not clamp (contract: no clamp, full frame incl. bars).
6. **Ringing:** Lanczos/EWA chroma overshoot at saturated edges; Catmull-Rom is the safe default.

## 3. Options

- **A. Keep libplacebo (Original = Bill's).** Zero cost, zero risk. Already the default. Selectable always.
- **B. VP separable kernel** (Lanczos-3 or Catmull-Rom/Mitchell, per-axis, siting-aware offset per source tag, fp16 out). Cheap, deterministic, no clamp, one pass for CbCr as a compute shader. Gain over libplacebo's Catmull-Rom is small except: single resampling with the re-base, exact siting per source, no clamp, 12-bit/float precision. Matches the "prefer separable, simple" rule.
- **C. Luma-guided (joint bilateral / CfL-style)** at the processing size, guided by the full-res luma (post-repair luma preferred). Sharper coloured edges (fixes colour bleed on text/graphics); risk of faithful-image violations (invented chroma edges, grain leaking into chroma), so it must be bounded (blend limit, edge gate) and off by default. Reuse shaders/CfL_Prediction.glsl logic only after a bounded rewrite (it is graphics-oriented).
- **D. Net chroma repair add-on.** Not recommended now (section 5).
- **Placement:** an either/or with Original. In the slot, output is 2 planes (luma, full-res CbCr), shift 0, chroma_location tag set to "centre/none" so libplacebo does not shift or upsample. Runs once per source frame, cached by sequence (barrier R4).

## 4. Interplay

- **Re-base:** chroma upscale goes BEFORE the re-base (right after input, slot order 120 is before nets/refine?). Slot order 120 sits after re-base per the slot list; resolve: upscale chroma to the SOURCE luma size first, then the re-base resamples a 4:4:4 image (luma and chroma by identical kernel and shift), so the plane-shift correction disappears for chroma. Cost: re-base then runs on full-res CbCr (2 channels at source size, cheap). Needs an order check in slots.json (one-line change: put the chroma slot before the re-base, or before nets A only). Owner-visible choice not needed.
- **Motion:** vectors are on luma at source size; the chroma slot does not use or change them. Option C is guided by luma of the same frame only (no temporal use), so vector validity is unaffected.
- **Nets:** luma-only; they see the luma plane unchanged. The slot must not touch luma (byte-identical luma plane). Net A input stays what it is; chroma reaches output untouched by nets.
- **DV tunnel (12-bit):** the unpack gives luma plus half-width chroma; the slot runs after the unpack, output fp16/16-bit, so the DV path keeps 12 bits. A DV tunnel frame has special tunnelled values; chroma is NOT chroma until after the DV unpack. The slot is bypassed (and shows it) when the tunnel is not unpacked to real YCbCr. In VPV output (future) 12-bit luma with 4:2:2 chroma on the display side: the slot output is full-res, a later 4:2:2 reduction is a separate step.
- **RGB / 4:4:4:** bypass, with "Chroma upscale: not needed (4:4:4)" in the Ctrl+I line (no silent fallback rule).
- **4:2:2 (P210):** horizontal-only upscale (the vertical factor is 1); siting is LEFT for 4:2:2. One axis, cheaper.
- **Subtitles/crop/A-lens/NLS:** slot sits before geometry changes, so crop/NLS operate on 4:4:4; the subtitle cut-paste path works on a full frame already. Check subtitle detection uses luma only (it does, SubtitleBoxDetector uses analysis luma plus a chroma neutral test, which will see the full-res chroma: verify).

## 5. Chroma repair add-on: evidence needed

Justified only if real footage shows: (a) visible colour blocking/ringing/bleed beyond codec grain after the best upscaler; (b) the artefact survives with a faithful (Catmull-Rom/Lanczos) upscale at normal viewing; (c) an A/B on at least 5 real Apple TV clips (24p film, nature, sports) with the owner preferring the repaired chroma; (d) the repair does not touch grain or saturate (no hue shift, BTB/WTW preserved). Gather: clips with saturated reds/blues and graphics, chroma-only difference crops of the upscale vs source decoded as 4:4:4 where a true reference exists (some Apple TV RGB vs 4:2:2 captures of the same title). Without such evidence: no add-on. Decision point after package P3 numbers.

## 6. Recommended design and packages

Default stays Original/libplacebo. Add key `chroma_upscale_vp` = off | separable | guided (default off). `chroma_upscaler` keeps its meaning for libplacebo when VP is off.

| # | Package | Scope | Files / hook | Tests | Hours | Model |
|---|---|---|---|---|---|---|
| P0 | Verify and measure (item 61 open) | Confirm what libplacebo does with 4:2:0/4:2:2 in our path; dump both planes, siting per source tag; measure chroma plane_upscaler cost; Original gets no change | tests only (BetaGpuTests, ChainGpuProbe) | measurement report; siting matrix (TOP_LEFT/LEFT/odd sizes) | 2-3 (~0.6M tokens) | opus (judgement) with sonnet probes |
| P1 | Slot skeleton + key | `chroma_upscale_vp` schema key, slots.json status placeholder to built-off, Config status line, Ctrl+I line, bypass reasons; zero passes when off | MainConfigSchema.h, RendererProfileConfig.h, slots.json, components-master.json, LVR call site near the pre-pl_render chain (~23524) | byte-identity slot off (hash of output vs current), zero-pass counter, config round-trip | 3-4 (~0.8M) | sonnet |
| P2 | Separable VP kernel | compute shader, siting offsets from the tag, per-axis (422 horizontal only), fp16 2-plane output, no clamp, once per source frame (cache by sequence, discontinuity invalidation), libplacebo tag set so it does not upsample | new module file (host = call sites only), GLSL under shaders/, call site in LVR | siting tests (impulse and edge shift within 0.05 px per tag), BTB/WTW preservation (values below 16 and above 235 survive), 12-bit precision, odd sizes, re-base ordering, DV unpack feed | 6-8 (~1.5M) | opus design, sonnet implementation |
| P3 | Compare gate | A/B captures vs libplacebo: dE on synthetic edges and real clips; decide default | tools + doc | numbers into the chain gate | 2-3 (~0.5M) | opus |
| P4 | Guided (CfL-style) mode, optional | bounded luma-guided; blend cap; off by default; only if P3 shows chroma bleed worth fixing | reuse/rewrite CfL_Prediction.glsl | grain-leak test (flat grain patch in luma must not appear in chroma), edge dE, no halos | 6-10 (~1.5M) | opus |
| P5 | Repair add-on | only after evidence (section 5) | net runner (WP-9/10) | n/a | not scoped | opus |

Total P0-P3 about 13-18 agent hours (~3.4M tokens); P4 adds 6-10.

Dependencies: needs no net runner for P0-P4. P5 depends on the net runner and TrainKit. P2 depends on the barrier fix that makes the input side run once per source frame (R4) and the slot ordering vs WP-9/10 (nets run on luma only, but the re-base must see 4:4:4 if we choose upscale-before-re-base). Keep the either/or: Original forces off.

## 7. GPU cost per frame at 4K (estimate)

Chroma plane 1920x1080 to 3840x2160 (4:2:0), or 4:2:2 horizontal only.

| | RTX 3090 | RTX 3050 Ti laptop |
|---|---|---|
| libplacebo plane_upscaler (Catmull-Rom, today) | ~0.15-0.25 ms | ~0.6-1.0 ms |
| P2 separable (6-tap, 2 passes or 1 compute) | ~0.15-0.3 ms | ~0.6-1.2 ms |
| P4 guided | ~0.4-0.8 ms | ~2-3.5 ms |

Net effect of P2 when it replaces libplacebo's chroma upscale is about zero (libplacebo then skips its own). Confirm with P0 measurement. At 4K30 on the laptop all variants fit the budget; guided mode is the only one to watch.

## 8. Owner questions (plain language)

1. Do you want the colour (chroma) detail upscale to stay with the existing player settings by default, with our own version only as an option you can switch on? (Recommended: yes.)
2. Should I first spend about 3 hours measuring how good the current colour upscale really is, and only build our own if it shows a visible gain? (Recommended: yes; the plan stops at P0/P3 if no gain.)
3. A "sharper colour edges using the picture as a guide" mode: wanted as an optional extra, or skip? It is the only mode that could look different from the original image, so I would keep it off by default.
4. For colour repair of streaming artefacts (blocky or smeared colour): I suggest waiting until you have seen real clips where it bothers you. Can you flag such titles when you spot them?
5. For the cinema chain, is it acceptable that this stage is skipped (and says so on screen) for RGB sources and for Dolby Vision tunnel frames that are not yet decoded into normal colour?
