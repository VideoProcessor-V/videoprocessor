# Settings organisation (pseudo-final, 2026-10-24)

A one-page map of where every setting lives and **which control sets which**, to read alongside the
settings preview (`D:\codex\vp-settings-preview\Open settings preview.bat`). Nothing was removed:
every control still exists exactly once.

## The idea in four rules

1. **One master per group.** Where several settings move together (a tier, a slider, a feature's
   on/off, a set of fixes), the master is on the unit's **Main** page and the settings it sets are on
   **Advanced**, greyed with "Set by <master>". Tick **Custom** on that card to change them by hand;
   untick it and the master's values come back. A feature's tuning is greyed "Used only when
   <feature> is on" while the feature is off.
2. **One live control per key.** A VP-V switch owns its key; the old duplicate control elsewhere is
   hidden in both pathways. In the **Original pathway** every VP addition is off, so the whole VP-V
   page is disabled (with a note saying so); Bill's own settings stay live on his pages. None of
   Bill's keys is on the VP-V page (checked against billslack2 v1.4.02-beta).
3. **One slider per unit.** The tier is the unit's control. Where a unit also has a taste slider
   (Motion: the de-judder amount; Scaling: the picture slider), the tier decides whether it applies:
   at the Classic Motion tier the amount is greyed (no de-judder); below the Mid Scaling tier the
   picture slider is greyed (re-base off) and it never offers its own "Off".
4. **Same order everywhere.** Main: summary, tier, taste master, switch cards, where the rest is.
   Advanced: set by the tier, set by the slider, feature tuning, measured internals (collapsed),
   planned (collapsed), stages (collapsed).

## Page tree

**VP-V** (sidebar): Pathway, Profile (the same one the Rendering page shows), Global tier,
Fallbacks engaged; then one tab per unit, each with **Main | Advanced**.

| Tab | Main | Advanced |
|---|---|---|
| Input | Capture; Signal and content; Total delay | Content labels: tuning; Readouts (diagnostics) |
| Motion | **tier**; De-judder amount (your taste); Motion engine and hiccup repair; Render ahead; Per-shot easing | Set by the Motion tier; Set by the de-judder amount (the 8 slider fields); De-judder shaping (yours: fade matching, pulldown, vector options); Render ahead: tuning; Per-shot easing: tuning; Motion estimator (measured) |
| Repair | **tier**; Deband | Set by the Repair tier; Texture restore and detail pulse: limits |
| Scaling | **tier**; Re-base (your picture slider); Grain-aware scaling | Set by the Scaling tier; Re-base evidence; Re-base timing; Grain-aware scaler thresholds (measured) |
| Colour | Range and precision; SDR tone shape; Room-light adaptation; Tone mapping and display (Bill's pages) | SDR tone shape: values; Room-light adaptation: values |
| Dynamic Drive | where everything is on the Dynamic Drive page (below) | - |
| Output | **tier**; Temporal dither; Presentation and delay | Set by the Output tier; Temporal dither: tuning |
| Pipeline (expert) | LUT stage, module order, stages off | |
| Proposed retirement | VP-V switches proposed for retirement; Proven fixes (master: all on); Experiments that lost; the list of all 41 | |

**Bill's pages:** General (Input processing now says "set these four here once"), Queue, DV,
Shaders, Actions, Shortcuts, Calibration, Logs, Updates, DirectShow - unchanged. **VP Renderer >
Rendering**: profile settings, Tone Mapping (General / Spline / Other curves / Peak detection /
Highlight response), Processing, and a line pointing to VP-V. The "All raw settings (expert)" group
appears only while it holds a shown control; after this pass it holds none (every VP control has its
home on VP-V), so it is not shown.

**Dynamic Drive** (ours, its own page) now uses the same scheme, **Main | Advanced**:
- Main: Dynamic Drive (on/off, the command source - your script, VP's compensate or none - adaptive
  gamma, look-ahead, floors), LED profile, LED speeds (the preset), Projector connection.
- Advanced: LED speeds: values (set by the preset until Custom); Contrast and cuts; Advanced LED (its
  two masters); Light curve, steps and ramps; Highlights; Cuts and fades; Script mode only; Compensate
  mode only (each its own small card, set by "Advanced LED values" until Custom); Projector response;
  Advanced projector; Experimental. Cards the chosen source does not use stay hidden, as before.

## Master -> derived groups

| Master (where) | Sets (where) | Rule |
|---|---|---|
| Motion tier (Motion > Main) | optical_flow, motion_ahead_frames, dejudder_est_split (Advanced > Set by the Motion tier); Classic also sets the de-judder amount to none | greyed until Custom; untick = back to the tier |
| De-judder amount slider (Motion > Main) | dejudder_strength, _strength_even, _even_rates, _object_strength, _motion_rolloff, _roll_floor/_lo/_hi (Advanced > De-judder fields) | greyed until Custom (ticked by itself when the slider reads Custom); untick = the slider's Reset |
| Render ahead (Motion > Main) | dejudder_gate, render_ahead_utilisation | used only while on |
| Per-shot easing (Motion > Main) | intent_motion_policy, _min, _min_shot_s, intent_ease_s | used only while on |
| Repair tier | deband_vp, detail_pulse_stabilise, texture_restore | greyed until Custom |
| Detail pulse / texture restore (set by the tier) | detail_pulse_cap, texture_restore_cap | used only while their stage is on |
| Scaling tier | source_resolution, adaptive_downscale, adaptive_w_base | greyed until Custom |
| Picture slider (Scaling > Main) | source_resolution_auto_min_confidence, _evidence_seconds, _evidence_shots | greyed until Custom; the slider itself greyed below Mid |
| Re-base automatic (set by the tier) | engage wait, crossfade, fade back | used only while re-base is automatic |
| Output tier | temporal_dither | greyed until Custom |
| SDR tone shape (Colour > Main) | sdr_white_headroom, sdr_knee_start, _knee_strength, sdr_contrast_recovery | used only while on |
| Room-light adaptation (Colour > Main) | ambient_adapt_strength, _screen_reflect, _max_black_nits, _tau_up_s, _tau_down_s | used only while on |
| Content labels (Input > Main) | intent_content_hold_seconds | used only while on |
| Proven fixes: all on (Retirement tab) | the 14 renderer fix switches | greyed until Custom; untick = all back to their default (on) |
| Dynamic Drive command source (Dynamic Drive page) | which cards and rows show (Contrast and cuts, LED speeds, Experimental, LED profile, pacing rows, ...) | unchanged show/hide |
| Advanced LED values (Advanced LED card) | 5 groups: Light curve, steps and ramps; Highlights; Cuts and fades; Script mode only; Compensate mode only (28 keys) | greyed at your script's values until Custom; choosing the script's values again resets every field; a group the chosen source does not use stays greyed |
| LED fixes under test (Advanced LED card) | the 9 LED fix switches (in Dynamic Drive, Contrast and cuts, Experimental) | greyed on until Custom; back to all on |
| LED speeds preset (Dynamic Drive > Main) | rate_gain/min/max up and down, recovery rates (Advanced > LED speeds: values) | greyed until Custom; unticking puts every field back to "preset" |
| Queue policy (Queue page) | queue_size, lead_frames, target_frames, reset % | unchanged (Advanced shows the values) |
| Total delay (Queue page target delay) | frames and padding per frame rate | shown on Input > Total delay |

## Still not final (for your review)

- The VP-V and Dynamic Drive pages fit the default window (a test checks it with the Windows fonts);
  with large text scaling they may still need scrolling.
- `sdr_input_transfer` exists in both Rendering and Color / Output profiles (Bill's; left alone).
- The LED fix switches stay in their cards on the Dynamic Drive page (greyed by their master in the
  Advanced LED card) rather than moving to the Proposed retirement tab, so their context stays.

Source: `src/VP-V/config/VpvOrganisation.h` (the table: cards, masters, locks, LED groups),
`src/VP-V/config/VpvSection.cpp` (cards, moves, locks, the raw group, the Dynamic Drive regrouping,
the fit), one lookup change in `ConfigEditorWindow.cpp` (commented).
