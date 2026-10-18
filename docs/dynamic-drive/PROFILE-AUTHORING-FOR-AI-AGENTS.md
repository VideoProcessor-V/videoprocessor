# Writing a Dynamic Drive device profile (guide for AI agents)

This guide is for an AI agent (Claude, ChatGPT or another) that a VideoProcessor
(VP) user has asked to write a Dynamic Drive device profile for the user's
projector. The input is the projector's RS-232 control manual. The output is
one small text file with the extension `.vpprojector`.

Read the whole guide before you write anything. Where this guide and the
manual disagree about the projector, the manual wins. Where this guide and VP's
validator disagree about the file format, the validator wins.

## 1. What this is

Dynamic Drive (formerly "LED drive") is VP's dynamic control of a projector's
light source over RS-232. VP dims the light source on dark scenes, raises it
before bright scenes, and compensates the picture so the viewer sees no change.
It sends a few short serial commands, quickly and safely.

Everything projector-specific lives in a device profile: the serial settings,
how a command is built (prefix, text, value format, terminator), how a reply is
read, the projector's light and contrast ranges, the light source's timing, the
step and pacing limits, the floors and the restore values.

VP ships one profile, for the Barco F35 / FL35. It is compiled in and also
written out as `profiles\dynamic-drive\barco-fl35.vpprojector`. It is the
worked example in section 12.

The schema is closed. It describes light, contrast, a handshake, a dim mode and
a custom dim. It has no slot for power, lamp, input, reset, firmware or factory
commands, and you must never add one.

Schema 1 supports only:

- a serial (RS-232) connection;
- text (ASCII) commands;
- replies that end with a carriage return (CR);
- replies that carry a decimal number.

If the manual describes a binary protocol, a checksum, a length byte, a login,
a network-only interface or LF-terminated replies that you need, stop and tell
the user (section 8.4).

## 2. Who does what

You can read the manual and write the file. You usually cannot run VP, open the
serial port, watch the projector or use a camera: ask the user to do those
steps and paste back the results, with the exact command or setting and what to
look for. Never guess a value. If the manual does not give one, mark it
unknown, put a conservative placeholder in the file with a `#` comment saying it
is an estimate, list it in your hand-back (section 14) and ask the user.

## 3. Safety rules (read these first)

A wrong profile can leave the projector dim, send a command the projector
misreads, or in the worst case switch something off. Follow every rule.

1. Never add a power, lamp, standby, input, reset, firmware or factory command.
   The schema has no slot for them and the validator's risky-word guard exists
   to catch them. Do not hide one inside another command's template.
2. Never set `[profile] reviewed = yes` on your own. Set it only after the
   user has read the flagged template and the manual entry with you and has
   said yes (section 5.1).
3. Never exceed the manual's ranges. `[light] min` and `max` are the manual's
   smallest and largest accepted light values, exactly. VP never sends a value
   outside them, but it trusts what you wrote.
4. Restore values must be right. On exit and when Dynamic Drive is disabled VP
   sends light = `[restore] light`; when the projector has a contrast command it
   also sends contrast = `[restore] contrast`. The idle restore uses
   `led_ceiling` instead (section 6.4). A wrong restore value can leave the
   projector dim after VP exits. Check both against the manual and against what
   the projector does at its normal setting.
5. Start with conservative limits: a small `max_step`, a `min_command_gap_ms`
   at or above the manual's minimum interval, high floors, and long (slow)
   `latency_ms` and `settle_ms`. Make them more aggressive only after
   measuring, and only with the user's agreement.
6. Keep read-back off unless it is truly necessary. The owner's rule is
   `read_back = no`. Describe read-back in the profile (so the connection test
   can use it) but leave it off by default.
7. Test first with the projector visible and the user present. Never run the
   first live test unattended.
8. Keep Dynamic Drive disabled (`enabled = false` in `[led_drive.Default]`, or
   a dry run) until the validator says OK and the user agrees.
9. Never copy manual text into the file or the hand-back. Record facts
   (command names, ranges, intervals) and section numbers.
10. If you cannot tell whether a command is safe, leave it out and ask.
11. Before you edit the user's `VideoProcessor.cfg`, make a timestamped backup
    copy next to it. Change only the keys you were asked to change. Keep
    comments and unknown keys.

## 4. Workflow

Do these steps in order. Stop at any step that fails.

1. **Read the manual** (section 8). Note the section number of every fact.
2. **Check that schema 1 can express it.** If not, stop (section 8.4).
3. **Fill in the template** from section 13 or the FL35 file. Fill in every
   required key (5.3). Put a `#` comment above each value saying where it came
   from.
4. **Validate offline.** Ask the user to run, in PowerShell in the VP folder:

   ```
   tools\validate_dd_profile.ps1 -Path <your-file.vpprojector>
   ```

   Add `-VideoProcessorExe <path to VideoProcessor.exe>` if it cannot find it
   (by default it tries the repository's `x64\Release\VideoProcessor.exe`, then
   `C:\Videoprocessor\vp\VideoProcessor.exe`). It runs
   `VideoProcessor.exe /validate_dd_profile <file>` and prints `OK ...` with a
   summary and example bytes for each framing, or `INVALID <reason>`. Exit code
   0 = valid, 1 = invalid, 2 = usage or tool error. Nothing is sent to any
   projector and no serial port is opened. Fix every `INVALID` and run it again.
5. **Compare the example bytes with the manual**, character by character,
   including the terminator. The `OK` report shows the light command at its
   maximum (and contrast at neutral) in each framing. The validator cannot do
   this check.
6. **Select the profile and do a dry run** (sections 6 and 9.1).
7. **Connection test**, user present, projector visible (9.2).
8. **Supervised exercise** (9.4).
9. **Measure latency and settle** (section 10).
10. **Hand back** (section 14).

## 5. File format rules

The profile uses VP's INI-like cfg format.

- Sections are `[section]`. Keys are `key = value`. Both are case-insensitive.
- A comment starts with `#` or `;` at the start of a line or after whitespace.
- The **first** `:` or `=` on a line separates the key from the value. A key
  can never contain `:`. A value can contain `:` and `=` (`prefix = :`,
  `curve = 60:0.55`).
- Values are trimmed. To keep leading or trailing spaces, wrap the value in
  double quotes: `suffix = " \r"`. The quotes are removed when read.
- A literal `#` or `;` after a space inside a value starts a comment. Write it
  as `\x23` or `\x3B`. (Inside a `[reply] pattern` these are also valid
  regular-expression escapes.)
- The schema is closed. An unknown section or key, a malformed line, a
  duplicate key and a duplicate section are all errors. A profile is used
  completely or not at all.

### 5.1 Command templates, escapes and placeholders

A command is sent as `prefix + template (with the value) + framing suffix`.

| Escape | Meaning |
| --- | --- |
| `\r` | carriage return (0x0D) |
| `\n` | line feed (0x0A) |
| `\t` | tab (0x09) |
| `\\` | backslash |
| `\"` | double quote |
| `\xHH` | one byte, two hex digits |

Any other backslash sequence, or a lone backslash at the end, is an error.

Placeholders in `set` templates:

| Placeholder | Value 7 | Value 255 |
| --- | --- | --- |
| `{v}` or `{v:d}` | `7` | `255` |
| `{v:3d}` | `  7` (space padded) | `255` |
| `{v:03d}` | `007` | `255` |
| `{v:x}` / `{v:X}` | `7` | `ff` / `FF` |
| `{v:02X}` | `07` | `FF` |
| `{v:04x}` | `0007` | `00ff` |

The width is 1 to 9. The format must end in `d`, `x` or `X`. A hex format
cannot be used for a negative value. Use `{v}` for a plain decimal number. A
`{` or `}` that is not a placeholder must be written `\x7B` or `\x7D`.

- Every `set` template (light, contrast, dim mode, custom dim) needs a `{v}`.
  Use exactly one.
- `query`, `query_min`, `query_max` and the handshake `send` must not contain
  `{v}`. A template must not render to nothing.

Letter case: `[framing.<name>] case` applies to the **literal text** of the
template only, never to the value, prefix or suffix. Choose `x` or `X` in the
placeholder for the case of hex digits. Use `as_written` unless the manual says
the projector needs one case.

Risky-word guard: the validator refuses a template that contains any of these
words, in any letter case, anywhere in the text (including inside longer
words): `POWR`, `POWER`, `PWR`, `STBY`, `STANDBY`, `LAMP`, `RST`, `RESET`,
`FACT`, `FIRM`, `FWUP`, `UPDATE`, `INPUT`, `SHUT`, `REBOOT`. It checks `set`,
`query`, `query_min`, `query_max` and the handshake `send`. A legitimate light
command can contain one by chance (`FIRST`, `CONFIRM`). If the validator
reports a hit:

1. Re-read the manual entry. Is it really the light or contrast command?
2. If not, remove it.
3. If yes, tell the user which word hit and which manual entry the command
   comes from, and ask for confirmation. Only after an explicit yes, set
   `[profile] reviewed = yes` and say so in your hand-back.

### 5.2 Units

VP's own scales never change: light level 0 to 255; contrast 50 to 100
(50 = neutral). Every number in a profile is in the **projector's own units**
(device units), the units the manual uses. VP converts:

- Light: VP level 0..255 maps linearly onto `[light] min..max`:
  `device = round(min + level * (max - min) / 255)`. Identity for 0..255.
- Contrast: VP 50..100 maps linearly onto `[contrast] min..max`. Identity for
  50..100.
- `max_step` is device units. VP converts it to VP levels
  (`max_step * 255 / (max - min)`, at least 1).
- Floors and `[light] curve` points are device units. VP converts them to VP
  levels when it reads the profile.

A projector with a light range of 0 to 100 percent keeps `min = 0`,
`max = 100`, and every floor, step and restore value is a percent.

### 5.3 The schema

"int" is a whole number in the range shown. "bool" accepts `yes/no`,
`true/false`, `on/off`, `1/0`. "text" is free text of the stated length. The
ranges are VP's sanity bounds. The manual's ranges are tighter, and only you
can check them. A key that is not required and not written keeps the default
shown. For the FL35 these defaults are the Barco values, so state every key you
rely on; do not rely on defaults for another projector.

#### `[profile]`

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `schema` | int, 1 only | yes | none | Always `1`. |
| `maker` | text, 1-64 | yes | none | Maker name, for display. |
| `model` | text, 1-64 | yes | none | Model name, for display. |
| `version` | text, 1-32 | yes | none | Your profile's version, for example `1`. |
| `source` | text, up to 400 | no | empty | Where the facts came from: manual title, part number, revision. A reference, not copied text. |
| `notes` | text, up to 400 | no | none | Free notes. VP checks the length and ignores the content. |
| `reviewed` | bool | no | `no` | A human has reviewed templates the risky-word guard flagged (5.1). |

#### `[transport]`

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `type` | `serial` | yes | none | Only `serial` exists. |
| `baud` | int, 1200-921600 | yes | none | Baud rate (the full range loads). |
| `data_bits` | `7` or `8` | no | `8` | Data bits. |
| `parity` | `none`, `even`, `odd` | no | `none` | Parity. |
| `stop_bits` | `1` or `2` | no | `1` | Stop bits. |
| `flow_control` | `none`, `rts_cts`, `xon_xoff` | no | `none` | Flow control. |
| `read_back_supported` | bool | yes | none | The projector sends replies VP can read (5.4). |
| `read_back` | bool | no | `no` | Default for waiting on replies. Keep `no`. `yes` needs `read_back_supported = yes`. |
| `reply_line_end` | `cr` | no | `cr` | Replies end with CR. Only `cr` exists. |

#### `[framing]`

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `prefix` | bytes with escapes, may be empty | no | empty | Bytes sent before every command (for example `:`). |
| `send_only` | framing name | yes | none | The framing used when VP sends without waiting for replies (read-back off). Must name a `[framing.<name>]`. |
| `probe_order` | comma list of framing names | if `read_back_supported = yes` | empty | With read-back on, the framings VP tries in order, using the handshake and a light query. Each must exist and have `probe = yes`. |
| `fallback` | framing name | no | none | Used when no probed framing gets an answer. Must exist. |

Framing names use letters, digits and `_` (at most 32). At least one
`[framing.<name>]` section is required.

#### `[framing.<name>]` (one section per framing)

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `suffix` | bytes with escapes, not empty | yes | none | The bytes that end every command (for example `\r`). |
| `case` | `as_written`, `lower`, `upper` | no | `as_written` | Letter case of the template's literal text. |
| `probe` | bool | no | the value of `read_back_supported` | The projector can acknowledge commands sent this way. `probe = yes` needs `read_back_supported = yes`. A `probe = no` framing is never waited on. |

Most projectors need exactly one framing, for example `[framing.cr]` with
`suffix = \r`. Add more only when the manual allows several terminators and you
cannot tell which the projector wants (the FL35 has three). The framing names
are also the values the user's `terminator` key may take (section 6).

#### `[reply]` (only when `read_back_supported = yes`)

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `pattern` | ECMAScript regular expression, at least two capture groups | yes, with read-back | none | Matches one whole reply line, without the CR. |
| `name_group` | int 1-9 | no | `1` | Capture group holding the command name. |
| `value_group` | int 1-9 | no | `2` | Capture group holding the value. |

The value group must hold a plain decimal integer of at most 10 digits (a sign
is allowed). Hex values cannot be read. The two groups must differ and must not
exceed the pattern's capture groups. The whole line must match, so anchor with
`^` and `$` and allow for spaces with `\s*`. With `read_back_supported = no`
leave the `[reply]` section out entirely.

#### `[command.light]` (required)

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `set` | template with `{v}` | yes | Sets the light level, in device units. |
| `query` | template, no `{v}` | no | Reads the current level. Needs `read_back_supported = yes`. |
| `query_min` | template, no `{v}` | no | Reads the projector's lowest accepted level. |
| `query_max` | template, no `{v}` | no | Reads the projector's highest accepted level. |
| `reply` | name, up to 16 of letters, digits, `_`, `-` | with read-back | The name the projector's replies carry for this command, matched case-insensitively against the reply's name group. |

#### `[command.contrast]` (optional; see the warning)

Keys: `set` (template with `{v}`, required), `query` (template), `reply` (name).
Adding the section also requires `[contrast]` with `min` and `max`, and
`[restore] contrast`.

Contrast warning. VP's contrast assist models the control as the FL35's CNTR:
50 is neutral, and higher values add light on highlights roughly as
(value/50) to the power 2.41. Another projector's "contrast" may be a different
control (a gain, a gamma, an iris). Add `[command.contrast]` only if the manual
shows a control that behaves like this and the user agrees. If in doubt, leave
it out. Without it VP forces `cntr_assist` off and refuses the
`internal_script` command source (it needs contrast); `compensate` still works
with the light control alone.

#### `[command.handshake]` (optional)

Keys: `send` (template, no `{v}`, required), `reply` (name). A command that
makes the projector start sending replies (the FL35's `ECHO1`). Include it only
if the manual says replies are off until a command enables them.

#### `[command.dim_mode]` (optional)

Keys: `set` (template with `{v}`), `standard` (int 0-65535, required), `custom`
(int 0-65535, required), `reply` (name). For projectors with a separate dim
mode (the FL35's LDMM: 0 = standard, 1 = custom). VP sends the `standard` value
on restore. Leave the section out when the projector has no such mode: VP then
sends no dim-mode reset.

#### `[command.custom_red]`, `[command.custom_green]`, `[command.custom_blue]` (optional, all three or none)

Keys: `set` (template with `{v}`), `min` (int), `max` (int), `reply` (name). The
custom dim (the FL35's LDCR/LDCG/LDCB, per-colour light). Rules:

- All three channels and `[command.dim_mode]` must be present together.
- `min` must be 0 and `max` at least 40, because VP's custom dim sends 0 to 40.
  Set `max` to the manual's real maximum, never above it.
- If the manual's range is narrower (for example 0 to 20), leave the custom dim
  out entirely. VP then forces `ftb_custom_dim` off.

#### `[light]` (required)

| Key | Type / range | Required | Meaning |
| --- | --- | --- | --- |
| `min` | int 0-65535 | yes | The manual's smallest accepted light value. |
| `max` | int 1-65535 | yes | The manual's largest accepted light value. Greater than `min`. |
| `floor` | int, min..max | yes | The lowest light when VP's look-ahead covers latency plus settle. |
| `floor_reactive` | int, min..max | yes | The lowest light without that cover. The higher of the two applies. |
| `floor_black` | int, min..max | yes | The lowest light for true black. At most `floor` and at most `floor_reactive`. |
| `curve` | pairs `drive:relative_light`, comma separated | no | Drive-to-light map, for example `60:0.55, 80:0.78, 100:1.00`. Drive values are device units within min..max and rising. Relative light is above 0, at most 10, and rising. Empty = light proportional to the drive value. Leave it empty until measured. |

A higher floor means less dimming, so floors are a safety choice as well as a
picture choice. Never set a floor below what the manual says the light source
needs (for a laser or LED, its minimum stable level).

#### `[contrast]` (only with `[command.contrast]`)

| Key | Type / range | Required | Meaning |
| --- | --- | --- | --- |
| `min` | int 0-65535 | yes | Contrast at VP's neutral. Also the value restored. |
| `max` | int 0-65535 | yes | The manual's largest contrast value. Greater than `min`. |
| `max_usable` | int, min..max | no | The highest value that still adds light. Defaults to `max`. |
| `dmd_clip` | `-1` or int, min..max | no | The value at which full-white code reaches the imager's maximum at full light. `-1` = `max_usable`. If absent it is 59 (the FL35 value), so **always state it**. |

A `[contrast]` section without `[command.contrast]` is an error.

#### `[timing]`

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `latency_ms` | int 0-2000 | yes | none | Dead time from the command to the light starting to move. |
| `settle_ms` | int 0-5000 | yes | none | After the dead time, the light settles to the new level as a first-order lag: 63 percent of the step after this time. |
| `contrast_latency_ms` | int 0-2000 | no | 80 | The same for contrast. |
| `contrast_settle_ms` | int 0-5000 | no | 0 | The same for contrast. |
| `ack_timeout_ms` | int 20-5000 | no | 300 | Read-back only: how long to wait for a reply. |
| `no_reply_backoff_ms` | int 0-60000 | no | 2000 | Read-back only: pause after a missing reply, if the manual says to wait. |

#### `[limits]`

| Key | Type / range | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `max_step` | int 0-65535 | yes | none | Largest light drop in one command, device units. A cut jump is sent whole. `0` = no limit: do not use it. At most `max - min`. |
| `min_command_gap_ms` | int 0-5000 | yes | none | Never two commands closer than this. At or above the manual's minimum interval. |
| `burst_commands` | int 0-1000 | no | 0 | Pause after this many commands. `0` = no pause. |
| `burst_pause_ms` | int 0-60000 | no | 5000 | Length of that pause. Set both keys if the manual limits bursts. |

#### `[restore]`

| Key | Type / range | Required | Meaning |
| --- | --- | --- | --- |
| `light` | `ceiling` or int, min..max | yes | The light sent on exit and disable. `ceiling` = VP's `led_ceiling` (255 = full light by default), converted to device units. A number is a device value. |
| `contrast` | int | with `[command.contrast]` | The contrast sent on restore. Must equal `[contrast] min`. Leave the key out without a contrast command. |

### 5.4 Read-back

Read-back means VP waits for the projector's reply to each command and reads
its range. The owner's rule is **no read-back unless truly necessary**. Describe
read-back (`read_back_supported = yes`, with `[reply]`) when the projector
supports it and the manual gives the reply format, so that the Config "Test
connection" button works and a user can turn `read_back = yes` on in the cfg.
Set `[transport] read_back = no` either way. With `read_back_supported = no`:
no `[reply]` section and no `query`; the connection test sends nothing and
reports `UNVERIFIED` (9.2), so the user confirms by eye; and a cfg with
`read_back = yes` makes VP refuse the profile (6.2).

### 5.5 What the validator checks

VP refuses the whole file for any of these:

- an unreadable file; a malformed line, duplicate key or duplicate section; an
  unknown section or key; a value out of range or of the wrong type;
- a missing required key. The list is exactly: `[profile]` schema, maker,
  model, version; `[transport]` type, baud, read_back_supported; `[framing]`
  send_only; `[command.light]` set; `[light]` min, max, floor, floor_reactive,
  floor_black; `[timing]` latency_ms, settle_ms; `[limits]` max_step,
  min_command_gap_ms; `[restore]` light;
- no `[framing.<name>]`; a framing without `suffix`; `send_only`, `fallback` or
  a `probe_order` entry naming a missing framing, or a `probe_order` entry with
  `probe = no`;
- `[light]` min not below max; a floor outside min..max; `floor_black` above
  `floor` or `floor_reactive`; a numeric `[restore] light` outside min..max;
  `max_step` above `max - min`; a curve drive value outside min..max;
- with `[command.contrast]`: missing `[contrast]` min or max or
  `[restore] contrast`; `min` not below `max`; `max_usable` or `dmd_clip`
  outside min..max; `[restore] contrast` not equal to `[contrast] min`; and a
  `[contrast]` section without the command;
- a custom dim that is partial or lacks `[command.dim_mode]`, a channel with
  `min` not 0, `max` below 40 or `min` not below `max`, or a dim mode without
  `standard` and `custom`;
- `probe = yes` or `read_back = yes` while `read_back_supported = no`; with
  `read_back_supported = no`, a `[reply]` section or a light `query`; with
  `read_back_supported = yes`, no `[reply] pattern`, no `probe_order`, a pattern
  that does not compile, group numbers above its capture groups, equal
  `name_group` and `value_group`, or no `[command.light] reply`;
- a risky word in a template while `reviewed` is not `yes`; a template that
  does not render at both ends of its device range in every framing.

A validator OK means the file is well formed and self-consistent. It does not
mean the commands are right for the projector (section 11).

## 6. Selecting a profile and the cfg keys

### 6.1 Selecting

In `[projector.Default]` of `VideoProcessor.cfg`:

```
[projector.Default]
protocol = barco_ascii
device_profile = profiles\dynamic-drive\my-projector.vpprojector
port = COM3
```

- `device_profile` is a path, absolute or relative to the folder of
  `VideoProcessor.cfg`. Empty or absent = the compiled-in FL35 profile.
- `protocol = barco_ascii` is the value for **any** profile-driven serial
  projector (the Config label is "Barco serial (RS232)"). It means "drive the
  projector over RS-232 with the selected device profile". `none` means never
  open a port.
- `port` (for example `COM3`) is the user's serial port, not part of the
  profile.
- In the Config app, Dynamic Drive page, Connection section, the "Device
  profile" row has a combo box (the built-in FL35, each `*.vpprojector` in
  `profiles\dynamic-drive` next to the cfg or the app, and "Custom file (path
  below)"), a path field, "Browse...", and a status line. The status line
  reads "Profile OK: ..." or "VP will refuse this profile and run no Dynamic
  Drive: ...". Tell the user to copy the finished file into that folder so it
  appears in the list.

### 6.2 When VP refuses a profile

VP **refuses** the selected profile, forces `protocol` to `none`, runs no
Dynamic Drive (no projector commands, no picture compensation) and logs why,
when any of these holds:

- the file fails validation;
- the cfg's `terminator` names a framing the profile does not have;
- the cfg has `read_back = yes` but the profile has `read_back_supported = no`.

VP never sends commands from a profile it has refused, and never sends a value
outside a declared device range. `internal_script` with a profile that has no
contrast command is also refused: VP logs a warning and sends nothing (use
`compensate`).

### 6.3 Precedence and what the cfg overrides

VP builds the projector settings in this order: built-in defaults, then the
device profile (device units converted to VP units), then every key that
`[projector.Default]` names explicitly. So a cfg key **overrides** the profile.
The override keys are in **VP units**, not device units:

| cfg key | Overrides | Unit / range |
| --- | --- | --- |
| `baud` | `[transport] baud` | 1200-921600 |
| `ack_timeout_ms`, `min_command_gap_ms`, `burst_commands`, `burst_pause_ms` | `[timing]`, `[limits]` | as the profile |
| `max_levels_per_command` | `[limits] max_step` | **VP levels**, 0-255 |
| `read_back` | `[transport] read_back` | yes/no (see 6.2) |
| `echo` | whether the handshake is sent | yes/no |
| `terminator` | the framing used | `auto` or a framing name of the profile |
| `led_latency_ms`, `led_settle_ms` | `[timing] latency_ms`, `settle_ms` | ms |
| `cntr_latency_ms`, `cntr_settle_ms` | contrast timing | ms |
| `cntr_max_usable` | `[contrast] max_usable` | **VP contrast**, 50-100 |
| `dmd_clip_cntr` | `[contrast] dmd_clip` | **VP contrast**, -1 or 50-100 |
| `flux_table` | `[light] curve` | pairs `lpw:relative_flux`, `lpw` in **VP levels** 0-255 |
| `light_min`, `light_max` | the light range | **VP levels**, `auto` or 0-255; leave `auto` |

The floors work the same way. The profile's floors (device units converted to VP
levels) replace VP's built-in 100 / 100 / 60 (a non-built-in profile only). An
LED behaviour profile (`profile_file`) and `led_floor`, `led_floor_reactive`,
`led_floor_black` in `[led_drive.Default]` (or `[dynamic_drive.Default]`) still
win over them, in VP levels 0-255.

Prefer to put values in the profile and keep these cfg overrides absent. Check
the user's cfg for any of them and report what you find, especially a
`min_command_gap_ms` below the manual's interval.

At the end VP forces `cntr_assist` off when the profile has no
`[command.contrast]`, and `ftb_custom_dim` off when it lacks the custom dim
commands, whatever the cfg says.

### 6.4 Restore

- On exit, and when Dynamic Drive is disabled, VP sends `[restore] light`
  (`ceiling` = `led_ceiling`), contrast neutral when it drove contrast, and dim
  mode standard.
- The idle restore (`led_idle_restore_s`, default 10 s with no video) still
  uses `led_ceiling`, not `[restore] light`. For a profile whose `[restore]
  light` is a number other than the maximum, the idle restore goes to the
  ceiling (default full light). Say this to the user if the two differ.
- A crash restart and the start-up state also use `led_ceiling`.

## 7. Do you need replies?

With `read_back = no` VP never reads replies. You need the reply grammar only
for the connection test or for a user who wants read-back. If the manual's reply
format is unclear, write a send-only profile (`read_back_supported = no`) and
say so. Do not guess a pattern.

## 8. Finding each value in the projector's manual

Look in the chapter usually called "RS-232 control", "Serial communication",
"Communication protocol", "External control" or "Command reference". Some
projectors have a separate protocol document. Record the section or page number
for every fact.

### 8.1 Where to look

| Profile value | Where it usually is | Pitfalls |
| --- | --- | --- |
| `baud`, `data_bits`, `parity`, `stop_bits`, `flow_control` | A "serial settings" table, often written like "19200, 8N1". Some projectors have a menu setting for baud. | "8N1" = 8 data bits, no parity, 1 stop bit. A projector set to another baud in its menu needs that baud: ask the user what the menu shows. Use the RS-232 entry, not RS-422. |
| `prefix`, command text, `suffix` | The "command format" or "syntax" section, usually with an example such as `:LPW1255<CR>` or `LIGHT 050<CR>`. | `<CR>` = 0x0D = `\r`; `<LF>` = `\n`; `<CR><LF>` = `\r\n`. A space in an example may be part of the command or only layout: check a second example. The suffix is what the **projector** expects to receive. |
| Light command | A command called brightness, light output, laser power, LED level or similar. | Do **not** use a power on/off, standby or "lamp power" entry. If the entry switches the light source on or off, it is not the command. Use only a command that sets a level. |
| Value format | The entry's parameter column. | **Percent versus absolute**: write the range the manual accepts for that command. **Hex versus decimal**: `FF` and `255` differ (`{v:02X}` versus `{v}`). **Zero padding**: `050` needs `{v:03d}`. Copy the example's width. |
| Contrast command | "Contrast". | Not brightness (black level) and not a lamp mode. See the contrast warning in 5.3. |
| Minimum interval, "busy", rate limits | "Command interval", "Timing", "Note", "Caution". Typical wording: wait at least N ms between commands; wait for the reply; after a missing reply wait N s; no more than N commands in a period. | `min_command_gap_ms` must be at or above the largest interval stated. A burst limit goes in `burst_commands` and `burst_pause_ms`. "Wait 2 s after a missing reply" is `no_reply_backoff_ms`. If the manual says to wait for the reply, keep a gap well above the projector's usual reply time. |
| Restore value | The projector's normal state: full light. | Ask the user which level they use normally. It is usually the maximum. |

### 8.2 Replies

- **Echo mistaken for a reply.** Some projectors or serial adapters repeat each
  command before the real reply (`:LPW1255` then `%001 LPW1 000255`). The
  pattern must match the real reply. VP logs lines that do not match as
  `projector reply "..." ignored while waiting for ...`.
- **LF versus CR.** Replies must end with CR. For LF-only replies set
  `read_back_supported = no`.
- The value must be a decimal number in one capture group and the command name
  in another. Allow for status numbers (`%001`) without capturing them. Replies
  with no name are not supported. A rejected command's `NAK` or error line does
  not match and is ignored, which is correct.
- Some projectors reply only after a handshake command.

### 8.3 Light-source behaviour

A projector that ramps its light over seconds needs a longer `settle_ms` and may
not follow Dynamic Drive's steps; say so plainly. Use this feature only for
light sources the manual describes as dimmable by command (laser, LED,
laser-phosphor), never for a lamp, and tell the user if the manual warns that
frequent changes shorten the light source's life. If the light command works
only in a certain mode, ask the user to set it and record it in `notes`.

### 8.4 When the manual is out of scope for schema 1

Stop and tell the user, with the reason, if the manual shows:

- a **binary** protocol, a **checksum**, a length field, or a header that
  varies by command;
- a **login or password** step (do not ask for or store the password);
- a need to **power the projector on** over RS-232 first (VP never sends power
  commands);
- **LF-only** replies when read-back is needed (a send-only profile with
  `read_back_supported = no` is still possible);
- an interface that is **not serial** (network only, USB only);
- no command that sets the light level absolutely (only up/down or presets).

Do not bend the schema to fit. Say what is not supported.

## 9. Dry run, connection test, exercise and the log

The runtime log of the deployed VP is `C:\Videoprocessor\vp\logs\vp.log`.
Ask the user to confirm that its modification time matches the running VP, and
to start VP from `C:\Videoprocessor\vp` so the log and relative paths resolve
there.

### 9.1 Dry run

A dry run never opens the serial port: use `mode = log_only` or
`protocol = none` with the profile selected. VP logs (quoted from the code):

- `Dynamic Drive: projector dry run - commands are logged, nothing is sent`
- `Dynamic Drive: dry run, would set LPW1 %d` and `... would set CNTR %d`
- `Dynamic Drive: running mode=%s protocol=%s tick=%d ms gamma_boost=%.2f led_latency=%d ms`
- `Dynamic Drive: device profile %s%s%s` (the profile's display name, then
  ` from ` and its path for a file; shown when the protocol is not `none`)

`LPW1` and `CNTR` are VP's historic names for the light and contrast commands.
The `would set` values are **VP levels** (light 0-255, contrast 50-100), not
device units. They show that the levels move in plausible steps and respect the
floors. They do not show the bytes: check the bytes with the validator's
example output (section 4, step 5).

The controller's decision lines (every send, otherwise about every ten
seconds) are `LED compensate: apl=... light A/B sent ...` and
`LED script: apl=... light A/B sent contrast C/D ...`: current level, target,
and `sent` when a command was sent, in VP levels.

### 9.2 Connection test

Config, Dynamic Drive page, "Test connection". The projector must be visible
and VP not playing video. The steps use VP levels: light to 60, read back, wait
5 seconds, light to 255 (or the projector's reported maximum when lower), read
back, pass or fail. Device conversion happens as for any command. VP level 60 is
about 24 percent of the range, and may be below the profile's floors: tell the
user to expect a visible dim.

- With `read_back_supported = no` the test **sends nothing**. It reports
  `UNVERIFIED: the device profile ... has no read-back, so the projector cannot
  confirm commands and nothing was sent; check the light changes by eye with
  Dynamic Drive running`. The user checks by eye during the supervised exercise.
- With a refused profile it reports `FAIL: device profile refused: <reason>`.
- Otherwise the steps appear in Config's details view (for example
  `LED 60: confirmed 60`, `LED %d: projector reports %d`,
  `LED %d: not sent (no acknowledgement within %d ms)`).

### 9.3 Log lines when the port is open

(`%s` and `%d` are filled in at run time.)

| Line | Meaning |
| --- | --- |
| `Dynamic Drive: WARNING device profile refused (%s); no Dynamic Drive - nothing is sent to the projector` | The profile was refused (6.2). No Dynamic Drive runs. Also logged when the configuration is applied. |
| `Dynamic Drive: WARNING command_source internal_script needs a contrast command, which the device profile %s does not have; no Dynamic Drive - nothing is sent to the projector (use compensate)` | Profile has no contrast; the script source is refused. |
| `Dynamic Drive: barco_ascii on %s %d baud, framing=%s acknowledged=%d range=%s device=%s` | The port opened. Check the baud, the framing, whether the projector acknowledges (1) or not (0), and the device name. |
| `Dynamic Drive: read_back off; sending with framing "%s" without waiting for replies` | Read-back off; VP uses this framing. |
| `Dynamic Drive: projector did not answer with \r or cr\r framing; sending the old script's exact bytes without waiting for replies` | Read-back on, `terminator` auto, no answer; VP fell back to the send-only framing. |
| `Dynamic Drive: %s %d not sent (%s): %s` | A value outside the declared device range was refused (VP name, VP value, profile, reason). Never sent. |
| `Dynamic Drive: no %s reply within %d ms to "%s"` | A reply was awaited and did not arrive. |
| `Dynamic Drive: projector reply "%s" ignored while waiting for %s` | A line that did not match the pattern or the expected name (often an echo). |
| `Dynamic Drive: projector write of "%s" failed: %s (%d failed write(s) since the last report)` | A serial write failed. Check cable, port and baud. |
| `Dynamic Drive: projector found: light=... contrast=... range=...; light set to N; contrast set to 50` | Start-up state read and first commands (VP levels). |
| `Dynamic Drive: light restored to %d%s` | The restore (VP level; for exit this is `[restore] light` converted). `%s` is empty or ` (not confirmed)`. With read-back off, `(not confirmed)` is expected. |
| `Dynamic Drive: contrast restored to 50%s`, `Dynamic Drive: LED dim mode restored to standard%s` | Contrast (VP neutral, sent as the profile's `[contrast] min`) and dim mode restored. |
| `Dynamic Drive: no video for %d s; LED back to %d, CNTR 50 (led_idle_restore_s)` | The idle restore (uses `led_ceiling`). |

What the log can and cannot show: it records VP's decisions, the framing and
the restores. It has **no per-command timestamp** of what the driver wrote
(VP's internal landing model uses the driver's write times, but no line reports
them). So the log cannot give the projector's response time. Measure latency
and settle with a camera or a light meter (section 10).

### 9.4 Supervised exercise

With the projector visible and the user present, ask the user to play a clip
with Dynamic Drive on and look for these, in order:

1. A slow fade to black: smooth change. A staircase means `max_step` or the
   light curve need work.
2. A hard cut from dark to bright: no flash and no dip. A flash or a dip means
   `latency_ms` is wrong (section 10).
3. A static mid-grey scene for 30 seconds: no breathing.
4. A full-field 5 to 10 percent grey: it stays still.
5. Stopping VP: the projector must return to the restore light by itself.
   Ask the user to confirm this by eye in the first sessions. If it does not,
   switch Dynamic Drive off and fix the restore values.

Stop at once if the projector does anything unexpected: flicker, a mode change,
an on-screen message, an error code.

## 10. Measuring latency and settle time

`latency_ms` is the dead time from the command to the light starting to move.
`settle_ms` is the time constant of the settle afterwards (63 percent of the
step after `settle_ms`).

VP sends commands ahead of a cut by the latency and models the light's approach
to choose the picture gain. A model faster than the real light gets the picture
brightness wrong during the change, so longer values are the safe error. When
the look-ahead covers `latency_ms + settle_ms` VP may go down to `floor`;
otherwise it uses the higher `floor_reactive`.

### 10.1 Conservative starting values

| Key | Starting value |
| --- | --- |
| `latency_ms`, `settle_ms` | 250, 1000 (or the manual's response time plus margin) |
| `floor`, `floor_reactive` | about 60 to 70 percent of the light range |
| `floor_black` | about 40 to 50 percent of the light range |
| `max_step` | no more than 5 percent of the light range |
| `min_command_gap_ms` | the manual's minimum interval, or 500 if none is stated |

Mark these as estimates in the file. For comparison, the FL35's values are
`latency_ms = 80`, `settle_ms = 300`, floors 100 / 100 / 60 of 255 and
`max_step = 12`; they do not carry over.

### 10.2 Measuring

Without instruments: show a bright white field, make one light step (the
connection test, or by hand in the menu), and ask the user how quickly the
brightness changes: instantly, under half a second, one to two seconds, more.
Set `settle_ms` to the longer end and `latency_ms` to the delay before anything
moved, plus margin. Call it a rough estimate in the hand-back.

With a camera: put the screen and the serial adapter's TX light (or a PC
display showing the moment of the command) in one shot. Film at 60 frames per
second or faster, exposure and focus locked. Step through frame by frame:
`latency_ms` = command to first visible change; `settle_ms` = from there to
about 63 percent of the change (95 percent takes about three times as long).
Repeat for a rise, a fall and several step sizes, and use the larger values.

With a light meter: point it at the screen centre, show a white field, log at
the meter's highest rate and step the light through the range the profile uses.
`latency_ms` = command to first change; `settle_ms` = time to 63 percent.
Readings at three or more drive values also give `[light] curve`; leave it
empty without them.

### 10.3 Trying values without editing the profile

Back up `VideoProcessor.cfg` (timestamped copy). In `[projector.Default]` set
`led_latency_ms` and `led_settle_ms` (they override the profile). Apply in the
Config app (the running module logs `Dynamic Drive: settings applied to the
running module (...)`) or restart VP; the start-up line
`Dynamic Drive: running ... led_latency=%d ms` shows the value in use. Repeat
the hard-cut test (9.4, item 2). When the values are settled, write them into
the profile, validate again, and **remove** the cfg overrides so they do not
hide the profile.

## 11. Validation checklist

The validator (5.5) is necessary, not sufficient. Run it after every change.

### Only someone with the manual can check

Tick each item and put the manual's section number beside it in the hand-back.

- [ ] Baud, data bits, parity, stop bits and flow control match the manual and
      the projector's menu. The validator's `transport:` line shows what you
      wrote.
- [ ] The example bytes in the validator output equal the manual's example
      character by character: prefix, command, value width, case, terminator.
- [ ] `[light] min` and `max` equal the manual's range for that command,
      exactly, in the manual's units (percent, steps, hex or decimal).
- [ ] `[restore] light` gives the light the user wants after VP exits
      (normally full). `[restore] contrast` (if any) is the neutral value.
- [ ] Units: every floor, `max_step`, curve point and restore is in device
      units, not VP's 0-255.
- [ ] `max_step` is small (at most 5 percent of the range until measured) and
      not 0.
- [ ] `min_command_gap_ms` is at or above the manual's minimum interval; burst
      limits are set if the manual has them; the user's cfg does not override
      the gap to a smaller value.
- [ ] The floors are not below what the manual or the light source allows.
- [ ] There is no power, lamp, standby, input, reset, firmware or factory
      command anywhere in the file.
- [ ] `reviewed = yes` is absent, or the user explicitly approved it for a named
      template.
- [ ] `read_back = no`. `read_back_supported` is `yes` only if the manual gives
      a CR-terminated reply with a name and a decimal value and the pattern was
      checked against real or manual-quoted replies.
- [ ] Contrast is present only if the control behaves as in 5.3 and the user
      agreed.
- [ ] Custom dim is present only if all three channels have `min = 0` and a
      `max` of at least 40 in the manual, and dim mode is present.
- [ ] Estimates are marked as estimates. Nothing was guessed silently.
- [ ] No manual text was copied into the file.

## 12. Worked example: the Barco F35 / FL35 profile

This is `profiles\dynamic-drivearco-fl35.vpprojector`, field by field. Read
the file beside this section. The same profile is compiled into VP, and a test
checks that the file and the compiled-in values drive identical command bytes.
The facts come from the Barco RS232 ASCII protocol document (601-0381-00) and
VP's earlier FL35 driver. No manual section numbers are given because this
guide does not copy the manual; when you work from a manual, cite its sections.

| Field | Value | Where it came from and why |
| --- | --- | --- |
| `[profile]` `schema`, `maker`, `model`, `version`, `source` | `1`, `Barco`, `F35 / FL35`, `1`, the protocol document's number | Display text and a reference. `reviewed` is not written (so `no`): no FL35 template contains a risky word. |
| `[transport]` `baud`, `data_bits`, `parity`, `stop_bits`, `flow_control` | `19200`, `8`, `none`, `1`, `none` | The protocol document's serial settings (19200 8N1, no flow control). |
| `read_back_supported` | `yes` | The FL35 answers, so the connection test can verify. |
| `read_back` | `no` | The owner's rule: send only, never wait, unless the user turns read-back on. |
| `reply_line_end` | `cr` | Replies end with CR. |
| `[framing]` `prefix` | `:` | Every FL35 command starts with a colon. |
| `[framing.cr]` | `suffix = \r`, `probe = yes` | `:MNEMvalue\r`, the documented form. |
| `[framing.literal_cr]` | `suffix = cr\r`, `probe = yes` | The same with the letters `cr` before the CR (`:LPW1255cr\r`), a form seen in the field. |
| `[framing.script]` | `suffix = cr\r\n`, `case = lower`, `probe = no` | The old control script's exact bytes: lower-case mnemonic, `cr`, CR LF (`:lpw1255cr\r\n`). `case = lower` changes only the template's literal text. Never waited on. |
| `send_only`, `fallback` | `script`, `script` | With read-back off VP sends with `script`. With read-back on and `terminator = auto` VP tries `probe_order = cr, literal_cr` using the handshake and a light query, then falls back to `script`. These names are what the cfg's `terminator` accepts. |
| `[reply]` `pattern`, groups | `^\s*%\d{3}\s+([A-Za-z0-9]{4})\s+(\d{1,9})\s*$`, name 1, value 2 | Replies with echo on look like `%001 LPW1 000255`: a three-digit number, the four-letter name, the value (read as decimal 255). The pattern matches the whole line. |
| `[command.light]` | `set = LPW1{v}`, `query = LPW1?`, `query_min = LPW1 ?N`, `query_max = LPW1 ?M`, `reply = LPW1` | Light is `LPW1` plus a plain decimal value: `:LPW1255\r` with the `cr` framing. The queries read the level and the projector's lowest and highest accepted level (note the space). |
| `[command.contrast]` | `set = CNTR{v}`, `query = CNTR?`, `reply = CNTR` | The contrast control VP's assist models (50 neutral). |
| `[command.handshake]` | `send = ECHO1`, `reply = ECHO` | Turns reply echo on. VP sends it only with read-back on. |
| `[command.dim_mode]` | `set = LDMM{v}`, `standard = 0`, `custom = 1` | The LED dim mode. VP restores `standard`. |
| `[command.custom_red/green/blue]` | `LDCR{v}`, `LDCG{v}`, `LDCB{v}`, `min = 0`, `max = 40` | Per-colour custom dim, range 0 to 40: exactly what VP's custom dim uses, hence the schema's `min = 0`, `max >= 40`. |
| `[light]` `min`, `max` | `0`, `255` | `LPW1`'s range; the identity map with VP levels. |
| `floor`, `floor_reactive`, `floor_black` | `100`, `100`, `60` | VP's long-standing FL35 floors: with look-ahead cover, without it, and for true black. |
| `curve` | empty | The drive-to-light curve is not measured: light is proportional to `LPW1`. |
| `[contrast]` `min`, `max` | `50`, `100` | `CNTR`'s range. 50 is the normal contrast and VP's neutral. |
| `max_usable` | `74` | The highest `CNTR` that still adds light (a light-meter step). |
| `dmd_clip` | `59` | The `CNTR` at which white code 235 reaches the imager's maximum at full light. An estimate until a fixed-LED white sweep measures it. |
| `[timing]` `latency_ms`, `settle_ms` | `80`, `300` | Camera-measured estimates for the FL35's light. |
| `contrast_latency_ms`, `contrast_settle_ms` | `80`, `0` | Contrast acts as a pure dead time. |
| `ack_timeout_ms`, `no_reply_backoff_ms` | `300`, `2000` | Read-back only. Barco: wait at least 2 s after a missing reply. |
| `[limits]` `max_step` | `12` | The largest light drop in one command (a cut jump is sent whole). |
| `min_command_gap_ms` | `200` | Barco documents 500 ms. The old script ran 200 ms gaps for years without a lost command, so 200 is the established working value, **below the documented figure**. Do not copy that: for another projector use the manual's interval. |
| `burst_commands`, `burst_pause_ms` | `0`, `5000` | Barco documents a 5 s pause after 20 commands; the FL35 has run for years without it, so the pause is off. |
| `[restore]` `light`, `contrast` | `ceiling`, `50` | On exit and disable: light to `led_ceiling` (255 = full light) and `CNTR` to 50, which equals `[contrast] min`. |

What the validator prints for this file (`ValidateDeviceProfileReport`):

```
OK device profile <path>
  model: Barco F35 / FL35, profile version 1, schema 1
  transport: serial 19200 baud, 8 data bits, parity none, 1 stop bit(s), flow none; read-back supported, default off
  commands: light yes, contrast yes, handshake yes, dim mode yes, custom dim yes
  light 0-255, floors 100/100/60, max step 12, gap 200 ms
  timing: latency 80 ms, settle 300 ms
  restore: light ceiling, contrast 50
  framing cr: light max = ":LPW1255\r", contrast neutral = ":CNTR50\r"
  framing literal_cr: light max = ":LPW1255cr\r", contrast neutral = ":CNTR50cr\r"
  framing script (send-only): light max = ":lpw1255cr\r\n", contrast neutral = ":cntr50cr\r\n"
```

The last three lines are what you compare with the manual. An invalid file
prints `INVALID <reason>` and a second line saying VP would run no Dynamic
Drive with this profile selected.

## 13. Hypothetical second example (not a real projector)

This example is **invented** to show a different syntax. It describes no real
product. Never use it for a real projector: its commands, ranges and numbers
come from no manual. VP's repository also has a real, validator-passing
fixture of the same kind (a percent-scale light, no contrast, no read-back):
`test-fixtures\dynamic-drive\synthetic-percent.vpprojector`. Compare the two
when you are unsure of the syntax.

The imaginary manual says: 9600 baud, 8 data bits, no parity, 1 stop bit, no
flow control. Commands are text ending with CR. The light command is `LIGHT`, a
space, and a zero-padded 3-digit percent 000 to 100 (`LIGHT 050<CR>`). There is
no usable reply and no contrast command. At least 500 ms between commands.
Full light is 100.

```
# HYPOTHETICAL profile for an imaginary projector. Not a real product.
[profile]
schema = 1
maker = Example
model = Hypothetical 100
version = 1
source = imaginary manual, section 4.2 (command format) and 4.3 (light)
reviewed = no

[transport]
type = serial
baud = 9600
data_bits = 8
parity = none
stop_bits = 1
flow_control = none
# No reply the VP reader can use: send only.
read_back = no
read_back_supported = no

[framing]
send_only = cr

[framing.cr]
suffix = \r
case = upper
probe = no

[command.light]
set = LIGHT {v:03d}

[light]
# Percent, 0 to 100, as the manual accepts.
min = 0
max = 100
# Estimates, not measured: high floors until measured.
floor = 60
floor_reactive = 60
floor_black = 40
curve =

[timing]
# Estimates, not measured.
latency_ms = 250
settle_ms = 1000

[limits]
# 5 percent of the 100-step range.
max_step = 5
# The manual's minimum interval is 500 ms.
min_command_gap_ms = 500

[restore]
light = ceiling
```

Checked against every cross-check in `LoadDeviceProfile` (5.5): all required
keys are present; `send_only` names `[framing.cr]`; there is no `fallback`,
`probe_order`, `[reply]` or light `query` (all allowed or required absent with
`read_back_supported = no`), and `probe = no` agrees; `min` 0 is below `max`
100, the floors 60/60/40 lie within 0..100 and `floor_black` is at most both
other floors; `max_step` 5 is at most 100; `[restore] light = ceiling` needs no
range check (VP converts `led_ceiling` 255 to 100); there is no
`[command.contrast]`, hence no `[contrast]` and no `[restore] contrast`; there
is no dim mode and no custom channel; `LIGHT {v:03d}` has a `{v}`, renders as
`LIGHT 000` and `LIGHT 100`, and has no risky word, so `reviewed` stays `no`.

The validator should print `OK`, `commands: light yes, contrast no, handshake
no, dim mode no, custom dim no` and `framing cr (send-only): light max =
"LIGHT 100\r"`. VP would run it as follows: `cntr_assist` forced off,
`internal_script` refused, `compensate` works; the connection test sends
nothing and reports `UNVERIFIED`. This is expected output worked out by hand
from the rules, not captured from a run: if the real output differs, trust the
validator.

## 14. What to hand back to the user

1. **The profile file**, with a `#` comment above each value saying whether it
   is from the manual (and where), measured, or an estimate. Tell the user to
   copy it into `<VP folder>\profiles\dynamic-drive\`.
2. **The validator output**, in full, for the final file.
3. **Every estimate or unknown value** and how to measure it: `latency_ms` and
   `settle_ms` (section 10); floors and `max_step` (tighten by watching 9.4);
   `curve` (light-meter readings); contrast values and `dmd_clip`, if present;
   anything the manual left unclear (units, terminator, reply name).
4. **The citations**: the manual's section or page for each command, range,
   interval, serial setting and restore value. Facts, never copied text.
5. **The cfg overrides found** in `[projector.Default]` and the floor keys in
   `[led_drive.Default]` (6.3), noting each that differs from the profile.
6. **What was and was not tested**: validator, dry run, connection test (or
   `UNVERIFIED`), supervised exercise, restore-on-exit check.
7. **Remaining risks**, for example "the settle time is an estimate", "the
   restore value was not yet seen on the projector", "the idle restore uses
   `led_ceiling`".

Remind the user that Dynamic Drive stays disabled (or in a dry run) until the
validator says OK and the first supervised test has been done with the
projector visible.
