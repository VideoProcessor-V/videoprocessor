# Refine net (`scaling.refine`)

Unit: **scaling** | side: input | order: 110 | status: **placeholder** | hook point: prepass (VP NetRunner, once per source frame)

## Purpose / why

ONE refine net at the end of the 4K route, trained to the truth: inputs = picture + continuous per-axis scale (sx, sy) + A-ran flag. Corrects the combined artifacts of the fixed 1-D chain. Per-axis C is dropped (harmful in round 1).

## Contract

- Input: planes at the processing size + sx, sy + A flag (+ path: re-based / native)
- Output: same size and format; bounded residual
- Geometry: active area, mirror pad
- Delay share: 0 frames; target ~A-class 4K cost (~4 ms worst case before fp16/tile skip)
- Look-ahead: none

## Settings

- Existing keys: (none)
- Planned keys: `refine_net`
- Tiers: Upper only (Mid optional if cheap)

## Fallbacks

- "Fallbacks engaged" entry: Refine: bypassed (fingerprint mismatch / group underperforms / low latency)
- Behaviour: Bypass per combination group; off in low latency unless a cheap variant exists.

## Net fingerprint / upstream

- Fingerprint: A version + state, 1-D chain identity, texture restore/deband robustness, VP commit
- Upstream modules: `scaling.route_4k`, `repair.encoding`

## How to build / test

Build the solution via the shared build-slot runner (see AGENTS.md); tests single-threaded; GPU tests only on mains with `VP_TEST_ADAPTER=NVIDIA`.

Placeholder tests:
- byte-identical with the slot off
- zero GPU passes when off

Isolation tests:
- per combination group quality + MTF/HF energy/grain kept; no ringing

Contract tests:
- fingerprint bypass shows under 'Fallbacks engaged'

Chain tests:
- Upper preset at 24p/200 ms on the 3090

## Known issues / open items

- single net TOLD whether A ran vs two versions (owner: one told, if cost is identical) - decide from round 2
- low-latency variant or rule

## Where the code lives (live file list)

Code location: **none (placeholder)**. The authoritative list is `live_files` in [MANIFEST.json](MANIFEST.json).

- (none yet)

## Migration directive

Code moves into this folder only when this slot is touched (opportunistic until M1, a dedicated phase after M1, finished by M2). A move = `git mv` + project/include updates + this module's `live_files` + `docs/design/components-master.json` (regenerate with `python tools/check_module_manifests.py --write`) in the SAME commit, with the build, this module's tests and the chain-gate subset passing. Never leave a copy behind; one module per change; never during a release/deploy window. Update this README in the same commit as any slot change.

## Links

- Design: [VP-pipeline-final.md](../../../../../docs/design/VP-pipeline-final.md)
- Slot data: [slots.json](../../../../../docs/design/slots.json)
- Decisions: ADR-0008 in [docs/decisions](../../../../../docs/decisions/README.md)
