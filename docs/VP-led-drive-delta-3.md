# Dynamic Drive module — delta 3: use the projector's reserve (hybrid LED + CNTR)

2026-10-11. Supersedes "CNTR is never driven" (answers-1 §1, delta-1) for
`compensate` mode. `script` mode is unchanged. Instructions relative to the
current implementation; keep the file structure.

> **Owner 2026-10-17:** "byte-identical to the script" below means the script's decisions, bands
> and rhythm (and the FL35 command stream). The script is the reference for intent, not for its
> coding limitations (picture-only, full-coverage peaks; read-back off). See
> docs/VP-led-core-sources.md, "The script is the reference for intent".

## Why

The owner's light-meter table (CNTR vs LPW1 at equal white, fit
(CNTR/50)^2.41) shows that raising CNTR raises *absolute* white: ≈2.5× at
CNTR 74. The projector runs the DMD well under its maximum at CNTR 50. VP's
gain is in code space and tops out at code 255 ≈ 1.2× the light of code
235, so VP alone can compensate only ~20% of dimming before the knee must
compress highlights. The projector's reserve is the headroom the old
script used; the new design must use it too.

## Control law (`compensate` mode)

The controller still produces one desired **floor factor** `F ≥ 1` per
frame (how much darker the floor should be than nominal), from the same
curve, ramps, deadband, persistence and early-ramp logic as now. It is
realised by three actuators:

1. `LPW1 = flux⁻¹(flux(led_ceiling) / F)` — the LED supplies the floor
   reduction (slow, rate-limited as now).
2. `CNTR = 50 · F^(1/E)` where `E = compensation_eotf` — the projector
   supplies the matching light so content is unchanged. Clamp to
   `cntr_max_usable` (measured, see below); beyond it the knee applies as
   today.
3. VP gain `g` = the residual between the exact target and what the
   projector is actually doing (CNTR quantised to an integer step ≈ 5%
   light; both commands behind their latencies): `g = F_target /
   (F_led_actual · (CNTR_actual/50)^E)`, applied in linear light as now,
   with the knee. In steady state `|g − 1|` is a few percent.

Content light is therefore constant through every move; only the floor
moves; highlights keep their gradation up to the DMD's real maximum.

## Rates and cuts

- **Dimming:** LED and CNTR move together at the LED's slow rate
  (`rate_down`); VP smooths CNTR's integer steps. No speed required.
- **Brightening on a cut:** headroom is needed immediately and VP cannot
  make light. Send **one CNTR command timed on the cut** (lookahead: so it
  lands on the cut frame; reactive: immediately), to the value that gives
  the new scene its headroom at the *current* LED. Then raise the LED at
  `rate_up` and ease CNTR back down in step so content stays constant,
  VP trimming the residual. This is the old script's contrast dump, now
  masked by the cut and matched by the LED.
- **Brightening within a scene** (no cut): both at `rate_up`, paired.
- Early ramps before a known cut: unchanged — a paired LED/CNTR move keeps
  content constant, so it is allowed within the current scene's headroom.

## Serial

Two commands per update instead of one. The driver already coalesces; send
LPW1 and CNTR as a pair per tick (CNTR first on brightening, LPW1 first on
dimming) and keep the measured gap between them. Expect ~5 updates/s at a
200 ms gap; the VP gain hides the rest.

## Measurements — use what exists; refine only if ever needed

No new meter session is required. Work from the owner's existing table:

- `E = 2.41` from the table's fit.
- `cntr_max_usable: 74` — the table's last point, known to produce more
  white without visible clipping. Treat it as the clamp. If the owner ever
  wants more range, a single white-field sweep above 74 would find the
  DMD plateau; not planned.
- `cntr_latency_ms` = `led_latency_ms` until there is a reason to think
  otherwise; the script drove both at the same pace for years.
- The assumptions that CNTR is a pure gain (black unaffected, mid-grey
  ratio constant) are consistent with the table and with the script's
  years of use; take them as given. If a later cinema session shows the
  floor rising with CNTR, that is the one thing worth measuring.

## Config

```
[projector.Default]
cntr_max_usable: 74
cntr_latency_ms: 80

[led_drive.Default]
cntr_assist: on        # off = VP-gain-only compensate (today's behaviour)
```

`cntr_assist: off` must reproduce the current `compensate` behaviour
exactly (regression test). Startup still sets CNTR 50 and restores it on
exit/disable, as now.

## Tests

- Pairing: for a synthetic F ramp, `LED × CNTR^E` is constant to within
  one CNTR step, and `g` stays within that step.
- Cut: one CNTR command on the cut frame, LED then ramps, CNTR eases back,
  content term constant throughout.
- Clamp at `cntr_max_usable`: beyond it the knee engages, never a CNTR
  above the clamp.
- `cntr_assist: off` identical to the previous build.
- `script` mode untouched.

## Documents

Update the module guide §4.2/§4.3 and the brief's Stage 7 to this law;
mark answers-1 §1's "never driven" as superseded for `compensate` mode.

## Owner requirements added 2026-10-11 (before implementation)

1. **Fallback that is today's script, exactly.** The owner's script must
   remain available as a selectable mode. It may add software (VP gain)
   interpolation to smooth the script's visible steps, but must otherwise
   behave like the script does today.
   - Check first whether the existing `script` mode already matches the
     script exactly. Then add the optional step interpolation as a
     separate switch, off = byte-identical to the script.
2. **The lookahead must be adapted** to the hybrid law. Contrast (CNTR)
   moves timed on cuts, and paired LED/CNTR ramps, must be planned from
   the queued-frame statistics, not only reactively.
3. **Refinements so deeper dimming is practical.** Today two things limit
   how low the LED can go:
   - **slow recovery**: the LED's `rate_up` back to brightness;
   - **the large contrast distance**: the bigger the dim, the bigger the
     CNTR jump needed when a bright scene arrives.

   These need design work before or alongside delta 3's implementation.
   Bring options to the planner. Candidates, to be evaluated rather than
   assumed:
   - use the 200 ms queue to start recovery before a known bright cut;
   - bound the dimming depth by how far the projector can recover within
     the lookahead window;
   - split the CNTR jump across the cut and the next few frames, with VP
     gain masking the remainder;
   - measure the real LED/CNTR rate limits rather than taking today's
     settings as fixed.
