# LED compensate review: look-ahead, exit from black, step sizes, low drive (2026-10-17)

Analysis agent, read-only, with simulations. None of the figures below is measured on the projector:
LED latency 80 ms, settle 300 ms and gap 200 ms are placeholders.

## Summary for the owner

- **Look-ahead can be off.** The LED look-ahead runs only when the queue profile sets
  `active_picture_lookahead_frames` above 0. The shipped sample leaves it commented out, so the depth
  is 0. The cinema test config sets 8 (about 167 ms at 24p). The Low_Latency profile sets 0, so 60p
  is always reactive. Even at full depth the controller uses only the frame at +latency and the
  furthest frame.
- **Exit from black lands short.** CNTR is sent first. The cut's LED jump then waits one 200 ms gap
  and lands about 113 ms after the cut. The cut frame shows about 44% of its correct brightness and
  reaches 95% after about 245 ms. `black_prelift_max` never takes effect: the cut jump replaces it
  before it is sent.
- **Fade-in step.** CNTR 59 is held on a visible dark picture, then released 59 to 50 in one command
  (light x0.67). The release is invisible only if the gain lands on the right frame.
- **Mid-range is fine.** Where the curve leaves 255, the script moves about 0.4% of light per
  command, and compensate hides the step on the picture. The visible steps are elsewhere:
  - script: CNTR (4.9% per unit), the +7/-5 LPW1 steps;
  - compensate: the black floor near the LED floor (3.4% per command).
- **Deep dimming.** The picture is fully recovered down to the effective floor of LPW1 102 (x2.5
  linear gain), and precision on the 8-bit link improves. The limits:
  - small highlights (under 0.5% of the area) are compressed by up to 1.3 stops;
  - gamma and flux model error, not yet measured.
- **Biggest unknown: the LED response.** If the LED is fast, the 300 ms settle placeholder makes the
  picture overshoot (up to x1.9 during the fade-up after a cut from black) and run about 5% too
  bright on floor ramps.

## Compensate code

- **Gain timing.** Latency is rounded to whole frames (at most 3 ms error). Larger errors:
  - each send is credited to the next BeginFrame, adding 0-42 ms at 24p;
  - render-to-light latency (max_frame_latency 2, plus projector input lag) is not modelled.

  Net effect: the gain is probably 60-100 ms late against the LED, so each command's 1.4-3.4% step
  shows for 1-2 frames. Fix: timestamp sends in ms. Measure led_latency_ms as the LED-vs-picture
  offset.
- **Settle model.** A tau mismatch gives about rate x tau, roughly 5% lag error on floor ramps.
  CutRecovery limits against the model, so a fast real LED overshoots up to gain ceiling x CNTR light
  = about 1.88.
- **Gamma.** compensation_eotf 2.41 against a real 2.2 makes the picture 7.7% too dark at LED 102.
- **Pairing.** Update uses the LED model at the current time while CNTR lands 80 ms later, so a
  descending CNTR sits briefly above need. Using ledModel.Predict(cntrLatency) fixes it. The headroom
  is overstated while protection ramps up after a cut.
- **Docs.** The policy doc's "codes 236-255" claim does not match the shader. The knee tends to 1.0
  (WTW now passes untouched since the 2026-10-17 fix).

## Look-ahead

Depth = min(active_picture_lookahead_frames <= 8, frames queued). Default 0 (VideoProcessorApp.h:62);
cap 8 (ActivePictureDecisionTimeline.h:137).

Planned jump for a cut from LPW1 102 to 255 (simulated):

| Jump timing | Cut-frame brightness | Time to 95% |
|---|---|---|
| Today, 83 ms ahead | 0.51 | 264 ms |
| When the cut enters the window (167 ms) | 0.70 | 180 ms |
| 333 ms ahead | 0.94 | 14 ms |

Dark cuts need no extra depth: the gain absorbs the LED fall.

## Exit from black (compensate)

Today: preposition at first sight sets LED 120 and CNTR 59. CNTR takes the port slot first, and the
LPW1 is replaced by the cut jump, which lands at +113 ms. Cut frame 0.44, 95% at 244 ms.

| Variant | Cut frame | Time to 95% |
|---|---|---|
| LED first, pre-lift 255 | 0.54 | 113 ms |
| 8 frames ahead, CNTR first | 0.68 | 78 ms |
| Reactive 60p | 0.30 | 420-620 ms |

Problems:
- (a) prelift is never sent;
- (b) the hold is not black-only (fade-in release step);
- (c) the 59 to 50 descent is 9 steps of 4-5%, each masked only if the timing is right;
- (d) overshoot if the LED is fast;
- (e) custom dim: three commands a gap apart, so the cut frame is about 0.30.

Script mode (the owner's script, flagged only): the cut frame has correct midtones but clips above
code ~164 until the fast dump. The LED reaches 255 after 26.5 s.

## Step sizes (about 1-2% visibility)

Script LPW1, light per command:

| LPW1 | 1 level | 5 levels | 7 levels |
|---|---|---|---|
| 255 | 0.4% | 2.0% | 2.7% |
| 200 | 0.5% | 2.5% | 3.5% |
| 100 | 1.0% | 5.0% | 7.0% |

CNTR is 4.9% per unit at 50. The 50/51 highlight toggle and the APL 30-45 slope exceed the threshold.
These are the owner's tables: flagged, not changed.

Compensate floor per command (gap 200 ms):

| LPW1 | Up | Down |
|---|---|---|
| 102 | 3.4% | 2.5% |
| 255 | 1.4% | 1.0% |

Steps exceed 2% going up below LPW1 175 and going down below 125. Fix: rate_domain flux +
max_floor_step_pct 1.5.

## Ranked actions

### Before deploy

1. Look-ahead depth: warn when the depth is 0 in compensate, plus a separate `led_lookahead_frames`.
2. `preposition_black_only`: hold CNTR and pre-lift only on true black.
3. `led_landing_ms`: land each send at send time + latency, in ms.
4. `cut_lead: window`: jump when the cut enters the window, with a port reserve.
5. Docs: prelift does nothing at short depth; correct the codes 236-255 claim.

### After deploy, needs measurement

1. Measure dead time, tau, internal ramp, the LED-vs-picture offset, flux_table and gamma.
2. Flux-domain rate limit.
3. Small-highlight tolerance.
4. Deeper LED look-ahead (about 380 ms).
5. LED-first preposition.
6. Frame-phase sends.
7. Pairing predict.
8. Script: script_smoothing_ms, plus script_light_smoothing (docs/VP-led-light-smoothing-plan.md).
