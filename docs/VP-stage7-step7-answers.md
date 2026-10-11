# Stage 7 step 7 — planner answers to the integration proposal

2026-10-11. The mapping of the control policy onto the controller is
correct; build 7a → 7b → 7c-1..3 as proposed. Answers to the four
questions, one correction, and two notes.

## Q1 — serial bandwidth while paired: send CNTR only when its integer changes

Alternating LPW1 and CNTR every 200 ms is acceptable, but there is a
better rule that falls out of the pairing: CNTR needs a command only when
its *paired integer value changes*. The VP gain already bridges anything
smaller than one CNTR step (≈4.9% light), so between integer crossings
there is nothing for CNTR to say. Within a scene the floor moves slowly,
so CNTR crossings are rare and the LED keeps nearly all the bandwidth
(≈5 updates/s). Implement: LED on every slot; CNTR takes a slot only on an
integer crossing (or on a cut dump, which always goes first). Log the
ratio of LED to CNTR commands so the owner can see it.

## Q2 — the LED model before measurement: ship with defaults, but bias them fast

Ship with defaults and the §6 camera measurement in the tally, yes. One
correction to the stated error direction. The recovery gain is
`ramp_target / modelled_LED_light`, so:

- if the real LED is **faster** than the model, modelled < real and the
  displayed brightness is `ramp × real/modelled` — **brighter** than the
  ramp, i.e. the LED's step shows through, which is the thing the
  gain-shaped recovery exists to prevent;
- if the real LED is **slower**, displayed is darker than the ramp — a
  slower fade-up, harmless.

So the safe side is a model that is *at least as fast* as the real LED.
Until measured, set the defaults fast-biased: `led_settle_ms: 300` (not
600), `led_latency_ms: 80`. If the camera shows the LED is slower, the
only effect of the bias is a slightly slower fade-up until the numbers
are entered. Record this in the policy's §6.

## Q3 — script mode and the port: synchronous, like the script, not queued

"No coalescing in script mode" is right; "queue behind a slow port" is
not. The script never queued — it sent, then slept, so the port was idle
when the next send came and the schedule simply stretched if a send took
longer. Script mode should do the same: each send in the schedule blocks
until the port has taken it (and the reply, if read-back is on), then the
schedule's offset counts from that moment. Byte-identical on the wire and
time-faithful to within the port's own speed, with no queue to drain.

## Q4 — order: confirmed

7a, 7b, 7c-1, 7c-2, 7c-3, each audited, then one deploy with everything
before the first projector test. The owner wants the LED drive in first.

## Notes

- The three "inherent differences" in script mode (APL crop, highlight
  metric, pacing) are correctly documented, not fixed. The highlight
  difference is the one to watch in the side-by-side: the brightest 3×3
  block is slightly less sensitive than the brightest pixel of a
  downscale, so script mode may trigger its dump a little later on tiny
  speculars. If the owner notices it, use the block maximum from the
  measurement's 4×4 level instead (closer to the script's 854×480 scale).
- rc3: with `peak_detection: off` and the sliders on Auto the curve is
  libplacebo's default, so the owner's profile is unchanged by the merge;
  the updater stays out of the deploy. Both as they should be.
- Stage 6 complete with the NLS-mapped mask: the dither refinement is
  now on for the owner's main profile. Good.
