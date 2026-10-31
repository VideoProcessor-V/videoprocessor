<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# motion.estimation / global_model — global camera model (Plan 19, phase 1)

A sub-component of the `motion.estimation` slot (no new slot: `docs/design/slots.json` is frozen).
Phase 1 is the CPU data side only: the fitter, the per-pair record, the accessor policy and the
frame-analysis-store record. Nothing is wired into the renderer yet (no GPU texture, no shader, no
settings keys, no host call sites); the renderer projects do not compile these files, only the test
project does. Authority: `plan19-implementation-camera-model.md` sections 1-2, the audit, and
`docs/design/plan19-phase1-delegation.md`.

## Purpose
From the verified block-vector field of one source-frame pair (frame N-1 to N, after cadence
detection) fit ONE global camera model (translation, similarity, affine or homography), label every
block (Camera / Object / Overlay / Unknown) and give a calibrated confidence. Later phases use the
model as a prior and as a filler for blocks the measurement cannot trust (flat sky, periodic
texture), and to narrow the matcher's search.

## Files
| File | Role |
|---|---|
| `CameraMotion.h` | record + conventions (header-only) |
| `GlobalModelGeometry.h/.cpp` | minimal solvers, weighted LS, invert/compose, decomposition, interpolation, degeneracy tests |
| `GlobalModelFit.h/.cpp` | `PairInput`, `Config`, `Fitter` (plan section 1 steps 1-11) |
| `GlobalModelTemporal.h/.cpp` | `TemporalFilter` (prediction + consistency only), `FixedLagSmoother` |
| `GlobalModelObjects.h/.cpp` | 1-3 object models with persistent ids (only when `Config::objects`) |
| `PixelResidualReference.h/.cpp` | CPU reference / shader-mirror spec of the tiny-mover measure (not on the runtime path yet) |
| `CameraAccessor.h` | per-consumer policy, blend state, `FieldVector` (pure, header-only) |
| `GlobalModelSearchHints.h/.cpp` | phase-2 matcher hints (data API only, nothing wired) |
| `../../core/FrameAnalysisStore.h` | `kCamera` group, `Record::camera`, `PublishCamera` |

## Inputs and outputs
Input (`PairInput`): the forward block field (vectors in source px + trust 0..1), optional backward
field, bar rectangle (active picture), text-guard tiles, optional texture / pixel-residual maps,
engine global translation, cut / fade / flash flags, fade gain/offset, source pair
(`seq`, `pairFrom`, `pairTo`, `pairSeconds`), resync / re-base generations, expected vector
precision, an optional deadline, and optionally the queued next pair.
Output (`CameraMotion`): validity and `Reason`, model type and parameters (time-normalised via
`rates` per second), confidence + its signals, per-block label / residual / model vector /
object id / tiny-mover flag, object models, cost.
Coordinates: source pixels, origin at the active-picture centre; `Params` maps frame N-1 to N.

## Determinism
Same input gives bitwise-equal records. Sampling seed = `seq ^ (pairFrom * 0x9E3779B97F4A7C15)`
(splitmix64; no `std` distributions), fixed loop and reduction orders, no unordered containers, no
threads, no clocks other than the injected `Config::clock` (cost only). All thresholds are relative to
the block size, picture size, expected precision or a hypothesis count: no hard-coded resolutions,
rates or cadences (tested from 1000x1000 up to 8K grids and with 1/24, 2.5/24 and 1/60 s pairs).

## Prior / filler never override
The model supplies a vector only where the measurement is untrusted (or, with
`preferModelWhenAgrees`, where the measurement already agrees with it within noise). A trusted
measured vector that disagrees with the model is kept, and its block is labelled Object. Any block
whose `pixelResidual` flags a tiny fast mover (ball, puck) is Object and `tinyMover = 1`, never
Camera. Object / Overlay / Unknown blocks never receive the model vector. Policy disabled, record
absent, invalid or without block data: the accessor returns exactly the caller's input
(byte-identical to not having the model). A store that never asks for `kCamera` behaves exactly as before.

## Scene handling
`cut`: Reason Cut, no fit across it; with `next`, the first post-cut pair is fitted with a full search
and published as this frame's seed (`seededFromNext`, PostCutSeeded). `flash`: Reason Flash,
confidence 0, temporal state not updated; with `next` a bridged model (FlashBridged). `fade`: fitted
normally (`inFade`), never forced to 0. A generation change resets the Fitter. A missed deadline
yields `valid = false`, Reason DeadlineMissed (a visible fallback; no state is updated).

## SHADER MIRROR SPEC (for phase 2; the CPU `FieldVector` in `CameraAccessor.h` is the reference)
GPU data (written once per source frame, block resolution):
* Texture `cameraModel`: RG16F = model vector (modelX, modelY) in source px at the block centre; a
  second texture (or the channels of an RGBA16F) holds label (uint 0..3 packed) and residual
  magnitude; same grid as the existing field texture.
* Constant buffer: `confidence`, `weight` (the consumer's `BlendState` output, computed on the CPU per
  frame), `preferModelWhenAgrees` (bool), `blockSize`, `enabled`.
Branch order (identical to the CPU, float arithmetic only, no recursion or containers):
0. `!enabled`, no valid record, block outside the grid, `weight == 0` or label data missing ->
   return the caller's value unchanged (measured if trusted, else the caller's hold value).
1. measured vector trusted: with `preferModelWhenAgrees`, label Camera and
   `|res| <= 0.1*|model| + 0.0156*blockSize` -> `mix(measured, model, weight)`; otherwise measured.
2. not trusted and label Camera -> `mix(hold, model, weight)` (weight == 1 gives exactly the model).
3. else (Object / Overlay / Unknown) -> the caller's hold value.
Weight (CPU, once per frame per consumer): zero for Cut / Flash / DeadlineMissed / NoInput /
GenerationReset / Disabled / invalid; zero at once below `floor`; otherwise
`smoothstep((confidence - thr)/blendWidth)` with `thr = threshold - hysteresis` while the weight is
already above 0, moved by at most `rateLimit` per frame (no pop). Output-space mapping (crop, re-base,
NLS) happens after the choice, as today.
`PixelResidualReference` is the spec of the per-block share of pixels with
`|cur(x) - prev(H^-1 x)| > k * noiseRms` (bilinear previous-frame sample, clamp at borders).

## Tests
`../tests/Camera*Tests.cpp`, CPU tier, all synthetic: geometry, temporal / objects / residual,
accessor + hints, store record, fitter behaviour (pan, zoom, rotation, perspective, noise, objects,
tiny movers, sky, cut, fade, flash, parallax, crowds, logos, credits, letterbox, shake, pulldown,
inverse, determinism, generations, deadline, 8K) and a cost report (`CAMERA_COST ...` lines).
