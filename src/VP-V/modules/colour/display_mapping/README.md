# DV display mapping (draft owner module, `colour.display_mapping`)

Draft owner of the DV display-mapping split (successor of `colour.sdr_tone_shape`'s DV parts). Its three pipeline stages are the placeholder slots `colour.dm_pre` (RGB input), `colour.dm_curve` (libplacebo colour map) and `colour.dm_trims` (PRE_OUTPUT); CAM16 stays slot `colour.appearance`.

## Design goal, philosophy and decisions

Sources: handover refs/125-dv-display-mapping-proposal.md (design, parts marked STALE superseded by the brief) and lead8/brief-DVDM.md; written by lane DVDM 2026-11-14.

- P1 is behaviour-free: the Dolby-style curve and trim headers (DvDolbyStyle, DvTrimSelection, DolbyVisionTrims, DvHostStage) and AmbientAdaptation move here under the two-commit pure-move rule (forwarding headers are kept at the old paths so Bill-owned files keep 0 changed lines).
- Slot ids are appended, never inserted; the three dm slots are placeholders (no pass, no hook, zero cost) until their stages land.
- DV gating stays on `colour.sdr_tone_shape` in P1; re-gating DV onto the dm slots changes behaviour and is a later phase.
- The faithful (Dolby) trim order is the P7 target. Original pathway stays Bill byte for byte.

## CF-1 P2: DvMath fold and the dv_metadata decision

- `DvMath.h` is the one implementation of the ST 2084 PQ transfer and of the BT.2100 ICtCp matrices (with Mul/Invert). `DolbyVisionTrims::PqFromNits/NitsFromPq` and `DvDolbyStyle::kRgb2020ToLms/kLmsToIctcp/Mul/Invert` forward to it with identical arithmetic (bit for bit, `DvMathTests`). At this tip the colour unit held a single PQ implementation and a single ICtCp matrix set (DvTrimSelection and DvDolbyStyle already called DolbyVisionTrims); the fold moves them to a neutral home so the trims, selection and curve share one. PQ copies outside colour (LedRawTransfer.cpp, LightLevelLut.h, RegionEvidence, AdaptiveDownscale, HdrVerify, PatternCodes) are not touched here; reported to the lead.
- `product.dv_metadata` is NOT activated: the metadata record (`convertedPayload->dolby` / DvMetadataHold) is produced by input.signal and read in the VP-V modules only by this module (DvHostStage) plus the diagnostics/compare capture inside input.signal itself, so the "at least two reader modules" condition does not hold. It stays internal. Revisit when a second module (for example CAM16 / L11 handling) reads it.
- Room-light parts of the original P2 are out of scope (DV2-ROOM-LIGHT).
