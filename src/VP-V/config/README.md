# VP-V config

The VP-V Config section: one top-level 'VP-V' master section with a sub-page per unit,
self-contained in our own source.

- **Status.** The settings agent is building it on `feat/settings-tests`. It lands here.
- **Source of the pages.** They are generated from `docs/design/slots.json`: per-unit tier
  sliders, toggles listed beneath, and unbuilt slots shown as "not yet implemented".
- **Renderer keys the skeleton reads today** (global, `[vprenderer]`; validated in
  `RendererProfileConfig.h`):
  - `pipeline_original_pathway` on|off: every VP addition off (ADR-0019).
  - `pipeline_order`: slot names moved within their zone, for example
    `repair.encoding, scaling.rebase` (ADR-0021). An invalid order is rejected with a reason.
  - `pipeline_slots_off`: slot names switched off at slot level.
  - `calibration_lut_stage` libplacebo|vp: where the calibration LUT runs (R1).
