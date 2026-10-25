# WP-23: shared luma decode for Bill's analysers (UPSTREAMABLE patch)

Status: built and CPU-tested on `feat/wp23-fas` (2026-10-25). **UPSTREAMABLE**: no `src/VP-V`
dependency; offer to Bill as one small PR only with the owner's approval for that PR (D7). Never
against enchywastaken.

## What it does

Today the same captured frame is decoded to luma several times on CPU threads: SceneDetector,
the active-picture evidence, the subtitle detector, and VP's own readers (efficiency audit
2026-10-24 §3). `AnalysisLumaSource` is already the shared, renderer-independent view of the
frame. The patch lets it carry ONE decoded luma plane per frame:

- `struct AnalysisLumaPlane` — `Sample(x, y).luma` of every pixel, filled once by whichever
  producer owns the frame (`TryClaim()`, fill, `Publish()`), then read-only.
- `AnalysisLumaSource::sharedLuma` — an optional `shared_ptr` to it; copies of the source share it.
- `SampleLuma(x, y, luma)` — `Sample(x, y).luma`, from the published plane when there is one.
- `SharedLumaRow(y)` — the published plane's row, or null.
- His luma-only reads switch from `Sample()` to `SampleLuma()`: `SceneDetector::Analyze` (1),
  `ActivePictureEvidence` (`Luma()` helper and the inner-row mean: 2), `SubtitleBoxDetector` (6
  luma-only scans: card and edge darkness probes). Reads that also use
  chroma are unchanged. In his v1.4.02 the luma-only row reader `MeasureLumaRowContent` also reads
  the shared row (one extra hunk in the upstream variant).

We feed his code its INPUT once; every decision (crop, subtitles, scene cuts) stays his.

## Why it cannot change a decision

- The plane holds exactly `Sample().luma` (tested per pixel for all 16 synthetic formats:
  `FrameAnalysisProducerTests::SharedDecodeEqualsSamplePerPixelForEveryFormat`).
- Readers never wait. Until `Publish()`, or when the plane has another size, `SampleLuma` calls
  `Sample()` as before: same value, same failures (out of range, invalid source).
- Without a producer the member stays null and nothing changes (upstream behaviour).
- Equivalence on recorded inputs (10-frame sequences with a cut, every decoded format, plain vs
  shared): `SharedLumaEquivalenceTests` — SceneDetector, SubtitleBoxDetector (all result fields,
  signatures, sample counts), ExtractActivePictureEvidence + global near-black: identical.

## Files and patches

| Patch | Applies to | Use |
|---|---|---|
| `wp23-bill-analysers.patch` | our tree (`ac49a1de`, his files with our earlier edits) | the in-tree patch-set entry (also the first commit of `feat/wp23-fas`) |
| `wp23-bill-analysers-upstream-v1.4.02.patch` | `origin/v1.4.02-beta` (6ba412bd), checked with `git apply --check` | the PR to offer Bill; includes the `MeasureLumaRowContent` hunk |

Files touched (all Bill's, none moved): `AnalysisLumaSource.h/.cpp`, `SceneDetector.cpp`,
`ActivePictureEvidence.cpp`, `SubtitleBoxDetector.cpp`. Size: ~40 lines in the view, 9 one-line
call changes in the analysers. The end-state patch set grows by this one hook; it shrinks again
when Bill takes it.

## Merge note (WP-21)

His v1.4.02 `AnalysisLumaSource` emits delivered codes with an `AnalysisLumaRange` tag; ours maps
full range to studio codes inside `Sample()`. The shared plane holds whatever `Sample()` emits, so
it follows whichever model the merged file uses without change. VP's own range conversion is in
one VP function (`VpFrameAnalysis::SamplerCodesAreFullRange`); see
`wp23-frame-analysis-store.md` "Range model".
