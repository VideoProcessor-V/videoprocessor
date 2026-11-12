<!-- Copyright(C) 2026 VP-V contributors; SPDX-License-Identifier: GPL-3.0-only -->
# VP-V segments

Segment runners (`EntranceSegment`, `OutputSegments`, `OutputSegmentsMerge`) and the verbatim `.inl` files that
hold bodies moved out of `LibplaceboVideoRenderer.cpp` by M2 P4 (doc 52 section 4).

## `.inl` files (out-of-line members of the renderer's `Impl`)

Same pattern as `modules/motion/estimation/EngineKeeper.inl`: included ONCE by `LibplaceboVideoRenderer.cpp` right after the
`Impl` struct, never stand-alone, never by another translation unit. Bodies are moved verbatim (only the `Impl::` qualification
is added; default arguments stay on the declaration in `Impl`). Render thread, `renderMutex` held, as before the move.

| File | Members | Step |
|---|---|---|
| `CoreSegments.inl` | `UpdatePipelineSlots`, `OrderFrameSlotHooks` | P4-2 |
| `EntranceHostMembers.inl` | `DrainEntranceBeforeRender`, `EntranceOnceFieldAhead`, `EntranceOncePhasePicture`, `EntranceOnceExtra`, `RenderCrossfadeMix` | P4-5 |
| `EntrancePlanner.inl` | `PlanEntrance` (re-base factor, VP deband parameters, detail pulse / texture restore parameters; called once after `NotifyEntrance`) | P4-e..i |

## Entrance stage order and the TD slot

`PlanEntrance` decides the per-frame entrance parameters in a fixed order: re-base, then the repair stack (VP deband, detail pulse
stabilise, texture restore), then the reserved `TdApply` position. Every temporal-detail (TD) method writes `td.delta` (catalog rows
`td.delta` / `td.maps`, `core/Products.h` `kTdDelta` / `kTdMaps`); the `TdApply` slot applies it, and the slot never moves. It is a
declared position only: no TD behaviour exists yet. The detail pulse / texture restore parameter blocks are filled only here
(`DetailRestoreParamsFilledOnlyByThePlanner` pins it; `ProcessBAsA` saves, overrides and restores them for a B picture).

