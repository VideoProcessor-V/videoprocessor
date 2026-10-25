# BTB/WTW preservation (`colour.extended_range`)

Unit: **colour** | side: processing | order: 175 | status: **built** | hook point: libplacebo

## Purpose / why

Keep below-black and above-white codes through every conversion (owner rule: both kept in all cases). Cross-cutting: VP relabels the source and its hooks do the conversion.

## Contract

- Input: picture + source/target colour
- Output: same, with BTB/WTW carried
- Geometry: n/a
- Delay share: 0 frames
- Look-ahead: none

## Settings

- Existing keys: `preserve_extended_range`, `extended_range_more_cases`
- Planned keys: (none)
- Tiers: always on

## Fallbacks

- "Fallbacks engaged" entry: BTB/WTW: clipped: <reason>
- Behaviour: Hook failure (latched), a late restore under libplacebo's frame mix, or smooth motion's mixed refreshes clip BTB (FallbackCatalog::BtbWtw; call site WP-4).

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: `colour.tone_gamut`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- ExtendedRange tests

Contract tests:
- BTB/WTW probe codes identical with every output slot on and off

Chain tests:
- chain gate BTB/WTW probes

## Known issues / open items

- (none yet)

## How the code works

Checked against the code 2026-10-25 (M2 step 4 + WP-13).

### Public API

- `ExtendedRange.h` (namespace `ExtendedRange`): `Decide(enabled, Settings, source, output, lut, lutType)` returns a `Decision` (`Reason` plus the modes: gamma/primaries conversion, LUT carry, late/scaled restore, HDR pass-through). `ForTemporal(decision, frameMixing)`: a late restore cannot follow libplacebo's frame mix (`TemporalMix`). `Describe(decision)`: one log line. `Prepare(decision, Hooks&, sourceColor, output, list, singlePlaneRgb)` relabels the source to the output's colour space and places the hooks (carry last at RGB, restore first at SCALED, late restore first at PRE_OUTPUT). `Hooks` owns the four `pl_hook`s (carry, restore, its texture form, late restore) and their float textures: `Owns(signature)`, `Release()`, `HoldsTextures()`, `CarryPasses()` (efficiency audit #7: one pass with two outputs, or two passes and why). The maths (`DescribeTransfer`, LUT sampling/extrapolation) is CPU-testable.
- `ExtendedRangeStage.h` (WP-13): the host glue as one object. `Prepare(Frame, sourceColor, targetColor, hooks)` (decision, relabel, hooks, both log lines), `NoteDisabledHooks(disabled, count)` (latches the feature off for the session), `LateRestore()`, `Release()`, `LastDecision()`, `MixClipsBtb()`, `Fallback(preserveRequested)` (the "Fallbacks engaged" decision through `FallbackCatalog::BtbWtw`).

### Data flow

The renderer (render thread, `RenderLocked`) fills `ExtendedRangeStage::Frame` from the active settings and the frame (peak detection on, temporal renderer active/mixing, one-plane RGB source, target LUT) and calls `Prepare` before the de-judder cache registration. The relabel flag goes to the temporal renderer (`historyColor`) and the crossfade. After each render the renderer passes libplacebo's disabled-hook list to `NoteDisabledHooks`. The crossfade mix (`RenderCrossfadeMix`) reads `LateRestore()`. The hooks run inside libplacebo's passes.

### Threading

Render thread only (decision, hooks, textures). No locks.

### Config keys actually read

Per renderer profile (display-rule overrides: `[vprenderer.Default]` and matching `[vprenderer.<name>]`):

- `preserve_extended_range`: `on` / `off`, default `on` (owner: BTB and WTW kept in all cases). The Original pathway sets it `off`.
- `extended_range_more_cases`: `on` / `off`, default `on`: the carry also for BT.1886/sRGB, SDR in other primaries, around a calibration LUT and for HDR passed through unmapped (`off` = the Stage 10 eligibility).
- Not a key: peak detection (`renderParams.peak_detect_params`) makes HDR dynamic tone mapping a refusal (`HdrMapped`).

### Fallbacks a user sees

- `BTB/WTW` under "Fallbacks engaged" (ADR-0001/0002) when preserve is on and libplacebo disabled a carry hook (latched until restart), a late restore met libplacebo's frame mix, or libplacebo's smooth motion clips BTB on mixed refreshes. Wording: `FallbackCatalog::BtbWtw`. The renderer call site is WP-4's (`silent-fallbacks-callsites.patch`); after the WP-13 rebase it reads `extendedRange.Fallback(...)`. On a base without WP-4 these cases reach only the log.
- Design exclusions (HDR mapped, output primaries, unsupported LUT, ...) are not downgrades: log/status only.

### What it needs to be lifted

libplacebo (hooks, dispatch, shaders), `pch.h`, `FallbackCatalog.h` (VP-V core, header-only), `DebugLog.h` for the stage's log lines. No renderer state: everything comes in through `Frame`.

### Test it standalone

- CPU: `ExtendedRangeCarryTests` (maths, Decide); `OutputGlueStagesTests` methods `ExtendedRange*` (the stage against a copy of today's host code, the failure latch, the fallback decision).
- GPU (WARP unless `VP_TEST_GPU=hardware`; only when GPU tests are allowed): `LibplaceboLutParserTests` extended-range methods, `CrossfadeMixTests`, `OutputGlueStagesGpuTests.ExtendedRangeStageRendersTodaysCodes` (Category GpuHardware).

`vstest.console.exe x64\Release\VideoProcessor-Test.dll /Platform:x64 /TestCaseFilter:"FullyQualifiedName~ExtendedRangeCarryTests|FullyQualifiedName~OutputGlueStagesTests" /Logger:"trx;LogFileName=extended_range.trx"`

## Where the code lives (live file list)

Code location: **migrated**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/modules/colour/extended_range/ExtendedRange.cpp`
- `src/VP-V/modules/colour/extended_range/ExtendedRange.h`
- `src/VP-V/modules/colour/extended_range/ExtendedRangeStage.h`
- `src/VP-V/modules/colour/extended_range/ExtendedRangeStage.cpp`

Forwarding headers (legacy path; removed in M2 step 0): `src/VideoProcessor-Lib/vprenderer/ExtendedRange.h`.

The forwarder stays for LibplaceboVideoRenderer.cpp (Bill's host) and two upstream test files (LibplaceboLutParserTests.cpp, MotionRendererTests.cpp), which this move does not edit. ExtendedRangeStage.* is the host glue (WP-13): the host's call sites are in docs/design/m2-output-callsites.patch.

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0002 in [docs/decisions](../../../../../docs/decisions/README.md)

<!-- vpv-settings-tests:begin (generated by tools/vpv_tests/seed_module_readmes.py) -->

## Settings

- Keys: `preserve_extended_range`, `extended_range_more_cases`.
- Tiers: always on.
- VP-V page, Colour tab: not tiered (libplacebo does the colour in every tier; the cheaper combined HDR->SDR LUT for Classic/Low is not yet implemented.).
- Listed under the tier: `preserve_extended_range`.
- Plain-language guide: [docs/settings/VP-settings-reference.md#6-colour](../../../../../docs/settings/VP-settings-reference.md#6-colour).
- Fallback entry ("Fallbacks engaged"): `BTB/WTW: clipped (<reason>)`.

## Tests

- Placeholder (byte-identity on/off, zero cost): n/a.
- Isolation (fixed inputs, expected outputs, cost): ExtendedRange tests.
- Contract (formats, geometry, fallback entry): BTB/WTW probe codes identical with every output slot on and off.
- Chain (total delay at the 4/3/2-frame budgets): chain gate BTB/WTW probes.
- Combinations: pairwise / high-risk / random lines in `tests/vpv/` that set this slot's unit (`tools/run_combinations.ps1`); matrix: `docs/design/slot-test-matrix.md`.

<!-- vpv-settings-tests:end -->
