# M2 steps 10-13 + S1: code findings (2026-10-25)

From the mandatory README-vs-code pass (migration-m2-plan.md, "Mandatory for every M2 module move") on
`input.source_analysis` (VP files), `repair.deband`, `repair.texture_restore` and `scaling.rebase`, done on
feat/m2-entrance. The move commits change no behaviour; each finding is fixed in its own `fix:` commit, or left
open here with the reason. Line numbers are at the move commits.

Status: **fixed** (commit named in the history), **open** (needs a host call site, a GPU check, or an owner call).

## scaling.rebase

| # | Finding | Status |
|---|---|---|
| R1 | `SourceRebase.cpp` ~242: `formatNote` is never cleared, so "Fallbacks engaged: re-base precision" stays after the format recovers. | fixed (fd79b91f) |
| R2 | `SourceRebase.cpp` ~257-261: a failed kernel-matched `InversePlane` fails the whole re-base (native frame + pause) instead of falling back to the polar path for that plane. | open (behaviour change; needs the GPU byte-identity subset) |
| R3 | Host ~22703: the re-base failure pause (5-60 s) and "not applied while smooth motion/deinterlacing" are log-only (ADR-0001). | open (host call site: EngageFallback("Re-base", ...)) |
| R4 | `EntranceSegment.cpp` Rebase: "Re-base failed" logged only when `Failures()==1` (once per session). | open (with R3) |
| R5 | `SourceRebase.cpp:6`, `SourceResolutionMonitor.cpp:3` include the legacy `<KernelInverse.h>` (a forwarder removed at M2 step 0). | fixed (see `fix: re-base and monitor include KernelInverse ...`) |
| R6 | Stale comment `SourceRebase.h` ~23-25 ("auto is native until step B exists"). | fixed (with R5) |
| R7 | Listed fallback "Re-base: off (<reason>)" is never produced; the real entry is "re-base precision". | fixed in slots.json/manifest (README pass commit) |

## input.source_analysis (VP files)

| # | Finding | Status |
|---|---|---|
| S1 | `SourceResolutionMonitor.cpp` ~703-848: `FitTile` runs under `mutex` (queue 165). | open (fix lives on perf/byte-identical; merge it, do not duplicate) |
| S2 | `SourceResolutionMonitor.cpp` ~615: `snapshot.error` never cleared after a successful readback. | open (status-text change; small, needs the latch tests) |
| S3 | Host ~30612: the whole frame is passed to `LookAheadFrame`, the header says the active picture. | open (host; comment or call) |
| S4 | Stale comment `SourceResolutionMonitor.h:27` (auto default is `auto`). | fixed (with R5) |
| S5 | `SourceResolutionMonitor.cpp` ~813-832: the kernel fit runs even when the kernel cannot be used. | open (CPU cost only; efficiency item) |
| S6 | Monitor GPU failure / unreadable source shows only on the status line; `auto` quietly stays native. | open (host: Fallbacks entry) |
| S7 | Ten `source_resolution_*` keys read by the code were missing from slots.json/manifest; `rebase_kernel_match` belongs to scaling.rebase. | fixed (README pass commit) |
| S8 | "Source analysis: labels unknown" is a planned entry (label detector not built). | kept as planned |

## repair.deband

| # | Finding | Status |
|---|---|---|
| D1 | Host ~2254 `LibplaceboDebandYieldsToVp`: after a VP debander failure libplacebo's deband stays off, so the frame has no deband; `FallbackCatalog::PlanDeband` exists but is not wired. | open (host call site; WP-4 silent-fallbacks patch covers PlanDeband) |
| D2 | `EntranceSegment.cpp` Deband: main-path failure logged only when the total failure count is 1 (other slots count too). | open (with D1) |
| D3 | Extra / crossfade / look-ahead deband failures and the detail extra/crossfade/A-advance `(void)` results are silent. | open (with D1) |
| D4 | `VpDebander.cpp` ~627-635: a main-slot failure after a partial weight write leaves `prevValid` true, so a later repeat holds against partly overwritten weights. | not a bug (checked): a new frame writes the OTHER weight set and `curIndex`/`prevValid` advance only on success; a repeat rewrites the current set from the same inputs, so a partial failure leaves identical values. No fix. |
| D5 | `VpDebander.cpp` ~814: `++held` counts frames where no plane used the hold. | fixed (027814a5) |
| D6 | `low_bitrate_handling` read by the threshold but missing from the key lists; listed fallback "Deband: libplacebo deband (...)" never produced. | fixed (README pass commit) |
| D7 | Host status ~12669: deband/detail shown "on" when the slot is switched off. | open (host) |

## repair.texture_restore

| # | Finding | Status |
|---|---|---|
| T1 | `TextureRestore.cpp` ~732: `if (repeat && !m.have)` can never be true. | fixed (ddfed07e) |
| T2 | `TextureRestore.cpp` ~693 rejects RGB (DetailPulse handles it): inactive on every RGB picture. | open (feature; port the luma-of-RGB path) |
| T3 | "Texture restore / Detail pulse stabilise: inactive (...)" is a status-list line only, no Fallbacks entry or notice. | open (host: FallbackStatus) |
| T4 | Host ~22828: the inactive text lags a frame. | open (host) |

## S1 entrance segment (commit 1)

- Behaviour-free by construction (the host's code moved verbatim). GPU byte identity not run (agents run no GPU
  tests); listed in m2-entrance-callsites.md.
