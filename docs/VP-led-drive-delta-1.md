# LED drive module — delta 1 (apply to the current implementation)

2026-10-09. Instructions only; no history. Read with
`VP-led-drive-delta-1-answers.md`, which takes precedence where they differ. Each item says what to change
relative to what exists now. Keep your existing file structure. Where a
change touches the config, update `VideoProcessor-test.cfg` defaults and the
unit tests in the same commit. Commit as
"Stage 7 step 5: delta-1 amendments".

---

## 1. Flux table

Add to `[projector.Default]`:

```
flux_table:    # optional; pairs lpw:relative_flux, e.g. 60:0.55, 80:0.78, 100:1.00
```

- Piecewise-linear interpolation between points; outside the listed range,
  clamp to the end values.
- When absent: `flux(lpw) = lpw / lpw_max` (linear) and log one startup
  line saying the linear default is in use.
- Everywhere the controller computes the LED gain term as
  `light_full / light_actual`, replace with
  `flux(led_ceiling) / flux(light_actual)`.
- The highlight policy's headroom limit uses the same flux ratio.

## 2. Latency parameter

- Rename `led_response_ms` → `led_latency_ms` and move it to
  `[projector.Default]` (it is a property of the projector: command
  transmission plus response).
- Use it both ways:
  - When a frame's statistics are available ahead of presentation
    (lookahead): send the LED command `led_latency_ms` before that frame;
    apply the gain on that frame.
  - When they are not (e.g. `Low_Latency`, or a change the lookahead did
    not see): send the command immediately and delay the gain by
    `led_latency_ms`, rounded to whole output frames, so gain and LED
    still land together.
- The gain continues to follow the modelled LED trajectory (one-pole lag
  with `led_latency_ms`), not the command.

## 3. Ramps in `compensate` mode

Add to `[led_drive.Default]`:

```
rate_up: 400      # max LED levels per second when brightening (compensate mode)
rate_down: 120    # max LED levels per second when dimming (compensate mode)
deadband: 3       # levels; no movement while |target − current| <= deadband
early_ramp: true  # see item 4
```

- In `compensate` mode the controller runs **per output frame**, not per
  tick: each frame it moves the LED level toward the target by at most
  `rate / output_fps`, and holds still inside the deadband. The gain is
  computed from the ramped level each frame.
- `script` mode is unchanged: per-tick step tables, `tick_ms`,
  `anti_pump_band`.
- The serial driver already coalesces to the newest target; keep that. The
  modelled trajectory drives the gain, not the ACK stream.

## 4. Early ramp before a known cut (`compensate` mode only)

Replace the rule "never move the LED earlier than its latency" with:

- `script` mode: unchanged — never earlier than `led_latency_ms`.
- `compensate` mode, when `early_ramp: true` and the lookahead shows a cut
  or a large APL change ahead: the ramp may start early so the LED reaches
  the new level on the cut frame. Constraint: dimming ahead of the cut may
  go only as far as the **current** scene's highlights permit within
  `highlight_tolerance_stops` (the gain keeps content constant; only floor
  and highlight headroom move). Brightening ahead of a cut is
  unconstrained. Within those bounds, start as early as the lookahead
  allows.

## 5. Lookahead statistics on raw queued frames

The fork's queued-frame walk operates on raw captured frames before tone
mapping. Implement lookahead statistics there as follows:

- From each queued frame's v210 luma plane (subsampling allowed), map each
  sample through a **1D input-code → output-code transfer** and compute
  `apl`, the highlight percentile and the block means from the mapped
  values. Build the transfer once per active rendering profile from: the
  input transfer (PQ for HDR/LLDV, gamma for SDR), the static tone-map
  curve at the configured target nits, `output_gamma`, and the
  limited-range encode. Rebuild it on profile change.
- Keep the exact GPU measurement at PRE_OUTPUT for the frame being
  rendered, as already implemented.
- For each presented frame, compare the raw-predicted APL (from when it was
  queued) with the exact rendered APL. Maintain a smoothed ratio (window of
  a few seconds) and multiply queued predictions by it.
- Cut and fade detection run on raw luma.
- `script` mode does not use raw-frame predictions; it keeps the exact
  PRE_OUTPUT measurement of the presented frame.

## 6. Tests to add

- Flux table interpolation and the linear default.
- Gain timing: with lookahead, command precedes frame by `led_latency_ms`;
  without, gain is delayed by the same amount.
- Ramp limits and deadband in `compensate` mode on a synthetic APL step.
- Early-ramp bound: dimming ahead of a cut stops at the current scene's
  highlight limit; brightening ahead is not limited.
- Raw-to-output mapping: a synthetic raw frame through the 1D transfer
  matches the exact path within a small tolerance; the correction ratio
  converges on a synthetic mismatch.
- `script` mode behaviour unchanged by all of the above.
