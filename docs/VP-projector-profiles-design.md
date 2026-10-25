# Projector profiles for Dynamic Drive (design, 2026-10-17; not built)

Status: design only. Nothing here is built. Queue item 12 ("Dynamic Drive rename + projector
profiles", work queue 2026-10-17). File and line references are at merge-v1.4.02 (2026-10-17).

## Decided (owner, 2026-10-17)

- Open the LED engine to other projectors and displays: users load a custom projector profile
  file; the repo ships instructions for an AI agent to author the file from the device's control
  manual (validator, dry run, connection and exercise tests, template-only commands, restore on
  exit).
- The Barco FL35 becomes the first profile, with a byte-identical command stream (golden byte
  tests against today's driver).
- Shape accepted ("projector-profile design done"): `ProfileDriver` + INI `.vpprojector` files,
  a `CommandId` enum, `ITransport` (serial/TCP), validator, dry run, exercise test, AI authoring
  guide.
- The config key is `device_profile` (`profile_file` already names the LED behaviour profile).
- "LED drive" is renamed **Dynamic Drive** (UI labels, Config page, logs, docs). `[led_drive]` and
  `[projector.*]` keys keep working; `[dynamic_drive]` is accepted as an alias; no forced
  migration of user configuration. The rename is done with the code by the LED fix group; this
  document uses the new name only in its title.
- Dynamic Drive look-ahead depth = whatever the active queue profile has queued, in script and
  compensate mode; low latency (no queue) = current-frame fallback; `led_lookahead_ms` is an
  optional cap only (0 = whole queue).
- Read-back stays off by default; the script is the reference for intent (docs/VP-led-core-sources.md).

## Open

- Every format and UI detail below is a proposal until built and reviewed.
- Remote VP: RPC upload of a profile or a clear refusal.
- Contrast on non-Barco devices (needs a `contrast_gain_table`), iris devices, and TV backends
  (Sony, LG, Samsung; section 6): later, not scheduled.
- The effort table is in human-developer days; agent wall-clock is far shorter.

## Design report (design agent, 2026-10-17, read-only, merge-v1.4.02)

### Summary

The LED engine can be made to work with other projectors without rewriting its control logic. The FL35 driver keeps all its hard parts in code: the worker thread, coalescing, generations, gap and burst timing, write-failure reopen, resync and bounded exit. Only four small Barco-specific pieces need to become data:
- how a command is formatted;
- how a reply is parsed;
- the order in which framings are tried at startup;
- a handful of constants: the 2 s no-reply back-off, contrast "50", the ECHO handshake, and the LPW1/CNTR/LDMM/LDCx names.

The plan:
- Add an INI profile file (`projectors/<maker>-<model>.vpprojector`, parsed by the existing ConfigFile/ConfigSchema code).
- Turn `BarcoAsciiDriver` into a `ProfileDriver` that renders only the templates the profile declares.
- Make the FL35 the first built-in profile.
- Prove byte-identity by running the current 38 protocol tests and a set of recorded-byte scenarios against both drivers before the old driver is deleted.

Effort is about 3–4 weeks. When this was written `docs/VP-auto-calibration-design.md` was not on merge-v1.4.02, so the calibration fields below are designed from the current `[projector.*]` keys. (It is now in docs/; reconcile `[calibration]` with its outputs when building.)

### What is Barco-specific today (file:line)

- **Command names are strings in the interface:** `ProjectorDriver.h:94-139` (QueueSequence, SendNow and SendScript take `"LPW1"`, `"CNTR"`, `"LDMM"`). Callers are at `LedDriveModule.cpp:36-66` (custom dim LDCR/LDCG/LDCB/LDMM) and `:1194-1404`.
- **Framing:** `BarcoAsciiDriver.cpp:72-79` (`":"+name+arg+suffix`, lower-case option) and `:346-397` (tries cr, then literal_cr, then the script's bytes; ECHO 1; `LPW1?`). Range queries `" ?N"` / `" ?M"` are at `:418-421`.
- **Reply grammar:** `ParseReply` at `:54-70` (`%001 MNEM 000nnn`).
- **Constants:** the 2000 ms no-reply back-off is at `:262-270`; the 200 ms write-retry floor at `:266`; the resync default CNTR 50 at `:841`; contrast 50 in `ProjectorDriver.cpp:133-163` (BringToKnownState) and `:165-…` (RestoreFullLight).
- **Transport:** fixed at 8N1 with no flow control (`SerialPort.h:15-17`). `ReadLine` splits on `'\r'` (`:22-25`), so binary replies can't be read. There is no TCP transport.
- **Config:** `ProjectorConfig` is at `LedDriveConfig.h:59-112`; `protocol` accepts only `barco_ascii|none` (`:532`). The factory is at `ProjectorDriver.cpp:126-131`.
- **Connection test** forces `barco_ascii` and read-back on (`ConnectionTest.cpp:65-95`).
- **Config UI:** the protocol combo is at `ConfigEditorWindow.cpp:12964-12966`; the test is at `:13117` and goes through VP's RPC (`VideoProcessorDlg.cpp:3253`).
- **Name clash:** `profile_file` already means the *LED behaviour* profile (`LedDriveConfig.h:902-915`). The new key must be different; I propose `device_profile`.

### 1. Profile format (INI)

```
[profile]   schema=1  maker=Barco  model=F35/FL35  source="Barco 601-0381-00 §…"  reviewed=yes
[transport] type=serial|tcp  baud=19200 data_bits=8 parity=none stop_bits=1 flow=none
            host= port= keepalive_s=  reply_end=\r  reply_mode=line|fixed:N
[framing.cr]          prefix=:  suffix=\r    case=upper  acknowledged=auto
[framing.literal_cr]  prefix=:  suffix=cr\r  case=upper
[framing.script]      prefix=:  suffix=cr\r\n case=lower acknowledged=no
[framing]   try=cr,literal_cr   fallback=script   readback_off=from_terminator
[reply]     ack=^%(\d{3}) (?<cmd>\w{4}) (?<value>\d{1,9})$   error=^%\d{3} (?<cmd>\w{4}) !(?<code>\d+)
[command.handshake]  template=ECHO1      optional=yes
[command.light]      template=LPW1{v}    query=LPW1?  query_min=LPW1 ?N  query_max=LPW1 ?M
                     encoding=int|percent|steps:… |hex2  device_min=0 device_max=255  vp_map=identity|linear
[command.contrast]   template=CNTR{v}    query=CNTR?  range=50-100 neutral=50  kind=gain|iris
[command.dim_mode]   on=LDMM1  off=LDMM0
[command.custom_dim] channels=LDCR{v},LDCG{v},LDCB{v}  range=0-40
[command.power_query] template=…   (query only; a power *set* is rejected by the schema)
[timing]   min_command_gap_ms=500 burst_commands=20 burst_pause_ms=5000 ack_timeout_ms=300
           no_reply_backoff_ms=2000  led_latency_ms=80|measure  cntr_latency_ms=80|measure
[capabilities] light=yes contrast=yes dim_mode=yes custom_dim=yes readback=yes
[limits]   light_min=0 light_max=255 contrast_max_usable=74
           restore_light=ceiling restore_contrast=neutral restore_dim_mode=off
[calibration] flux_table=…  dmd_clip_cntr=59  contrast_gain_table=…   (optional)
```

- **Templates** support `{v}` with a format spec (`{v:03d}`, `{v:02X}`), escapes (`\r`, `\x02`) and `{checksum:sum8|xor8}` over a declared byte span.
- **Precedence**, mirroring `LedDriveConfig.h:1083-1099`: built-in defaults, then the profile, then `[projector.Default]`. Port, latencies and flux_table stay user overrides in the .cfg.
- **Calibration results** go to the user's .cfg, not into the shared profile.
- **Value units:** VP's internal units stay "light 0–255" and "contrast 50–100, 50 neutral, gain (c/50)^E", so the rate tables, floors and `max_levels_per_command` keep their meaning. The profile maps them to device units.
- **Contrast on other devices:** without a `contrast_gain_table`, contrast on a non-Barco device is limited to start/restore, and `cntr_assist` is forced off.
- **Iris:** `kind=iris` (an iris only reduces light) is accepted but can't be used by the assist in v1.

### 2. Generic driver

`ProfileDriver` is `BarcoAsciiDriver` with these changes:
- `Format()` becomes `profile.Render(CommandId, value)`.
- `ParseReply` becomes the regex from `[reply]`.
- `SelectFraming` walks `[framing] try/fallback`.
- The `2000` comes from `timing.no_reply_backoff_ms`.
- The `50` constants come from `contrast.neutral` / `limits.restore_contrast`.
- The ECHO in the resync (`:827`) comes from `command.handshake`.

The interface's `std::string mnemonic` becomes `enum class CommandId { Light, Contrast, DimMode, CustomR, CustomG, CustomB, Handshake }` (`ProjectorDriver.h:94-139` and the module call sites). `protocol: barco_ascii` stays as an alias for the built-in FL35 profile, embedded as a resource so it needs no file.

**Stays in code:**
- the worker and coalescing;
- `lightGeneration` / `contrastGeneration` cancellation, checked before and after the gap wait (`:87-127`);
- abortable waits;
- write-failure handling: 5 failures, close, reopen back-off from 2 s doubling to 30 s, rate-limited logs (`:162-186`, `:286-340`);
- the 200 ms retry floor (a safety rule, not data);
- the resync after a reopen;
- the bounded exit;
- the burst counter;
- the split dimming drop;
- read-back off by default (send-only, no handshake or queries, as at `:351-363`).

**Moves into data:** bytes, reply grammar, framing order, value encoding, ranges, timings, capabilities, restore values.

The transport becomes `ITransport`: `Open(TransportSpec)`, `Write`, `ReadLine` or `ReadFrame(n)`. `Win32SerialPort` gains parity, stop bits and flow control; `TcpTransport` uses Winsock with SO_KEEPALIVE and reconnects on the same back-off path.

### 3. Validation and safety

- **Schema:** `ProjectorProfileRules()` in the ConfigSchema style. Errors name the section, key and reason, for example `[command.light] template uses {v:02X} but encoding=int`. Cross-checks: range covers the limits; a regex compiles and has `value`; a checksum span is valid.
- **Closed set:** there is no raw/free-form command field. Unknown sections are an error. Power, lamp, input and firmware commands have no schema slot, so they can't be expressed.
- **Keyword warnings:** templates containing POWR/OFF/LAMP/RST/FACT/FW/UPD produce a warning that blocks loading unless `reviewed=yes`.
- **Runtime rule:** `Render` refuses any value outside the declared range and limits. Bytes are produced only by `Render`.
- **Dry run:** `[projector.Default] dry_run=yes` runs `ProfileDriver` over a `LoggingTransport` that logs the exact bytes. This is better than `NoneDriver`'s "would set LPW1".
- **Connection test**, generalised:
  1. open;
  2. handshake or a harmless query (light query, else power query), matching the ack regex;
  3. the existing 60/255 steps only when the device supports read-back.

  An ack-less device reports "UNVERIFIED — confirm visually" instead of FAIL. The test runs on the selected profile, not a forced `barco_ascii`.
- **Exercise test** (new `ExerciseTest.cpp`):
  - steps light from the ceiling down to the floor in about 6 steps, 3 s each;
  - then contrast neutral, up to the maximum usable, and back to neutral;
  - can be cancelled at any step;
  - always ends with `RestoreFullLight`;
  - each step is logged with its bytes.
- **Restore on exit or crash:** unchanged (`RestoreFullLight`, `LedShutdownRestore.h`, LDMM 0 at start), with the values coming from `[limits]`.

### 4. Config UI

- **Selection:** the "Protocol" combo becomes "Projector": None, built-ins from `<install>\projectors\` (read-only), user profiles from `%ProgramData%\VideoProcessor\projectors\`, and "Load custom profile…".
- **Loading a custom profile:** validate first; if a same-named file exists, keep it as `.bak-YYYYMMDD-HHMM`; copy the new file into the user folder; write `device_profile=<name>`.
- **Remote VP:** the existing note (`:12955`) says the file must be on the VP computer. For a remote VP the copy needs an RPC upload or a clear refusal.
- **Display:** a capabilities panel (light / contrast / dim / read-back / transport) and the validation result.
- **Buttons:** "Test connection" and "Exercise" reuse the RPC/local path at `:13095-13135`.
- **User data:** the installer never writes to the user folder or the .cfg.

### 5. Authoring guide: `docs/projector-profiles/AUTHORING-FOR-AI-AGENTS.md`

**Steps for the agent:**
1. Find the protocol section: transport settings, syntax, reply format, timing rules.
2. Fill `[transport]` and `[framing]`.
3. Map each VP capability to one command, or mark it `capability=no`.
4. Fill the templates, encodings and ranges, citing manual page or section numbers.
5. Mark unknowns as `measure` or `# UNKNOWN`.
6. Never add power, lamp, input, firmware or factory commands, and never guess.
7. Run `VideoProcessor-ConfigCore --validate-profile <file>`.
8. Produce the file plus a test plan: dry run, connection test, exercise test with the projector visible, then the latency measurement.
9. Ask the user to run each step and report the results.

**Worked FL35 example**, each field traced to its source: framings from `BarcoAsciiDriver.cpp:346-363`; reply from `:54-70`; gap and burst from `.h:11-16` (Barco's 500 ms, 5 s after 20); the 2 s back-off from `:262`; queries from `:418-421`; custom dim from `LedDriveModule.cpp:36-66`; latencies and `cntr_max_usable` from `LedDriveConfig.h:88-110`.

**Checklist and pitfalls:**
- response timing and minimum gaps;
- binary vs ASCII (`reply_mode=fixed`, checksums);
- ack-less devices (read-back off; the test is visual);
- echo of the sent command mistaken for an ack;
- TCP idle disconnects (keep-alive, reconnect);
- devices that need power-on or a login prompt;
- percent vs absolute values;
- slow light ramps inside the device (measure the latency).

### 6. Beyond projectors (later)

- **Sony Bravia:** REST picture settings over HTTP with a PSK, or Simple IP Control on TCP 20060.
- **LG webOS:** SSAP over WebSocket with pairing; backlight control goes through community-documented, unofficial endpoints.
- **Samsung:** consumer TVs have no official backlight API; commercial displays use MDC (TCP 1515, binary with checksum).

These need HTTP and WebSocket transports plus authentication. TV backlights also often ramp internally, which limits the dynamic use.

Licence and ToS, stated neutrally: some interfaces are unofficial and can change or conflict with the maker's terms. Manuals are copyrighted, so profiles should record command facts and section citations, not copied text.

### 7. Tests, files, effort

**Tests:**
- **Golden byte stream:** parameterise `LedDriveProtocolTests.cpp` (38 tests) over `BarcoAsciiDriver` and `ProfileDriver` with the FL35 profile.
- **Recorded scenarios:** run each framing (cr, literal_cr, script, read-back on and off) through handshake, SetLight with a split, Jump, CancelLight in the gap, SetContrastAsync(-1), sequence, SendScript, write failure and reopen, and restore. Compare `FakeProjector::writes` byte for byte (`LedDriveTestSupport.h:22-157`), and compare gaps within a tolerance.
- **Validator:** a table of bad profiles, each with its expected message.
- **Safety:**
  - an out-of-range value is not sent;
  - an undeclared command can't be expressed;
  - a power-set slot is rejected;
  - restore values come from the profile;
  - dry run writes nothing.
- **Transport:** a binary/checksum test and a fake TCP reconnect test.

**New files:**
- `led/ProjectorProfile.{h,cpp}` (parse, schema, Render, MatchReply)
- `led/ProfileDriver.{h,cpp}`
- `led/Transport.{h,cpp}` and `led/TcpTransport.cpp`
- `led/ExerciseTest.{h,cpp}`
- `projectors/barco-fl35.vpprojector`
- `docs/projector-profiles/AUTHORING-FOR-AI-AGENTS.md`
- `src/VideoProcessor-Test/ProjectorProfileTests.cpp`

**Effort:**

| Work | Days |
|---|---|
| Profile and validator | 3–4 |
| Driver refactor and golden tests | 4–5 |
| CommandId changes in the module | 1–2 |
| Transports | 2–3 |
| Connection and exercise tests | 2 |
| Config UI | 3–4 |
| Guide | 2 |

Delete `BarcoAsciiDriver` one release after the golden tests pass.

### Critical files for implementation
- src\VP-V\modules\drive\projector_control\BarcoAsciiDriver.cpp
- src\VP-V\modules\drive\projector_control\ProjectorDriver.h
- src\VP-V\modules\drive\led_compensation\LedDriveConfig.h
- src\VP-V\modules\drive\projector_control\SerialPort.h
- src\VideoProcessor-Test\LedDriveTestSupport.h
