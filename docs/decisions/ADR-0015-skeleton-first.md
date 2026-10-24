# ADR-0015: Skeleton first: zero-cost placeholder slots, tiers per unit

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: all slots, control.config

## Context

Components land faster and safer when every slot's place and contract is fixed first and each is tested in isolation and end to end.

## Decision

Order of work: necessary bug fixes -> skeleton (every slot a placeholder pass-through that can be engaged/disabled, ZERO-COST when off: no pass, no copy, byte-identical) -> settings (per-unit Classic/Low/Mid/Upper + global, toggles beneath, unbuilt slots marked) -> active checklist -> components in pipeline order -> add-ons. Tests follow the slots (placeholder / isolation / contract / chain) and a pairwise matrix generated from slots.json.

## Consequences / open

Fewer than 4 tiers where a unit lacks the granularity.
