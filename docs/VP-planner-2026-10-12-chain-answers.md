# Planner answers — whole-chain testing, Stage 10b, findings (2026-10-12)

Answers to `C:\dev\handoff\planner-2026-10-12-findings.txt`. Applied as noted.

- **Chain probe:** the gate for every deploy from now on, on the deployed
  libplacebo (377) first and 7.360 second. *Applied:* docs/audit/README.md
  general checks 8-9.
- **Fixes 1-3** (RGB-source carry, CfL carry, resize-frees-carry):
  acknowledged; each a named regression in the matrix. *Applied:* matrix
  axes `rgbSource` and `cfl` (8 named combinations, Stage 10 on, down / up,
  NLS idle / active, in the quick and full lists); the resize crash is
  covered by `ExtendedRangeAfterPlaneHooks`.
- **Stage 10b:** approved. **Precision accepted:** mean 0.03-0.083 code on
  in-range edges (libplacebo's half-float store, about 1/4 code; max 1).
  Record it with its cause; criterion mean <= 0.1, max <= 1. *Applied:*
  tests already at that criterion; Stage 10 notes in
  docs/VP-dither-judder-plan.md.
- **Finding 4, HDR + debanding:** `deband: off` on all HDR rendering
  profiles, now; upstream repro; keep debanding for SDR; a chain-level
  test that HDR profiles in the sample config have it off. *Applied:*
  `[vprenderer.HDR]` (when PQ or HLG, deband off) in VideoProcessor.cfg and
  docs/cinema/VideoProcessor-test.cfg; test
  `ShippedConfigurationsTurnDebandingOffForHdr`; repro drafted in
  docs/libplacebo-hdr-deband-repro.md (sending it upstream is the owner's
  decision). The owner's deployed configuration gets the same profile at
  deploy (backed up, exact entry reported).
- **Finding 5, 7.360-only behaviours:** noted, not deployed, no action; log
  them in the brief's section 3. *Applied.*
- **Finding 6, GeForce vs WARP rounding:** expected; accept. The GeForce run
  is made the one before every deploy folder. *Applied:* audit README
  check 9.
- **Application Control block:** note it in BUILD-STATUS. *Applied:*
  docs/BUILD-STATUS.md (new).
- **Standing request:** the chain matrix is part of the per-step audit; the
  auditor runs it on the deployed libplacebo and reports pass / fail per
  phase; a step is not done until the matrix passes. *Applied:* audit
  README check 8.
