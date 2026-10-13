# Delta 14 — two hand rules against the soap-opera look (replaces the strength model idea)

2026-10-13. Owner is not sold on a rated-clip taste model; these two rules
need no training, no model, and nearly no GPU time (D1's global vector
and the per-block field already exist). Both are per frame, so the
global-`s` rule holds. Each behind a setting; off = today's D2.

## What causes the look (so the rules are aimed)

1. Temporal sample density — the strength dial.
2. Blur-to-displacement mismatch — source blur is fixed; small at low `s`.
3. **Fast motion smoothed** — film's strobing in fast action is part of
   the film look; smoothing a slow pan removes a hitch, smoothing fast
   action removes the strobing people expect.
4. **Object motion smoothed** — smoothing the camera (pan, track) is
   welcome; smoothing characters moving within the frame reads as video.
5. Regional inconsistency — the hold policy's domain, handled.

Rules 3 and 4 are where a strength model would have learned most of its
behaviour.

## R-A — non-monotonic strength versus motion (`dejudder_motion_rolloff: on`)

Replace D2's monotonic ramp `s_eff = s × ramp(|v|, lo, hi)` with a
rise-then-fall shape:

```
s_eff = s × rise(|v|, lo, hi) × fall(|v|, roll_lo, roll_hi)
```

- `rise` as D2 today (0 at static, 1 from `motion_scale_hi`);
- `fall` = 1 below `roll_lo`, smoothly to `roll_floor` (default 0.25 of
  `s`) at `roll_hi` and above;
- `|v|` is the frame's **global** motion magnitude (D1), in source px per
  source frame; defaults `roll_lo 8`, `roll_hi 24` at 4K (≈ a brisk pan
  to a whip pan); the owner tunes by eye.
- Same ±0.25/frame rate limit and cut hold as D2.

Effect: slow and medium pans get the full correction; fast action keeps
most of its strobing. Off = D2 as today.

## R-B — camera/object split (`dejudder_object_strength: 0.5`; 1.0 = off)

Decompose each block's vector into the global (camera) component and
the local residual: `v = v_global + v_local`. Warp with

```
displacement = s_eff × δ × (v_global + object_strength × v_local)
```

- `object_strength` 1.0 reproduces today; 0.5 halves the smoothing of
  motion *within* the frame while the camera is smoothed fully; 0 warps
  the camera only (objects keep their 24p stepping against a smoothed
  background — probably too far, but the owner may try it).
- Applies in both warp bodies after `field_vector()`; the occlusion mask
  and the grain map still read the true field (as with D2's scale).
- Trust and hold unchanged: a block is trusted or held on its full
  vector; the split only scales what is applied.
- When D1 finds no reliable global vector (static or chaotic motion),
  `v_global` = 0 and the rule reduces to scaling the whole field by
  `object_strength` — acceptable, logged.

## Tests

- R-A: synthetic pans at 2, 12 and 30 px/frame give `s_eff` ≈ 0, full,
  and `roll_floor × s` respectively; cut holds; off = D2 exactly.
- R-B: a pan with a moving object — the background lands at the full
  `s_eff` position, the object at the scaled residual; `object_strength
  1.0` is byte-identical to today; both warp paths.
- Matrix axis gains R-A on, R-B 0.5.

## Order

Small; can go in with the next build after the audit if the owner wants
it in the deploy, otherwise first post-cut item. It supersedes the
strength-model variant in the 11b note (variant 3), which is withdrawn.
