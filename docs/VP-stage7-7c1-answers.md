# Stage 7 step 7c-1 — planner answers

2026-10-11. On stage7-rc2 91216a2a (7c-1) and bb1a1231 (7a/7b audit fix).
Acknowledged as built; three answers; carry on with 7c-2.

## Acknowledged

- 7a/7b fix: a live script→compensate switch waits until CNTR is back at
  50 and sends nothing meanwhile; the script stops its tick on a settings
  change. Both right.
- 7c-1: dead time + first-order settle (fast-biased 300 ms; 0 = old
  model); CNTR model; software first with VP's headroom defined by the
  sustained highlight and the allowed stops; CNTR as the smallest whole
  value covering the rest; CNTR only on integer crossings, up at once,
  down with a 2% margin; CNTR commands take the next slot ahead of a
  waiting LPW1; rate law per direction; CNTR to 50 on exit, disable and
  the switch to script. The "CNTR could never return to 50" bug caught
  by the build is exactly the kind the test harness is for.

## 1. Regression definition — acceptable as built; do not overload the key

`cntr_assist: off` means "no CNTR"; the settle model and the rate law are
separate settings with their own meaning, and folding them into the key
would make it a mode switch rather than a feature switch. Keep them
separate. Define the previous build as a named regression recipe in the
test suite and in BUILD-STATUS — `cntr_assist: off`, `led_settle_ms: 0`,
`rate_gain_up/down: 0` — and run the pre-7c-1 compensate tests under it.
Update the policy's wording from "`cntr_assist: off` = today's compensate
exactly" to "the regression recipe = today's compensate exactly."

## 2. Rate-law defaults — follow the script's tables

The owner asked to keep his current kinetics. Derive `k`, `min` and `max`
per direction so the per-second rate at distances 25, 50, 100 and 150
matches the script's tables (≈10–17 levels/s up, slower down), and seed
the defaults from that; the 400/120 figures were placeholders from
answers-2, not tuned values. Keep a second, faster preset ("compensated"
— roughly the 400/120 numbers) available in config for the owner to try
once compensation is on, since the floor-only movement under pairing is
less visible than the uncompensated movement the script's rates were
tuned against. Default = script-matched; the owner decides by eye.

## 3. Highlight floor with assist — keep a margin until 7c-2

Until 7c-2's dump exists, a bright cut with CNTR high falls back to the
old recovery (LED jump plus a paired CNTR descent against the model),
which is the drift-prone path the policy was written to replace. So for
7c-1 cap the assist at roughly half the headroom — CNTR no higher than
about 62 (≈1.7×) — and lift it to the full `cntr_max_usable` in 7c-2
together with the dump. Make the cap a config key (`cntr_assist_max`) so
the lift is a config change and the Code auditor can check 7c-2 removes
the restriction rather than the key.

## Next

7c-2: bright and dark cuts, the gain-shaped recovery, `max_cut_dip`,
lifting `cntr_assist_max` to `cntr_max_usable`.

## Implementer note

- 1: done. There is no BUILD-STATUS file in the repo; the recipe is named in
  the module guide (§4.2 box), the policy §7 / §9, the profile example and
  the test helpers `Compensate()` / `Port()`. Building it caught that
  `rate_gain: 0` would have fallen back to the new `rate_max` (17.5)
  instead of the fixed `rate_up` / `rate_down`; it now always uses the
  fixed rates (test RegressionRecipeUsesTheFixedRates).
- 2: defaults up gain 0.25, min 2.5, max 17.5; down gain 0.15, min 2.5,
  max 12.5 levels/s (one step per 400 ms tick). Up lies between the
  script's dark and bright tables at distances 25, 50, 100 and 150; down
  is within 1.25 levels/s of its table. `rate_preset: compensated` = 4.0 /
  1.2, min 20 / 10, max 400 / 120; explicit `rate_*` keys override it.
- 3: `cntr_assist_max: 62` ([led_drive], as in your §7), the lower of it
  and `cntr_max_usable` applies.
- Your copy of the policy lacked the owner's note on black frames (added
  after your base); the three wording changes were applied to the current
  file, keeping that note.
