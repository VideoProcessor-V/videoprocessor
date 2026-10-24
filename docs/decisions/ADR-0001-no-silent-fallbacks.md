# ADR-0001: No silent fallbacks: one 'Fallbacks engaged' heading

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: control.diagnostics, all slots

## Context

An automatic downgrade that is only logged means watching a worse picture without knowing it (owner: "Definitely important as you wouldn't know").

## Decision

Every automatic fallback or quality downgrade is visible: one heading "Fallbacks engaged: (list)" (each entry = fallback + short reason; "none" when clean) in the Ctrl+I overlay, the Config status and the Ctrl+Alt+D bundle, plus a brief one-time transient notice when it happens. No always-on OSD. Applies to Bill's build too.

## Consequences / open

Each slot declares its fallback entry (slots.json `fallback.entry`). Existing log-only cases (capture GPU unpack, DV unpack, BTB clip under frame mixing, matcher fallback) must be audited and converted (queue item 45).
