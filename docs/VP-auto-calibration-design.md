# Automatic calibration (design, 2026-10-17; built 2026-10-18 on feat/calibration-remainder)

> **Status 2026-10-18:** built on `feat/calibration-remainder`: the core (de1d92a0) plus the
> ArgyllCMS meter under ConPTY, renderer patterns, the session, the RPC and the Config wizard.
> User guide: docs/calibration/AUTO-CALIBRATION.md; report: docs/agent-reports/calibration-remainder.md.
> **Change from this design:** patches bypass the sigma-delta and dither (exact whole codes,
> owner 2026-10-17), rather than measuring through the sigma-delta.
> **Phase 2 (2026-10-18, `feat/calibration-phase2`):** the Dynamic Drive (LED) characterisation
> run, the ambient-light check (ambient_screen_reflect), the HDR (PQ) verify with dE ITP and the
> real-meter transcript recording; report docs/agent-reports/calibration-phase2.md. Latency and
> settle need a fast light sensor (`led_sensor_port`); a colorimeter alone gives the flux table,
> the CNTR curve and compensation_eotf. Custom-dim (LDMM 1) characterisation is not built.

## Summary

The owner puts a meter on a tripod facing the screen and clicks "Calibrate" in the Config app.

**What VP does:**
- Shows its own patches through the real output path: sigma-delta on, 8-bit limited link,
  BTB/WTW codes included.
- Builds a 3D LUT (.cube, the format VP already loads) and a display profile (peak, black, EOTF,
  gamut).
- Measures the LED values that are guesses today: flux_table, LED latency/settle,
  cntr_max_usable, dmd_clip_cntr and compensation_eotf.
- Runs a verify pass that reports dE2000.

**Safety and config:**
- The config is backed up byte for byte. Only the needed keys change, after a diff the owner
  confirms.
- Every earlier run can be restored with one click.
- LED/CNTR always return to the normal state at the end.

**Meter:** Calibrite Display Pro HL (i1Display family, supported by ArgyllCMS).
- Test it with Argyll 3.5.0. One report has a Plus HL hanging in 3.1.0, so buy returnable.
- An RGB laser needs a spectral correction (CCSS/CCMX) for an accurate white point.

**Time (FL35):**

| Step | Time |
|---|---|
| Warm-up | 30 min, unattended |
| Quick (greyscale + matrix) | about 10 min |
| Standard (9³ = 729-patch LUT) | 35-45 min |
| Thorough (17³) | about 2.5 h |
| Verify | about 4 min |
| LED measurements | about 15 min, first time only |

## Architecture

The calibration runs inside VP, which owns the renderer and the projector port. The Config app is
the UI, talking over RPC (Operation::Calibration = 10).

New files:

| File | Purpose |
|---|---|
| VP-V/modules/output/calibration_lut/calibration/Meter.h | IMeter; Argyll spotread / simulated / ambient lux meters |
| VP-V/modules/output/calibration_lut/calibration/PatternPlan.h | Patch sets and time estimates |
| VP-V/modules/output/calibration_lut/calibration/DisplayModel.h | Fit, inverse, dE2000, .cube writer |
| VP-V/modules/output/calibration_lut/calibration/LedCharacterize.h | Flux table, CNTR curve, latency/settle fit |
| VP-V/modules/output/calibration_lut/calibration/CalibrationSession.h | Step machine, per-patch journal, resume, safety snapshot/restore |
| VP-V/modules/output/calibration_lut/calibration/CalibrationOutputs.h | Versioned run folder, key-only config edits |
| vprenderer/CalibrationPatternSource | Pattern frames and bypass parameters |
| VideoProcessor-Config/CalibrationWizard | The wizard UI |

## Meter and licence

- **ArgyllCMS is AGPLv3.** Do not link it. Run spotread.exe as a separate process (an aggregate,
  not a combined work).
  - Drive it under a Windows ConPTY (CreatePseudoConsole), sending a keypress per reading and
    parsing the "Result is XYZ:" lines.
  - The user installs Argyll; `[calibration] argyll_path` points to it.
- **Vendor SDKs** are licensed to OEMs only. Leave a slot for them later.
- **Placement:**
  - projector: tripod 30-50 cm from the screen centre, facing the screen;
  - TV: meter flat on the screen centre.
- **Room light:** the ambient sensor watches it, and a patch is measured again if the room light
  changes by more than 20%.

## Patterns through the real output path

- Full-screen RGBA16F patches at output size, in the LUT input space. Values below 0 or above 1
  produce the BTB/WTW codes.
- The patch is declared to be in the target colour space already (the ExtendedRange trick), which
  turns off tone, gamut and transfer conversion.
- **Off:** deband, peak detection, NLS, user shaders, subtitles, OSD.
- **Kept:** the sigma-delta, limited-range encoding and the ExtendedRange restore.
- Frames are presented continuously, so the meter averages the sigma-delta as a viewer's eye does.
- The LED module is held for the whole run, and CancelLight clears any queued command, so APL
  drive cannot move LPW1 between patches.

## Measurements

| Set | Contents | Time |
|---|---|---|
| Greyscale | 5% steps; near-black in 1% steps; BTB codes 4/8/12/15; WTW 240/254; peak; black | about 3 min |
| Primaries and secondaries | 25/50/75/100% | about 1 min |
| 3D set | 5³ / 9³ (default) / 13³ / 17³ | 3 / 15 / 45 / 105 min |
| Drift reference | white re-measured every 50 patches | about +2% |

The run reports the additivity error (white against R+G+B).

**FL35 LED measurements:**
- **Flux table:** LPW1 from 60 to 255 in steps of 5, both directions (catches hysteresis), 3 s dwell.
  Also custom-dim 0-40.
- **CNTR curve:** codes 235 and 254 at CNTR 50-74, giving dmd_clip_cntr and cntr_max_usable.
- **Latency and settle:** too fast for a colorimeter.
  - Use a VEML7700 at 25 ms (new fast sensor-sketch mode) or, better, a BPW34/OPT101 photodiode
    on the ESP32 ADC at 1 kHz.
  - Fit dead time and lag, and check for an internal ramp.
  - A picture step against an LPW1 step gives the LED-vs-picture offset.

## LUT maths

1. **Forward model:** per-channel monotone spline + 3x3 matrix + regularised thin-plate residual.
2. **Target:**
   - slot primaries: BT.709 / P3-D65 / BT.2020 clipped to the display gamut;
   - output_gamma, or BT.1886 with the measured black;
   - D65 white, scaled to the largest reachable level.
3. **Inverse:** Gauss-Newton in linear light. Out-of-gamut colours are chroma-clipped at constant
   hue and lightness (LCh), so the neutral axis stays exact.
4. **Grid:** 33³ (up to 65³).
   - Monotone neutral axis.
   - End slopes fitted to the measured BTB/WTW (so ExtendedRange::LutExtrapolate is correct).
5. **File:** .cube without DOMAIN lines.
6. **Output:** one measurement gives three LUTs.
7. **CPU cost:** under a few seconds.

## Outputs

**Run folder:** `luts/calibration/<display>/<timestamp>/` contains:
- raw.jsonl
- display_profile.cfg (the reserved `[display.<name>]` keys plus an output fingerprint)
- the three .cube files
- verify.json and report.html
- before/ (byte copies of the config files)

`luts/calibration/index.json` lists every run.

**Config edits:** made with SetExisting/SetKnown and SaveSafely. Only these keys change:
- calibration_lut_* in the active colour profile;
- `[projector.default]`: flux_table, led_latency_ms, led_settle_ms, cntr_latency_ms,
  cntr_settle_ms, cntr_max_usable, dmd_clip_cntr;
- compensation_eotf.

## Verify

- **Patches:** about 60, including ColorChecker, skin tones, near-black, and 20 random patches not
  used for the fit. Measured before and after.
- **Report:** dE2000 average and maximum, gamma and CCT per step.
- **Pass:** average < 1.5 and maximum < 3.
- **Startup check:** warn if the output fingerprint no longer matches the calibrated one.

## Safety

- **Start:** a snapshot of the current state plus a `calibration-active` marker.
- **On finish, cancel, error, meter unplug, Config silent for 60 s, or exit:** RestoreFullLight
  (reset dim mode, restore contrast), bounded.
- **After a crash:** BringToKnownState on the next start.
- **Never left behind:** CNTR above 50, or LDMM at 1.
- **OLED:** rest breaks.
- **The wizard tells the user to:** turn off dynamic modes and ABL-type features.

## Wizard

1. Meter and Argyll path.
2. Placement, with a live aim reading.
3. TV dynamic modes off; room dark (sensor check).
4. Warm-up timer.
5. Plan, with its time estimate.
6. Run, with progress and a live greyscale chart.
7. Review: before/after, diff, Apply.

Cancel, resume and revert are always available.

## Tests (CPU)

- **Simulators:**
  - display: primaries, gamma, black, crosstalk, 8-bit limited range, sigma-delta average,
    BTB/WTW, CNTR clip, flux, latency;
  - meter: noise, low-light floor, matrix error.
- **Fit and LUT:** the fit recovers the model; round trip < 0.5 dE; the .cube passes the existing
  parser; end slopes match LutExtrapolate.
- **LED fits:** latency within 10 ms; tau within 10%.
- **Safety and config:** cancel at any step restores state; key-only diff; resume.

## Effort

About 25 days.
