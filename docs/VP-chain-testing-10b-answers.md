# Planner answers — whole-chain testing, Stage 10b, findings (2026-10-12)

The chain probe is the most valuable test infrastructure in the project:
three crashes found before the projector ever saw them, and a
160-combination matrix as a standing regression. Keep it as the gate for
every deploy from now on, on the deployed libplacebo (377) first and 7.360
second.

## Fixed (1–3) — acknowledged

Texture carry for single-plane RGB sources (subtitle-move composition,
RGB captures) and for any hook on the planes before RGB (CfL); the
resize-frees-carry access violation. All three would have been "VP
crashes when I turn two features on" on the projector. Add each as a
named regression in the matrix if not already.

## Stage 10b — approved; the precision item is accepted

Converting the primaries *before* scaling (where libplacebo does) and
splitting the picture into a clipped part for libplacebo plus a
per-channel remainder texture that VP restores after scaling (footprint
average, no ringing) is the right structure: it is what makes "Stage 10
on = off within 1 code away from out-of-range content" true for the
non-linear shaders too, which 10a could not.

**The mean of 0.03–0.083 code on in-range edges is accepted.** The 0.03
criterion was written before the texture-form restore existed; the
residual is libplacebo's half-float store (≈¼ code) and is bounded (max
1 code). It is below the sigma-delta's own decorrelation term (±0.15
code) and an order of magnitude below anything visible. Do not look for
another route; record the number and its cause in the Stage 10 notes and
relax the test criterion to mean ≤ 0.1, max ≤ 1, so the test documents
the real floor rather than a stale target.

## Findings for decision

4. **HDR + libplacebo debanding: black specks (~1 in 10⁴ bright pixels).**
   Decision: `deband: off` on all HDR rendering profiles, now. The
   owner's HDR path is LLDV at 12-bit source precision, where debanding
   has nothing to do, and output-side banding is the sigma-delta's job.
   It is a libplacebo bug (not a VP hook), reproducible on WARP and on a
   GeForce — worth a minimal repro to libplacebo upstream with the
   version noted. Keep debanding available for SDR profiles, where 8-bit
   sources can genuinely need it; add a chain test that any HDR profile
   in the sample config has it off.
5. **7.360-only behaviours** (bars at code 0, output reaching 0–3 /
   1023, the repair re-encoding ≈0.42 low on average): noted, not
   deployed, no action — but they are a reason the Beta (377) suite is
   the acceptance gate and 7.360 is secondary. Log them in the brief's
   §3 so they are not rediscovered.
6. **GeForce vs WARP 1-code rounding differences:** expected; accept.
   Make the GeForce mode the one run before every deploy folder, since
   it is the hardware the projector will see.

## Beta / GeForce

Passing on both, every chain phase included, is the state to be in
before the first cinema session. The Application Control block that
cleared itself: note it in BUILD-STATUS so the next occurrence is not
mistaken for a build problem.

## Standing request

Make the chain matrix part of the per-step audit: the Code auditor runs
it on the deployed libplacebo and reports pass/fail per phase in the
audit file. A step is not done until the matrix passes.
