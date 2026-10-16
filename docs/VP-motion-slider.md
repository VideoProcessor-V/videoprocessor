# Motion page — the five-position slider: rationale, presets, descriptions

2026-10-16. The de-judder has grown dozens of keys; a person cannot set
them. The slider reduces the choice to one question — *how much should
motion be changed from what was shot?* — and moves only the keys that
answer it. Everything else is either always on (because it only reduces
error) or an advanced key that no position touches.

## 1. The rationale: three knobs make the graded effect

The soap-opera look has three causes a setting can move (the others —
source motion blur, the hold policy — are fixed):

| Cause | Knob | Effect of raising it |
|---|---|---|
| Temporal sample density | `dejudder_strength` (uneven cadences), `dejudder_strength_even` (even) | frames placed nearer their true times; from "hitch removed" to "60 fps motion" |
| Object motion smoothed | `dejudder_object_strength` (R-B) | characters within the frame move as smoothly as the camera |
| Fast motion smoothed | R-A roll-off (`roll_floor`, `roll_lo/hi`) | fast action loses its strobing |

The slider moves exactly these three, monotonically, so each step is a
visible, explainable increase in "video-ness." Nothing else changes
between positions.

## 2. Always on, at every position (they only reduce error)

M6 per-pixel trust + landing check, estimator-lite E1–E4, the global
prior, vector smoothing, hold hysteresis 2, feathered hold 4 px, trust-
adaptive strength D4 (0.6), the 3:2 mask / occlusion mask, dither
external vectors, the cadence gates (≥ 2 refreshes, Low_Latency off,
50/60p excluded), `motion_engine: auto`. These are not taste; they are
quality. They appear in the advanced box but no position alters them.

## 3. The five positions

| # | Name | Who it is for | strength (uneven) | strength (even) | even rates | object_strength | R-A roll-off |
|---|---|---|---|---|---|---|---|
| 0 | **As shot** | purists; checking a source; A/B reference | 0 | 0 | — | — | — |
| 1 | **Film** | 24p drama; the owner's default taste | 0.15 | 0 | 30 only | 0.4 | strong: floor 0.15, roll 6–18 px |
| 2 | **Cinema** (default) | 24p film generally; the hitch gone, nothing else | 0.25 | 0 | 30 only | 0.5 | standard: floor 0.25, roll 8–24 px |
| 3 | **Natural** | documentaries, nature, travel; smoother camera, objects mostly as shot | 0.35 | 0.6 | 30 + 29.97 | 0.75 | mild: floor 0.5, roll 12–36 px |
| 4 | **Real** | sports, live events, anything meant to look like being there | 0.5 | 1.0 | 30 + 29.97 | 1.0 | off (floor 1.0) |

Descriptions shown under the slider (one line each):

- **As shot** — No motion processing. Frames shown exactly as the source
  provides them, with the cadence the display imposes (3:2 at 24p).
- **Film** — Removes most of the 3:2 hitch on pans while keeping the film
  feel: people and fast action move as they were shot.
- **Cinema** — Removes the 3:2 hitch. Camera movement is even; people
  keep most of their 24p character; fast action keeps its strobing.
- **Natural** — Smoother camera and gentler object motion, with 30p
  material (documentaries) brought most of the way to 60. Fast action
  still partly strobes.
- **Real** — Maximum smoothness: every frame at its true time, people and
  action fully smoothed, 30p doubled. This is the "video" look; it suits
  sport and live events and will look wrong on films.

GPU cost marker: 0 none; 1–2 +; 3 ++; 4 ++ (the matcher cost is the same
from 1 upward; the markers reflect extras per source frame).

## 4. The advanced box (below the slider)

Shows every key the slider moves, filled with the position's values and
editable; a "Reset to preset" button restores them; a changed value
shows the slider as "Custom" without moving it. Below that, collapsed
by default, "Quality (always on)" with the §2 keys and "Engine" with
`motion_engine`, the estimator options and the experiment switches (H3
taper, D5, optical flow) — present, off, and not touched by the slider.

## 5. Why these numbers

- `strength` steps 0.15 / 0.25 / 0.35 / 0.5 are the points where, on the
  corpus, judder removal goes ≈55 / 80 / 90 / 97% on pans; above 0.5 the
  gain is small and the risk is not. Even-cadence strength starts only at
  Natural because 2:2 has no hitch to fix — anything there is smoothing
  by intent.
- `object_strength` 0.4 → 1.0 is the one knob that most changes "people
  look like video"; it rises faster than `strength` because it is cheap
  in artefacts (the camera is already smooth) and expensive in look.
- R-A roll-off weakens by widening the roll-off band and raising the
  floor, so each step keeps strobing on less of the fast-action range.
- 29.97 joins at Natural, where the user has said "smooth the
  documentary," and that is also where American broadcast sits.

## 6. Tests

- Each position maps to exactly the table's values; "As shot" is
  byte-identical to the de-judder off; the always-on keys are identical
  across positions; editing a sub-key marks "Custom"; reset restores.
- The corpus run per position reports judder removal and damaged tiles
  so the five rows have numbers beside the words.

## 7. The same scheme for the other pages (brief)

Picture (Stage 12): Off / Detect only / Conservative / Standard /
Aggressive — moving `source_resolution` (native → auto), `deband_vp`,
residual steadying, `ssim_bitrate_floor`. Scaling: Fast / Standard /
Best — chroma, upscaler model, SSIM blend + grain map, `fbo_float32`.
Dither: Off / Standard / Motion-carried. Light: Off / Picture only /
Script / Compensate / Compensate deep. Each with the same advanced box
and the same "Custom" rule.
