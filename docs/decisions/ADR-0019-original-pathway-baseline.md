# ADR-0019: An "Original pathway" baseline reproduces Bill's pathway with every VP addition off

- Status: Accepted
- Date: 2026-10-24 (owner decision; recorded 2026-10-24)
- Slots: all slots (origin field in slots.json)

## Context

The owner approved the VP-side net runner (barrier B1) and the once-per-source-frame input side
(B3) on one condition: the pipeline must act EXACTLY as designed and stay MODULAR. Switching
every VP addition off must reproduce Bill's original pathway (billslack2/videoprocessor, the
v1.4.0x beta line, e.g. the bill-v1.4.02 worktree). This proves the additions are pure add-ons.
It also gives a known-good reference for every comparison.

## Decision

- Every slot carries an **origin** (slots.json `origin`, PipelineSlots.h `Origin`):
  - `upstream`: Bill's pathway as it was.
  - `mixed`: the slot exists upstream but VP added options inside it.
  - `vp`: a VP addition.
- The **Original pathway** mode (`Registry::ApplyOriginalPathway()`; later a Config profile
  "Original pathway") turns every `vp` slot off. Every `mixed` slot runs in its **upstream
  form**, with the settings overrides listed in slots.json `original_pathway.settings`. No VP
  pass runs and no VP hook is registered.
- **Tests:**
  1. CPU: in original mode no VP slot runs and the entrance is identity (PipelineSkeletonTests).
  2. GOLDEN: the renderer capture harness renders synthetic fixtures (patterns we own, never
     film frames) in original mode. It compares them with golden outputs recorded from an
     upstream-equivalent build: the bill-v1.4.02 line with the SAME libplacebo build. The
     comparison is byte-identical, except for the documented exceptions in
     VP-pipeline-final.md 3.7, each with a justification and a tolerance.
  3. TIMING: the queue/present cadence in original mode matches the upstream build for the same
     input rate (frame order, repeats, drops), except for the documented VP queue fixes.

## Consequences / open

- Every new VP feature must keep the baseline intact. A feature that cannot be switched off is a
  design violation.
- The settings agent must verify the override values marked "verify" against the upstream
  build.
- Some differences cannot be removed and are listed as exceptions: the libplacebo version/fork
  patches, VP bug fixes in upstream code paths, and config migration. Each is justified in the
  design doc.
