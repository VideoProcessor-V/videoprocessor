# Cut plan — what remains before the first deployment

2026-10-12. Owner's decision: build everything planned before the first
projector deployment, each item behind a setting, gated by the GeForce
chain matrix. This is the list to the cut; nothing is added to it without
a new decision.

## To build (in order; each: implement → chain matrix on GeForce → audit → commit)

1. **Delta 6** settings-first commit (approved, with the global-hotkey
   and DirectShow-only additions).
2. **Stage 8** adaptive downscale per the design note and the 10-12
   answers (option A faithful branch, native profile keys, scalar grain
   first, then the vector grain map with the matcher run for all
   film-rate sources).
2b. **Stage 8b** adaptive upscale blend (docs/VP-stage8b-upscale-blend.md;
   added by the owner 2026-10-12).
3. **Refinements D1–D4 + S1** (one step): global-motion prior,
   motion-scaled and trust-adaptive strength, vector smoothing,
   grain-aware dither carry. **D5** behind its switch, off.
4. **S2** unorm intermediates with the libplacebo patch rebuild.
5. **BUILD-STATUS.md**: every stage, its setting(s), default, and the
   first thing to check; the regression recipes; the key-override list.

*Owner decision (2026-10-12, later): items 3-5 are built together first,
then ONE full gate and ONE audit, not one per step. Plan:
docs/VP-build-plan-refinements.md.*

## Not before the cut (recorded, not built)

- Stage 9 ambient-light switching (needs the sensor hardware; the
  rendering-profile key `sdr_black_nits` already allows a manual
  lights-on profile).
  *Implementer note (owner, 2026-10-12): Stage 9 was already built at the
  owner's earlier request (`[ambient]`, the Arduino sensor reader); it
  stays in the build, `mode: off` by default, untouched by the first
  session.*
- S3 (STBN / second-order shaping) — only if the flat-field test shows a
  pattern.
- The SVP-class estimator — not planned.
- Upstream PR to Bill — after the projector has confirmed the hook chain
  and the time-based queue.

## The deployment

- One deploy folder (portable layout, exe + renderer DLL + patched
  libplacebo + shaders + the FL35 test config + BUILD-STATUS.md +
  DEPLOY.txt), built from the commit that passes the full chain matrix on
  the GeForce with the deployed libplacebo (377).
- Test-config state for the first session: sigma-delta on with motion
  carry; de-judder `s` 0.25 with the occlusion mask; Stage 10 on; Stage 8
  on with the grain map; refinements D1–D4 and S1 on, D5 off; LED drive
  `mode: log_only` for the first minutes, then `compensate`; HDR
  profiles `deband: off`; edge blanking 10/15.

## First cinema session (checklist, ~40 minutes)

1. Launch; edges blanked; picture present; stats overlay (Ctrl+I) showing
   cadence, queue delay, per-present render time. Note the render time.
2. **Ramp** (sigma-delta): by eye — banded / grainy / smooth; long-exposure
   photo for the record. Flat mid-grey and near-black fields: flicker?
3. **Slow pan over detail** (de-judder, dither carry): judder gone at
   s 0.25? any shimmer on fine texture? held regions visible? Try s 0.15
   and 0.35.
4. **Grainy sky over sharp foreground** (Stage 8): grain calmer, texture
   intact, no halos on a horizon or text; compare the four A/B states.
   1080p source with grain: CNN alone → adaptive upscale on.
5. **Extended range** (Stage 10): a 235→255 ramp continues above white
   with the switch on; a PLUGE at codes 4/12/16/20 shows whether the
   FL35 reveals sub-black at all (record either way).
6. **LED drive**: `script` mode side by side with the Python script for a
   few minutes; then `compensate`: the four short tests from the policy
   (fade to black, hard cut dark→bright, static mid-APL 30 s, 5–10% grey
   field), then headlights and a lightning flash.
7. **Measurements while there**: LED and CNTR step curves with the phone
   camera; the three-field gamma check (codes 16/126/235 plus 40/80/180);
   the fixed-LED white sweep above CNTR 59 if time allows.
8. Write the numbers and the verdicts into BUILD-STATUS; switch off
   anything that disappointed; the config is the record.

That session is the cut on the owner's side: after it the project is in
tuning, not building.
