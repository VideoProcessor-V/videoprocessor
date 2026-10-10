# LED power vs contrast for equal white (light-meter test, original light engine)

Provided by the owner on 2026-10-10 (`LEDPowerV.S.Contrast.xlsx`): a friend
measured, with a light meter on the FL35's **original** light engine, which
contrast (CNTR) setting gives the same white brightness at each LED power
(LPW1). The beta_22 script's curve and contrast bands were an approximation
of this table; it deliberately stops compensating at the low end (contrast
74, LED 99 - exactly its floor of 100) because the highlight crush and the
transition become obvious as contrast is pushed further.

| LED (LPW1) | Contrast (CNTR) |
|---:|---:|
| 255 | 50 |
| 244 | 51 |
| 236 | 52 |
| 220 | 53 |
| 212 | 54 |
| 202 | 55 |
| 196 | 56 |
| 186 | 57 |
| 177 | 58 |
| 170 | 59 |
| 165 | 60 |
| 159 | 61 |
| 152 | 62 |
| 146 | 63 |
| 140 | 64 |
| 135 | 65 |
| 131 | 66 |
| 121 | 68 |
| 113 | 70 |
| 106 | 72 |
| 99 | 74 |
| 92 | 76 |
| 87 | 78 |
| 82 | 80 |
| 77 | 82 |
| 73 | 84 |
| 69 | 86 |
| 66 | 88 |
| 62 | 90 |
| 59 | 92 |
| 56 | 94 |
| 53 | 96 |
| 51 | 98 |
| 48 | 100 |

## What it shows (fit, 2026-10-10)

Equal white means `flux(LED) x (CNTR/50)^g = flux(255)`. With flux taken as
proportional to LPW1, the whole table fits `LED = 255 x (50/CNTR)^g` with
**g = 2.41** (worst error 4 LED levels, rms 1.0). With g = 2.2 the worst
error is 9.5 levels (rms 7.1).

So, on the original light engine:

1. **CNTR is a gain on the encoded signal with 50 as unity** - the
   assumption behind `mode: script` (answers-1 §1, `gain = contrast / 50`)
   is confirmed.
2. **Light output was proportional to LPW1** - the linear flux default
   (answers-2 B1) held for the original engine. Owner, 2026-10-10: the
   projector measures an LED drive ramp at every startup (colour balance and
   drive current vs output), so the output tracks LPW1 on the non-stock
   engine too, and anecdotally it does. The linear default therefore stands;
   a `flux_table` measurement is an optional check, not a requirement.
3. **The effective display gamma relating signal to light was about 2.4,
   not 2.2** (with flux proportional to LPW1, the 2.41 can only come from
   the projector's own gamma). `compensation_eotf` (the exponent the compensation hook
   linearises with in `mode: compensate`) defaults to 2.2 from answers-1
   §7 ("the FL35 runs film 2.2"). If the projector behaved like 2.4 while
   VP assumes 2.2, compensation would come out slightly too bright (x1.05
   at LED 150, x1.09 at 100, x1.15 at 60). Owner decision 2026-10-10: keep
   2.2 - the projector has its own gamma settings, reports 2.2 at the
   setting in use, and the gamma of the old measurement is unknown. It can
   be changed in an LED profile (`compensation_eotf`) if the projector
   shows otherwise.
