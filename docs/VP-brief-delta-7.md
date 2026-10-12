# Brief delta 7 — apply to the repo's `docs/VP-dither-judder-plan.md`

2026-10-12. Changes only; apply against the repo's current copy (I am not
sending a full brief). Commit as "Docs: brief delta 7".

## 1. Status line

Replace the status paragraph with:

> Status (2026-10-12): Stages 2 (blanking, hook chain, shaders), 3's
> prerequisites, 4 (motion-carried dither), 5 (time-based queue), 6
> (de-judder with NLS-mapped occlusion mask, M1′), 7 (LED drive through
> step 7c-2; 7c-3 pending) built, audited, and merged onto RC4 with its
> beta libplacebo. Stage 10 (BTB/WTW carry) in progress — the RC4
> libplacebo broke the earlier carry. The projector has not yet seen any
> of it: the ramp test (Stage 3) and the first LED session remain the two
> results everything downstream waits on. Next: Stage 10, then Stage 8,
> then 6d if measurement says so.

## 2. Constraint 12 (new) — settings first

Add after constraint 11:

> 12. **Settings are the primary control; keys are backup only.** Every
>     feature's normal operation is governed by saved settings (Config
>     app, per profile where natural). Keyboard shortcuts exist only as a
>     backup for A/B in the cinema, override until restart or profile
>     change, and are shown on the overlay when active. No behaviour may
>     be reachable only by a key. (`docs/VP-delta-6-settings-first.md`.)

## 3. Stage 6 — add 6d

Append to Stage 6:

> **6d — cached-warp extras (optimisation, gated on measurement).** Each
> extra today re-renders the whole chain on a warped source (≈10 ms at
> 4K). 6d caches A at PRE_OUTPUT and warps *that* in output space with a
> 4-tap kernel, running only the OUTPUT stage per extra (≈2.5 ms). Build
> if the stats line shows the 4K total with everything on above ≈60% of
> the frame period. Spec: `docs/VP-stage6d-cached-warp.md`.

## 4. Stage 8 — replace the section

Replace the Stage 8 text with:

> ### Stage 8 — Adaptive downscale (4K → 1600p, anisotropic)
> A blend between the faithful kernel (`ewa_lanczossharp`, linear light,
> anti-ring) and igv's SSIM downscaler, luma only, per pixel:
> `w = w_base × (1 − edge) × grain_scale`. The edge term is from the
> source (Sobel, soft ramp) and sends hard edges to the faithful kernel
> (halo control). The grain term is built in two steps: a per-frame
> scalar (fallback), then a **per-pixel map from the de-judder's trusted
> motion vectors** — the motion-compensated temporal difference separates
> grain from texture where spatial measures cannot — so the SSIM weight
> drops exactly where grain is and holds on texture, without flicker.
> Shadow-biased, snapped on cuts, both branches in one light domain
> (`downscale_light`, default linear; gamma matches the owner's madVR
> look). Gated on source height > output height. Disabled by default;
> SSIM alone stays the baseline. Spec: `docs/VP-stage8-downscale-design.md`.
> Needs the de-judder's matcher running (`dejudder_strength > 0` or
> `occlusion_mask: on`) for the vector map; falls back to the scalar
> otherwise.

## 5. GPU budget (new short section after the build order)

Add:

> **GPU budget, 4K cinema profile, everything on (estimates; the stats
> line reports the real numbers):** ≈10 ms per render of A (libplacebo
> scale/tone map ≈3–4, CfL 1–3, SSIM + blend ≈2, motion-carried dither
> ≈1.5, LED measurement/compensation <1, carry/repair <0.5). De-judder
> extras: ≈10 ms each as built (full re-render), ≈2.5 ms each with 6d.
> At 24p on 60 Hz: ≈25–35 ms per 41.7 ms today, ≈15–16 ms with 6d.
> Levers if tight: grain map off, CfL → ewa_lanczossharp chroma, SSIM
> off. 1080p sources are a quarter of this.

## 6. References

Add: `docs/VP-stage6d-cached-warp.md`, `docs/VP-stage8-downscale-design.md`,
`docs/VP-delta-6-settings-first.md`, `docs/VP-led-drive-control-policy.md`
(the LED policy, superseding deltas 3/3b for behaviour).
