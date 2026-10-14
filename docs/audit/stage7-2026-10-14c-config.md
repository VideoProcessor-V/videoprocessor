# Audit add-on — Config ease of use (stage7-rc2 at 8ec8b52d, owner 2026-10-14)

Scope: every Config control added or changed since 9002d72c (`git log 9002d72c..HEAD -- src/VideoProcessor-Config`),
plus a re-check of the LED page against stage7-2026-10-14b.md. It is judged as a non-developer setting up a projector
and a picture.

## How this review was done
- It is a code and UI review only. Nothing was built, nothing touched the GPU, nothing was launched, and
  C:\dev\vp-investigate was not touched.
- The reading was split across three read-only reviewers. Every item marked **(confirmed)** I checked in the code
  myself.
- The Config tests (`x64\Release\VideoProcessorConfigTests.exe`, CPU only) **did not run**: Windows Application
  Control blocked the program. I did not rebuild (not allowed) or change any security setting. The build agent's
  next rebuild should give a binary that loads. Line numbers below are ConfigEditorWindow.cpp (CEW) unless another
  file is named.

## The short version
The new settings are all there and they all save correctly. Every key and every choice matches what VP reads, and
comments and unknown keys are kept. The problems are in what the owner **sees**:
1. **Unset settings show "Auto" instead of their real default (confirmed).** This applies to every new de-judder,
   capture and SDR setting.
2. **Nothing on the Shaders/Rendering cards is greyed when it does nothing (confirmed).** The de-judder card is one
   flat list of 46 rows.
3. **The LED page has two first-time traps (confirmed).**
   - Protocol shows "Barco serial" when VP actually uses "none", so nothing is sent.
   - A new file shows adaptive gamma as on, and the first pick of a source saves it on.

## Most valuable simplifications (build in this order)
1. **Show the real defaults (confirmed; affects every card).**
   - Today: for the `vprenderer` section, `fallback()` (CEW:7522-7543) returns `"AUTO"` for every key it does not
     list, and none of the new keys are listed.
     - Number fields therefore show the text "Auto" (CEW:7675-7689), never the placeholders 4.5, 0.25, 0.80 and so on.
       VP does not accept "auto" for these keys.
     - On/off and choice combos with no Auto item show "Default: Auto" or "Inherited: Auto" (CEW:7771-7774). This
       never says whether the default is On or Off.
     - Affected keys:
       - `capture_path` (really standard)
       - `sdr_tone_shape` (off), `sdr_white_headroom` (0), `sdr_knee_start` (0.80), `sdr_knee_strength` (0.5)
       - `adaptive_faithful` (polar), `upscale_light` (linear)
       - `dejudder_even_rates` (30), `dejudder_warp_stage` (rendered), `dejudder_low_res` (all)
       - all ~20 de-judder on/off switches (including the 2026-10-14 defaults: split off, fade match on, pixel
         trust on) and all de-judder numbers
   - Fix: add the runtime defaults (LibplaceboVideoRenderer.cpp:874-966, AdaptiveDownscale.h:51-63) to
     `fallback()`, or return empty for keys that have a placeholder.
   - Also add a test that an older file without the key shows the runtime default.
   - As things stand, the owner cannot tell what any de-judder switch is set to.
2. **LED: Protocol shows Barco while VP runs "none" (confirmed).**
   - With no `protocol` in the file, the combo falls back to its first item, "Barco serial (RS232)" (CEW:3380). VP's
     default is `none` (LedDriveConfig.h:64), which opens no port.
   - The shipped VideoProcessor.cfg has no `[projector]` or `[led_drive]` section, so this is every new install:
     - Compensate and VP's copy of the script send nothing.
     - The Python relay cannot open the port.
   - Re-picking Barco writes nothing (it is already shown). The owner would have to pick None, then Barco.
   - Fix: show the runtime default, or write `protocol: barco_ascii` when a driving source is picked.
3. **LED: adaptive gamma on a new file (confirmed).**
   - Today:
     - The box shows "on" whenever `command_source` is empty (CEW:9914-9916). That includes a file with no
       `[led_drive]` section at all, while the help says "Off (the default for now)" (CEW:9928).
     - The first source pick then writes `adaptive_gamma: true` (CEW:9977-9978), which also switches the script
       smoothing on (LedDriveConfig.h:954).
   - The runtime does the same for a section-less file (`adaptiveGamma = true` struct default, LedDriveConfig.h:229),
     so the display is accurate, but the result is the opposite of the intended "off by default with a source".
   - Fix: treat a file as "older" only when it names `mode`; otherwise start the box off.
4. **Split the de-judder card into three.** Today it is one flat 46-row list (CEW:6335-6661), described only by
   the cost legend.
   - **De-judder (open):**
     - preset / 24p strength, 30p strength + "applies to"
     - Keep fast motion's strobing + its 3 values
     - Smoothing of motion within the frame
     - Follow motion through fades + Fade floor
     - Mark these as **taste** (never fitted). "Keep fast motion's strobing" is row 20 today and "Smoothing of
       motion within the frame" is row 24.
   - **Motion estimation — measured, leave alone (collapsed):**
     - tolerance through matcher size
     - split + its threshold together (they are 17 rows apart today, CEW:6489/6602)
     - pixel decision + lo/hi
     - pixel check + share
     - estimate ahead
   - **Experiments (collapsed):** Fill uncovered areas, Taper + Taper ends, warp stage = Source.
5. **Grey what does nothing, and say why** (the de-judder, adaptive and SDR cards: no `setEnabled` anywhere in
   CEW:6276-6760).

   | Grey this | Unless |
   |---|---|
   | strength-shaping rows (motion/trust scale, roll-off, R-B, fade floor, feather/hysteresis/taper, warp stage, estimate ahead) | a de-judder strength > 0 |
   | Fall from / Down to the floor at / Floor | Keep fast motion's strobing |
   | Split above | Split poorly matched blocks |
   | Taper ends at | Taper |
   | Pixel decision lo/hi | Decide pixel by pixel |
   | Pixel check share | Check matches pixel by pixel |
   | Fade floor | Follow motion through fades |
   | Cut test | Hold only the unmatched areas |
   | 30p applies to | 30p strength > 0 |
   | Neighbour agreement weight | candidates on |
   | Skip the search on still scenes | global-motion prior |
   | Fill uncovered areas | Warp stage = Source (ignored with Rendered, the default) |
   | adaptive downscale fields / upscale weight, SuperRes, light / Faithful branch filter | their blend on |
   | SDR knee start, knee strength, SDR contrast recovery | SDR tone shape = Knee |
   | LED boost knee rows (CEW:10402-10412) | Gamma boost not Off |
   | LED Look-ahead | hide (not grey) unless VP's copy / Compensate |
6. **Put things where the owner will look for them (confirmed for capture and SDR).**
   - **"Capture conversion"** is inside the collapsed **"Tone Mapping: General"** section, after Saturation
     (CEW:6285, 6321). Move it to VP Renderer › Processing (next to `video_conversion`), or to a "Low latency" card.
   - **SDR tone shape and headroom** sit in Rendering › Processing between "Keep below-black" and "Dithering"
     (CEW:6871-6903). Give them their own section, "SDR highlights", next to Tone Mapping.
   - **The three dither (sigma-delta) rows** are inside the de-judder card (CEW:6404, 6637, 6645): occlusion mask,
     carry on grain, carry motion on extras. Give them a "Temporal dither" card.
   - `upscale_light` is on Rendering (CEW:6738) while `downscale_light` is on Scaling (CEW:7002). Put them together.
   - **LED:** move the "Fallback: the script writes the port itself" box and the COM port into the Python card.
     Move "Projector connection" up, after the LED drive card (it is 7th of 8 today).
7. **Make the low-latency rule buildable.**
   - Today the capture tooltip says "Meant for a low-latency profile chosen by a rule (e.g. sources above 59 Hz)".
     A Rendering profile only has the free-text "When should VP use this profile?" field, so the owner would have
     to hand-write `${cadence} > 59`.
   - The "Use this profile for sources above N fps" helper exists only on the Queue page (CEW:5216-5279).
   - Fix: add the same helper to Rendering profiles, or a one-click "Low-latency profile".
8. **Wording clean-up (exact text):**
   - Capture tooltip (CEW:6327-6329): remove "(owner 2026-10-14)" and "on a laptop RTX 3050 Ti". Say plainly when
     it does nothing: other formats, or a video-conversion override (the latter is my reading; unverified).
   - De-judder preset:
     - "Not set (off)" (CEW:6340) is wrong in a named profile: it inherits the default profile's strength. Use
       "Use default" / "Use inherited".
     - Its tooltip "Off below the uneven film cadences" (CEW:6348) is garbled. Suggested: "Only for 24p/25p film;
       off in the Low latency queue and for RGB or interlaced input".
   - Add tooltips to "Pixel decision: full from / held from (x tolerance)" (CEW:6554/6557) and "CNN weight on clean
     pictures" (CEW:6729). They have none.
   - Rename:
     - "Pixel check: share allowed off" → "Pixel check: hold above this share wrong"
     - "Down to the floor at" → "Floor reached at (source px/frame)"
     - "Dither carry motion on extras" → "Dither reuses the de-judder's motion"
     - "Faithful branch filter" → plain words
   - State the default in each on/off tooltip. Today only four do (CEW:6436, 6441, 6466, 6628). Split especially:
     it is now off by default, and the tooltip should say why.
   - Warp stage tooltip "not yet measured" (CEW:6416): give the measured figure or remove the remark.
   - SDR:
     - The knee choice shows raw lowercase "knee"; `friendlyChoiceLabel` has no entry. Suggested label: "Knee (soft
       highlight roll-off)".
     - "0 = today." (CEW:6883) → "0 = reference white at peak (off)".
     - Headroom "(0 to 0.5)" has no unit. Show it as % of peak.
     - "SDR sources only" (CEW:6875): VP also needs SDR output (LibplaceboVideoRenderer.cpp:16621).
     - The recovery tooltip's "VP's own: libplacebo's … Auto uses the contrast recovery value above" refers to a
       field in another collapsed section labelled for HDR. Say "Auto = follow Tone Mapping › Contrast recovery".
   - `chroma_upscaler` (CEW:6974-6985): remove "(Bill's VP)" and the "0.3 of 1023 codes" detail. It has no "Auto: …"
     preview, unlike its neighbours.
   - `fbo_float32` tooltip: remove "0.84 of a 10-bit step" and the GPU model. "Auto: off with the Low_Latency queue"
     depends on the queue being named exactly `low_latency` (LibplaceboVideoRenderer.cpp:18877). Renaming that queue
     changes Auto silently; say so, or key it off the queue's own setting.
   - "Low_Latency" with an underscore appears in user-visible tooltips (CEW:6349, 6709).
   - The internal codes (M1-M6, D1-D5, R-A/R-B, H1-H3, E1-E4, "delta N", "Stage 8b") appear only in code comments,
     never in labels or tooltips. **Good.**
9. **GPU cost markers.**
   - Show the legend once at the top of Rendering and of Scaling. Today it is the description of three sections,
     and markers also appear elsewhere with no legend: extended range, SDR shape/recovery, fbo_float32,
     chroma_upscaler.
   - Missing markers:
     - Warp stage = Source (the most expensive choice on the card)
     - Decide pixel by pixel, Check matches pixel by pixel, Two whole-picture motions, Motion search reach
     - Capture GPU
   - A "saves GPU time" note would help on matcher size, estimate ahead and dither carry motion.
10. **Ranges:**
    - Add a validator to `sdr_contrast_recovery` (auto / off / 0-2; CEW:6896). Bad text is only caught at Apply.
    - Warn when a lo ≥ hi pair is typed (pixel lo/hi, roll-off lo/hi, motion scale lo/hi). I found no guard in the
      runtime either (not verified to the end).

## LED page — re-check against 14b
**Resolved:**
- Source comes first (CEW:9879-9904).
- Compensate rows and cards are hidden for the other sources (CEW:10705-10714).
- There is a "Python script" card, shown only for python. The "Listen monitor (Mode: Listen)" title is gone.
- None + adaptive gamma is explained (CEW:9908-9909).
- The shadow-lift rows are greyed without adaptive gamma (CEW:10715-10719; tested).
- Older `mode:` files show as they behave: adaptive gamma on; `mode: listen` = fallback ticked.
- The first source change writes `command_source`, `mode`, `adaptive_gamma` and `python_drives_port`.
- Open then Cancel writes nothing.
- `lookahead` and `python_drives_port` are asserted in the test (ConfigEditorWindowTests.cpp:1082).

**Still open / new, after Simplifications 2 and 3 above:**
1. **Old script and listen files gain smoothing on a source change.**
   - The box shows on, and the first change writes `adaptive_gamma: true`, which sets `script_smoothing` on
     (LedDriveConfig.h:954).
   - In those files smoothing was separate and off by default (`script_smoothing` false, LedDriveConfig.h:278).
   - Fix: for older script and listen files, show the box as `script_smoothing` or shadow lift > 0.
2. **Re-picking the source already shown writes nothing.**
   - So the owner cannot convert an older file, and cannot "choose" None on a new file.
   - On an older listen file, unticking the fallback writes `python_drives_port: false`, which VP ignores without
     `command_source` (LedDriveConfig.h:945). Config then shows the box off while VP still leaves COM1 to the script.
   - Fix: write the source keys on `activated`, not only on index change.
3. **Compensate with adaptive gamma off is not explained.** The LED dims with no compensating gain, so the picture
   darkens. Tick the box when Compensate is picked, or warn.
4. **None + adaptive gamma writes `mode: log_only` for older builds** (CEW:9973). It should be `picture_only`, and it
   should follow the box when the box changes.
5. **Cards shown when the source does not use them:**
   - "Projector response" shows for every source. `cntr_max_usable` and `dmd_clip_cntr` are compensate-only; the
     settle times probably are too (not verified to the end).
   - "LED profile" shows for python and none.
   - Nothing greys when "LED drive on" is off.
   - Experimental is the 3rd card; it should be last.
6. **Wrong help:** "Contrast response time … only used when the script's contrast steps are smoothed"
   (CEW:10276-10277). Compensate uses it too (LedDriveModule.cpp:462, 490-491).
7. **Stale text:**
   - "or to or from Log only" (CEW:10056)
   - "Highlight protection only matters in Compensate." (CEW:10055) and "Compensate mode only." (CEW:10121). Both
     controls are now hidden outside compensate, so the sentences are redundant.
   - "The original listen…" (CEW:9952)
   - "No listen data yet" (CEW:10445, 10501)
   - "VP addendum lines" (CEW:10457, 10554)
   - "Your beta 22 script" (three places)
   - The schema hint "the script VP starts in mode listen" (LedDriveConfig.h:529), shown in validation errors
8. **Jargon:**
   - The monitor shows "mean2", "APL", "brightest block", `tp`/`tc` (CEW:10528-10536).
   - "Try 0.3 after the dark-hair test" (CEW:10417).
   - "highlight amount (brightness x area)" has no scale (CEW:10421).
   - Two pickers both have a "None": Protocol and Command source.
9. **Small code point:** the `listenPort` handler lacks the `if (!document_) return;` every other handler has
   (CEW:10556-10566).
10. **Range differences, Config narrower than VP.** A value outside the Config range is clamped for display but not
    written:
    - `led_floor` family and `black_prelift_max`: 60-255 in Config, 0-255 in VP
    - latencies: 0-500 in Config, 0-2000 in VP
    - `compensation_eotf`: 1.8 in Config, 1.0 in VP
    - `rate_max_*`: minimum 1 in Config; 0 = "use rate_up" in VP

**First-time setup on a new file, as it stands:**

| Source | Fields to touch today | After fixes 2-3 and 6 |
|---|---|---|
| Your Python script | 4-5 over 3 cards: LED drive on, source, script path, Protocol (None then Barco), untick adaptive gamma | 3: LED drive on, source, script path (+ COM port if not COM1) |
| VP's copy of your script | 3-4 | 2 |
| Compensate | 3 | 2 |
| None (picture only) | Already shown, so it cannot be picked: LED drive on, then switch away and back | 2 |

## Settings in VP with no Config control, and controls VP ignores
**Renderer:**
- Every key VP reads in the input and scaling groups (RendererProfileConfig.h:604-736) has a Config control.
- Every Config key and choice value matches the parser and the schema. There is no control that VP ignores.
- Not exposed (intentionally test-only, off):
  - D3 two-frame blend (`blendFrames`)
  - `pixelFastFrom` / `pixelFastFloor`
- Also not exposed: the M6 option "failed blocks' own vectors where detailed" from 5c936394 (`pixelOwnTexture` /
  `pixelOwnError`, `pixelTrustedVectors` hard-coded true, LibplaceboVideoRenderer.cpp:11643). It is reachable only
  from tests. If the owner is meant to try it, it needs a key and a control.

**LED:**
- Keys with no control:
  - `tick_ms`, `rate_up`, `rate_down`, `deadband`, `early_ramp`, `cut_jump`, `bright_cut_dump`, `led_ceiling`
  - `curve`, `steps_up_*`, `steps_down`
  - `apl_weights`, `highlight_percentile`, `highlight_persist_ms`, `apl_persist_ms`, `knee`, `anti_pump_band`
  - `script_highlight_code`, `highlight_area_*`, `highlight_release_ms`
  - `fade_frames`, `fade_min_step`, `cut_threshold`, `ftb_black_ms`
- The page says these come from the LED profile (CEW:9840-9842). That is fine.
- **Projector keys that neither Config nor the LED profile can set**, only a hand edit (CEW:10167):
  - `baud`, `light_min`, `light_max`, `ack_timeout_ms`, `min_command_gap_ms`, `max_levels_per_command`
  - `burst_commands`, `burst_pause_ms`, `echo`, `terminator`, `flux_table`
- That conflicts with the owner's rule "every setting has a Config control" and with the page's own comment
  (CEW:10361-10362). Suggest an "Advanced projector" collapsed card.

## Saving and reloading (item 6)
- **Saving:**
  - Each control writes only when changed. Picking the empty or "inherited" item removes the key.
  - Per-profile sections save to the selected profile (the existing test covers the cinema / other split).
  - The writer (ConfigEditorCore `ConfigDocument`) edits line by line and keeps comments and unknown keys.
  - Apply validates against VP's schema before an atomic save.
- **Minor saving points:**
  - A new key is appended just before the next `[header]`, so it can land after comments that belong to the next
    section.
  - Removing a key deletes its inline comment with it.
- **Tests — not covered:**
  - No Apply → **reopen** check exists for any new key.
  - These new keys have no Config test at all:
    - `sdr_*`, `chroma_upscaler`, `capture_path`, `fbo_float32`, `adaptive_faithful`
    - `dither_vectors`, `dither_grain_tolerance_scale`
    - `dejudder_strength_even`, `dejudder_even_rates`, `dejudder_warp_stage`, `dejudder_match_tolerance`,
      `dejudder_frame_trust`, the de-judder switches
    - `adaptive_upscale*`, `upscale_light`, the adaptive downscale family
    - LED: `listen_script`, `python`, `listen_port`, `listen_log`, `protocol`, the boost knee and shadow-lift keys,
      the contrast/cuts and speed keys
  - `python_drives_port` is asserted only as false (never toggled to true).
  - Missing scenarios:
    - a new file with no LED section
    - older `mode: listen` / `mode: script` files
    - `command_source` with a conflicting `mode`
    - open and Cancel leaves the file byte-identical
  - Test line 987 asserts "on for a mode-only file", which also locks in Simplification 3 for a section-less file.

## Not checked (no build, no run)
- Whether the pre-filled "Auto" text can be edited without select-all (QDoubleValidator).
- How the pages look at small window sizes.
- The Config test binary's results. It was blocked by Application Control; please re-run it after the build agent's
  rebuild: `x64\Release\VideoProcessorConfigTests.exe`, from x64\Release.

## On hold until the GPU is free
None of this review needs the GPU. After the build agent's rebuild (CPU only, but it must wait for the build):
1. `x64\Release\VideoProcessorConfigTests.exe` (from x64\Release) — expect the known RPC failures and flake only.
2. By hand in Config: open an older file, then Apply, OK and reopen. Check that de-judder, SDR, capture and LED
   values survive, and that the file differs only in the edited lines (`fc /n` against a copy).
