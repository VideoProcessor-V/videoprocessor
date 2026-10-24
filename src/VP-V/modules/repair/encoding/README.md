# Encoding repair (entrance stage 1) (`repair.encoding`)

Unit: **repair** | side: input | order: 70 | status: **placeholder** | hook point: prepass (VP NetRunner, once per source frame)

## Purpose / why

Remove codec damage (blocking, ringing, mosquito, smear) at the ENCODED resolution, where the damage lives. Choices: net A (A_motion / A_single) / VP restore module / off.

## Contract

- Input: luma at the encoded size (after re-base; or delivered size when native/un-reversed) + A labels + state flag; A_motion also takes neighbour frames and vectors; RGB capture: the r16 matcher luma is repaired and the luma delta added to R, G, B (barrier B1)
- Output: same size and format; luma only (chroma untouched); bounded residual
- Geometry: active area only, mirror padding (crop/A-lens/NLS safe)
- Delay share: 0 extra frames (A_motion uses already-queued neighbours)
- Look-ahead: A_motion: neighbours in the queue; A_single: none (low latency)

## Settings

- Existing keys: (none)
- Planned keys: `encoding_repair`
- Tiers: Classic off; Low VP restore or small A; Mid A; Upper A_motion

## Fallbacks

- "Fallbacks engaged" entry: Repair: bypassed (net fingerprint mismatch / missing weights / DV IPT input)
- Behaviour: Bypass = pass-through; shown under 'Fallbacks engaged'.

## Net fingerprint / upstream

- Fingerprint: label schema, pass order H->V, r16 intermediate, re-base shader identity, VP commit
- Upstream modules: `input.source_analysis`, `motion.estimation`, `scaling.rebase`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- byte-identical with the slot off
- zero GPU passes when off

Isolation tests:
- A vs the clean frame at the encoded size; grain >= 90-95%

Contract tests:
- luma-only; active-area mirror pad; bypass on DV IPT

Chain tests:
- nets + low latency; DV tunnel + repair

## Known issues / open items

- round-2 DIAGNOSTIC decides whether A is kept (>= ~0.5-1 dB on low-bitrate streams with grain kept), else VP restore
- RGB-capture luma path
- real Apple TV validation before default-on

## Where the code lives (live file list)

Code location: **none (placeholder)**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- (none yet)

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0005, ADR-0008, ADR-0009 in [docs/decisions](../../../../../docs/decisions/README.md)
