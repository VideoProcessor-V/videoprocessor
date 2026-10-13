# Delta 11 — shadow-preserving gamma boost (toe), and the dark-hair test

2026-10-13. Recorded from the planner's handoff.

## The observation

The owner sees dark hair in low-light HDR scenes come out washed out
(lifted, flat). The diagnosis is still to be confirmed on the projector.

The likely cause is the global gamma boost (encode 2.4 on a 2.2 display):

- it is steepest, relative to the signal, at the bottom of the range;
- that flattens near-black texture.

The old script's contrast lift did the same on dimmed scenes. The new
compensation removes that cause by construction.

## Change: a mid-tone boost with a toe

This goes in the compensation shader, where the boost already lives. It
replaces the global boost `v₁ = v^(g_out / gamma_boost)`:

- **Above `boost_knee`** (default 0.10 of the encoded range) the boost
  applies as today.
- **Below it** the curve blends smoothly into the identity (smoothstep over
  `boost_knee_width`, default 0.05), so the darkest tones keep their native
  2.2 contrast.
- **Shape.** The curve is continuous and monotonic, and sign-preserving as
  before (BTB unaffected).

**Keys and defaults.**

- `gamma_boost_knee` (0 = today's global boost) and `gamma_boost_knee_width`.
- The deploy default is knee 0 (today's behaviour). The owner switches to
  0.10 after the test below confirms the cause.

The LED gain and the highlight knee are applied after the boost, on the
final value, exactly as delta 4 specifies. The toe changes only the boost
term.

## The ANSI branch — scene-adaptive shadow lift (`shadow_lift_ansi`, default 0 = off)

**Why lift helps here.** Under a scatter veil, the hair's texture is ΔL on
a constant added light, so it is seen as ΔL / (L + veil). Lifting the
darks raises L and ΔL together and makes the veil a smaller fraction, so
the texture becomes *more* visible.

The ANSI case therefore wants the opposite of the signal case at the
bottom of the range: **more** lift, and only when something bright in the
frame can scatter.

**How it works.**

- **Where.** In the same compensation shader, as a toe that goes up: below
  `boost_knee`, add a lift `t × shadow_lift_ansi × veil_est`.
- **`veil_est`** ∈ [0, 1] is a per-frame scalar from the LED module's
  existing highlight statistics: the highlight percentile × area, through a
  soft ramp. These are the same numbers the headroom policy uses.
- **Smoothing.** `veil_est` is slewed with the LED module's scene rate law,
  so it never pumps, and it resets on a cut.

**What the viewer gets.**

- A dark scene with no bright region gets native contrast.
- A dark scene with a lit face gets its darks lifted in proportion to the
  predicted veil.

This is a perceptual compensation, not a removal of scatter. The real
levers remain the stray-light work, but this is the one software can make.

Keys: `shadow_lift_ansi` (strength, 0–1) and `shadow_lift_ansi_ramp_lo/hi`
(the veil ramp on the highlight statistic). Off by default; the owner sets
it after the test.

## Test on the projector (first session, under the LED item)

Use a low-light scene with dark hair, compensation on.

**0. Signal or optics first.**

- Compare a uniformly dark scene with the hair against one where a bright
  region (lit face, lamp, window) shares the frame.
- **Flat in both** → signal; continue with step 1.
- **Fine in the dark scene, flat next to the bright region** → ANSI scatter.
  - This is a veil of real light, not a signal lift, and software cannot
    subtract it.
  - The levers are the stray-light work: the lens/prism path, the ISCO
    entrance-mask test, the room.
  - The software response is the scene-adaptive shadow lift. Try 0.3 on
    the bright-neighbour scene and confirm the dark scene is unchanged.
  - The global boost *helps* in this case, so the toe is not wanted here.

**1. Boost on (today).** Note the hair.

**2. Boost off** (`gamma_boost` = output gamma).

- If the hair's texture returns, the boost is the cause.
- Then set `gamma_boost_knee: 0.10` and confirm the hair stays while the
  mid-tones keep the lift.

**3. Still flat with the boost off?** The cause is upstream (the tone-map
target black, the Dolby engine's black). Try `contrast_recovery` on the HDR
profile before anything bespoke.

A dedicated shadow-recovery pass is not planned; it would be the third
resort only.

## Unit tests

**Toe.**

- Knee 0: byte-identical to today's boost.
- Knee 0.10:
  - the output equals today's boost above the knee + width;
  - it equals the identity below the knee;
  - it is monotonic across the band;
  - codes 40 and 1019 are preserved (sign and WTW).
- The LED gain test (constant light 255→100) is unchanged with the toe on.

**Shadow lift.**

- A dark frame with no highlight statistic: no lift.
- The same dark region with a synthetic bright patch elsewhere in the
  frame: lift proportional to the ramp.
- Slewed, and reset on a cut.
- Sign and WTW preserved.
