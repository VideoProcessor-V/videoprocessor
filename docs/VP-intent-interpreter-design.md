# Anticipated intent interpreter (DV2 analogs from DV1 + measurement) - design, 2026-10-17

Owner idea: DV2 content falls back to DV1 tunnelling, so VP can approximate DV2's functions from DV1
metadata plus its own scene analysis. Rules: faithful, no AI, taste never fitted, switches default
off, BTB/WTW and grain kept. Not built.

## Summary

The DV1 tunnel carries picture facts only:

| Level | Content |
|---|---|
| L1 | Scene min / avg / max |
| L2 / L8 | Trims for other displays |
| L5 | Active area (maybe) |
| L6 | MaxCLL / MaxFALL (maybe) |
| L11 | Content type (maybe) |

Nothing in it says how motion should be handled. So the interpreter measures each shot and labels
every value as inferred or from metadata. Those values only steer controls the owner already sets,
within limits the owner sets. It invents nothing, and pixels change only through existing stages.

**Ranking** (owner mix: 24p film, 30p documentaries, 50/60 sport; living rooms count too):

| Rank | Function | Status |
|---|---|---|
| 1 | Content intent: parse L11/L5/L6, rule-based `content` variable for when: rules | Small, pays off at once |
| 2 | Motion intent | Display first, modulation after judging |
| 3 | Ambient adaptation | Off in the FL35 room; biggest visible gain for living rooms |
| 4 | Black intent | Report now; active part waits for the VP curve |
| 5 | Display intent | Mostly exists via dv_trims; report only |

## Shared core: IntentDescriptor

- **One descriptor per shot**, keyed by the source sequence of the shot's first frame.
- **Each field** is {value, source in Off/Inferred/DvL1/DvL11/Dv2, valid}, following the
  SceneDescriptor.h source-tag pattern.
- **Producer:** ShotIntentObserver, a QueuedFrameStatistics::IObserver (the LedLookahead pattern).
  It sees queued frames before display, so it knows cuts ahead.
- **Shot boundaries** come from SceneDetector (hard cut confirmed, frames back, near-black entry).
- **New parsing** in CR210DolbyVisionTunnelFormatter.cpp:261-300:
  - L5 lets the measurement skip the bars;
  - L6 fills maxCll / maxFall;
  - L11 gives content type, reference mode and frame-rate-conversion hints (libdovi's public
    layout; verify against the live "title DM extension levels" log).
- **Cost:** CPU only, about 0.1-0.3 ms per frame on luma reduced to about 192x108. Nothing on the
  GPU.

## 1. Motion intent (Authentic Motion analog)

**Measurements:**
- pan speed v, in picture heights per second, from projection correlation (heights, so the
  same pan reads the same on any aspect ratio);
- steadiness (spread of v within the shot);
- edge contrast;
- shot length and class;
- the L11 frame-rate-conversion hint, when present.

**Algorithm:**
- Visibility J = band(v) x edge term. band() is zero for nearly static shots, peaks at moderate
  pans and falls where motion blur dominates.
- Shot scale = min + (1 - min) x smoothstep(J_lo, J_hi, J).
- J_lo and J_hi are fixed, documented, owner-editable constants. Nothing is fitted from data.
- dejudder_strength stays the master; the scale only multiplies it. It never raises strength.

**Timing:**
- Applied once per shot, at the cut. The GPU ramp already restarts there (DejudderWarp.h:100-109).
- If the look-ahead missed the cut, the value waits for the next cut. It never changes mid-shot.

**Hysteresis:**
- dead band 0.1;
- shots shorter than `intent_motion_min_shot_s` inherit the previous scale;
- fades keep the previous value.

**Output:** multiplies dejudderEffectiveStrength (LibplaceboVideoRenderer.cpp ~13156 at HEAD).

**Owner decision needed:** R-A (`dejudder_motion_rolloff`) already reduces strength on fast pans.
Pick one policy before turning on modulation.

**Switches:** `intent_motion = off | display | modulate` (default off), `intent_motion_min = 1.0`
(1.0 = no change). Phase A is display only (OSD and log).

**Effort:** M.

## 2. Black intent (Precision Black analog)

**Inputs:** L1 min, the measured shadow p1/p5/p10, and DisplayProfile black (the meter-measured FL35
black once a profile carries it).

**Report:** how many shadow steps between L1 min and p10 fall below display black + one 8-bit step.

**Never feed L1 min into libplacebo min_luma.** That maps the scene floor to black and crushes it.

**VP-owned curve (later):** slope ≥ 1 below a shadow knee tied to L1 avg, compressing highlights
instead.

**LED:** a shadow-protect cap on dimming. Dimmed black must not push the scene's p5 below one 8-bit
code after compensation.

**Guards:** code 0, BTB and the floor never move.

**Switch:** `intent_black = off | report | led_protect`.

**Effort:** report S, LED M, curve L.

## 3. Display intent (one-way mapping)

- Compression need = PQ(L1 max) - PQ(display peak).
- Trim choice reads DisplayProfile.peakNits.
- Never expand above the master (that would invent highlights).
- Output: a DV-card line, e.g. "scene 1000 nit -> 48 nit (-4.4 stops), trim L2 100 nit".
- Switch: `intent_display = off | report`.
- Effort: S.

## 4. Ambient adaptation (Light Sense analog, living rooms)

**Inputs:**
- AmbientSensor lux, with a new continuous SmoothedLux() (today only a dark/lit state);
- the display profile;
- owner calibration `ambient_screen_reflect`: cd/m2 per lux, measured once lights on vs off.

**Smoothing:**
- median of 5 readings (rejects flicker and people walking past);
- asymmetric EMA in log-lux: up tau 15 s, down tau 90 s;
- quantised to about 1/6-stop steps of effective black;
- steps applied at cuts (or a slew below 1%/s on static pictures);
- on sensor loss, hold, then decay toward off.

**Model:** veil V = lux x reflect; effective black B_eff = B + strength x V.

**Adaptation:**
- render to B_eff through the BT.1886 black point (ResolveTargetBlack);
- optional BT.2390 surround gamma between owner-set dark and lit endpoints.

**LED:** dimming floor raised to B_eff. Dimming below the veil cannot be seen. Peak drive never rises
automatically.

**Switches:** `ambient_adapt` (off), `ambient_adapt_strength` 0-1 (0), `ambient_max_black_nits`,
`ambient_tau_up_s`, `ambient_tau_down_s`.

**Guards:**
- strength 0 is bit-identical;
- output is monotonic;
- BTB/WTW order is kept;
- nothing changes above the midtones;
- every step is logged and shown on the OSD.

**Cost:** about 1 Hz on the CPU. Each step costs a one-off libplacebo rebuild, which is why steps
happen at cuts.

**Effort:** M.

## 5. Content intent (Content Intelligence analog)

**Inputs:** pulldown lock and film Hz, source_class, motion statistics, twos-holds, static UI, and
L11 content type (which wins when present).

**Rules:**

| Signal | Class |
|---|---|
| measured video-class source with sustained large pans | sports |
| pulldown or film-like rate | film |
| twos-holds plus flat areas | animation |
| long zero motion | static UI |
| otherwise | video |

The class changes only after N seconds and at a cut.

**Output:** a `content` variable for when: rules. It only selects owner profiles.

**Switch:** `intent_content`.

**Effort:** S-M.

## 6. Readiness for real DV2

- **Precedence:** Dv2 > DvL11/DvL1 > Inferred > owner default.
- **Inferred values are always logged alongside**, e.g. "motion scale dv2=0.40 inferred=0.55". That
  also validates the heuristics.
- **DV card:** one row per field showing value, source and confidence.
- **Needs** a DV2 EDID behind the Vertex2 and a decodable DV2 tunnel. Until then this is groundwork.

## Tests

- CPU synthetic: pans, cuts, fades, flashes, filter step responses, strength-0 identity, BTB/WTW
  order.
- Recorded-stream fixtures from StreamRecorder: 24p film, 30p documentaries, sport, telecine,
  animation.
- Projector: meter readings vs predicted B_eff.

## Owner decision 2026-10-17

Build all five. Config: one group headed "Experimental content-adaptive settings" with a checkbox per function (content recognition, motion intent, ambient adaptation, black protection, display mapping), each off by default, with its sub-settings (strength, ranges, taus) beneath its checkbox. The motion-intent policy question (vs dejudder_motion_rolloff) is decided after the display-only phase.

## Owner decision 2026-10-17: motion intent as selectable policies

Instead of one policy, motion intent offers several, chosen by the owner (exclusive buttons in Config, under the motion-intent checkbox):
- **Display only** - per-shot analysis shown on the OSD/log, strength unchanged (the default when the checkbox is ticked).
- **Ease on fast pans** - reduce strength as pan speed rises (today's dejudder_motion_rolloff behaviour, moved here as one policy).
- **Keep fast pans, ease on slow deliberate shots** - full strength where judder is most visible; ease where the shot is slow, steady and the film look is likely intended.
- **Visibility-weighted** - scale by the judder-visibility estimate J (pan speed band x edge contrast), easing on static and on blur-dominated shots.
- **Metadata only** - act only on L11 frame-rate-conversion hints or real DV2 Authentic Motion when present; otherwise unchanged.

All share the rules above: owner strength is the master and is never exceeded, changes only at cuts, dead band + short-shot inherit, video-class excluded, owner-set minimum (intent_motion_min). Key: intent_motion_policy = display | ease_fast | ease_slow | visibility | metadata. Each policy is logged with the inferred values so they can be compared on the same film.

## Status 2026-10-17 (intent remainder)

Items 2-5 are built behind the same group in Config, "Experimental content-adaptive settings" (renderer keys, default profile and every `[vprenderer.<name>]` profile; validated in `RendererProfileConfig.h`). Every function is a checkbox, off by default, with its sub-settings beneath it (greyed while off); every control has a tooltip naming its key. Content recognition (`intent_content`, `intent_content_hold_seconds`) is item 1.

| Function | Control | Key | Values | Default |
|---|---|---|---|---|
| Motion intent | checkbox | `intent_motion` | off, on | off |
| | exclusive buttons: Display only / Ease on fast pans / Keep fast pans, ease on slow deliberate shots / Visibility-weighted / Metadata only | `intent_motion_policy` | display, ease_fast, ease_slow, visibility, metadata | display |
| | Minimum strength scale | `intent_motion_min` | 0.0 - 1.0 | 0.5 |
| | Shortest shot that changes the scale (s) | `intent_motion_min_shot_s` | 0.5 - 30 | 2.0 |
| Ambient adaptation | checkbox | `ambient_adapt` | off, on | off |
| | Strength | `ambient_adapt_strength` | 0.0 - 1.0 | 1.0 |
| | Screen reflectance (cd/m2 per lux) | `ambient_screen_reflect` | 0.0 - 1.0 | 0.01 |
| | Maximum effective black (nits) | `ambient_max_black_nits` | 0.0 - 10.0 | 1.0 |
| | Brighten time constant (s) | `ambient_tau_up_s` | 1 - 600 | 15 |
| | Darken time constant (s) | `ambient_tau_down_s` | 1 - 1800 | 90 |
| Black protection | checkbox (ticking selects Report only) + exclusive choice Report only / Protect shadows from LED dimming | `intent_black` | off, report, led_protect | off |
| Display mapping readout | checkbox | `intent_display` | off, report | off |

**Rationale for the two defaults that differ from the phase-A text above.** With a per-function checkbox as the off switch, `intent_motion_min` 1.0 (or strength 0) would make a ticked motion intent inert, so the default is 0.5: a ticked function does something visible, yet never more than halves the owner's de-judder strength for a shot. Likewise `ambient_adapt_strength` 1.0: the checkbox is the off switch, and a ticked function renders the full room-light veil into the black. Strength 0 still must be bit-identical (tested by the core).

**Rules carried by the controls.** Motion intent never raises `dejudder_strength`; it only scales it down (scale in [`intent_motion_min`, 1]), at cuts. Ambient adaptation uses the `[ambient]` light sensor (manual mode: the lit room's `lux_lit`) and is meant for rooms that are not dark theatres. Black protection never moves code 0 or BTB/WTW. Motion constants are documented constants in code, not keys.

**User-facing docs.** `CONFIGURATION.html` does not list renderer-profile keys (the optical-flow, de-judder and `intent_content` keys are not there either), so no entries were added; the Config tooltips are the user-facing reference for these keys.
