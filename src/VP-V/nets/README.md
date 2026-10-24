# VP-V nets

The VP-side **NetRunner** and the net fingerprints go here. Nothing is built yet: the owner
approved the runner on 2026-10-24 (barrier B1).

- **What it does.** It executes the TrainKit exporter's mpv-format passes standalone on
  VP-owned textures through `pl_dispatch`, using a shim for the DESC/HOOK/BIND/SAVE/WIDTH/
  HEIGHT/COMPONENTS/COMPUTE directives.
  - Intermediates are fp16 by default; the intermediate format is part of each net's
    fingerprint.
  - **Why not a libplacebo LUMA hook:** it would run after deband and texture restore, and never
    on RGB capture.
- **Where it runs:**
  - the front end (`repair.encoding`, `scaling.refine`), once per source frame through the
    entrance stage list;
  - inside the hybrid scaler's POST_KERNEL hook (D).
  - On RGB capture it repairs the r16 matcher luma and adds the luma delta to R, G and B.
- **Fingerprints** cover:
  - the label schema;
  - the pass order H->V;
  - the intermediate format;
  - the upstream versions and states;
  - `SlotRegistry::UpstreamKey` (the module order before the net, ADR-0021);
  - the VP commit.

  A mismatch bypasses that net only, visibly ("Fallbacks engaged: nets: bypassed (...)").
