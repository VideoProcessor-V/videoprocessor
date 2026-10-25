# Module findings 2026-10-25: M2 steps 4/5/8 (colour.extended_range, output.dither, scaling.output) + WP-13

The mandatory M2 README-vs-code pass (migration-m2-plan.md, "Mandatory for every M2 module move")
on `feat/m2-output`. Each finding is fixed in its own commit after the behaviour-free move commits.

| # | Module | Finding | Fix |
|---|---|---|---|
| 1 | scaling.output | No visible fallback: adaptive downscale on with SSimDownscaler missing (libplacebo's downscaler runs) or a GPU failure of the blend (plain SSIM) reached only the log and the shader status line. `FallbackCatalog::OutputScaling` existed, tested, but was not wired ("wire with the owning change": this one). | `fix:` commit: `AdaptiveDownscaleStage::Stage::Fallback()` + one host line in the call-site patch (`EngageFallback`/`ReleaseFallback`; `SyncFallback` once WP-4 is in). CPU test. |
| 2 | output.dither | `slots.json` / MANIFEST listed `sigma_delta` as a key; it is a value of `temporal_dither`, which was missing. | docs commit: key lists corrected, README regenerated. |
| 3 | scaling.output | Key list had 6 of the 23 keys the code reads (the `adaptive_*` tuning keys, the upscale keys, `ssim_bitrate_*`, the residual-stabilise keys were missing). The fallback text said "Lanczos (hybrid unavailable)", which the code never produces. | docs commit. |
| 4 | colour.extended_range, output.dither | The `BTB/WTW` and `dither` downgrades are log-only on this base. WP-4 (`silent-fallbacks-callsites.patch`) wires both; not duplicated here. `ExtendedRangeStage::Stage::Fallback()` gives WP-4's line the same decision after the rebase. | WP-4 (integ/batch1). |
| 5 | output.dither | MANIFEST `live_files` lacked `OutputEncodingRepair.h` and `CustomHookChain.h`. | fixed in the step 5 move commit (2c393254). |
| 6 | host | After the patch the renderer no longer uses `DownscaleLight` but keeps its include (it sits in the segments patch's context). | M2 step 0 include sweep. |

Not findings (checked): the ER decision/relabel/hook order, the AD measure/blend order, the
de-judder cache's NLS map (the inline copy equals `KnownNlsMapping()`), and the next-frame vector
guard behave as the README now describes.
