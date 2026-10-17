# LED profile files - a guide

For the owner, and for any agent asked to help build or tune an LED profile.

## What a profile is

VideoProcessor's Dynamic Drive has three layers of settings, each overriding
the one before:

1. **Built-in** - the values in `led-profile-example.cfg`. They reproduce
   the old `beta_22.py` script (its curve and step tables).
2. **An LED profile file** (optional) - a small file with a
   `[led_drive.Default]` section holding only the lines you want to change.
3. **The main configuration's `[led_drive.Default]`** - the core settings
   the Config app's Dynamic Drive page edits: `enabled`, `mode`, `led_floor`,
   `led_floor_reactive`, `led_floor_black`, `highlight_tolerance_stops`,
   `gamma_boost`, the experimental fade-to-black settings, and
   `profile_file` (which file to use). These always win.

So a profile changes the *behaviour* (curve, ramps, thresholds); the page
keeps the everyday choices (on/off, mode, how low the LED goes).

## Making one

1. Copy `docs/cinema/led-profile-example.cfg` next to the main configuration
   (`VideoProcessor-test.cfg`), e.g. as `my-led-profile.cfg`.
2. Change the lines you want; delete the rest.
3. In the Config app: Dynamic Drive page -> LED profile -> Browse..., choose the
   file, Apply. (Or add `profile_file: my-led-profile.cfg` to
   `[led_drive.Default]`; a name without a folder means "next to the main
   configuration".)
4. To go back: "Use built-in" (or delete the `profile_file` line).

VP re-reads the profile whenever the configuration is applied, and when the
profile file itself changes and the configuration is applied again. Edits
carry on from the LED's current level; they do not flash the projector to
full light.

## Rules VP enforces (an invalid profile is rejected with a message)

- The section must be `[led_drive.Default]`.
- `enabled` and `profile_file` are not allowed in a profile (main file only).
- Values must be in range, the same rules as the main file (listed in
  `src/VideoProcessor-Lib/led/LedDriveConfig.h`, `LedDriveRules()`).
- Together with the main settings: `led_floor_black <= led_floor <=
  led_ceiling`, and `highlight_area_small < highlight_area_large`.
- Projector settings (`port`, `led_latency_ms`, `flux_table`, command timing)
  are not read from a profile; they live in `[projector.Default]`.

## What each setting does, and what to change first

| Setting | Mode | Effect | Typical change |
|---|---|---|---|
| `curve` | both | LED level for each picture brightness (APL code : level) | the main shape of the dimming; lower the middle points to dim more in mid-bright scenes |
| `led_ceiling` | both | maximum LED level | lower to cap brightness |
| `tick_ms`, `steps_*`, `anti_pump_band`, `script_highlight_code` | script | the script's own stepping and contrast rules | leave as the script had them unless comparing |
| `script_smoothing` | script | off: the script's real LPW1 and CNTR commands, picture untouched; on: VP also eases the picture across each whole CNTR step (timed with the projector's `cntr_latency_ms`) | leave off to compare with the script; on if the contrast steps are visible |
| `rate_up`, `rate_down` | compensate | LED speed in levels per second | lower `rate_down` for gentler dimming; raise `rate_up` for faster recovery after a dark-to-bright cut |
| `deadband` | compensate | ignore tiny target changes while still | raise if the LED "breathes" on a static scene |
| `rate_preset` | compensate | `script` (default) matches the script's LED speeds; `compensated` is faster | try `compensated` by eye once compensation is on |
| `rate_gain_*`, `rate_min_*`, `rate_max_*` | compensate | rate law: speed proportional to the distance, between min and max; overrides the preset | leave; `rate_gain` 0 restores the fixed `rate_up` / `rate_down` |
| `cntr_assist`, `software_first`, `cntr_assist_max` | compensate | the projector's contrast helps only where VP's gain would compress highlights; lets the LED dim further in highlight scenes | leave on; off = no contrast |
| `cut_cntr_dump`, `shaped_recovery` | compensate | on a cut to a brighter scene: contrast drops to 50 and the picture fades up smoothly with the LED | leave on; compare with `cut_cntr_dump: false` (contrast held) by eye |
| `max_cut_dip` | compensate | the darkest start of that fade-up you accept; sets the LED floor (about 102 at 0.5) | lower for deeper dimming and darker fade-ups |
| `recovery_bias_pct` | compensate | a contrast left above 50 comes down this much on the bright side | leave |
| `dmd_clip_cntr` (`[projector]`) | compensate | the contrast at which white stops getting brighter at full LED; 59 = where your script's dump ended | set from the fixed-LED white sweep |
| `cut_cntr_level` | compensate | the contrast a cut to a brighter scene goes to (default = `dmd_clip_cntr`) | lower by taste if highlights crush after a cut |
| `recovery_rate_*` | compensate | how fast the picture fades up for what the LED and contrast cannot yet supply | leave |
| `black_prelift_max` | compensate | coming out of black: how far the LED may lift while the screen is still black (the contrast goes to the cut level then too) | 120; lower if the black visibly lifts before a cut, 60 = never |
| `led_settle_ms`, `cntr_latency_ms`, `cntr_settle_ms`, `cntr_max_usable` (`[projector]`) | compensate | how fast the LED and contrast respond, and the highest useful contrast | from the camera / light-meter measurement; until then 300 / 80 / 0 / 74 |
| `early_ramp` | compensate | start before a cut seen in the queue | leave on |
| `cut_jump` | compensate | a seen cut: one jump timed to land on the cut frame | leave on; off ramps instead |
| `bright_cut_dump` | compensate | an unseen dark-to-bright cut: straight up, like the script's dump | leave on; off ramps at `rate_up` |
| `knee`, `highlight_*` | compensate | how highlights hold the LED up | raise `highlight_release_ms` if the LED drops too soon after headlights pass |
| `apl_persist_ms` | compensate | flash rejection window | raise if lightning or muzzle flashes still move the LED |
| `cut_threshold`, `fade_frames`, `fade_min_step` | both | scene-change detection | raise `cut_threshold` if ordinary motion is taken for cuts |
| `apl_weights` | both | how brightness is measured | leave |
| `compensation_eotf` | compensate | the projector's light exponent on the final code (2.41, the FL35 light-meter fit; delta 4); also on the Dynamic Drive page as "Projector gamma", which wins. It no longer sets the gamma boost | leave |

The most important everyday setting - how low the LED goes - is not in the
profile: it is "Lowest LED level" (`led_floor`, 60-255) on the Dynamic Drive
page, with "Lowest level in fades to black" (`led_floor_black`, default 60)
beside it. 60 is the lowest LPW1 accepts.

## For an agent building a profile

- Start from `led-profile-example.cfg`; keep only changed lines.
- Change one group at a time and say why, in a comment above the line.
- Check the file with VP's own validator: load the main configuration with
  `profile_file` set and run the native test
  `LedDriveDelta1Tests::LedProfileFileFillsInWhatTheMainSettingsLeaveOut` as
  a model, or start VP and read `C:\logs\vp.log` for "Dynamic Drive:
  configuration rejected (...)".
- Never put `enabled`, `profile_file` or projector settings in the profile.
- The owner judges on the projector with the acceptance tests in
  `docs/VP-led-drive-module.md` section 7 (slow fade to black, hard cut dark
  to bright, a static scene for 30 s).

## Fade to black below 60 (experimental)

LPW1 stops at 60. `SHUT` is out (the FL35 here has no mechanical shutter)
and `PMUT` only blanks the pixels at the current LED level. The projector's
custom LED dim mode (`LDMM` 1 = custom, `LDCR` / `LDCG` / `LDCB` per
colour) may reach lower; it bypasses the automatic colour balance, which
does not matter on black, but the three colours are separate commands, so
changing them one after another can tint the black for a moment.

The Dynamic Drive page has an **Experimental** box for this (off by default;
its options are greyed out until it is ticked):

| Setting | Key | Default | Meaning |
|---|---|---|---|
| Method | `ftb_custom_dim` | off | `together`: all three colours straight to the deepest level (one quick tint); `turns`: the colours take turns in small steps, never more than one step apart |
| Step interval | `ftb_step_ms` | 100 | time between custom-dim commands (like the old script's 100 ms contrast steps) |
| Starting custom level | `ftb_custom_start` | 40 | where the custom dim starts (1-40; 40 is about 40/255, a small step down from LPW1's 60) |
| Deepest custom level | `ftb_custom_floor` | 0 | how low the fade goes (0-39; 0 turns the LEDs off - true black) |
| Step size per colour | `ftb_turn_step` | 2 | levels each colour moves per command when taking turns (smaller: less colour shift, slower) |
| (profile only) | `ftb_black_ms` | 500 | how long the picture must be black first |

Compensate mode only. When the picture has been black for `ftb_black_ms`
and the LED is at the fade-to-black level, VP sets the three colours to the
matching value, switches custom mode on and steps down. At the first light
on screen - or seen coming in the queue - it puts the three colours straight
back (speed matters more than colour as the picture returns) and switches
custom mode off; LED commands then resume. On startup and exit VP always
sets custom mode off when the experiment is configured.
