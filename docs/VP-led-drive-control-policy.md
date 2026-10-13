# LED drive — control policy (standalone, supersedes deltas 3 / 3b / 3-answers for behaviour)

2026-10-11. This is the complete statement of how the LED drive controls
the projector in `compensate` mode. Where earlier deltas differ, this file
wins. Deltas 1, 2 and 4 (measurement, PRE_OUTPUT hook, gain order,
E = 2.41) still apply. `script` mode is defined at the end.

---

## 1. Facts the policy is built on

- **The LED is slow.** An `LPW1` step takes a large fraction of a second
  to settle (owner's recollection; to be measured, §6). This is why the
  old script ramps the LED slowly.
- **CNTR is fast** and is a pure gain on the encoded signal ahead of the
  projector's gamma. The light-meter table gives E ≈ 2.41 and shows CNTR
  74 ≈ 2.5× the white of CNTR 50 at the same LED: the projector has a
  large reserve above CNTR 50.
- **VP gain** is in code space: instantaneous, float, but it can only push
  a pixel up to code 255 (≈1.2× the light of code 235) and can never add
  light beyond that. It *can* always reduce.
- **Pumping** comes from the floor or the content moving visibly within a
  scene. A cut masks any change that lands on the cut frame.

## 2. Actuator roles

| Actuator | Speed | Role |
|---|---|---|
| LED (`LPW1`) | slow | the black floor; moves at the owner's tuned rates |
| CNTR | fast | headroom; moves paired with the LED within a scene, dumped on a bright cut |
| VP knee | instant | rolls off highlights above the live headroom |
| VP gain | instant | fine residual; the "dump" when the knee's tolerance is exceeded |

**Governing rule — software headroom first, in every situation.** VP's
gain and knee are applied per *rendered* frame — every 41.7 ms for a 24p
frame the renderer renders once and DXGI repeats, every 16.7 ms when the
de-judder's extra presents are rendered — continuous in amplitude and
unpaced by the serial port: they do all of whatever can be done without
*adding* light (every reduction, every dump,
and the first ≈20% of restoration via codes 236–255). CNTR is used only
for the part software cannot do — restoring light beyond that — and only
by the shortfall. The LED supplies the floor.

## 3. The policy

### 3.1 Dimming into a dark scene — software first
Compensate with VP gain up to its code headroom (within
`highlight_tolerance_stops`; dark scenes have few highlights, so the knee
costs almost nothing) **before** spending CNTR. Pair CNTR with the LED only
for the remainder. This keeps CNTR's reserve unspent for the bright cut.

### 3.2 Within a scene — nothing abrupt, and rates scale with distance

> **Two rate families** (recovery-rate answers, 2026-10-11): the rate law
> below (`rate_*`, script-matched) is the law **within a scene**, and also
> for the paired LED/CNTR move after a cut in either direction (content is
> constant during a paired move, so slow is invisible). The displayed-light
> ramp for the remaining shortfall after a bright cut (and after the exit
> from black) runs under the fast `recovery_rate_*` (§3.3). The LED command
> on a cut is one command to the scene's level, not rate-limited.
> `rate_preset` sets both families.
LED and CNTR move only as a pair, CNTR following the LED's *measured*
light curve so content is constant and only the floor moves. No CNTR step
ever happens within a scene; that is where steps were visible.

**Rate law (all ramps — LED pair and VP gain):** proportional to the
distance from target, as the script's step tables are, in continuous
form: `rate = clamp(k × |distance|, rate_min, rate_max)` per actuator and
direction, so a ramp is fast while far from
its target and settles rather than creeps as it arrives. The VP gain's
ramp runs per rendered frame with the same law; on a cut it is not
ramped at all (the dump is instantaneous), and the proportional law then
governs the recovery. Replace the fixed `rate_up` / `rate_down` with
`rate_gain`, `rate_min` and `rate_max` per direction; the defaults are
derived from the script's tables (≈10–17 levels/s up at distance 100,
slower down) because the owner asked to keep his kinetics; a faster
"compensated" preset is available in config for him to try by eye.

### 3.3 Bright cut — dump the projector's contrast, keep brightness up in software

> **Owner correction (2026-10-11, confirmed by the planner; supersedes the
> "dump to 50 / partial dump" text below).** VP's gain cannot add absolute
> light: a white code can only go to the top of the code range (≈ +20–30%).
> While the LED is low, only CNTR's light headroom supplies real light, so
> dumping CNTR to 50 on a bright cut throws away brightness software cannot
> recover. Therefore:
> 1. **Bright cut:** CNTR goes to `cut_cntr_level` (default 59 =
>    `dmd_clip_cntr`, where the script's highlight dump ends), never to 50
>    because of the cut; if already below it, kept as it is; higher only to
>    hold `max_cut_dip`. "The dump" below means this.
> 2. **Exit from black / a very dark scene cutting to bright** (§3.5): CNTR
>    raised to `cut_cntr_level` during the black, the LED started early
>    (capped at `black_prelift_max` while black), VP's gain prepared — none
>    of it visible on black.
> 3. **At the cut:** LED × CNTR at `cut_cntr_level`; VP's gain to its
>    maximum; the knee rolls off what would clip within
>    `highlight_tolerance_stops`.
> 4. **After the cut:** LED and CNTR paired at the script-matched rates,
>    CNTR descending from `cut_cntr_level` as the LED rises, VP's gain
>    interpolating the integer steps. The displayed-light ramp covers only
>    the remaining shortfall, at the fast `recovery_rate_*`, bounded by the
>    modelled light. The LED command on a cut is still one command to the
>    scene's level.

Lowering CNTR and lowering VP gain do the *same* thing to mid-tones and to
headroom (both scale the code that reaches the DMD). So there is no reason
to keep CNTR high through a bright cut, and every reason not to: a high
CNTR would have to descend paired with a slow, modelled LED during the
recovery. Dump it on the cut instead, and the recovery becomes a
software-shaped fade-up under the LED's climb, with the LED model needed
only as a ceiling.

1. **On the cut frame (masked): CNTR dumps** — one command, no stop-short
   — to the value at which the knee's compression of the incoming scene
   (from queued statistics when lookahead has them) is within
   `highlight_tolerance_stops`. Out of black that is a full dump to 50;
   out of a shallow dim it is partial. If CNTR was never raised (the
   dimming was software-only, §3.1) there is nothing to dump.
2. **Software keeps brightness up and contains the crush:** the VP gain
   goes to its code ceiling (≈1.2× of what remains, via codes 236–255 —
   it cannot replace what the dump removed) in the same frame;
   the knee compresses whatever would still exceed the DMD's maximum.
   Only if that compression would exceed the tolerance does the gain come
   below 1 (the gain branch of the highlight policy) — the scene plays
   darker and fades up.
3. **Recovery — brightness follows the rate law, not the LED's curve.**
   The LED is commanded to the scene's level and follows its own physical
   curve (dead time + settle). The *displayed* brightness climbs under the
   proportional rate law, with the LED's modelled light as the ceiling:
   `gain = min(gain_ceiling, ramp_target_light / led_light_actual)`.
   Wherever the LED is ahead of the ramp — as it is just after an
   aggressive pre-lift, in the steep part of its settle — the gain trims
   the excess, so the eye sees a smooth fade-up and never the LED's step
   response. The gain lands exactly on 1 as the ramp and the LED both
   arrive; model error affects only the final approach and never produces
   drift. This is what makes the pre-lift's aggressiveness a speed
   setting rather than a visibility risk: it decides how soon the ramp
   can finish, not how it looks.
   Any CNTR left above 50 by a partial dump descends paired with the LED,
   biased to the bright side by `recovery_bias_pct` with the gain
   trimming the excess (the gain can only reduce, so the error stays
   correctable).

   *Relation to the script's dump:* the same move — the script dumped
   CNTR fast to restore highlight headroom and paid for it in darkness,
   stopping 10 short of target so some crush persisted. Here the dump is
   one command sized to the tolerance; the software multiplies what is
   left by up to 1.2× (a full dump 74→50 is ×0.39 in light, so the scene
   starts near ×0.47) and removes the residual crush with the knee on the
   first frame; the fade-up thereafter follows the rate law under the
   LED's climb. The script's staging is reproduced only in `script` mode.

### 3.4 Dark cut
Paired early ramp within the lookahead window, bounded by the current
scene's highlight headroom; any remainder completes after the cut under the
rate law. CNTR must never rise ahead of the LED's actual dimming.

### 3.5 What the lookahead adds (cinema profile)
- **Head start before a bright cut:** begin the LED's climb as early as
  the window allows, paired with CNTR coming down, so the dark scene's
  content stays constant and only its floor rises slightly in its last
  frames. The LED arrives at the cut part-way up, shortening the
  compressed interval afterwards. The window (~167 ms at 24p) starts a
  slow LED but cannot finish it; plan on a partial.
- **Hold before a known cut:** no drift is started in a scene's last
  frames that would have to be reversed.
- **Exit from black (special case):** when the queue shows a bright scene
  coming while the screen is black, pre-position everything at once —
  CNTR to `cut_cntr_level` (owner correction, §3.3) (on black it has no
  visible effect, so this is free), and the LED commanded straight to
  `black_prelift_max` in one command — the rate law does not apply on
  black, only the LED's own settle time does. `black_prelift_max` is the
  amount of black given up in the last ~167 ms before the cut in exchange
  for a shorter fade-up; it never touches the fade-to-black itself, which
  stays at `led_floor_black` until the queue shows the bright scene. 60
  (no pre-lift) protects the black absolutely; 120 is the starting guess;
  set by eye. At the cut the gain goes to its ceiling and the knee
  contains the rest; the fade-up is shortened by whatever the LED climbed
  early. A fade-in from
  black needs no bound: picture and LED rise together.
- **As built (7c-3):** the exit-from-black pre-positioning applies while
  the frame the LED lands on is black or very dark (APL < 20); the LED gets
  one command to at most `black_prelift_max` and CNTR one command to
  `cut_cntr_level` (held until the cut). In other scenes the head start is
  the early ramp at the scene rate plus the seen cut's single LED command
  landing on the cut frame. `black_prelift_max: 255` = before 7c-3.
- **Exact dump** (restated for §3.3): the incoming scene's statistics size
  the CNTR dump to the tolerance, not to a fixed stopping point.

### 3.6 Reactive (`Low_Latency`, or an unseen cut)
The same policy with no head start and the CNTR dump sent immediately;
the dead time is the accepted brief dip. The reactive scene floor is
`led_floor_reactive` (100), as before.

## 4. Floor depth

With CNTR dumped on a bright cut, what the floor costs is the **depth of
the fade-up** afterwards: the scene starts at roughly
`(floor_light / ceiling_light) × gain_ceiling × (CNTR_retained / 50)^E`
of its correct brightness and climbs under the rate law, bounded by the
LED. CNTR's reserve helps
only through the part of it the tolerance lets a partial dump keep.

One taste parameter governs the trade: `max_cut_dip` — the darkest
start of a fade-up the owner accepts after a bright cut (0.5 = the scene
may start at half brightness). The controller derives the scene floor
from it as the lowest LED level whose start after a full dump
(`f_floor × g_ceiling_white`, ≈1.26 for white highlights) is at least
`max_cut_dip` — with 0.5 that is ≈0.40 of ceiling flux, about level 102.
The effective scene floor is `max(led_floor, that value)`, so `led_floor`
may be set low and `max_cut_dip` left to govern. The script's floor of
100 corresponds to a dip of ≈0.47.

`led_floor_black` is separate and may stay deep (60, or custom dim); §5.

## 5. Fade to black — unchanged, and harder to come back from

The fade-to-black plan (track the floor down to `led_floor_black`,
custom-dim below `LPW1` 60 when configured, gain unchanged) **does not
change** under this policy. What does change is the owner's expectation
of the way out: from a true black the LED is at its lowest, so a hard cut
from black to a bright scene gets a full CNTR dump, the software's 20% and
the knee on the first frame, and still has the longest fade-up this system
produces — the LED's whole climb from its minimum. Fade-*ins* (the usual exit from
black) are fine because the picture and the LED rise together. With
lookahead, the hard-cut case is shortened by the exit-from-black
pre-positioning in §3.5 (CNTR set early for free, LED pre-lifted up to
`black_prelift_max`); without lookahead it is the full climb. Log it, and
let the owner judge it by eye before deciding whether `led_floor_black`
should stay at 60.

## 6. Measurements (once, phone camera at 60 fps, mid-grey field)

- LED 255→100 and 100→255: dead time and 63% settle time →
  `led_latency_ms`, `led_settle_ms`.
- CNTR 50→74 and back: → `cntr_latency_ms`, `cntr_settle_ms`.
- White field (code 235), LED 255: luminance vs CNTR 74 upward in steps
  of 5 until it stops rising → `cntr_max_usable` (bounds the paired
  compensation within scenes and what a partial dump may retain).
- **Fixed-LED white sweep** (owner correction): white field (code 235) at
  LED 255 fixed, CNTR 50 upward until white stops rising → `dmd_clip_cntr`.
  The light-meter table was taken at equal white (LED turned down as CNTR
  went up), so it establishes E and the pre-gamma gain but not where white
  clips at full LED; the script's crush above CNTR ≈ 60 is the evidence
  until this sweep is done.
Until measured: `led_latency_ms 80`, `led_settle_ms 300` (deliberately
**fast-biased**: the recovery gain is `ramp / modelled LED light`, so a
model faster than the real LED errs on the dark side — a slower fade-up —
while a model slower than reality lets the LED's step show through),
`cntr_latency_ms 80`, `cntr_settle_ms 0`, `cntr_max_usable 74`. Models are
dead time + first-order settle; the pairing and the gain use modelled
light, not the command.

## 7. Config (new or changed keys)

```
[projector.Default]
led_latency_ms: 80
led_settle_ms: 300           # fast-biased until measured; see §6
cntr_latency_ms: 80
cntr_settle_ms: 0
cntr_max_usable: 74
dmd_clip_cntr: 59            # CNTR at which code 235 reaches the DMD max at full LED: the script's crush
                             # point until the fixed-LED white sweep (§6); the equal-white table does not
                             # establish it. Knee threshold = min(VP headroom, DMD headroom).
                             # Do NOT put the clip into the CNTR need: above it more CNTR adds no highlight
                             # light (h x F is the same however VP gain and CNTR share it), so each CNTR step
                             # would ask for the next. Instead: while the scene has highlights CNTR stops at
                             # dmd_clip_cntr; the highlight floor uses CNTR only up to it; the fade-up
                             # ceiling shrinks in proportion above it; without highlights the assist may go
                             # to cntr_max_usable.
flux_table:                  # optional; linear if absent

[led_drive.Default]
mode: compensate             # compensate | script | log_only | picture_only | listen
                             # picture_only and listen NEVER open the RS-232 port (planner 2026-10-13):
                             # the owner's Python script keeps COM1. picture_only = VP's picture terms,
                             # gain exactly 1; listen = the script drives, VP hears it over UDP
                             # (listen_port, listen_log) and eases each CNTR step.
cntr_assist: on              # off = no CNTR (feature switch, not a mode)
cntr_assist_max: 100         # 7c-2: no cap beyond cntr_max_usable (was 62 until the dump existed)
cut_cntr_level: 59           # bright cut / exit from black; default = dmd_clip_cntr
recovery_rate_gain / recovery_rate_min / recovery_rate_max   # fast ramp for the fade-up's shortfall
# regression recipe for "the previous compensate exactly":
#   cntr_assist: off, led_settle_ms: 0, rate_gain_up/down: 0
software_first: on
cut_cntr_dump: on            # off = knee/gain only on cuts, CNTR descends paired
recovery_bias_pct: 3
highlight_tolerance_stops: 0.33
max_cut_dip: 0.5             # darkest start of a fade-up after a bright cut
black_prelift_max: 120       # LED level the pre-lift may reach while the screen is still black
led_floor: 100
led_floor_reactive: 100
led_floor_black: 60
rate_gain_up / rate_gain_down, rate_min_*, rate_max_*, deadband: from the script's tables
script_smoothing: off        # script mode only
```

## 8. `script` mode (the fallback)

Sends the script's actual bytes — `LPW1` and `CNTR` values, order and
pacing as `beta_22.py`, tick for tick, including both fast contrast dumps
and the anti-pump band. `script_smoothing: off` (default) = byte-identical
on the wire and no VP gain at all; `on` = the same commands plus VP gain
interpolating between the integer CNTR steps and across command latency.
Verified against the script line for line; the two documented one-level
differences stay as the script has them.

## 9. Tests

- Pairing: for a synthetic floor ramp, modelled `LED-light × CNTR^E` is
  constant to within one CNTR step and the gain stays within it.
- Software-first: in a dark-scene dim, CNTR does not move until VP gain
  has reached its headroom.
- Bright cut: one CNTR dump command on the cut frame sized to the
  tolerance from the queued statistics; gain at its ceiling and knee on
  the same frame; no CNTR step within the following scene; recovery gain
  = min(ceiling, target/actual) landing on 1; any residual CNTR descends
  paired with the bias and gain ≤ ceiling throughout.
- Dark cut: CNTR never above the paired value for the modelled LED.
- Rate law: for distances of 10, 50 and 150 levels the per-frame step is
  proportional between `rate_min` and `rate_max`; a ramp settles at the
  target with no residual creep; on a cut the VP gain changes in one
  frame.
- Gain-shaped recovery: with a modelled LED step response (dead time +
  settle) and a pre-lift, the displayed brightness after the cut follows
  the rate law — monotonic, no inflection from the LED's curve — and the
  gain never exceeds the ceiling or falls below the ramp's requirement.
- `max_cut_dip`: the derived scene floor gives a fade-up start no darker
  than the setting on a synthetic black→bright cut; lowering the setting
  lowers the floor.
- Fade to black unchanged versus the previous build; hard cut from black
  logged. Exit from black with lookahead: CNTR set to the dump point
  before the cut, LED pre-lift never above `black_prelift_max` while the
  frame is black, gain at ceiling on the cut frame.
- The regression recipe (`cntr_assist: off`, `led_settle_ms: 0`,
  `rate_gain_up/down: 0`) and `cut_cntr_dump: off` reproduce the earlier
  behaviours exactly; `script` mode byte-identical with smoothing off.

## Owner note (2026-10-11): use the black frames, not only the cut frame

Whenever a bright cut is preceded by black (a fade to black, or black
between scenes), the work that §3.3 does *on the cut frame* is done
*during the black* instead, where it cannot be seen:

- **Contrast drop during black.** CNTR goes to the incoming scene's dump
  point (sized to `highlight_tolerance_stops` from the queued statistics)
  while the screen is black. On black, CNTR changes nothing visible, so
  timing and the serial gap stop mattering: the command only has to land
  before the cut.
- **Software accommodation during black.** VP gain is set to its ceiling
  for the incoming scene, and the knee is prepared, also during the black.
  Gain × black = black, so this is invisible too. The cut frame then shows
  the new scene already compensated, with nothing changing on that frame.
- **LED ramp begins early.** With lookahead the LED's climb starts as soon
  as the queue shows the bright scene coming. The black's level rise is
  bounded by `black_prelift_max`, so the black stays black within the
  owner's tolerance.
- **The cut frame itself** then carries no command at all in the black →
  bright case. The fade-up after the cut follows the rate law under the
  LED's climb (§3.3 step 3), starting from wherever the early ramp got to.
- **Without lookahead** (Low_Latency, or an unseen cut) this is not
  possible, and §3.6 applies.
- **Implementation:** "black" uses the module's existing near-black and
  fade detection on the queued frames. The CNTR command and the gain
  change are scheduled for the first black frame on which they are safe,
  with the CNTR command placed `cntr_latency_ms` ahead.
