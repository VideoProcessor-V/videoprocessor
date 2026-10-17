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
