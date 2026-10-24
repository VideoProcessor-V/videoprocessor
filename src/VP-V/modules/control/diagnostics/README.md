# Status, fallbacks + diagnostics (`control.diagnostics`)

Unit: **control** | side: control | order: 920 | status: **partial** | hook point: host

## Purpose / why

One 'Fallbacks engaged: (list)' heading in the Ctrl+I overlay, Config status and the Ctrl+Alt+D bundle; crash state ring; logs.

## Contract

- Input: fallback events from every slot
- Output: status lines + one-time transient notices
- Geometry: n/a
- Delay share: n/a
- Look-ahead: n/a

## Settings

- Existing keys: (none)
- Planned keys: (none)
- Tiers: n/a

## Fallbacks

- "Fallbacks engaged" entry: (n/a)
- Behaviour: Owns the format: 'Fallbacks engaged: none' when clean.

## Net fingerprint / upstream

- Fingerprint: (none)
- Upstream modules: (none)

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- (none yet)

Isolation tests:
- FallbackRegistry unit tests

Contract tests:
- every slot's fallback entry reaches the overlay, Config status and Ctrl+Alt+D

Chain tests:
- (none yet)

## Known issues / open items

- audit all log-only fallbacks (item 45)
- reuse the FallbackRegistry built on fix/r6-baseline (barrier R5); do not duplicate

## Where the code lives (live file list)

Code location: **legacy**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- `src/VP-V/core/SlotTiming.h`
- `src/VP-V/core/FallbackStatus.h`
- `src/VideoProcessor-Lib/StateReport.cpp`
- `src/VideoProcessor-Lib/CrashState.cpp`
- `src/VideoProcessor-GUI/StatsOverlayWindow.cpp`
- `src/VideoProcessor-Lib/DebugLog.h`

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0001 in [docs/decisions](../../../../../docs/decisions/README.md)
