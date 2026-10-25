# WP-11: generic module status entry, VP status text, VP Ctrl+I and shortcuts out of the dialog

Work package WP-11 of `docs/design/integration-batch-plan.md` (branch `docs/integration-batch-plan`).
Branch `feat/wp11-status`, from `chore/early-migrations` ac49a1de.

The rule is "host = call sites only": the logic, the wording and the tests are committed on this
branch as new files; the changes to Bill's files are this patch, `wp11-callsites.patch`, for the
integrator to apply.

## Apply

```
git apply --check --cached docs/design/wp11-callsites.patch   # passes at the commit that adds it
git apply docs/design/wp11-callsites.patch
```

Generated against this branch's base files (`chore/early-migrations` ac49a1de). On a later base
use `git apply -3`. The renderer hunk is two small, self-contained insertions.

**Integrator: assign the plugin API number.** The patch leaves
`VP_LIBPLACEBO_PLUGIN_API_VERSION = 42;   // API: next (WP-11) - integrator assigns` and a
"next (WP-11 ...)" history line. Raise it to the next free number when merging (after feat/dv-l8's
`ToggleDvCompareCapture`, which takes 43 on `integ/batch1`). The two new virtuals are appended
last in `IVideoRenderer`.

## What it changes

### Upstreamable (offer to Bill, with the owner's approval per PR)

Self-contained, no `src/VP-V` dependency:

| File | Change |
|---|---|
| `src/VideoProcessor-Lib/ModuleStatusText.h` (new, committed) | The key/value status format: `key=value` lines, order kept, malformed lines and unknown keys skipped, one line per value, `module=` index for the empty module id, `status=` line in a command reply. Header-only, C++14, templated on the character type (no code-page conversion between a CString renderer and host) |
| `src/VideoProcessor-Test/ModuleStatusTextTests.cpp` (new, committed) | Format tests |
| `IRenderer.h` | `GetModuleStatus(const char* module, CString& status) const` and `InvokeModuleCommand(const char* module, const char* command, CString& reply)`, both defaulting to "not supported" |
| `LibplaceboRendererPluginApi.h` | History line "next (WP-11)"; the version placeholder |
| `LibplaceboPluginVideoRenderer.h/.cpp` | The proxy forwards both entries |
| `StatsOverlayWindow.h/.cpp` | Generic extension rows: `StatsExtraRow {label, value}` and two lists (`extraRowsAfterShaderRule`, `extraRowsAfterShaders`), drawn label-column + value (or the value alone without a label). This replaces VP's nine dedicated `StatsData` fields. For Bill's row-based OSD the same rows map one to one onto his `add(label, value, false)`, so an upstream version is two loops in `BuildStatsRows` |
| Test project | Registers `ModuleStatusTextTests.cpp` |

No new DLL export is needed: both entries are vtable methods, guarded by the API version and
the build-ID check as before.

Why two entries: status (`GetModuleStatus`) and actions (`InvokeModuleCommand`, e.g. a capture
toggle bound to a shortcut). After these, a new status line or renderer-side shortcut action
needs no new virtual and no version bump; only the module's keys/commands grow.

### VP-only

| File | Change |
|---|---|
| `LibplaceboVideoRenderer.h/.cpp` | `GetModuleStatus` answers module `vp` (keys `dejudder`, `fallbacks`, `led_drive`, `source_resolution`) and the index, one call to `VpStatusText::RendererModuleStatus` reading the existing getters (17 lines). Include of `VP-V/core/StatusText.h` |
| `VideoProcessorDlg.cpp` | VP's Ctrl+I text, crash-state keys, report lines and shortcuts become one call each into `src/VP-V/gui/VpStatusAdapter.h`: one `VpvGui::Poll` per stats refresh feeds `RecordCrashState`, `UpdateFallbacksStatus(heading)` and `FillCtrlIRows`; the Ctrl+Alt+D lines are `AppendBundleSettings`; `ContentIntentLine` / `AmbientOverlayLine` / `KeyOverrideSummary` keep only their fact gathering; the three VP entries leave `SHORTCUT_DEFINITIONS` (rebuilt in the same order by `VpvShortcuts::Merge`), their `ON_COMMAND`s become one `ON_COMMAND_RANGE` to `OnVpCommand` -> `VpvGui::DispatchCommand`; `OnCommandStateReport` and `OnCommandRawCaptureToggle` are gone |
| `VideoProcessorDlg.h` | `OnVpCommand`; `UpdateFallbacksStatus(const std::string&)`; two declarations removed |
| `Resource.h` | `ID_COMMAND_ROOM_LIGHTS` / `STATE_REPORT` / `RAW_CAPTURE_TOGGLE` move to `src/VP-V/gui/VpShortcuts.h` (same values; the block 33206-33299 is VP's). Our `Resource.h` diff against Bill's beta becomes empty |
| Test project | Registers `src/VP-V/tests/StatusTextTests.cpp` and `VpStatusAdapterTests.cpp` |

New files on the branch (VP-only): `src/VP-V/core/StatusText.h`, `src/VP-V/gui/VpStatusAdapter.h`,
`src/VP-V/gui/VpShortcuts.h`, `src/VP-V/gui/README.md`, the two tests.

### Effect on the patch set against Bill (end-state rule: smaller or no larger)

- `StatsOverlayWindow.cpp/.h`: VP's nine fields, nine draw blocks and nine height terms become
  two generic lists, one 11-line draw lambda and one height line. The forecast's 803-line
  conflict there was our per-line blocks against his rewrite; after this patch our side of that
  file is the generic rows only.
- `VideoProcessorDlg.cpp`: +55/-119 (net -64); VP text building leaves the file.
- `Resource.h`: our diff disappears.
- `IRenderer.h` / plugin API: +2 virtuals now, and no further growth per VP status line.

## Byte identity (Ctrl+I, crash state, report)

`src/VP-V/tests/VpStatusAdapterTests.cpp` holds verbatim copies of the old code (dialog fill,
overlay draw, crash-state block, report block, content/room/key-override lines) and compares
them with the new path (fake renderer answering `GetModuleStatus` exactly like the patched
renderer -> adapter -> new overlay draw) for: everything on; quiet (fallbacks none, LED off); known
but empty lines; a non-VP-Renderer renderer (de-judder hidden); a renderer without the VP module
(DirectShow); no renderer; every content/room/intent variant; non-ASCII text. Drawn text, row
counts (overlay height), crash-state map, fallbacks heading and report lines are all equal.

Notes:
- Renderer lines travel as wide text end to end (no conversion). The crash state and the report
  narrow them with `CStringA`, as before.
- The content line is built narrow and converted with `%hs`, as the old `Format("%hs")` did. The
  room line and key overrides convert as before (ANSI code page for narrow parts).
- Values are escaped in the module text (backslash, newline, carriage return) and restored
  exactly by Parse, so a reason holding an error string with a newline is unchanged (audit
  LOW-1; test `NewlinesAndBackslashesInRendererLinesAreIdentical`).

## Lines that stay where they are (and why)

- **De-judder line composition** stays inside the renderer (`PublishDejudderStatus`, render-path
  state): it moves with the motion/timing extraction (WP-7). It already reaches the GUI through
  the `dejudder` key.
- **LED line** (`GetLedDriveStatus`'s text) stays for WP-15 (LED glue); it reaches the GUI
  through `led_drive`.
- **Nets line:** not in the tree yet (WP-10's draft adds it to the renderer's shader
  descriptions). `VpStatusText::NetsLine` reproduces the draft's text byte for byte (tested), so
  WP-10 calls it instead of composing the line in the renderer. The draft's design note says
  "A (...), D (...)" with a comma; the draft code has none. The builder follows the code; change
  both together if the comma is wanted.
- **DV:** Ctrl+I has no DV line today. The DV luminance rows are the operator view's
  (`ModernOperatorView`, Bill's API 28 plus our scene minimum) and stay.
- **B3:** B3's downgrades appear inside "Fallbacks engaged" (the `fallbacks` key); no separate
  line exists.
- **The renderer's shader-description lines** (adaptive downscale, source resolution, deband,
  detail pulse, texture restore) stay in `ActiveShaders()`: moving them would change the
  "Shaders: N Active" count. They can become module keys as their glue moves (WP-6..18).
- **Room lights and report-a-problem handlers** are dispatched by the adapter but their bodies
  stay in the dialog for now (ambient glue: WP-16; diagnostics glue: WP-17). Raw capture's
  handler moved into the adapter.
- **The old per-line getters** (`GetDejudderStatus`, `GetLedDriveStatus`,
  `GetSourceResolutionStatus`, `GetFallbacksStatus`, API 25-41) stay in the vtable; the GUI no
  longer calls the VP ones. Retiring them (a further `IRenderer.h` shrink against Bill) is a
  follow-up once nothing else uses them.

## After rebasing onto integ/batch1 (feat/dv-l8 merged)

feat/dv-l8 adds `ToggleDvCompareCapture` (API 43), `ID_COMMAND_DV_COMPARE_CAPTURE 33209`, a
`dv_compare_capture` table entry and `OnCommandDvCompareCapture`. Under WP-11 these become:
the id in `VpShortcuts.h` (33209, inside the VP block), a `kDefinitions` entry
`{ "dv_compare_capture", ID_COMMAND_DV_COMPARE_CAPTURE, "raw_capture" }`, and a
`DispatchCommand` case calling `InvokeModuleCommand("vp", "dv_compare_capture", reply)` (the
renderer answers it with its toggle; the reply's `status` line is shown, else
"DV comparison capture: needs the VP Renderer"). `ToggleDvCompareCapture` can then leave the
vtable (status getters added since API 42 switch to the generic entry, plan WP-11 step 3).

## Found by the snapshot tests (fixed before the patch was cut)

- Reading `line.GetString()` as an argument beside the getter that fills `line` took the
  previous key's text (argument order is unspecified; MSVC goes right to left). Each line is now
  read after its getter, in the renderer hunk and in the test fake.
- `CString::Format` and `snprintf` round a `%.1f` tie differently (3.25 lux: 3.3 vs 3.2). The
  room line keeps the old `CString::Format` through a formatter the adapter passes.

## Verify

- Build: Lib, Test, GUI, VPRenderer (Release x64), with the patch applied: clean (logs
  `D:\codexideoprocessor\wp11-build1.log`, `wp11-build2.log`).
- CPU tests: `ModuleStatusTextTests`, `StatusTextTests`, `VpStatusAdapterTests`, plus
  `ShortcutDefaultsTests` and `R6BaselineTests`: 35/35 passed (TRX in
  `D:\codexideoprocessor\wp11-trx`). Targeted set after the audit fixes (area + PipelineSkeleton, DelayGate, CombinationRunner,
  NativeStatsOverlayPlacement): 75/75 (`wp11-targeted.trx`). The full filtered suite runs once per
  integration group on integ/batch1 (coordinator policy 2026-10-25).
- Big test (WP-20): one on-screen Ctrl+I look (coordinator).
