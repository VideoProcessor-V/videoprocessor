# Module findings: M2 steps 6, 14, 15 + WP-14 (2026-10-25, `feat/m2-dv`)

The mandatory per-move pass (migration-m2-plan.md, "Mandatory for every M2 module move") for
`colour.sdr_tone_shape`, `input.signal` and `input.capture`: README checked against the code, key
lists corrected, fallbacks verified visible. Findings only; each fix is its own `fix:` commit (or,
for the renderer host, a fix patch for the integrator). The move commits change no behaviour.

| # | Module | Finding | Severity | Action |
|---|---|---|---|---|
| F-1 | sdr_tone_shape | `slots.json` / MANIFEST listed `sdr_tone_shape`, `dv_trims`, `dv_tone_mapping` only; the code also reads `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, `sdr_contrast_recovery` | low (docs) | fixed in the README-pass commit |
| F-2 | signal | Key list named `dv_trims` (the trims are `colour.sdr_tone_shape`'s) and missed `newlldv`, `newlldv_sdr_veto` (`[general]`) and `dv_tunnel_gpu_unpack` (listed under `input.capture`, but the DV unpack is this module's). Fallback entry text "Signal: DV tunnel unpack on the CPU ... (today log-only)" was stale: the entry is `Dolby Vision GPU unpack: CPU unpack for the session: <error>` and it is visible. README said the comparison capture is "off unless VP_DV_COMPARE_PATH" (the Config folder key and the shortcut also turn it on) | low (docs) | fixed in the README-pass commit |
| F-3 | capture | The capture GPU unpack fallback says `CPU formatters (4:2:0) for the session`, but the CPU formatter for V210 / UYVY / HDYC is P210 4:2:2 (`CreateAlphaFormatter`, `AlphaUsesP210Ingress`) unless the P010 conversion override is set: the status text misreports the fallback picture | low (status text) | fix patch `m2-dv-fixes.patch` (host text: "CPU formatters for the session") |
| F-4 | capture | (WP-4's patch engages the entry but never releases it) Early upload "not available" (second device / shared textures cannot be made) is LOG-ONLY: `capture_upload early` (or `auto` in low latency) silently takes the render upload. Violates ADR-0001 / vp-no-silent-fallbacks | medium | fix patch `m2-dv-fixes.patch`: `early upload: render upload (<error>)` engaged on the failed open, released when early upload is no longer wanted or the device goes |
| F-5 | capture | Fallback entry text "Capture: CPU 4:2:0 (GPU unpack failed: <reason>) ... today log-only" was stale (visible since r6; real ids `capture GPU unpack`, `native RGB GPU unpack`) | low (docs) | fixed in the README-pass commit |
| F-6 | sdr_tone_shape | `SDR tone shape: not applied` (shader missing) is still log-only in the host on this branch | medium | already covered: WP-4's `silent-fallbacks-callsites.patch` (`FallbackCatalog::SdrToneShape`); no second fix here |
| F-7 | signal | VP code inside Bill's `CR210DolbyVisionTunnelFormatter` (feat/dv-l8: `DvSignalLevels::ParseL9/L10`, `DvMetadataHold::Tracker m_dmHold`, the `dv_tunnel_fixes` bug-hunt switch). Not extractable in WP-14 (Bill's capture formatters are untouched by plan); it grows the patch set to his code | medium (structure) | addressed for the merge-1 additions: `IDolbyVisionMetadataExtension` (upstreamable) + `DvTunnelMetadataExtension` (VP-V), `m2-dv-formatter-hook.patch` / `-install.patch` |
| F-8 | signal | Overlap without the either/or setting: Bill's `newlldv` key enables VP's `HdrInferenceTracker`, which replaced his own newlldv heuristic inside `VideoProcessorDlg.cpp`; there is no setting that selects Bill's heuristic (vp-either-or-overlaps: the Original pathway should choose his) | medium (rule) | owner decision; belongs to the GUI-side signal extraction (WP-16/18), not this branch |
| F-9 | sdr_tone_shape | Host code nits carried verbatim into `DvHostStage` (behaviour-free move): `state.empty() &&` in the trims block is always true; the comment on `dvDebandSkipLogged` described `dvTrimsShaderMissing` (put back on the right member in the adapter) | trivial | left as moved; candidate for the WP-19 audit / a later tidy commit |
| F-11 | signal | Bill's DV formatter still carries ~1100 lines of earlier VP changes against origin/v1.4.02-beta (bug-hunt fixes, DV input fixes, AVX2); outside WP-14's merge-1 scope | medium (structure) | inventory + hook plan for the patch-set review (owner/integrator) |
| F-12 | sdr_tone_shape | With the slot switched off (pipeline_slots_off / Original pathway) and `dv_tone_mapping: dolby_style`, the renderer forced bt2390 and listed "DV tone mapping: the colour tone-shape slot is off" - off was treated as a fallback and was not byte-identical to no slot (configured curve) | medium (owner rule: off = byte-identical) | fixed in DvHostStage (`SlotOffIsOffNotAFallback`); effective with the call-site patch |
| F-10 | tools | `migration_path_map.py --fix` rewrote an old-path comment inside Bill's `LibplaceboLutParserTests.cpp` (and the check failed on it), though the forwarder keeps the path valid | low | fixed: `e3a41405` (the scan skips Bill's files). Consequence (audit): old-path includes in Bill's files and the host (e.g. `<vprenderer/SdrToneShape.h>` in `LibplaceboVideoRenderer.cpp`) are no longer reported; they work only through the forwarders, so M2 step 0 must `git grep` every forwarder path before deleting it |

## Not changed by design

- Bill's files used by these slots stay where they are (checked with `git cat-file -e
  origin/v1.4.02-beta:<path>`): `HDRData.cpp`, `ColorSpace.cpp`, `EOTF.cpp`,
  `vprenderer/Bt2020SignalController.h`, `vprenderer/AlphaNativeRgbIngress.h`, `CaptureInput.*`,
  `VideoConversionOverride.cpp`, `blackmagic_decklink/`, `magewell/`, `video_frame_formatter/`.
- Forwarding headers at every moved header's old path (host + Bill's includers), removed in M2
  step 0.
- The shaders stay in `shaders/` (deploy layout); only their reference-path comments changed.
