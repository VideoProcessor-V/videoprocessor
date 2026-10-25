# Efficiency fixes: host call sites (2026-10-24)

`LibplaceboVideoRenderer.cpp` is Bill's file (present on `origin/v1.4.02-beta`), so the branch
does **not** edit it (vp-host-call-sites-only). Its changes are patches here.

**Base (2026-10-25, regenerated after the audit and the modularity sweep):**
`efficiency-callsites.patch` is generated against the committed renderer blob of `integ/batch1`
**38badcb0**, which already contains the silent-fallbacks call sites (WP-4, 64ab319b) and the
modularity fixes. Verified with `git apply --check --cached` on a 38badcb0 index. (It still needs
the silent-fallbacks call sites present: on an older base apply silent-fallbacks first.)

**efficiency_fixes gates every behaviour change (modularity sweep 2026-10-25;
skeleton-notes/modularity-perf-byte-identical.md).** Off (also the Original pathway's override)
restores the old code path; on is the new one. Tests cover both states.

| Change | efficiency_fixes off | on |
|---|---|---|
| #3 early upload | old rule: `auto` early only in low latency; opened unguarded as before | `auto` early in every mode, VRAM guard (EarlyUploadPolicy::Requested / Input.guard) |
| #7 ER carry | the two original passes (`CarryPassReason::Off`, not a fallback, no entry) | one pass; two passes only as a listed fallback (`CarryFallbackEngaged`, released when one pass runs) |
| #8a sigma-delta grids | `{{output_width_q4}}`/`_b16` = full output size (as before) | the q4 / b16 grids (`TemplateValues::sigmaDeltaGrids`, part of the template key) |
| #8b ER idle release | textures kept until teardown (as before) | released after idle |
| timers, ChangeKey, monitor fit | no picture effect (identical by construction), ungated | |

Entrance span labels come from the slot table (`PipelineSlots::Get(id).name`). The early-upload fallback uses the silent-fallbacks batch's
`FallbackCatalog::EarlyUpload` entry (id "early upload") through `SyncFallback`: the catalog's line
in the open-failure branch is kept, and the VRAM-guard refusal, the open success and "not
requested" go through the same builder. It also needs this branch's VP headers
(EarlyUploadPolicy.h, FeatureTextureIdle.h, PassTimerHook.h, GpuSpanTimers.h, PassTimers.h).

| Patch | Applies to | Kind | Contents |
|---|---|---|---|
| `efficiency-callsites.patch` | integ/batch1 43ebff50 + silent-fallbacks patch | **VP-only** call sites | #3, #7 visibility, #8b (ER), timers T1-T5 |
| `efficiency-upstreamable-churn.patch` | d0212534 | **Upstreamable** | #10 change keys |
| `efficiency-upstreamable-churn.bill-v1.4.02.patch` | `origin/v1.4.02-beta` | **Upstreamable** (for a PR to Bill) | the same #10 hunks against Bill's tree |

The 2026-10-24 version (against d0212534, compiled in the renderer DLL there) is in this file's
history (commit 6804868c). The regenerated one has not been compiled on the integ base (it needs
this branch merged into integ first).

Audit LOWs fixed 2026-10-25: `SyncEarlyUpload` has no default argument and a frame size of 0 is
"not known yet" (no pool, no entry, asked again on the next raw frame) instead of "needs nothing";
the stats route says "uploaded early" only while the pool is open; #8a's assumption (HOOKED <=
output size) is commented at the templates and added to G8a. The ER hunk stays two statements
(overlaps WP-13: whichever lands second folds it into `ExtendedRangeStage`'s `CarryHooks()`).

## Upstreamable vs VP-only

- **Upstreamable**: #10 (change keys in Bill's render-thread logging; `ChangeKey.h` + tests).
  Byte-identical text, so the same log lines. A PR to Bill = the `.bill-v1.4.02.patch` plus
  `ChangeKey.h` and `ChangeKeyTests.cpp` (add the test to his test project).
- **VP-only**: everything else. #3 (EarlyUpload is VP's), #7 (ExtendedRange), #8a (VP shader and
  CustomHookChain), #8b (VP features), the source-resolution monitor (VP), timers (VP-V core),
  #9 (B3 ring, VP).
- DebugLog: `DebugLog::Log` (Bill's header) formats every call because every call is written
  (no level or rate state inside it to check first). The render-thread sites the audit lists
  already rate-limit before calling; the per-refresh string work was the change-signature
  ostringstreams, which #10 removes. No change to Bill's logger.

## efficiency-callsites.patch, hunk by hunk

1. Includes: `EarlyUploadPolicy.h`, `FeatureTextureIdle.h`, `PassTimerHook.h`.
2. **#3 early upload in every mode.** `EarlyActive()` = requested (`early` or `auto`) and not
   blocked by the guard; the low-latency condition is gone. `SyncEarlyUpload(frameBytes)` asks
   `EarlyUploadPolicy::Decide` (slots by need x one raw frame vs DXGI budget - usage - a 25 %
   / 256 MB reserve; an open pool's bytes are not counted against itself) before opening, at most
   every 5 s while closed. A refusal or an `Open` failure engages **"early upload"** under
   "Fallbacks engaged" with the reason (vp-no-silent-fallbacks); `render` is never a fallback.
   An open pool is not closed for memory (MotionEngineIdle handles pressure) so it cannot flap.
   The three raw unpack sites pass `payload.rawBytes`.
3. **Timers.** `passTimerHook` and `extendedRangeIdle` members beside the fallback registry.
   RenderLocked: `BeginFrame()` (1 refresh in 8 timed) and `CpuBegin()` at entry; `CpuMark`
   before the libplacebo render prep (analysis, crop, subtitles, overlays), before the swap
   chain submit (hook assembly + libplacebo render CPU), after the swap (`CpuWait`: present may
   block); `Install(renderParams)` after the PR75 callback is set (chained, so PR75 sampling still
   works); `Install(timedParams)` on each de-judder extra (chains the R1 sigma-delta callback);
   each extra's `WaitForVBlank` timed as `CpuWait`; the raw unpack sites add
   "upload: CPU copy + unpack submit (render thread)" or "(early slot)". The readout is logged
   every 30 s while frames are timed. T3: before each de-judder estimate
   `dejudder->SetPassSampling(Sampling())` (DejudderWarp's own per-pass pl_timers, no drain,
   polled when the pass next runs) and `AddGpuGroups("motion: ", TakePassGpuMs())` after it.
   T2: each entrance stage in `RunEntrance` is bracketed by a `GpuSpanTimers` span (D3D11
   disjoint + timestamps on the immediate context, polled with DONOTFLUSH, dropped if not ready
   after 16 polls) on timed refreshes; polled once per refresh; released at device teardown (Ctrl+Alt+D's `log-excerpt.txt` carries it). A dedicated
   status line would need a plugin API number, assigned at integration only.
4. **#8b** after `ExtendedRange::Prepare`: `extendedRangeIdle.Note(active, HoldsTextures(), now)`
   releases ER's textures after 10 s idle (backing off when it thrashes).
5. **#7 visibility**: if the one-pass carry is not in use (no storage images, not storable, the
   one-pass shader failed), "extended range one-pass carry (two passes ...)" is listed under
   "Fallbacks engaged" (same picture, one extra pass).

## Designed, not built today (WIP)

- **#9 B3 ring handover** (`CopyEntrance`, `StoreEntranceOwned`). Design: each entrance module
  (re-base, debander, texture restore, detail pulse) gets `bool SwapOutput(pl_tex& ringTex)`
  that exchanges its output texture with the ring entry's (same size/format, else false -> the
  copy). A module whose output is also its next frame's history (detail pulse) must refuse
  (`false`) or keep a separate history texture; the ring then holds the module's picture and the
  module renders the next frame into the ring's old texture, which every pass fully overwrites.
  Saves one plane copy per plane per source frame (~0.35 ms on the laptop at 4K). B3 is off by
  default; medium risk (texture lifetime), so it waits for G9.
- **#10 remainder**: `ExtendedRange::Describe` per frame (one string) and the per-frame hook
  vectors (`frameHooks`, rebuilt per extra) are VP code in the host; reserve-once members in
  `Impl` are a follow-up call-site change.
- **Timers still to wire**: the half-size inner MotionWarp's passes (its passTiming is not set by
  SetPassSampling); NVOF itself (QPC around the producer's fence); reading
  `EarlyUpload::Stats::copyCpuMs*` (recorded on the conversion thread) into the readout; a
  pl_timer around the GPU unpack.
- **#8b follow-ups**: AdaptiveDownscale, CrossfadeMix, re-base, debander, texture restore (the
  entrance ones must invalidate `entranceCache` and bump `entranceGeneration`), each after G8b.
