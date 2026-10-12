# Refinements — de-judder and dither (owner-approved; build now, before deployment)

2026-10-12. All items are per source frame or free; none adds a renderer
touch point or a per-refresh pass. Build all of them before the cut,
each behind its own setting (default on except D5 and S3), gated by the
GeForce chain matrix; the projector then decides which stay on.

## De-judder

| # | Refinement | What it does | Cost (4K, per source frame) |
|---|---|---|---|
| D1 | **Global-motion prior** | One whole-frame translation estimate (coarse-level phase correlation or a 1/32-res search) used as the start point for every block's search. Pans — the judder case — get near-exact vectors; blocks only find deviations. | ≈0.1 ms |
| D2 | **Motion-scaled strength** | `s_eff = s × ramp(|v|, v_lo, v_hi)`: zero for near-static scenes, full on pans. Removes most artefact opportunities where there is no judder to fix. | none |
| D3 | **Vector smoothing** | Median over the 3×3 neighbourhood of trusted blocks before trust is finalised; kills isolated outliers so they are not held. | ≈0.1 ms |
| D4 | **Trust-adaptive strength** | When the trusted fraction falls below a threshold, scale `s` down for that frame instead of holding more of it; smooth retreat rather than patchy judder. | none |
| D5 | *(optional, experiment switch)* Occlusion fill from the visible frame (B for uncovered, A for covered) instead of a hard hold. | Fewer held blocks; not a blend, but a departure from "hold" policy. | ≈0.2 ms |

Config: `dejudder_global_prior`, `dejudder_motion_scale_lo/hi`,
`dejudder_vector_smooth`, `dejudder_trust_scale`, `dejudder_occlusion_fill`
(off). All default on except D5. Tests: synthetic pan (D1 vectors within
½ px of truth), static scene (D2 gives s_eff 0), outlier block (D3 removes
it), low-trust frame (D4 scales s), with the existing WARP suite.

## Dither

| # | Refinement | What it does | Cost |
|---|---|---|---|
| S1 | **Grain-aware carry** | Raise the sigma-delta's `match_tolerance` where Stage 8's grain map says there is grain, so the residual survives film grain and the 10-bit average holds on the smooth picture beneath. The carried payload is bounded (±0.5 code) either way. | none (one texture read; the map exists) |
| S2 | **16-bit unorm intermediates** | Patch the fork's libplacebo to prefer `rgba16` (unorm) over `rgba16f` for FBOs. Removes the ±¼ 10-bit-step floor in highlights. | none in GPU time; slightly more bandwidth |
| S3 | *(conditional)* Spatiotemporal blue-noise texture instead of IGN; second-order noise shaping. | Only if the flat-field test shows a pattern or a limit cycle. | small |

Config: `dither_grain_tolerance_scale` (S1); S2 is a build change with a
test that the OUTPUT FBO format is unorm. S3 not built unless needed.

## Order

Now: S1 and D1–D4 together (one step, one audit); S2 with the libplacebo
patch rebuild; D5 built behind its switch (off); S3 not built unless the
flat-field test on the projector asks for it.
