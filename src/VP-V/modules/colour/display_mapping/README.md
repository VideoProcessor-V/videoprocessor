# DV display mapping (draft owner module, `colour.display_mapping`)

Draft owner of the DV display-mapping split (successor of `colour.sdr_tone_shape`'s DV parts). Its three pipeline stages are the placeholder slots `colour.dm_pre` (RGB input), `colour.dm_curve` (libplacebo colour map) and `colour.dm_trims` (PRE_OUTPUT); CAM16 stays slot `colour.appearance`.

## Design goal, philosophy and decisions

Sources: handover refs/125-dv-display-mapping-proposal.md (design, parts marked STALE superseded by the brief) and lead8/brief-DVDM.md; written by lane DVDM 2026-11-14.

- P1 is behaviour-free: the Dolby-style curve and trim headers (DvDolbyStyle, DvTrimSelection, DolbyVisionTrims, DvHostStage) and AmbientAdaptation move here under the two-commit pure-move rule (forwarding headers first, then removed).
- Slot ids are appended, never inserted; the three dm slots are placeholders (no pass, no hook, zero cost) until their stages land.
- DV gating stays on `colour.sdr_tone_shape` in P1; re-gating DV onto the dm slots changes behaviour and is a later phase.
- The faithful (Dolby) trim order is the P7 target. Original pathway stays Bill byte for byte.
