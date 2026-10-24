# ADR-0022: One self-contained VP-V Config section, namespaced keys, managed upstream controls

- Status: Accepted
- Date: 2026-10-24 (owner decision, relayed by the coordinator; recorded by the settings agent)
- Slots: control.config (930); every slot through its unit
- Related: ADR-0001 (no silent fallbacks), ADR-0017 (modules and migration), ADR-0019 (Original
  pathway baseline), ADR-0020 (one total delay)

## Context

Every VP addition so far went into Bill's Config pages: `ConfigEditorWindow.cpp` has about 16,000
lines with hand-built inline sliders. Each upstream menu change then conflicts with our changes.
The owner expects to rebuild Config at M2 from a fresh copy of Bill's then-current Config and plug
VP's part into it. The owner also wants tiers per unit plus a global tier (queue item 32/40), the
"Fallbacks engaged" heading, and no "bazillion ambiguous options".

## Decision

1. **One top-level "VP-V" section** in Config:
   - sub-pages per unit: Input, Motion, Repair, Scaling, Colour, Dynamic Drive, Output;
   - the global tier;
   - "Fallbacks engaged".

   It is implemented in VP-V's own files (`src/VP-V/config/`). A minimal,
   documented hook in upstream's `ConfigEditorWindow` adds it: one include, one page block and
   one navigation leaf (`src/VP-V/config/README.md`).
2. **The tier table is data.** `docs/design/vpv-tiers.json` is kept in step with
   `docs/design/slots.json` by `tools/gen_vpv_tiers.py`, which generates
   `VpvTierTable.generated.h` and has a `--check` mode.
   - A tier is a preset of EXISTING keys, read back from the values ("Custom" otherwise).
     No key stores the tier.
   - A unit lists only the tiers it can tell apart today. The global tier maps to each unit's
     highest tier at or below it, and the missing tiers are shown as "not yet implemented".
3. **Namespaced keys.** New VP-V keys live in `[vpv]` / `[vpv.*]`: the section is the namespace,
   so the keys keep their names. Today `[vpv]` holds the skeleton's global pipeline keys:
   - `pipeline_original_pathway` on|off (the pathway switch);
   - `pipeline_order`;
   - `pipeline_slots_off`;
   - `calibration_lut_stage`.

   The renderer reads `[vpv]` first, then `[vprenderer]`. Each key is validated like the same
   key in `[vprenderer]`, and unknown keys are an error.

   Why not `[vprenderer]`: Config renames a literal `[vprenderer]` root into a named profile
   (`profile_1`). The renderer reads global keys only from the literal root, so keys stored
   there would silently stop working after the first save.
   - Existing VP-added keys keep their names: read-compat is never broken.
   - The mapping and the gradual migration plan are below.
4. **Conflicts by mode.**
   - With the VP-V pathway ON, upstream controls for keys VP-V owns are shown GREYED with
     "Managed by VP-V", never removed.
   - In "Original pathway" mode the VP-V section is disabled, and the upstream menus behave
     exactly as upstream: their previous enabled state and tooltip are restored.
   - The renderer side of the Original pathway is ADR-0019 (`pipeline_original_pathway`).
5. **Gradual migration.** VP modifications that already sit inside upstream menus move into the
   VP-V section when they are next touched, never big-bang. The two existing graded sliders
   (motion, picture) stay where they are for now.

## Key namespace mapping (existing VP-added keys, unchanged names)

| Unit | Existing keys (renderer profile section) | Future `[vpv]` name |
|---|---|---|
| Input | capture_path, capture_upload, native_rgb_ingest, dv_tunnel_gpu_unpack | vpv.input.* |
| Motion | optical_flow*, motion_ahead_frames, dejudder_*, interpolation*, render_ahead* | vpv.motion.* |
| Repair | deband_vp*, detail_pulse_*, texture_restore* | vpv.repair.* |
| Scaling | source_resolution*, rebase_kernel_match, adaptive_* | vpv.scaling.* |
| Colour | preserve_extended_range, extended_range_more_cases, sdr_* | vpv.colour.* |
| Output | temporal_dither, occlusion_mask, dither_* | vpv.output.* |

**Migration rule.** A key moves only together with its Config control.
- The reader accepts BOTH names for at least one release, and the new name wins.
- Config writes only the new name, and removes the old one in the same save.
- A test checks that an old file loads unchanged.

## Consequences

- Upstream merges touch about 25 VP-V lines in `ConfigEditorWindow.cpp`, instead of thousands.
- The tier widget is generic (`VpvSection` + `VpvTierModel`), so new units and tiers are data
  changes.
- Greying is by object name (`config.vprenderer.<key>`). If upstream renames its controls, the
  hook's `upstreamControlName` is the one place to adapt.
- Open: apply the corrected Original-pathway overrides in `docs/design/original-pathway-verified.md`
  to `slots.json` (skeleton lead).
