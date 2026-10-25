# feat/wp23-fas: WP-23 frame analysis store + one luma decode

Branch `feat/wp23-fas` from `chore/early-migrations` ac49a1de. Rebase onto `integ/batch1` when the
coordinator gives its hash. Design and decisions: `docs/design/wp23-frame-analysis-store.md`. Hook
in Bill's files: `docs/design/wp23-bill-analysers.md`.

## Commits

- UPSTREAMABLE (Bill's files only): 1597709f, 07ce5f78, 8d622d9c. These are the shared luma plane
  in `AnalysisLumaSource` and his luma-only reads. They are also exported as
  `wp23-bill-analysers.patch`; do not apply that patch on top of this branch. There is also an
  upstream variant for v1.4.02, checked with `git apply --check`.
- VP-ONLY: e83dc8ff, 6179aea0, ebe4b897. These add the store, the producer, the host adapter, our
  consumers on the shared decode, the tests, the docs and `wp23-callsites.patch`. The integrator
  applies that patch; it compiles in the renderer.

## Audit

`skeleton-notes/audit-feat-wp23-fas.md`, by the WP-11 agent. M1, M2 and L1-L3 are fixed in
8d622d9c and ebe4b897.

## Tests (single-threaded, TRX in D:/codex/videoprocessor/wp23-trx)

- WP-23 classes plus the touched areas (AnalysisLumaRange, SceneDetector, SubtitleBoxDetector,
  ActivePictureEvidence, LED look-ahead, content intent, intent interpreter, HDR inference,
  NVOF): 380/380 passed.
- PipelineSkeleton, FallbackCatalog, MotionValidity and the LED measurement classes: 63/63 passed.
- Full filtered suite: stopped at 1651 passed and 1 failed, under the coordinator's new policy.
  The failure is `DeckLinkConvertersSustained4K60PerformanceComparison`, a timing test that ran
  under build load; it is not in WP-23 code.

## Not run

No GPU or renderer runs. The list of tests for later is in the design doc under "For later",
including the measurement of CPU saved.
