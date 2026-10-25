# feat/wp23-fas — WP-23 frame analysis store + one luma decode

Branch `feat/wp23-fas` from `chore/early-migrations` ac49a1de (rebase onto `integ/batch1` when its
hash is given). Design and decisions: `docs/design/wp23-frame-analysis-store.md`; Bill hook:
`docs/design/wp23-bill-analysers.md`.

## Commits

1. UPSTREAMABLE — shared luma plane in `AnalysisLumaSource` + Bill's luma-only reads
   (`wp23-bill-analysers.patch`; upstream variant for v1.4.02 checked with `git apply --check`).
2. VP-ONLY — store, producer, host adapter, consumers on the shared decode, tests, docs,
   `wp23-callsites.patch` (host; integrator applies after the Bill patch).

## Status

See the last commit's message and the report to the coordinator for build/test results.
