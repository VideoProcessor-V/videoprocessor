# Dynamic Drive projector profiles - report (2026-10-17)

Branch `feat/dd-projector-profiles`, worktree `D:\codex\videoprocessor\dd-profiles`, from local
`integ/2026-10-17` at 0642f68c. Local only (the GitHub account is suspended): nothing pushed, no CI.

## Format: VP's cfg format, `*.vpprojector`
`profiles\dynamic-drive\barco-fl35.vpprojector`. Chosen over JSON because:
- it is parsed by VP's existing `ConfigFile` and validated with `ConfigSchema` rules, so the
  errors have the same shape as every other cfg error (`[section] key 'k' must be ...; got '...'`);
- comments are allowed, so a profile can cite manual sections next to each value - important
  for AI-authored files that a human must check;
- the Lib has no JSON parser (only the Config app's Qt has one), and the validator must run in
  VP, the Config app and the tests from one header;
- the owner had accepted the INI `.vpprojector` shape in docs/VP-projector-profiles-design.md.

## What moved into the profile (DeviceProfile.h)
- **Identity:** `[profile]` schema (1), maker, model, version, source, reviewed.
- **Transport:** baud, data bits, parity, stop bits, flow control (Win32SerialPort applies
  them), `read_back` (default no, the owner's rule), `read_back_supported`.
- **Framing and commands:** prefix, framings (suffix, letter case, probe), probe order,
  fallback, send-only framing; templates for light set/query/min/max, contrast set/query,
  handshake, dim mode, custom dim R/G/B (`{v}`, `{v:03d}`, `{v:02X}`, escapes); reply regex.
- **Ranges** (device units; VP's 0-255 light and 50-100 contrast map onto them), floors, light
  curve, contrast max usable and DMD clip.
- **Timing:** latency, settle, contrast latency/settle, ack timeout, no-reply back-off (Barco's 2 s).
- **Limits:** max step, min command gap, burst count and pause.
- **Restore:** light (ceiling or a value), contrast (= VP's neutral).

The compiled-in FL35 profile is `DeviceProfile`'s defaults; with no `device_profile` key VP is
unchanged. Precedence: built-in -> device profile -> explicit `[projector.Default]` keys
(-> LED profile -> `[led_drive.Default]` for floors). Code that stays: worker, coalescing,
generations, gap/burst timing, write-failure reopen, resync, bounded exit, the 200 ms retry floor.

## Safety
- Strict closed schema: unknown sections/keys, malformed lines, duplicates, missing required keys,
  out-of-range values and broken cross-checks are refused with the file, section and key named.
- Templates with POWR/LAMP/INPUT/RESET/FIRM... need `reviewed = yes`; there is no slot for power,
  lamp, input or firmware commands.
- A refused profile, a terminator the profile lacks, or read_back without read-back support:
  protocol forced to none, WARNING logged by the renderer and the module, Config shows the reason.
  The port is never opened.
- Every byte comes from a template; a value outside the declared device range is not sent
  (new: on the FL35 this refuses CNTR outside 50-100, LDCx above 40, LDMM other than 0/1 - VP
  never sends those).
- No contrast command: CNTR assist off and internal_script refused (it needs CNTR). No custom
  dim commands: ftb custom dim off. No dim mode: LDMM is skipped.

## Files
- `src\VideoProcessor-Lib\led\DeviceProfile.h` (new, header-only): schema, loader, renderer,
  reply matcher, offline report.
- `LedDriveConfig.h`: `ProjectorConfig::device`, `device_profile` key, ApplyDeviceProfile,
  ReadDeviceProfile, refusal rules, floors and capability gating.
- `BarcoAsciiDriver.{h,cpp}`: RenderSet/RenderQuery/TransactSet; framing selection from the
  profile; reply grammar and back-off from the profile.
- `SerialPort.{h,cpp}`: SetFormat. `ConnectionTest.cpp`: refused / UNVERIFIED (no read-back).
- `LedDriveModule.cpp`: refusal logs, script-needs-contrast, exit restore from the profile, a
  profile change is a new connection. `LibplaceboVideoRenderer.cpp`: profile file in the change
  signature, WARNING log.
- `VideoProcessorApp.cpp`: `/validate_dd_profile <file>`. `tools\validate_dd_profile.ps1`.
- `ConfigEditorWindow.cpp`: Dynamic Drive > Connection > "Device profile" (list, path, Browse,
  status, tooltip).
- `profiles\dynamic-drive\barco-fl35.vpprojector`, `packaging\release-manifest.json` entry,
  `docs\dynamic-drive\PROFILE-AUTHORING-FOR-AI-AGENTS.md`.
- Tests: `DeviceProfileTests.cpp`, frozen pre-profile driver `LedDriveReferenceDriver.{h,cpp}`,
  fixture `test-fixtures\dynamic-drive\synthetic-percent.vpprojector`.

## Tests
RESULTS_PLACEHOLDER

## Owner checks on the real FL35
- Run with no device_profile, then with `device_profile: profiles\dynamic-drive\barco-fl35.vpprojector`:
  the log line "barco_ascii on COMn ... device=Barco F35 / FL35 ..." and identical behaviour.
- Exit (and idle) restore: LED back to the ceiling, CNTR 50, LDMM 0.
- Not changed: idle restore still uses led_ceiling (not `[restore] light`).
- Config: the Device profile row lists the built-in and the shipped file; a broken file shows
  "VP will refuse this profile".
