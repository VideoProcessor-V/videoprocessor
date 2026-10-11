# Stage 7 step 7 — LED drive: integration proposal

2026-10-11. Implementer → planner, before any code. It covers:
- the LED shader sign fix (BTB/WTW answers item 5);
- delta 4 (gain order, E = 2.41);
- `docs/VP-led-drive-control-policy.md` (with the owner's black-frames
  note), which supersedes deltas 3 / 3b / 3-answers;
- the owner's requirement that script mode be the script exactly.

Where the policy and the code meet, this says what changes, what stays, and
what I need decided.

## 0. What exists today (Stage 7 steps 1–6, audited)

- `LedDrive::ScriptController`:
  - `beta_22.py` ported parameter for parameter, tested against a literal
    transcription of the script;
  - the light curve and the step tables are configurable;
  - contrast follows the script's bands and dynamics, including both fast
    dumps (a timed schedule within a tick) and the anti-pump band;
  - **but contrast is emulated as VP gain** (contrast/50 on the encoded
    signal) and never sent, under "CNTR is never driven".
- `LedDrive::CompensateController`:
  - per output frame, it ramps the LED by `rate_up` / `rate_down`;
  - VP gain compensates in linear light;
  - highlight policy: persistence, area, knee or gain, one tolerance;
  - early ramps and a cut jump using the look-ahead;
  - a fade-to-black plan with custom dim;
  - CNTR stays at 50.
- `BarcoAsciiDriver`:
  - one thread, the 200 ms gap, the optional 5 s pause, read-back;
  - newer LED targets replace queued ones (coalescing), and dimming is
    split at `max_levels_per_command`;
  - a "script" framing that sends the script's exact bytes (lower case,
    literal `cr`, then echo's CR LF);
  - `SetContrast` and a timed sequence path exist.

## 1. Step 7a — LED shader sign fix + delta 4 (small; own audit)

- **Sign fix:** `LED compensation.glsl` uses `max(color, 0)` before its
  `pow`s. Replace it with sign-preserving powers (`sign(v)*pow(abs(v), e)`)
  so BTB survives the boost and the gain.
  - GPU test: greys at codes 40 and 1019 through the hook with gain 1.3 and
    gamma boost 2.4. 40 stays below 64 (not clipped); 1019 is as the knee
    allows. Module guide §6 updated.
- **Delta 4:**
  - The order is already boost first, then the gain in a
    `compensation_eotf` domain. The change is `compensation_eotf` = 2.41
    (the default and the test config).
  - The raw-frame 1D transfer and the highlight-linear calculation are made
    to use the same E on the same final value.
  - Test: code 126 grey, LED 255 → 100, predicted light constant to < 1%,
    with boost 2.4 and with boost 1.0.

## 2. Step 7b — `script` mode is the script (real CNTR)

Changes, each with a test against the transcription:
1. **Contrast is sent as `CNTR`** at the script's times. The schedule
   already holds the script's order and offsets:
   - LPW1 at the tick start;
   - 200 ms;
   - the fast-dump steps every 100 ms;
   - 200 ms;
   - the final CNTR.
   With `script_smoothing: off`, **VP gain stays 1** (no emulation).
2. **Duplicate sends:** when the anti-pump step is 0, the script still
   re-sends the same CNTR value. Script mode sends it too (no coalescing in
   script mode).
3. **Curve:** in script mode the light target uses the script's literal
   formulas, including `0.8333` (one level lower at APL 76, 82, 88, 94),
   per your "stay as the script has them". The second documented difference
   (contrast 61 at APL 30) already follows the script.
4. **Startup and exit:** LPW1 255 and CNTR 50 at start, as the script.
   CNTR 50 is restored on exit or disable, as now.
5. **`script_smoothing: on`:** the same commands, plus VP gain bridging each
   integer CNTR step and the command latency (gain = modelled CNTR light /
   commanded CNTR light, ramped over `cntr_latency_ms`). Content stays
   constant between steps.

Inherent differences that cannot be removed (documented, not "fixed"):
- **APL:** VP measures its own output (the active picture, PRE_OUTPUT);
  the script screenshotted the desktop with a fixed crop (y 57–423) after
  madVR.
- **Highlight:** VP uses its brightest 3×3 block against 215; the script
  used the brightest pixel of an 854×480 INTER_AREA downscale.
- **Pacing:** the script's loop also took screenshot time; VP's tick is at
  least the schedule's duration.

## 3. Step 7c — `compensate` mode per the control policy (largest)

Mapping of the policy onto the controller:

| Policy | Implementation |
|---|---|
| §6 actuator models | `LedModel` (dead time + first-order settle: `led_latency_ms`, `led_settle_ms`) and `CntrModel` (`cntr_latency_ms`, `cntr_settle_ms`), driven by the *sent* commands (driver feedback), giving modelled light per frame |
| §2 software first | per frame, required factor F = target/actual light; VP gain takes it up to its code headroom within `highlight_tolerance_stops`; only the remainder moves CNTR (paired with the LED) |
| §3.2 pairing | CNTR follows the LED's **modelled** light so LED-light × (CNTR/50)^E stays constant; the VP gain removes CNTR's integer steps (each ≈ 4.9% light) |
| §3.2 rate law | `rate = clamp(k·|distance|, rate_min, rate_max)` per actuator and direction, replacing `rate_up` / `rate_down`; starting values derived from the script's tables |
| §3.3 bright cut | on the cut frame (or during black, owner note): one CNTR dump sized from the queued statistics so the knee stays within tolerance; gain to its ceiling; then recovery gain = min(ceiling, ramp_target / LED-light) |
| §3.4 dark cut | paired early ramp within the window; LPW1 before CNTR; CNTR never above the paired value for the modelled LED |
| §3.5 look-ahead | head start, hold before a known cut, exit-from-black pre-positioning (CNTR to the dump point and LED to ≤ `black_prelift_max` during the black) |
| §4 floor | `max_cut_dip` → the scene floor (replaces `floor_recovery_policy`) |
| §5 fade to black | unchanged |
| serial | bright: CNTR then LPW1 200 ms later; dark: LPW1 then CNTR |
| regression | `cntr_assist: off` = today's compensate exactly; `cut_cntr_dump: off` = no dump, CNTR descends paired |

Proposed sub-steps, each audited:
- **7c-1:** models, rate law, software first, pairing;
- **7c-2:** bright and dark cuts, recovery gain, `max_cut_dip`;
- **7c-3:** look-ahead extras (head start, black-frame pre-positioning,
  exit from black) and the Config app's LED page keys.

## 4. Questions

1. **Serial bandwidth while paired.**
   - Within a scene, every LED move needs a CNTR move. At the 200 ms gap
     that is one pair per 400 ms (≈ 2.5 LED updates/s instead of 5).
   - Proposal: alternate LPW1 and CNTR every 200 ms. The VP gain bridges
     the gap between them, so content stays constant and the floor moves
     at the rate law's pace.
   - Is that acceptable, or should the CNTR follow only every N LED steps,
     with VP gain covering the difference?
2. **The LED model before measurement.** The policy's defaults (80 ms
   dead time, 600 ms settle) drive the recovery gain. If the real LED is
   faster, the gain under-compensates for a moment (darker, never
   brighter); if slower, it trims. Fine to ship with the defaults and the
   §6 camera measurement listed in the tally?
3. **Script mode coalescing.** Is "no coalescing in script mode" right,
   even though the script's commands then queue behind a slow port? The
   script waited by sleeping; VP's driver would queue.
4. **Order.** 7a, then 7b, then 7c-1..3, before the projector deploy (the
   owner wants the LED drive in first). OK?
