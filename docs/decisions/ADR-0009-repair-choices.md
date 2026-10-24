# ADR-0009: Repair: net A / VP restore / off; deband after; texture restore a separate add-on

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: repair.encoding, repair.deband, repair.texture_restore

## Context

A covers intra-frame artifact removal; texture restore covers long temporal memory; breathing is a sibling. If A did texture recovery, every downstream net's input would change.

## Decision

Encoding repair choices: net A / VP restore (VP's own texture restore + breathing module, not 'classical') / off. Deband runs after repair at source resolution. Texture restore + breathing is a separate premium add-on on A's output (Upper tier) after its efficiency rebuild. Breathing as an A fine-tune only once A demonstrably repairs. Keep A only if the round-2 diagnostic shows >= ~0.5-1 dB on low-bitrate streams with grain kept.

## Consequences / open

Open: deband need with A on.
