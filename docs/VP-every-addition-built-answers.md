# Planner answers — every addition built (2026-10-13, 9e53a687)

The gate is clean on the deployed libplacebo (Beta all pass, matrix
488/488 on both), the GeForce passes after 9e53a687 with the one
hardware finding (item 7). Answers below; one build request (item 7).

## 1. `picture_only` never opens the port — owner's change, confirmed

The owner's requirement is the right one: `picture_only` must coexist
with his Python script on COM1, so VP must not touch the port in that
mode. Built as stated (EffectiveProtocol "none", gain exactly 1, a
switch from `compensate` refused and the module rebuilt, the old
module's shutdown restoring full light). Record in the policy §7 that
`picture_only` and `listen` are the two modes that never open the port.

## 2. `listen` — confirmed; manual review first, then a fitting tool

A good owner request, and built the right way round: the script keeps
the port, VP hears each command over UDP, runs its own copy of the rules
on its own measurement, and shows the agreement. That is the side-by-side
the policy asked for, with the script in charge.

Answer to the question: **manual review of the first logs first**, then
the fitting tool. The reason is that the known inherent differences
(APL crop, highlight metric, pacing) mean the two will disagree in ways a
fit would absorb into thresholds that do not belong there. After one or
two evenings of logs the owner and I can see which disagreements are
those and which are real threshold differences; then a tool that fits
only the latter is worth building. Keep `listen_log` off by default as
the owner asked.

## 3. Veil from the instantaneous statistic — accepted

The instantaneous highlight × area through the ramp, slewed at 1/s and
reset on a cut, is the right choice: the sustained statistic's lag on a
cut is exactly the case where the veil appears at once (a lamp comes on).
Record the slew rate as a key if it is not one.

## 4. Delta 13 — accepted; keep the shape before the measurement

- Per channel in the output gamma, knee in linear light with slope 1 at
  the start, mixed by strength: as intended.
- **Order: keep it before the LED measurement.** The LED drive should
  measure what is shown; if the shape ran after the measurement, the
  highlight statistic would see highlights the knee has already rolled
  off, and the headroom policy would protect against clipping that no
  longer happens. Before is right.
- `sdr_contrast_recovery` as VP's own local-detail term, proportional to
  (1 − slope), doing nothing without the knee: correct, since libplacebo
  only applies its own while tone mapping. Say so on the tooltip ("VP's
  own; libplacebo's applies only to HDR").
- SDR in / SDR out only; off installs no hook: right.

## 5. R1 scope — accepted; the first-present case is correctly excluded

Using the external field only on extras, where the motion since the
previous present is exactly (shift − previous shift) × field, and
keeping the own search for the first present of a source frame (where
the previous present showed a different source frame), is the correct
scope — the field's meaning is exact on extras and only approximate
across the source-frame boundary. Held blocks as trusted zero, outside
the picture as trusted zero, feather left to the gate, whole-pixel
vectors from block centres mapped as the warps map, the r8 placeholder
after the rgba16f parse failure, and patch 0004 never destroying a
caller's texture: all right. Add a timing case for extras so the saving
is measured rather than assumed (one line on the stats line is enough).

## 6. H2 and the Beta 6d check — accepted

Holding every block on the first source frame after a start or a cut is
H2 by design; the check estimating twice is the right correction. The
number (9.68 vs 13.33 mean to the true halfway picture, from 12.52
before delta 9) is the first quantitative evidence that the refinement
round improved the synthesis. Record it in BUILD-STATUS.

## 7. Hardware finding — yes, build the fix: per-instance tag

A new hook inheriting a freed hook's storage memory, VALID_TAG included,
and trusting stale state on its first refresh is exactly the class of
bug the clean-log and hardware runs exist to catch. Fix it with a
**per-instance tag**: the renderer sets `//!PARAM instance_tag` to a
fresh value (e.g. a counter or the low bits of a time stamp) when it
creates the hook, and the shader writes and checks that value rather
than the constant. That invalidates inherited state without depending
on `frame == 0` semantics (a reset mid-stream would not restart `frame`
reliably). Also clear the storage textures explicitly on creation where
libplacebo allows it, as belt and braces. Test:
`SigmaDeltaGrainAwareCarry` and a new "two hooks in succession, identical
input, identical output on the first refresh" case, on the GeForce.

## Records

Patch 0004 goes into the brief's libplacebo facts and BUILD-STATUS as
you have done; I will carry it into the next brief delta. The import
library regenerated from the .def for the new export: note it in the
README so a rebuild does not drop it.

## Next

Item 7's fix, the audit, then the deploy folder on the owner's request.
The cut is reached.
