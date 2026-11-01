# Chroma upscale: chroma-from-luma (`scaling.chroma_upscale`)

Unit: **scaling** | side: input | order: 120 | status: **placeholder** (CfL module code and tests present; the slot stays a placeholder until the renderer call site lands, so it costs nothing) | hook point: prepass (before libplacebo renders the source frame)


**Grain map: one producer, many readers (owner rule 2026-10-31).** `Gpu::Params::grain` (RegionEvidence::GrainView, the one
read API of the single `grain.split` node; its GPU form per plane: luma and both chroma planes, computed once early on the decoded source's active picture);
without them, or when they do not fit, the module computes its own and the status says "ok; own grain split". The
chroma grain is added back exactly (through the Original kernel) in this module's output, which is the end of its
source-domain work; no later stage re-estimates grain from it.

## Purpose / why

libplacebo upsamples 4:2:0 / 4:2:2 chroma with a generic kernel. This module predicts the missing chroma detail from the
full-resolution luma (chroma-from-luma, a guided-filter local regression) where the two are locally correlated, and falls back
to the plain Catmull-Rom of the chroma elsewhere, so a failed prediction cannot do harm. The grain stays exact: the grain is
subtracted before the regression and added back unchanged at the end. When active, libplacebo receives 4:4:4 with zero
shifts and does not upsample again.

## Contract

- Input: luma + subsampled chroma (4:2:2 / 4:2:0, any `fx = W / cw`, `fy = H / ch` >= 1) at the processing size, as a `pl_frame`
  (plane 0 = Y, chroma in plane 1 with CB and CR, or in planes 1 and 2)
- Output: 2 planes: luma unchanged + a full-resolution CbCr plane (2 components, >= 16 bit, shifts 0; barrier R4)
- Geometry: chroma siting from `pl_plane::shift_x / shift_y` (luma-pixel units; -0.5 = left / co-sited on a 2x axis, 0 = centred);
  the active picture rectangle (below)
- Delay share: 0 frames; history 0; look-ahead none
- 4:4:4 input: nothing to upsample (the frame is returned as is, status "4:4:4: nothing to upsample")

## Algorithm

All values are codes of the signal (normalised value / code unit). The CPU model `ChromaUpscale::Upsample` (`ChromaCfl.h`) is the
specification; the GPU passes mirror it in float.

- **A. Grain guard.** The luma crop and each chroma crop go through `repair.grain_split` (the shared split product):
  `clean = base + s1 + s2`, `grain = X - clean`. The regression runs on the clean pictures; `G = chroma - clean chroma` is
  carried to the end. Luma block `B = max(1, floor(16 Scale(lines) + 0.5))`; chroma uses `Scale(lines / fy)`, with
  `Scale(l) = l > 2160 ? l / 2160 : 1`.
- **B. Luma at the chroma sites.** A separable tent of the clean luma with widths `fx`, `fy`, centred on the chroma sample at
  `px = (jj + 0.5) fx + shiftX - 0.5`, normalised, rows inner then columns. (2x co-sited gives [1/4 1/2 1/4], 2x centred
  [1/8 3/8 3/8 1/8].)
- **C. Regression** on the chroma grid in a (2r + 1)^2 box (r = 2): `a = cov / (varY + eps)`, `b = mC - a mY`,
  `rho2 = cov^2 / ((varY + eps)(varC + eps))`, `eps = (0.002 (2^depth - 1))^2`; `a` and `b` are box-averaged again (guided
  filter), `rho2` is not. Per channel (Cb and Cr), same luma. `lo / hi` = min / max of the clean chroma over 3 x 3. The GPU
  and the model use centred two-pass moments (equal to `E[Y^2] - mY^2` in exact arithmetic, but exact in float at codes of ~1000).
- **D. Upsample** to every luma pixel of the active picture with Catmull-Rom 4 x 4 of `a`, `b`, `rho2`, the clean chroma, `lo`,
  `hi` and the grain `G`:
  `out = base + M (clamp(A Yc + B, min(LO, HI), max(LO, HI)) - CR(a Ylo + b)) + G` (residual form: only the luma-predicted detail beyond what the Original kernel already carries is added, so grain the split left in the clean chroma is kept; the prototype's replace form `M pred + (1 - M) base` softened that grain by 6.5% in ChromaCflGrainIsPassedNotSmoothed), `M = clamp(CR(rho2), 0, 1)`. There is no range clamp:
  blacker-than-black and whiter-than-white chroma codes pass (BTB / WTW preserved); the only clamp is the local neighbour range
  of the clean chroma, and the grain is added after it.
- **E. Bars.** Outside the active picture the output is the plain Catmull-Rom of the RAW chroma with taps clamped to the bar's own
  side, so a flat bar stays exactly its code.

## Active picture rule

Inside the active picture (letterbox / pillarbox) EVERY tap (grain split, luma downsample, box windows, min / max,
Catmull-Rom) clamps at the picture's border: luma at the luma rectangle, chroma at the chroma rectangle
`[floor(left / fx), ceil(right / fx)) x [floor(top / fy), ceil(bottom / fy))`. The bar boundary is an edge; nothing reads across it,
so the picture's output does not depend on the bar level, and the bars never see the picture. Rectangles aligned to the
subsampling factors are exact; an odd boundary puts one chroma row / column shared by bar and picture on the picture side.
An empty rectangle means the whole plane.

## Settings

- Key `chroma_upscale_vp` = `off` | `cfl` (case-insensitive), default **off** (until GPU-validated); Original pathway: off.
- Existing key `chroma_upscaler` (libplacebo's own chroma upscaler) applies whenever this module is off or has failed.
- Fixed law (not user settings): radius 2, `epsFrac` 0.002, neighbour range 3 x 3. The model-only knob `CflParams::grainGuard`
  (false = regression on the raw planes, no add-back) exists for tests.

## Cost (estimates; ChromaCflGpuTiming measures on hardware)

- Per luma pixel the UP pass reads 16 taps x 5 textures = 80 texels (+ 1 luma split texel); the chroma-grid passes read about
  25 (LO) + 60 (STATS, both channels) + 25 (AVG) texels per chroma pixel; three grain splits (luma, Cb, Cr; the chroma ones at a
  quarter of the pixels each for 4:2:0).
- VRAM (`CflModule::TextureBytes`): 623,639,040 bytes (~595 MiB) at 3840x2160 4:2:0, ~149 MiB at 1920x1080 (the three splits'
  rgba32f textures dominate).
- Time, 4:2:0 at 4K: about 5 ms on an RTX 3090 (splits ~2.3 ms, UP ~1.5-2 ms, the rest ~0.5 ms); about 18-20 ms on the RTX 3050 Ti
  laptop. At 1080p about 1.3 ms and 5 ms. Estimates only, to be replaced by the timing test's numbers.

## Fallbacks

- "Fallbacks engaged" entry: `Chroma upscale: libplacebo (<reason>)`.
- Behaviour: any failure (no storable texture, unsupported plane layout, crop of a non-storable format, failed pass) returns false
  with `LastError()`; the renderer then renders the original frame and shows the entry. Off, 4:4:4: no pass.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `scaling.refine`, `repair.grain_split` (the grain guard reads the shared split on luma and both chroma planes)
- Declared product: `ChromaUpscale::kProduct` (`chroma.cfl`: source.luma, source.chroma, grain.split; GpuCompute; history 0)

## Renderer call sites NEEDED (not implemented in this folder)

- In the renderer, before `pl_render_image` of the source frame: if `chroma_upscale_vp == cfl` and the frame is subsampled YCbCr, call
  `CflModule::Apply` on the source `pl_frame` and pass the output frame (4:4:4, shifts 0) to libplacebo. On failure render the
  original frame and show `Chroma upscale: libplacebo (<reason>)` in the fallbacks status.
- Add `ChromaCflGpu.cpp` and the grain split's `GrainSplitGpu.cpp` to `VideoProcessor-VPRenderer.vcxproj` and
  `VideoProcessor-VPRendererBeta.vcxproj`; the test project compiles `tests/ChromaCflTests.cpp`.
- Plumb the active picture rectangle: `QueuedFrameStatistics::PictureRect` scaled to the plane (luma pixels), and `lumaLines`
  (the picture's lines) for the grain split's Scale.
- Read `chroma_upscale_vp` with `ChromaUpscale::ParseSettings`; Config editor control and the RendererProfileConfig key.

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU hardware tests only on mains with `VP_TEST_GPU=hardware`.

- `ChromaCflModelTests` (CPU): flat chroma exact (incl. BTB 40 / WTW 1000), siting at chroma sites (left-sited, co-sited), luma-correlated
  edge beats plain Catmull-Rom, 4:2:2, 4:4:4 = nothing, chroma grain passed not smoothed (isolate RMS within 5% of the plain
  upsample), determinism, letterbox / pillarbox bars exact and picture independent of the bar level, settings parse.
- `ChromaCflGpuTests` (WARP unless hardware): GPU vs model within 0.02 codes (4:2:0 left-sited and co-sited, 4:2:2, letterbox,
  pillarbox), bars bit-exact, off returns the same frame, output frame shape (2 planes, full size, zero shifts), planar =
  semi-planar, determinism; `ChromaCflGpuTiming` (category GpuHardware).

## Known issues / open items

- Renderer call sites and the Config control are not wired yet; the module is not dispatched in the player.
- Untested on real decoder textures (the crop needs a storable chroma format; otherwise the module fails visibly and libplacebo
  upsamples).
- Output code unit follows the source's bit encoding (`VpvGpu::CodeUnit` of the output texture); verify with libplacebo's decode of
  a 16-bit output when the source is 8-bit (sample_depth 8).
- interference with crop/A-lens/NLS, DV tunnel, subtitles (item 61)

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `ChromaCfl.h` (CPU model, settings parse, `kProduct`), `ChromaCflGpu.h` / `ChromaCflGpu.cpp` (GPU module)
- `tests/ChromaCflTests.cpp`

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0011 in [docs/decisions](../../../../../docs/decisions/README.md)
- Prototype: `tools/eval/candidates_chroma.py` (`cfl`) on the feat-eval-prototypes branch

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `chroma_upscale_vp`, `chroma_upscaler`.
- Tiers: not tiered until a component exists.
- VP-V page, Scaling tab: tiers Classic / Low / Mid; tier keys `adaptive_downscale`, `adaptive_w_base`, `source_resolution`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#5-scaling](../../../../../docs/settings/VP-settings-reference.md#5-scaling).
- Fallback entry ("Fallbacks engaged"): `Chroma upscale: libplacebo (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): byte-identical with the slot off; zero GPU passes when off.
- Isolation (fixed inputs, expected outputs, cost): MISSING.
- Contract (formats, geometry, fallback entry): libplacebo does not re-upsample when the slot outputs 4:4:4.
- Chain (total delay at the 4/3/2-frame budgets): MISSING.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->

## Grain split read path (SA2)

The grain guard reads the three shared grain splits (luma, Cb, Cr) ONLY through the one accessor of repair/grain_split
(GrainSplitGpu.h): `ViewGlsl` / `BindView` per plane, with the names prefixed per plane (gsL_, gsB_, gsR_: the three frames may
differ in layout, one shader binds several; the prefix is applied locally in ChromaCflGpu.cpp to a copy of the accessor text and
to the descriptor names). Shaders fetch `gsL_fetch(p)` etc. = (s1, s2, f, base) in codes and form clean = base + s1 + s2; no
direct texelFetch of a grain texture remains. Full generates the same math as before (output byte-identical).

Fallback rule, per plane: a shared product that fits (size, block, scale, code unit, component) is read; otherwise that plane
uses its OWN Full split, visibly, and returns to the shared product the first frame it fits again (status recomputed every
frame). Status keeps "ok; own grain split" / "ok; own grain split (the shared product does not fit)". `LastGrainStatus()` is
Ring (all planes read the shared products or the guard is off), Missing (a plane had none, none misfit) or WrongSize (a product
exists but does not fit); Shed is the node's. `GrainFallbackDecision()` is
GrainSplit::ReaderFallback("chroma-from-luma", status): the catalog entry "grain split (chroma-from-luma)". Tests:
ChromaCflGpuGrainProduct*, ...FittingFullProduct*, ...CompactProducts*, ...GrainFallbackRecoversAutomatically (WARP).

## Private GPU textures (skeleton rule)

This module still creates its own GPU textures through the `Ensure` wrapper (its persistent state and, when no shared product fits, its own grain split; see the static test src/VP-V/tests/PrivateTextureScanTests.cpp). It migrates in SA6/SA7/SA8/SA12 (doc D:\codex\vp-handover\51-skeleton-adoption-audit.md section 6): kept state moves to skeleton product nodes and per-render temporaries to the shared scratch arena (src/VP-V/core/ScratchArena.h). Its grain reads already go through the one accessor (`gs_fetch` / `gs_blocks`); a shared product that is Missing, Stale (another frame or unit version, `Params::grainSeq`), WrongSize or Shed (`Params::grainShed`) makes it use its own split, visibly (FallbackCatalog "grain split (...)"), and it recovers on the first fitting frame.
