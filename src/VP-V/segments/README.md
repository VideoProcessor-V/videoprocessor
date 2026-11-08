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
