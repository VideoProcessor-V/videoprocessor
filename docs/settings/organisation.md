# Settings organisation (first pass, 2026-10-24)

A one-page map of where every setting lives, to read alongside the settings preview
(`D:\codex\vp-settings-preview\Open settings preview.bat`). Nothing was removed: every control
still exists exactly once. VP's own settings moved off the Rendering page onto the VP-V page;
Bill's stay on his pages.

**How a VP-V unit tab reads, top to bottom:** what the unit does, its tier slider (Classic / Low /
Mid / Upper), small labelled cards with the switches you might use, a collapsed **Advanced** card
for measured internals ("leave alone"), then the planned (not yet built) settings and the unit's
stages (both collapsed). The VP-V page and the Rendering page always edit the same renderer profile
(choosing a profile on one selects it on the other).

## VP-V page (sidebar: VP-V)

Top: Pathway (VP-V / Original), Profile, Global tier, Fallbacks engaged.

| Tab | Cards (Advanced = collapsed) | What is in them |
|---|---|---|
| **Input** | Capture | capture unpack on the GPU, upload as frames arrive, native RGB, DV tunnel GPU unpack, DV trims |
| | Signal and content | source range, content labels (+ how long a label holds) |
| | Total delay | the one delay value and what it means at each frame rate |
| **Motion** (tier) | Motion engine and hiccup repair | fill dropped frames, interpolation (50/60p), hold or blend, estimate ahead |
| | De-judder (your motion slider) | the motion slider and its fields, motion vectors from (engine / matcher) |
| | Render ahead (under test) | render ahead, its utilisation |
| | Per-shot easing | ease per shot, policy, minimum, shortest shot, ease time |
| | Advanced: motion estimator | the ~30 estimator internals, de-judder gate |
| **Repair** (tier) | Deband | grain guard, VP replaces libplacebo's, reacts to compression |
| | Texture restore and detail pulse | their limits (caps) |
| **Scaling** (tier) | Re-base (your picture slider) | the picture slider (how much evidence), kernel match, subtitle and mixed-screen guards |
| | Grain-aware scaling | grain-true downscale filter, grain map, gentler on poor streams, SSIM / CNN steadiness, grain-aware upscale |
| | Advanced: scaler thresholds and re-base timing | adaptive downscale / upscale weights and thresholds, re-base engage wait, crossfade, fade back |
| **Colour** | Range and precision | keep below-black / above-white, 32-bit float processing |
| | SDR tone shape | the knee switch and its four values |
| | Room-light adaptation (experimental) | on/off, strength, screen reflectance, black limit, rise/fall times |
| | Tone mapping and display (Bill's pages) | where tone mapping, gamut, output gamma and the LUT are; the input colour-space note |
| **Dynamic Drive** | Dynamic Drive (its own page) | a map of the Dynamic Drive page's cards |
| **Output** (tier) | Temporal dither | no smear where new picture appears, grain tolerance (the dither type is the tier) |
| | Presentation and delay | where bit depth, libplacebo dither and the delay are |
| **Pipeline (expert)** | - | calibration LUT stage, module order, stages switched off |
| **Proposed retirement** | VP-V switches proposed for retirement | dither reuses motion, detail pulse fast path |
| | Controls of the candidates (collapsed) | the 14 "fixes" switches, the lost experiments (cost trust, interpolation grain, warp filter, NLS sampling, deband grain, the 4 de-judder experiments), render-ahead queue credit |
| | (list) | all 41 candidates with where each control is and this profile's value |

## Bill's pages (unchanged, except where noted)

| Page | Cards |
|---|---|
| General | General behavior, Display, Hardware, Timing, **Input processing** (now with a note: set these four here once), Renderer overrides, Room lights, File playback |
| Queue | Profiles, Profile details (+ Advanced) |
| DV | Profiles, Profile details |
| VP Renderer > **Rendering** | Profile (name, shortcut, rule, quality), Tone Mapping: General / Spline Curve / Other Curves / Peak Detection / Highlight Response (Beta), Processing (deband strength, libplacebo dither, bit depth), a line pointing to VP-V, then **All raw settings (expert)**, collapsed |
| VP Renderer > Color / Output, Scaling, Screen, Zoom, Subtitles, Motion | unchanged (Motion = smooth motion vs VP's de-judder, deinterlacing) |
| VP Renderer > Processing, DirectShow > Input Processing | the four input-policy overrides, now with a note: leave on Inherited unless this renderer must differ |
| Dynamic Drive | Projector connection, Dynamic Drive, LED profile, LED speeds, Contrast and cuts, Projector response, Advanced LED, Advanced projector, Experimental (unchanged; already carded) |
| Calibration, Shaders, Actions, Shortcuts, Logs, Updates | unchanged |

**All raw settings (expert)** (Rendering page, collapsed) holds what is left of VP's there: the
controls VP-V manages (greyed "Managed by VP-V": capture path, source range, the VP deband and
texture switches, ...), the content-adaptive diagnostics (black protection, display mapping
readout), and the switches still under test that VP-V shows elsewhere. Switching VP-V to the
Original pathway un-greys them.

## Rules used

- One live control per key. A VP-V tier or switch owns its key; the old control is greyed
  "Managed by VP-V" (on the Rendering page) or hidden (where it sits inside a VP-V card).
- VP's own rows were **moved**, not copied; Bill's sections were not moved.
- Retirement candidates appear only on the Proposed retirement tab, except: the three a tier or
  switch manages (motion look-ahead frames, dither reuses motion, detail pulse fast path: their old
  controls are greyed/hidden), the two estimator diagnostics (inside Motion > Advanced), and the
  [general], [led_drive] and [queue] candidates, which stay on their own pages (General, Dynamic
  Drive, Queue), marked in their tooltips.
- Input colour space / HDR luminance / video conversion: General is the one place; the two
  renderer pages keep their overrides (Bill's design), now labelled as overrides.

## Still awkward (for your review)

- The Motion tier and the motion slider both touch motion; the tier picks the features, the slider
  is your taste. Likewise the Scaling tier and the picture slider (the slider is only how eager the
  re-base is).
- `sdr_input_transfer` exists in both Rendering and Color / Output profiles (Bill's; not changed).
- The Dynamic Drive page's Advanced LED card is large (56 controls) and holds the 9 LED
  retirement candidates; it was left as it is.
- The Original pathway disables only VP-V's own tiers and switches; the moved rows stay usable.
- In All raw settings, a few managed keys are edited through a tick box or buttons beside a hidden
  drop-down (content labels, per-shot easing, room light); only the drop-down is greyed, so those
  boxes still work there (as before this pass). The VP-V switch is the one to use.
- The VP-V page is wider than the default window (its tab row); widen the window or scroll.

Source: `src/VP-V/config/VpvOrganisation.h` (the table), `src/VP-V/config/VpvSection.cpp` (the
cards, the moves, the raw group), one lookup change in `ConfigEditorWindow.cpp` (commented).
