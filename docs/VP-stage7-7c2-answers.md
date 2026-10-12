# Stage 7 step 7c-2 — planner answers

2026-10-11. Your readings are right on all four, with one addition on Q1
that resolves a contradiction in the record, and one definition on Q3.
Acknowledged: rate defaults from the script's tables, `rate_preset:
compensated`, `cntr_assist_max 62`, the named regression recipe, and the
`rate_gain 0` fix (fixed rates, never the new max).

## Q1 — size of the dump: your reading, plus a DMD-clip term to be measured

Your reading is correct as the policy stands: §1 says CNTR up to
`cntr_max_usable` is pure reserve, so CNTR never compresses and the
tolerance only limits VP's gain. The dump therefore exists for §3.3's
reason — avoiding a paired CNTR descent against the LED model — not to
relieve compression. Build it as you propose:

- bright cut, `cut_cntr_dump: on`: CNTR to 50 on the cut frame (nothing
  to dump if it is already 50 after a software-only dim);
- VP gain to the incoming scene's headroom `g_ceiling = 2^allowed /
  h_new`;
- fade-up under the rate law (Q2);
- partial dump only when a full one would start below `max_cut_dip`:
  keep the minimum CNTR `c` that holds the start at `max_cut_dip`, and
  that residual descends paired with `recovery_bias_pct`.

**The addition.** The record contains a contradiction the measurement must
settle: the light-meter table fits (CNTR/50)^2.41 cleanly to 74 (no
clipping), yet the owner reports substantial highlight crush in the
script where its dump stops (CNTR ≈ 60). Both cannot be true for the same
codes, unless the crush was of WTW (codes above 235) or the table's white
was itself at the DMD limit. So add one parameter now rather than
assume: `dmd_clip_cntr` — the CNTR at which code 235 reaches the DMD's
maximum. Default = `cntr_max_usable` (i.e. no clipping below the reserve
limit, as §1 says). The knee's threshold becomes
`min(VP headroom, DMD headroom at the current CNTR)`, so if the white-field
sweep shows white plateauing below 74, the owner enters that value and
CNTR starts counting against the tolerance automatically. Until measured
the behaviour is exactly your reading.

Keep `cut_cntr_dump: off` as the live alternative (CNTR held high through
the cut, paired descent with the bright-side bias and gain trim) so the
owner can A/B the two recoveries: the dump trades an immediate fade-up
for robustness against model error; the held CNTR gives full mid-tone
brightness at once if the model is good.

## Q2 — the recovery ramp moves in displayed light: correct

As proposed. `R` is the displayed fraction of full, expressed as the
equivalent LED level `L_R` with `Flux(L_R)/Flux(ceiling) = R`, climbing to
the ceiling under the up rate law (same k/min/max as the LED).
`Displayed = min(R, f·C·g_ceiling)`; `VP gain = min(g_ceiling, R/(f·C))`
with `f` the modelled LED and `C` the modelled CNTR light; starts on the
cut frame at `R0 = f·C·g_ceiling`, ends at `R = 1`. The gain lands on 1
when both arrive. This is the gain-shaped recovery of §3.3.

## Q3 — the scene floor from `max_cut_dip`: the higher of the two

As proposed for the derivation: the floor is the lowest LED level whose
start after a full dump, `f_floor · g_ceiling_white` (≈1.26 for white
highlights), is at least `max_cut_dip`; with 0.5 that is ≈0.40 of ceiling
flux, about level 102 — use your number, §4's "about 106" was a rougher
estimate; correct §4 to match. `led_floor` is not superseded: the
effective scene floor is `max(led_floor, floor_from_max_cut_dip)`, so the
owner can set `led_floor` low (even 60) and let `max_cut_dip` govern, or
keep a hard floor above it. `led_floor_reactive` is treated the same way
in reactive mode. `led_floor_black` is unaffected (§5).

## Q4 — which cuts, and the order: agreed

- Bright cut = the incoming target LED level is above the current one.
- Seen cut: CNTR timed to land on the cut frame (sent `cntr_latency_ms`
  ahead), LPW1 200 ms after it.
- Unseen cut: dump at once (§3.6).
- Dark cut: today's early paired ramp bounded by the current scene's
  highlights; LPW1 before CNTR; CNTR never above the paired value for the
  modelled LED.
- The black-frame variant (CNTR and gain set during black, pre-lift) is
  7c-3 with the look-ahead extras. Agreed.

## Implementer note (7c-2 as built)

- Q1: bright cut, `cut_cntr_dump: on` -> CNTR to `DumpTarget` (50, or the
  smallest value holding the start at `max_cut_dip`), sent once per cut so
  it lands with the LED's jump on the cut frame. During the fade-up CNTR has
  a ceiling at the dump point and the need carries `recovery_bias_pct`
  (3%), so a residual only descends, on the bright side. `cut_cntr_dump:
  off`: CNTR held (ceiling = its value), descending paired. With held CNTR
  the scene can already be at full brightness on the cut frame, in which
  case there is no fade-up at all.
- `dmd_clip_cntr` (`[projector]`, default = `cntr_max_usable`): VP's
  software headroom passed to the pairing becomes min(VP headroom,
  Light(dmd) / (highlight x Light(CNTR))); the fade-up's gain ceiling is
  limited the same way at the dump point; the highlight floor uses CNTR's
  headroom only up to `dmd_clip_cntr`. No effect at the default.
- Q2: `CutRecovery` as specified; it starts from the CNTR just sent (not the
  modelled one), so before the dump lands the limit only trims. It ends
  when R = 1 and full compensation fits under the ceiling (no jump).
  `shaped_recovery: off` = full compensation at once (before 7c-2).
- Q3: `max_cut_dip` (0.5) -> floor 102 at the defaults; the higher of it
  and `led_floor` (and `led_floor_reactive` in reactive mode).
- Q4: as agreed. CNTR is now paired on the LED's modelled light *now* (7c-1
  used the lower of now and cntr_latency_ms ahead), so it is never above
  the paired value for the modelled LED.
- `cntr_assist_max` default lifted to 100 (= no cap beyond
  `cntr_max_usable`); the key stays.
- The regression recipe grows by two keys for 7c-2's new behaviour:
  `max_cut_dip: 0` and `shaped_recovery: off` (with `cntr_assist: off`,
  `led_settle_ms: 0`, `rate_gain_up/down: 0`).
- Your policy copy again lacked the owner's black-frames note; your two
  changes (§4 floor wording, §7 `dmd_clip_cntr`) were applied to the
  current file, keeping it.
