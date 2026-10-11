# Stage 6b-2b + 6c — planner answers

2026-10-11. On the `stage7-rc2` package. M1′ proven on the harness is the
best news of the week; the three conditions were exactly the ones that
mattered. Decisions follow the read-me's numbering.

## 1. Which frame the mask describes — confirmed

B's mask (blocks of the new frame not visible in the previous one), used
on B's first refresh only; not on cadence repeats, extras or DXGI
repeats. Correct, and the reasoning matches: within a source frame the
sigma-delta's own gate covers the extras.

## 2. NLS — add the non-linear mapping as a small follow-on (6c-2)

"No mask with NLS" is acceptable *today*, but the owner's scope profile
uses NLS, so without the mapping the dither refinement is off for his
main viewing. The NLS stretch is a known, monotonic per-column function;
the mask is block-resolution; mapping each block's centre through that
function (and its inverse for the active rectangle) is cheap and exact
enough for a block mask. Build it as 6c-2 after the Code audit of this
step, behind the same `occlusion_mask` key, with a GPU test at the owner's
NLS settings. Keep the "logged once, mask off" fallback for any NLS mode
the mapping does not cover.

## 3. Mask-only mode — limit to film rates

Limit mask-only to sources at or below 30 fps. The refinement exists for
the cinema profile; 50/60p sources are video, mostly run in Low_Latency
anyway, and the matcher pass per source frame at 60p buys nothing the
owner would see. Log once when a faster source is refused.

## 4. Config placement — confirmed

Rendering page, per profile, beside the other input-owned keys; the
renderer disables it itself in Low_Latency and on video cadences. Right.

## Noted

- Delta 5: yes — `levels_full` and `swapchain_bits` derived from the
  active output, not typed. Add to the audit pass list.
- Plugin API 25: exe and DLL from one build, as before; make DEPLOY.txt say
  it in words.
- Brief: thank you for the repo copy; the next revision starts from it.

## On the next step (Stage 7 step 7)

Two things before the integration proposal:

1. A new file, `docs/VP-led-drive-control-policy.md`, **supersedes deltas
   3, 3b and the delta-3 answers for behaviour**. Read it instead of them.
   Deltas 1, 2 and 4 still apply. Its headline: software headroom first in
   every situation; nothing abrupt within a scene; on a bright cut the
   projector's contrast is *dumped* (one command, masked), the software
   keeps brightness up and contains the crush, and the recovery is a
   gain-shaped fade-up under the LED's slow climb; exit-from-black
   pre-positioning with lookahead; `max_cut_dip` and `black_prelift_max`
   as the taste parameters.
2. The 200 ms serial gap versus "both commands on the cut frame": under
   the policy only **CNTR** must land on the cut frame (the dump); the LED
   command is slow anyway and goes second, 200 ms later, which is inside
   its own dead time plus settle. Order on a bright cut: CNTR, then LPW1.
   On a dark cut: LPW1, then CNTR (CNTR must never rise ahead of the LED's
   actual dimming). No conflict with the gap.
