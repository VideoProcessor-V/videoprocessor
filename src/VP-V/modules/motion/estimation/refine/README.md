<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# FieldRefine v1 (U15): the `refined_field` (P3r) search and the per-pixel offset accessor

DMVR-style +-2 block search at half resolution with a parabolic sub-pel fit around the P3 vector (and the camera-model vector on P8 camera blocks); an on-the-fly BDOF/PROF-style per-pixel offset accessor that is never stored.
Unit: motion/estimation (sub-folder `refine/`, submodule of slot `motion.estimation`). Module side only: nothing here is registered in a renderer; the lane stage that runs it binds at P5a behind `IVectorEngine`.

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

No config key is read. The behaviour follows its consumers (the lane runs the stage only when a consumer of `refined_field` is present and P3 exists) and the code constants below. Hand-checked list of every constant in `FieldRefineRef.h` / `FieldRefineGlsl.h` (code defaults, not config):

* `Params::blockSource = 8` source px (half-luma block 4), `margin = 2` half px (window 8x8), `boundaryThr = 1.0` source px, `keepEps = 1/64` source px.
* `kRange = 2` (search +-2 half px, 25 candidates), sub-pel clamp +-0.5 half px, curvature floor 1e-6.
* Output vector in source px; stored form RG16F (effective resolution about 1/16 px for |v| < 128).

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| `VP_TEST_GPU` | `hardware` runs `FieldRefineWarpTests` on the real adapter instead of WARP | unset (WARP) | test |

## Products

* `refined_field` (P3r), producer `RefinedFieldProducer` (`FieldRefineProducer.h`): a PLACEHOLDER that publishes "absent" until P5a. Per 8x8 source block 8 B (`Packed`: vector RG16F, confidence u8, flags u8, padding); the size rule and history belong to the skeleton catalog (W1-RES), not here. Inputs when bound: P3 lag 0, P1 lag 0 and lag 1, P8 lag 0. Spans are invalid across slips (spanMask), like P2/P3.
* Flag values: `kRefined` 1, `kKept` 2, `kBoundary` 4, `kOccluded` 8; confidence is 0 when the mean cost is below 1e-6.
* Flags: `kRefined` (output differs from P3 by more than `keepEps`), `kKept` (unchanged; also invalid and occluded blocks), `kBoundary` (a finite 4-neighbour P3 vector differs by more than `boundaryThr`, or the best candidate sits on the edge of the +-2 range: per-pixel checks needed), `kOccluded` (input occlusion flag: no search).
* Confidence: `255 * clamp(1 - best / mean(25 costs))`, 0 on a flat picture. An invalid (non-finite) P3 vector gives a zero vector, confidence 0, flag kept: no search, no matcher.
* Consumer fallback when absent or late: TD, MV chroma, adaptive scaler, sigma-delta vectors and the optional consumers read unrefined P3 (visible, never silent).
* The producer never starts the matcher (R11): a pure function of the P3 field, the half-luma planes and the optional camera field / mask / occlusion flags.

### Per-pixel offsets (never stored)

`Offset()` (CPU) and `vpu_field_refine_offset(vec2 source_pixel)` (GLSL) return the PROF-style smooth term: the bilinear blend of the four nearest block-centre vectors minus the containing block's own vector. Neighbours flagged boundary / occluded contribute the own vector; a boundary or occluded own block returns 0 (no smearing across a discontinuity). About 20 ops per pixel. The BDOF gradient term of doc 72 is not part of v1 (open item).

## Status and fallbacks

* `FallbackCatalog` style entry `motion refinement` (`RefinedFieldProducer::Decision`): engaged when a consumer asked for `refined_field` and the producer is not Fresh; wording `motion: <refinement not bound ... | refinement late>; unrefined P3 vectors used`. The status line is `RefinedFieldProducer::StatusLine()`. Both are visible once bound to the Ctrl+I line / Config status at P5a.
* Registration of U15 in `tools/check_shared_units.py` is deferred (open item): the skeleton lead's W1-RES branch edits the same registry tail; the entry text is in the hand-back.

## Tests

* CPU: `FieldRefineTests` in `tests/FieldRefineTests.cpp` (parabola exactness, half-float codes, integer shift with zero cost, kept on an exact ramp, boundary / saturation flags, camera seed rule, invalid / occluded / flat blocks, determinism, hand-computed offsets, placeholder visibility).
* WARP (Category `GpuWarp`): `FieldRefineWarpTests` (search shader == `RefineField`, offset shader == `Offset`; near-tie blocks are excluded and counted).
* Command: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:FieldRefineTests,FieldRefineWarpTests /Logger:trx /ResultsDirectory:<dir>`; read the TRX Counters line. No physical GPU needed.
