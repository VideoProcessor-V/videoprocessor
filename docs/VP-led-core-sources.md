# LED core and command sources (owner 2026-10-14; Python source retired 2026-10-15)

Dynamic Drive is one core with a command source to choose from, not a set of
separate modes. The core opens the RS-232 port, sends the commands and keeps
the logs; the source decides what to send. Setup and beta testing only need
one setting changed: the source.

## Settings (`[led_drive.Default]`)

| Key | Values | Default | Meaning |
|---|---|---|---|
| `command_source` | `internal_script`, `compensate`, `none` | not set | Where the LED/CNTR commands come from. When it is not set, the older `mode` decides (see below). |
| `adaptive_gamma` | bool | off when `command_source` is set | VP's picture adapts to the LED. This covers the gain across each step (script smoothing for the script copy, the compensation gain for compensate) **and** the ANSI shadow lift. The lift's strength is still `shadow_lift_ansi`, default 0. Only the picture: compensate drives the LED and CNTR either way. |
| `lookahead` | bool | true | The queued-frame look-ahead of compensate. Off means it reacts to the frame on screen. |
| `script_lookahead` | bool | true | The script copy's look-ahead (owner 2026-10-17): it decides from the queued frame that will be on screen when its command lands. Off means the script's decisions on the frame on screen, exactly as before. |
| `led_lookahead_ms` | 0-2000 | 0 | An optional cap on both look-aheads. 0 = every frame the active queue profile has queued. |
| `script_pacing` | `script`, `tick` | `script` | The script copy's rhythm. `script`: the original script's own loop - `script_measure_ms` (its screenshot, default 150) before it acts, `script_echo_ms` per command (its echo, default 30; an LPW1 costs two), its sleeps, and the next loop at once. `tick`: a fixed `tick_ms` loop (400 ms). |

`command_source` maps onto the module's modes:

| Source | Mode | Notes |
|---|---|---|
| `internal_script` | script | VP's copy of the owner's dimming script. `script_smoothing` = `adaptive_gamma`. |
| `compensate` | compensate | The gain applies only with `adaptive_gamma`. |
| `none` | log_only | picture_only when `adaptive_gamma` is on (picture terms only). |

Older configurations that only name `mode` behave as before, with adaptive
gamma on. In Config, choosing a source writes `command_source` and the
matching `mode`, so older builds still read the file sensibly.

## The retired Python source (2026-10-15)

VP used to run the owner's Python script itself (`command_source: python`,
`mode: listen`: relay, feed, listen log). It was only ever a validator of
VP's copy, and VP's copy now matches it: on real clips VP's copy makes the
script's decisions on every loop, and with `script_pacing: script` sends the
same commands at the same moments (`LedScriptCorrelationTests`, which runs the
real script unchanged on a virtual clock through `tools\led_script_sim.py`;
the reference copy is `tools\led-reference\main_vp.py`).

- A file still naming `command_source: python` or `mode: listen` runs
  `internal_script` (logged once at start). VP then drives the port itself:
  if the script is still wanted, it runs outside VP, with VP's source set
  to `none` so the two do not share the port.
- The retired keys (`listen_script`, `python`, `listen_port`, `listen_log`,
  `python_drives_port`, `python_feed`) are accepted and ignored, so an
  older file is not rejected.
- The script is no longer shipped (`scripts\main_vp.py` is not installed).

## The script is the reference for intent (owner 2026-10-17)

"Byte-identical to beta_22.py" (the delta-3 lineage) holds for the script's decisions, bands and
rhythm, and for the FL35 command stream. The script is the reference for what the owner MEANT, not
for its accidental coding limitations. Where its behaviour comes from such a limitation, VP's copy
matches the intent (and asks the owner if unclear); the rules themselves are never "improved"
without him. Decided so far:
- Highlights/peaks are measured on the PICTURE ONLY (no subtitles or overlays, which the script's
  screenshot counted by accident), with full-coverage peak detection, not sparse samples.
- Read-back from the projector is off by default (the script never read back); resend only on
  real write/port errors.
- CNTR is never left above the needed value.
- Script mode was always meant to have look-ahead (the script lacks it only because a screenshot
  cannot see the future). At this commit the queued-frame look-ahead runs for compensate only
  (`LedDriveModule::WantedLookaheadFrames`); script-mode look-ahead is assigned to the LED fix group.

## Config

The Dynamic Drive card starts with "Dynamic Drive on" and the command source, then
adaptive gamma and, for the source that uses it, look-ahead (compensate) or
script pacing with its two times (the script copy). Only what the chosen
source uses is shown:

- compensate's floors, highlight protection and its cards (Contrast and cuts,
  LED speeds, Experimental) appear for Compensate only;
- the shadow-lift rows are greyed while adaptive gamma is off.

An older file naming the Python source shows, and runs, VP's copy of the
script.

## The script is the reference for intent (owner 2026-10-17)

The owner's script is the reference for what Dynamic Drive should do, not for
its coding limitations. Its screenshot could not see the future, counted
subtitles and overlays by accident, and was taken after a fixed delay. VP's
copy keeps the script's bands, steps, contrast logic and rhythm, and changes
only where the script was limited:

- **Picture only, full coverage** (`led_picture_only`, `led_peak_full_coverage`,
  both default on). The highlight peak is the largest area mean over boxes the
  size of one pixel of the script's 854x480 INTER_AREA downscale, derived from
  the real screen at run time, tiling the whole active picture. The detected
  subtitle (and where a moved subtitle is shown) is left out of the peak, APL,
  histogram and cut cells. With nothing detected, nothing is masked.
- **Look-ahead** (`script_lookahead`, default on). The copy decides on the
  queued frame that will be on screen when its command lands: send time plus
  `led_latency_ms`, and for a rise plus `led_settle_ms`, so the light has
  settled by its frame. A fall lands on its frame. A cut to a brighter scene
  in the queue gets that scene's LPW1 and CNTR landing on its first frame. A
  CNTR going down goes one command gap ahead of the LPW1, never after it. Out
  of black, the light may rise early to `black_prelift_max`, and no higher
  while the picture is still black (`preposition_black_only`'s black).
- **Floor.** When the queue covers `led_latency_ms + led_settle_ms`, the
  script's flat bottom band continues its curve down to `led_floor`, as
  compensate does on its covered path. Otherwise the floor is
  `led_floor_reactive`.
- **Queue = look-ahead.** Both sources look through every frame the active
  queue profile has queued, whatever `active_picture_lookahead_frames` is set
  to. `led_lookahead_ms` is only an optional cap. Low Latency queues nothing,
  so the script reacts to the frame on screen exactly as before. A queue shorter
  than the landing time uses its last frame, and a cut seen in it is sent at
  once. Only a real fault is logged as a warning: frames are queued but no
  statistics come from them.
- **CNTR is never left above need.** A schedule cut short by Apply or
  Reconfigure is completed at the next tick with the value the controller
  already believes. Failed idle-restore and leave-compensate writes are sent
  again, and a failed leave-script CNTR 50 goes to the driver's retrying
  queue.

