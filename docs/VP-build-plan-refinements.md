# Build plan — refinements D1–D5, S1, S2, BUILD-STATUS (to the cut)

2026-10-12. Implements cut-plan items 3–5 (docs/VP-cut-plan.md) and
docs/VP-refinements-dejudder-dither.md.

**Owner decision (2026-10-12, departs from the cut plan's per-step
gating):** build every remaining item first, then run ONE full gate
(full matrix on 377 then 7.360, GeForce runs) and ONE audit. Each
item still gets its own tests and its own setting; they are run
together at the end, not after each step.

## Ground rules

- Every item has its own setting. "Off" reproduces today's output exactly
  (an off-state test per item compares against the current path).
- Nothing adds a per-refresh pass: all new work is per source frame inside
  the matcher, or a uniform/texture read in passes that already run.
- No host readback of GPU results (the field stays on the GPU). Per-frame
  scalars that steer strength are computed on the GPU and read by the warp
  shaders.

## De-judder (src/VideoProcessor-Lib/vprenderer/DejudderWarp.*)

### D1 — global-motion prior (`dejudder_global_prior: on`)
- New pass **GLOBAL** before SEARCH: one 16×16 workgroup at L2 (1/32 res).
  Each thread scores one or more candidates in ±8 L2 texels (17×17) by
  whole-frame SAD over a strided interior subset of L2. A shared-memory
  argmin, then a ±1 half-texel refinement at L1. 4K cost is about 2 M
  texel reads, about 0.1 ms.
- Output: a 1×1 RGBA16F `globalVector` texture: (vx, vy) in L1 texels,
  z = best/second-best SAD ratio (confidence), w = 1.
- SEARCH: an extra L1 candidate `v1g` centred on the global vector, next to
  v1z/v1c, then refined at half resolution like v1c. When the confidence is
  poor (ratio > 0.9) it is skipped, so the search behaves as today.
- Plumbing: the texture is added in three places (header field,
  `EnsureTextures`, destructor list). Binding is added to `Search()`.
  `Settings::globalPrior` is a bool.

### D3 — vector smoothing (`dejudder_vector_smooth: on`)
- New pass **SMOOTH** between CONSISTENCY and FRAME_TRUST. It reads
  `field`, writes `field2`, then swaps, so that `Field()` returns the
  smoothed one.
- Per block, it takes the component-wise median of the trusted vectors in
  its 3×3 neighbourhood, including itself.
  - When at least 5 neighbours are trusted and the block's vector is more
    than 1 source px from that median, the vector is replaced by the median
    and the block stays trusted.
  - An untrusted block surrounded by at least 7 trusted neighbours that agree
    (spread ≤ 0.5 px) takes the median and becomes trusted.
  - Everything else is unchanged.
- The SAD (w) is kept, so the Stage 8 grain-error pass still sees the
  block's own match quality.

### D2 + D4 — motion- and trust-scaled strength
Keys: `dejudder_motion_scale_lo: 0.25`, `dejudder_motion_scale_hi: 1.5`
(source px per source frame; hi = 0 turns D2 off). `dejudder_trust_scale:
0.6` (the trusted share at which strength is full; 0 turns D4 off).

- FRAME_TRUST already reduces over all blocks. It is extended to also
  compute the mean |v| of the trusted blocks (falling back to the D1 global
  vector when that is confident). It writes:
  - `.r` = trusted share (unchanged);
  - `.g` = strength scale, = ramp(|v|, lo, hi) × ramp(share, minShare, trust_scale);
  - `.b` = mean |v| (diagnostic).
- **Pumping guard:** `.g` is rate-limited to ±0.25 per source frame against
  the previous frame's value, kept in a second 1×1 texture (ping-pong).
  A scene cut (share < minShare) still holds at once, as today.
- **Consumers:** WARP_LUMA/WARP_CHROMA (DejudderWarp.cpp) and WARP_BODY
  (RenderedWarp.cpp) multiply `shift` by `frame_trust.g`.
  - This is done in the bodies, not inside `field_vector()`, so the Stage 8
    grain map (which reads the true vectors) is not scaled.
  - When D2 and D4 are both off, `.g` = 1 exactly, so the output is
    identical to today's.
- Host: settings sync at LibplaceboVideoRenderer.cpp ~11034. No change to
  `shift` at ~11372 (scaling is GPU-side).
- **Stats overlay:** the warp log line gains "scale" only in tests
  (hostReadable). Live, the overlay keeps the configured s.

### D5 — occlusion fill (`dejudder_occlusion_fill: off`)
- In the warp bodies, an untrusted position whose block the B-side
  occlusion map marks "new" (uncovered) is filled from B at the same
  position. That position takes B's own block vector × (shift − 1) when B's
  block is trusted, otherwise zero. A covered or plain-untrusted position
  keeps the hard hold of A.
- This needs B and the backward field bound in the warp: two more bindings,
  installed only when the switch is on. It is off by default and is an
  experiment for the projector.

## Dither

### S1 — grain-aware carry (`dither_grain_tolerance_scale: 3.0`; 1 = off)
- The sigma-delta (a user shader at OUTPUT) cannot bind VP textures. The
  OcclusionMaskHook already writes alpha at OUTPUT just before it, so alpha
  carries both signals:
  - not occluded: a = 1 − 0.49·g;
  - occluded: a = 0.49·g;
  - where g ∈ [0,1] is the grain amount from the Stage 8 grain map:
    g = (1 − map.r) / (1 − grain_floor) on trusted blocks, else 0.
- The sigma-delta's existing test `src.a < 0.5` (occluded) is unchanged.
  The block search pass decodes g (averaged per block) and uses
  `match_tolerance × mix(1, grain_tolerance_scale, g)`.
- New shader PARAM `grain_tolerance_scale` (default 1.0) is set by the
  renderer the same way as `occlusion_mask`.
- The mask hook is installed whenever S1 is active and a grain map exists,
  even with the occlusion mask off. It then writes "not occluded" with g.
- **Off-state proof:** scale 1 or no grain map gives the same carry decisions
  as today, and alpha stays 0/1 when S1 is off.
- **Payload bound:** unchanged (±0.5 code). Only which blocks carry changes.

### S2 — superseded by the planner (2026-10-12): rgba32f for the output pass
Planner answer 1 (docs/VP-build-plan-refinements-answers.md): not unorm.
A fork-only `pl_render_params.output_fbo_float32` (default false) gives the
output pass (PRE_OUTPUT image, OUTPUT-stage hook FBOs, dither input)
rgba32f, falling back to the existing choice; rgba16f stays before colour
management. VP sets it unconditionally for the VP Renderer. If the output
pass's format is not separable in 377 without a larger change, S2 is left
out of the cut. Tests: PRE_OUTPUT/OUTPUT FBO formats float32 with the flag;
max error at codes 900-1019 below 1/16 step; BTB/WTW through the output
pass with Stage 10 off and on; the full matrix.

D3 addition (answer 4): a promoted block is scored at the median vector and
must be within `match_tolerance` before it is trusted. D2/D4 defaults and
the ±0.25 rate limit: approved as written.

#### Original S2 analysis (unorm), kept for the record
**Risk found while planning:** libplacebo picks ONE FBO format for every
intermediate (renderer.c `find_fbo_format`). A global switch to rgba16 unorm
would:
1. **Cut BTB/WTW and out-of-range colour** (clamps to [0,1]) when Stage 10
   is off. With Stage 10 on, the 10b remainder texture already carries them,
   so the picture is safe.
2. **Crush deep shadows in linear-light SDR scaling.** Code 65 vs 64 is
   about 1e-7 apart in linear light, far below the 1.5e-5 unorm16 step.
   libplacebo already turns linear scaling off for HDR on unorm FBOs, but
   not for SDR. rgba16f is exact there. This contradicts the gain S2 is
   for (the highlight floor).
3. Change scaler ringing near 0/1. Clamped lobes would act as a soft
   anti-ringer.

**Proposal (needs planner OK before building):**
- Add a fork-only `pl_render_params.prefer_unorm_fbos` flag (default false,
  so upstream behaviour is unchanged).
- VP sets it only when Stage 10 is on AND the scaling is in gamma light
  (no linear/sigmoid) — the case where unorm16 is strictly better.
- Otherwise rgba16f stays.
- Tests:
  - OUTPUT FBO format is unorm when the flag applies, float otherwise;
  - highlight precision (max error at codes 900–1019 below ¼ code);
  - BTB/WTW still kept with Stage 10 on;
  - the full matrix.
- Rebuild with the MSYS2 toolchain already used for 7.377-vp
  (C:\dev\libplacebo-build). Bump the DLL's VP patch tag. Copy it into
  3rdparty/libplacebo-beta/bin.
- 7.360 stays unpatched (S2 is Beta-only, the deployed libplacebo).

## BUILD-STATUS.md (extend docs/BUILD-STATUS.md)
- One row per stage/feature: setting key(s), default, test-config value,
  "first thing to check" at the projector, and the test names that cover it.
- Regression recipes: which key turns which feature off. "All off =
  Bill's VP": the exact override list.
- Known biases (7.360 repair −0.42 code, Beta LED round trip, the Beta
  4..1019 clamp, the HDR deband bug) and build gotchas (already there).

## Tests (written with each item; run once in the final gate)
| Item | Test |
|---|---|
| D1 | synthetic pan (23, −9) beyond ±coarse_range: vectors within ½ px of truth on ≥95% interior blocks, which today's search misses; off = identical field |
| D2 | static scene A=B: scale 0 → warp output == A; pan 3 px: scale 1 |
| D3 | one 16×16 outlier block in a pan: vector replaced by the median, trusted; off = identical |
| D4 | partial cut (40% salt-99 regions): share ~0.6 → scale < 1 and > 0; full cut still holds |
| D2/D4 | rate limit: static→pan sequence ramps by ≤0.25 per frame |
| D5 | uncovered region filled from B (matches B within 1 code), off = hold |
| S1 | grain-map block with high g: carry kept where today it drops; scale 1 = identical alpha and output |
| S2 | FBO format probe; highlight precision; BTB/WTW with Stage 10 |
| all | chain probe phases gain D1–D4/S1 on; matrix adds an "all refinements on" axis (doubles the combos; the full run stays ~25 min per libplacebo on WARP) |

## Config / GUI
- Parse tuples (LibplaceboVideoRenderer.cpp ~1948), fingerprint (~1011),
  live-apply (~8005/8074), validation (RendererProfileConfig.h ~623),
  Config card fields next to the matcher fields (ConfigEditorWindow.cpp
  ~6352). CONFIGURATION.html entries. Schema tests and editor tests.
- Shipped sample (VideoProcessor.cfg): refinements documented, at their
  defaults. The de-judder itself stays off (strength 0), so "off = Bill's
  VP" holds.

## Order of work
1. D1, D3 (matcher passes), then D2/D4 (FRAME_TRUST + warp bodies).
2. S1 (mask hook + shader).
3. D5.
4. S2 (only after the planner answers the risk above).
5. Config/GUI/docs, BUILD-STATUS.
6. One gate, then one audit, then the deploy folder.

## Open gate finding (from the 2026-10-12 gate, before this work)
- Chain vs reference (same hooks without LED/mask/sigma-delta) is
  systematically LOW by 0.25-0.6 code on every Stage-10-off combination:
  same on 7.360 and 377, WARP and the GeForce, matcher on or off (the mask
  is not the cause). Stage 10 on + full range is ~0. One Beta combo
  (scale 0.5/0.75, gamma light, Stage 10 off, limited, 10-bit) sits at
  0.602 against the 0.6 limit and fails the full matrix.
- Not yet known whether the chain is low or the reference high. Plan for
  the final gate: add an exact-code check (flat fields at known codes,
  chain and reference each against the expected code) to name the hook
  that moves the level, then fix it or set the limit from that
  measurement. The limit is not loosened before that.

## Questions for the planner
1. S2: accept the scoped flag (Stage 10 on + gamma-light only), or prefer
   another route? A plain global unorm switch fails BTB/WTW with Stage 10
   off and the linear-light shadows.
2. D2 defaults lo 0.25 / hi 1.5 source px per frame, and D4 full strength
   at share 0.6: OK as first values?
3. D2/D4 rate limit ±0.25 per source frame: OK?
4. D3: should an untrusted block surrounded by agreeing neighbours be
   promoted to trusted? Planned yes, limited to ≥7 of 8 agreeing.
