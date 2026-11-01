# Dither need: call sites NOT applied (Bill files)

Status: `dither_need` (output.dither) is implemented in VP-V files only. The lines below are what a later package adds to Bill's files, through the M2 hook layer, as generic inert hooks. Nothing here is applied on feat/dither-need-map.

## What is NOT live yet on integ/batch1
- No live GPU producer of the region block stats (`region_block_stats`) in the renderer, and no live light-level LUT texture upload. Until both land, the feature reads "unavailable" (`FallbackCatalog::DitherNeed`, "no precursors; no LUT") and the sigma-delta stays at its base amplitude, i.e. `dither_need: auto` is a no-op beyond the status line.
- The shared grain split GPU form (`GrainSplit::Gpu::Frame::blocks`) is the third input.

## Call sites
1. Settings key validation: `RendererProfileConfig.h` (SCHEMA lock): accept `dither_need` = `off|auto` (default off); parse with `DitherNeed::Parse`. Fingerprint it with the output settings.
2. Config editor choice (CE lock, `ConfigEditorWindow.cpp`): a two-way choice on the Dither page, or a control page under `control/`.
3. Renderer, per frame, before the OUTPUT hooks (render thread):
   - `LightLevel::LutCache::Get(settings, context)` -> upload `PackTexels` and `PackOutputTexels` (two 1024x1 rgba32f textures; re-upload only when `LastRebuilt()`).
   - Gather the inputs: the shared `gs_blocks` texture, the region precursor textures (`re_s0..re_s4`, `re_aux`), the domain (`SigmaDeltaDomain::FromTarget` -> `color`), the MEASURED frame interval `dt`, and the epoch-discontinuity flag (cut, resync, re-base, context change).
   - `DitherNeed::Gpu::BlockPass::Step(gpu, dispatch, args, frameArgs)`: it takes the reset / off / missing decision from `DitherNeed::StateTracker::Decide` (frameArgs carries the grid, LUT fingerprint, colour depth, `dt`, the discontinuity flag, `needed` from `NeedCache`, and `missing` bits for what only the host knows: no LUT, unsupported domain, no `set_texture`), validates the grain / precursor / LUT textures against the block grid and runs the pass. A skipped, missing or failed frame `Invalidate()`s the product: `Current()` is null and nothing is bound.
   - `DitherNeed::Gpu::HookGuard::Apply(hooks, count, state, mapping)` with the output->source mapping (`src = (pos + 0.5) * scale + off`, from the frame's crop and dst rects, and `dn_block` = the region block size) and release it after the render (RAII, like `NextVectorGuard`). When `Apply` returns false for a non-null product, report `DitherNeed::Gpu::HookGuard::MissingAfterApply()` (`kNoSetTexture` or `kPassFailed`) through the same status.
   - `FallbackCatalog::Sync(registry, FallbackCatalog::DitherNeed(requested, DitherNeed::MissingText(mask)))`.
4. Shader list: none (the sigma-delta shader file is VP-V; `VPDN` is a 1x1 placeholder like `VPVEC`).
5. libplacebo: `pl_mpv_user_shader_set_texture` (fork patch 0004, `PL_VP_MPV_USER_SHADER_SET_TEXTURE`) is required to bind `VPDN`; without it `HookGuard::Apply` returns false and the feature reports unavailable.
