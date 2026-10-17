# LED light smoothing - design proposal (2026-10-17, planner agent; not built)

Owner request: smooth out the projector's visible light steps so changes look like a slow ramp.

## Summary for the owner

- **internal_script:** VP does not hide the script's LPW1 steps today. `script_smoothing` bridges
  CNTR steps only. Proposal: an optional picture-gain crossfade around each LPW1 step. The script's
  decisions, commands and rhythm stay identical; only VP's picture gain eases the step in over about
  0.5-1 s.
- **compensate:** VP's gain already cancels LED steps on the picture, frame by frame. What remains
  is model error: latency, settle time and the flux curve have never been measured, so the gain
  switches slightly at the wrong time or by the wrong amount. The black floor also still steps.
- **Black floor (both modes):** projector black is LED light leaking through. No picture gain can
  change it; only smaller LED steps help. In compensate VP can cap each command's change in light.
  In script mode that would mean finer step tables, which is the owner's choice because the tables
  are the script's.
- **First step either way:** measure the LED. The existing light sensor can give the flux table;
  response time needs a faster sensor sketch.

## Why steps are visible

- **Step size in light.** Flux is assumed proportional to LPW1, so one level is about 1% of light
  at LPW1 100 and about 0.4% at 255. The script moves up to 7 levels up or 5 down per loop
  (LedDriveConfig.h:136-139). That is up to 7% at LPW1 100, against roughly 1-2% the eye notices.
- **Command rate.** The script makes about 2 changes per second. Compensate sends the newest target
  once per `min_command_gap_ms`, so each command carries rate x gap levels (3.5 at 200 ms, 9 at
  500 ms). Drops are capped at 12 levels, counted in levels, not light.
- **Projector response.** Unmeasured: `led_latency_ms` 80 and `led_settle_ms` 300 are placeholders,
  and it is not known whether the FL35 ramps internally.
- **Perception.** The eye follows ratios, so level-based steps are most visible in dark scenes.
- **CNTR.** One unit at 50 is about 4.9% of light, coarser than an LPW1 level, and it does not move
  the black floor.

## Options considered

| Option | Verdict |
|---|---|
| (a) Picture-gain crossfade | Yes for script mode, following the precedent of ScriptSmoother (LedController.cpp:76-151). Compensate already applies exact gain, so a relaxing gain makes no sense there. |
| (b) Alternate adjacent LED levels | No. With 200-500 ms gaps it is about 1 Hz flicker of 1%, which reads as pumping. It also doubles the command count. |
| (c) CNTR + LED sub-steps | No. CNTR is coarser than an LPW1 level, leaving it above need is a defect, and in script mode CNTR belongs to the script. |
| (d) Shape the schedule in light terms (stops/s) | Yes for compensate. It is the only lever for the black floor. For the script it means table changes, which are the owner's choice. |
| (e) Look-ahead planned ramps | Compensate only: start earlier, ramp slower, land on time. |

## Recommended design

### A. Script mode: `script_light_smoothing`

VP changes only the picture gain; the script's commands and timing are unchanged.

- **State.** The displayed light D in log domain follows the modelled projector light L. L is the
  flux of the landed LPW1 after `led_latency_ms` + `led_settle_ms`, reusing SettleModel.
- **Follower.** D follows L with a first-order time constant `light_smoothing_ms`, slew-limited to
  `light_smoothing_max_rate` stops/s. The difference D - L is clamped to the gain bounds.
- **Gain.** Encoded gain is exp((lnD - lnL)/E), multiplied by the CNTR smoother's gain. E is the
  compensation EOTF exponent.
- **Variant (i), default when switched on:** gain is at most 1, so the picture is never brighter
  than the projector. Increases are eased in after they land; decreases are pre-ramped over the
  roughly 230 ms between the decision and the landing.
- **Variant (ii):** `light_smoothing_max_gain` above 1 (for example 1.08), so decreases are eased
  too. The shader's soft knee protects highlights.
- **Instant reset:** on cuts, custom-dim enter/exit, idle restore and mode switches, D = L at once.
  Dumps stay instant.
- **Snap to unity:** when the gain is within 1e-4 of 1, it is set to exactly 1.0, so the picture
  passes untouched.
- **Precision.** The ramp is below one code per frame. The float32 gain ahead of the sigma-delta
  carries it, so it never bands. Below-black detail is kept (spow is sign-preserving). With a gain
  above 1, the knee compresses above-white detail slightly, bounded by `max_gain`.

### B. Compensate: no relaxing gain; three additions instead

1. **Calibrated models from measurement:** latency, settle, `flux_table`, and an optional
   `led_ramp_ms` if the FL35 turns out to ramp internally.
2. **`rate_domain: flux` + `max_floor_step_pct`** (for example 1.5%). Rates and per-command limits
   are set in light ratio, enforced in the controller rather than the driver. Dark ramps get slower;
   the floor step becomes invisible.
3. **`planned_early_ramp`:** rate = max(rate_min, distance / time to the scene), bounded by
   rate_max.

### Failure modes

| Failure | Effect | Handling |
|---|---|---|
| Missed command | A step-sized error | Eased back to the truth over the follower's time constant; never worse than the raw step |
| Wrong latency | A short blip | Ship default off; Config warns while latency and settle are placeholders |
| Change bigger than the gain bounds | The rest shows as a plain step | Gain clamped to its bounds |
| Switch off | Plain steps | Gain exactly 1 |

## Config (all default off or neutral)

`[led_drive]` keys:

| Key | Default |
|---|---|
| `script_light_smoothing` | off |
| `light_smoothing_ms` | 600 |
| `light_smoothing_max_rate` | 0.5 stops/s |
| `light_smoothing_max_gain` | 1.0 |
| `light_smoothing_min_gain` | 0.85 |
| `rate_domain` | levels (or flux) |
| `max_floor_step_pct` | 0 (off) |
| `planned_early_ramp` | off |

`[projector]` gains an optional `led_ramp_ms`.

The new switch is independent of `ApplyCommandSource` (LedDriveConfig.h:1012) and works only with
`adaptive_gamma`. Its Config control sits next to `script_smoothing`, with a "placeholder, not
measured" hint.

## Tests

- **CPU:** a SimulatedProjector (flux table, dead time, settle, gap, drops).
  - Continuity metric: the largest |d ln P| in any 100 ms window outside cuts. Below 0.5% with exact
    models; bounded with a 20 ms latency error.
  - Gain stays within bounds and returns to exactly 1 when quiet; D = L on cut frames.
  - Guard: LedScriptCorrelationTests must give identical command streams with the switch on and off.
- **GPU:** the gain ramp in the compensation shader is monotonic, keeps below-black, and gain 1 is
  bit-identical.

## Measurement (prerequisite)

The light sensor exists (AmbientSensor.h, VEML7700/TSL2591, tools/light-sensor/vp_light_sensor.ino).

1. **Flux table.** Dark room, white field, LDMM 0, CNTR 50. Step LPW1 from 60 to 255 with a 3 s
   dwell and average the readings. Also measure custom-dim values 0-40.
2. **Response.** A fast sketch: 25 ms integration, timestamped readings. Alternatively, 240 fps
   phone slow motion. Steps of 5/7/12 levels at LPW1 100 and 200, up and down, ten times each. Fit
   the dead time and settle time constant, and check for an internal ramp.
3. **Harness.** An opt-in test `VP_TEST_LED_MEASURE=COMproj,COMsensor` that prints config lines.

## Effort and order

1. Measurement harness and the owner's measurement run: 1.5 days.
2. Script-mode smoothing (i) then (ii), with simulator tests and the correlation guard: 2-3 days.
3. Config controls and the GPU test: 1 day.
4. Compensate flux-domain rate, floor cap and planned ramp: 2 days.
5. The owner judges on footage; everything stays default off until then.
