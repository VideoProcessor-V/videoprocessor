# S1 entrance segment: call sites and patch (WP-8 commit 1, M2 steps 10-13) — 2026-10-25

Branch `feat/m2-entrance` (from `integ/batch1` 2f75d36d + `chore/early-migrations` ac49a1de +
`feat/segment-runner` 8b4806a0). Spec: integration-batch-plan.md WP-8 and "M2 moves are IN the batch";
central-migration-needs.md §6.1 S1; b3-fallback-closure.md (S1 interactions). **VP-ONLY** (not upstreamable).

## What is committed vs what is a patch

| Part | Where | Kind |
|---|---|---|
| M2 moves 10-13 (`SourceResolutionDetector/Monitor`, `VpDebander`, `TextureRestore`, `DetailPulse`, `SourceRebase`), one `git mv` per module commit | committed | behaviour-free; forwarding headers at the old `vprenderer/` paths keep the host and Bill's `LibplaceboLutParserTests.cpp` building (removed at M2 step 0) |
| `src/VP-V/segments/EntranceSegment.{h,cpp}`, `src/VP-V/tests/EntranceSegmentTests.cpp` | committed | new VP files; nothing calls them until the patch is applied |
| `LibplaceboVideoRenderer.cpp` call sites + project entries (VPRenderer, VPRendererBeta, Test) | **`m2-entrance-callsites.patch`** (integrator applies on integ/batch1) | host = call sites only |

The patch applies with `git apply` on this branch's head (checked with `git apply --check`). Order
relative to the other host patches: see "Applying" below.

## The segment (commit 1: behaviour-free)

S1 = re-base -> [repair A: placeholder until the net runner is wired] -> deband -> texture restore
(+ detail pulse) -> [4K route] -> [refine] -> [chroma]. Placeholders have no component, so the
registry's zero-cost rule never lists them in `ActiveEntrance()` and the walk never reaches them.

Moved verbatim from the host into `EntranceSegment::Segment` (names kept for review:
`RunEntrance`->`RunStages`, `RunMainEntrance`->`RunMain`, `EntranceRebase/Deband/Detail`->`Rebase/Deband/
Detail`, `EntranceDetailLookAhead`->`DetailLookAhead`, `StoreEntranceOwned`->`StoreOwned`,
`EntranceProcessedA/B`->`ProcessedA/B`, `EntranceProcessBAsA`->`ProcessBAsA`, `EntranceEpochFor`->
`EpochFor`, `EntranceRingKey`->`RingKey`, `EntranceOwnedFrame`->`OwnedFrame`, the counters, the
"entrance once" fallback, the per-source-frame cache and the B3 ring):

- **One call per picture:** `entrance.Run(Request, frame)`, with `Request.path` = Main / Extra /
  CrossfadeFrom. Main covers the cached result, the B3 ring (A), the A-alongside rule (`aNeeded`, now
  computed inside) and, with `perPicture` (B already processed as its own A, audit HIGH 1), the
  per-picture stages. The host's three per-path blocks (main draw, extras, both crossfade-from sites)
  became one request each.
- **Motion consumers stay in the host for now** (`EntranceOncePhasePicture`, `EntranceOnceExtra`,
  `EntranceOnceFieldAhead`, `EntranceOnceSnapshotFor`): they interpolate/warp and estimate ahead (S0 /
  motion consumers, WP-9). They call `entrance.ProcessedA/ProcessedB/OnceFallback/OnceWorking/
  SetCurrentA/ForcedFallback`.
- **Host hand-backs** through `EntranceSegment::Host` (a 30-line adapter in the host): GPU, the shared
  entrance dispatch, Engage/ReleaseFallback, the motion estimate ahead, B's live field, the next frame's
  upload.
- **Ownership not moved in commit 1:** the modules (`sourceRebase`, `debander`, `detailPulse`,
  `textureRestore`) and their per-frame parameters stay host members (`Bindings` points at them),
  because ~150 other host lines (settings, status, source-resolution monitor, device release) use them.
  Moving them is a follow-up change (it shrinks the host further).
- **Byte identity:** same passes, same order, same slots, same formats, same parameters; only the
  owner of the code changed. The order of every side effect (dispatch creation, generation bumps,
  counters, logs, fallbacks) is unchanged. Two host members the segment alone used moved with it
  (`rebaseFormatNoteLogged`, `detailPulseFailLogged`/`textureRestoreFailLogged`). The log interval
  static in `LogCounters` stays a function-local static (shared, as before).

Host diff: about -850 / +100 lines in `LibplaceboVideoRenderer.cpp`.

## Applying

1. `git apply docs/design/m2-entrance-callsites.patch` on feat/m2-entrance (or on integ/batch1 after
   this branch is merged). It touches `LibplaceboVideoRenderer.cpp` (entrance block ~5595-6575, settings
   apply ~8108-8125, device release ~8959, extras ~17975-17990, main draw ~25113-25195) and the three
   project files.
2. Other host patches: `segments-upstream.patch`, `segments-callsites.patch` and
   `silent-fallbacks-callsites.patch` no longer apply on this base even BEFORE the M2 moves (checked at
   49976dcb: they were cut on d0212534; their owners rebase them). With all three segments patches
   force-applied where they do apply, this patch still applies cleanly (`git apply --cached --check` on a
   scratch index), so the regions do not overlap.

## Verification done

- Compile check (x64 Release, `VideoProcessor-VPRenderer` + `VideoProcessor-Test`, via
  vp-build-slots-run.sh) with the patch applied in the work tree: 0 errors (log
  `D:\codexideoprocessor\m2-entrance-logsuild3-compile.log`). The first attempt found one error: a
  local `std::string entrance` in the settings apply shadowed the member (now `this->entrance`).
- CPU: `EntranceSegmentTests` 5/5 (single-threaded, `m2-entrance-logs	rx\entrance-segment.trx`).
  The filtered CPU suite runs at the verification point (after the independent audit).
- `python tools/check_module_manifests.py`: 0 problems.

## GPU tests for later (not run: agents run no GPU tests)

Entrance byte identity, patch applied vs not applied, same commit, raw-R10 hashes:

1. `LibplaceboLutParserTests` re-base / deband / detail methods (unchanged modules, new paths).
2. `RendererCaptureHarnessTests::VpDebanderRunsThroughTheRenderer` and the detail-restore renderer runs.
3. The B3 harness (pipeline_entrance_once on): main, cadence repeats, phase pictures (interpolation,
   hiccup pairs), extras (rendered and source warp), B as its own A, `VP_TEST_ENTRANCE_ONCE_FALLBACK_EVERY`
   forced late fallbacks; counters "history breaks 0", ring reuses equal to before.
4. The source-resolution crossfade (main and extras' crossfade-from).
5. `ByteIdenticalToTheR6Baseline` (B3 off-equivalent inputs) and the frozen-b3base set.
6. The Original pathway (no entrance stage runs: identical).
7. Timing gate (render thread code moved; expect no change).

## Next (WP-8 commit 2, separate)

The small B3 closures (b3-fallback-closure.md cases 3, 5, 6, 7, 8, 13) and the ring hand-over need the
motion consumers in `src/VP-V` first (cases 6-8 are decided there) and a module API for writing into
ring-provided targets (#9). Per-stage GPU timers need a `pl_timer` per module slot. See
skeleton-notes/audit-feat-segment-runner-by-m2-entrance.md.
