# LED core and command sources (owner 2026-10-14)

The LED drive is one core with a command source to choose from, not a set of
separate modes. The core opens the RS-232 port, sends the commands, keeps the
logs and runs VP's copy of the script's rules for comparison. Setup and beta
testing only need one setting changed: the source.

## Settings (`[led_drive.Default]`)

| Key | Values | Default | Meaning |
|---|---|---|---|
| `command_source` | `python`, `internal_script`, `compensate`, `none` | not set | Where the LED/CNTR commands come from. When it is not set, the older `mode` decides (see below). |
| `python_drives_port` | bool | false | The fallback for `python`: the original listen. The script writes COM1 itself and VP only follows it and logs. |
| `adaptive_gamma` | bool | off when `command_source` is set | VP's picture adapts to the LED. This covers the gain across each step (script smoothing for the script sources, the compensation gain for compensate) **and** the ANSI shadow lift. The lift's strength is still `shadow_lift_ansi`, default 0. |
| `lookahead` | bool | true | The queued-frame look-ahead of VP's own sources. Off means they react to the frame on screen. The Python script decides from its own screenshots, so it has no look-ahead. |

`command_source` maps onto the module's existing modes:

| Source | Mode | Notes |
|---|---|---|
| `python` | listen | Relays when VP runs the script (`listen_script`) and is not set to `python_drives_port`. |
| `internal_script` | script | `script_smoothing` = `adaptive_gamma`. |
| `compensate` | compensate | The gain applies only with `adaptive_gamma`. |
| `none` | log_only | picture_only when `adaptive_gamma` is on (picture terms only). |

Older configurations that only name `mode` behave exactly as before, with
adaptive gamma on. If `adaptive_gamma` is named in such a file, it still
applies. In Config, choosing a source writes `command_source` and the
matching `mode`, so older builds still read the file sensibly.

## Relay (the `python` source)

- **Port and bytes.** VP opens the port with the script's framing and sends
  each LPW1/CNTR the script reports. The bytes are exactly what the script's
  `echo :<cmd><value>cr > com1` wrote: `:cntr50cr \r\n`, including the space
  cmd keeps before `>`. VP adds no clamp, gap, pause or reply wait. The only
  checks are whole numbers in the projector's ranges (LPW1 0-255, CNTR 1-100).
- **Startup.** Nothing is sent while Python loads and the script starts: no
  ECHO, no queries and no starting state. A long silence is normal. VP logs
  "waiting for the script's first command" every 10 s, then the first
  command and how long it took to arrive.
- **Exit.** VP sends nothing on exit. The script ends with VP and leaves the
  projector where it put it, as it always did.
- **What gets sent.** Commands are sent only while the script VP started is
  running. Anything else that reaches the UDP port is logged and refused.
- **How the script knows.** VP starts the script with `VP_LED_RELAY=1`. The
  bundled `main_vp.py` then echoes to `nul` instead of `com1`. It is the same
  cmd call, so the script's timing is unchanged. An older copy without this
  change still works: its writes fail because VP holds the port, and VP
  relays the commands anyway.
- **Fallback to listen.** If the port cannot be opened, or VP does not run the
  script, VP falls back to the original listen: the script drives COM1 itself.

## Data log (`listen_log`, `logs\led-listen.csv`)

The log is sized for sessions of several hours:

- one row per LPW1/CNTR, with `relay_sent` and `relay_write_ms` in relay;
- a TICK row only when VP's copy starts or stops agreeing with the script, or
  the disagreement changes;
- a `# summary` line every 10 s with the session's agreement and the relay
  counts.

Past 8 MB the file becomes `led-listen.prev.csv` and a new one starts, so at
most 16 MB is kept. A file with an older header is moved aside when it is
opened. `led-listen-now.txt` (the Config monitor's file) now also carries
`relay`, `relayed`, `failed` and `refused`.
