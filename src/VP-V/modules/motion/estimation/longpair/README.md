<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Long-pair service: I-to-I long-range flow (NVOF with external hints, seeded-search fallback)

`SubmitLongPair(lumaA, lumaB, hintField)`: asynchronous forward / backward field and cost on the NVOF 4x4 grid of half luma, once per detected I (previous I against the new I), seeded with the temporal-detail composed trail field. Not a skeleton product; the pair distance is the measured GOP length.
Unit: motion/estimation (sub-folder `longpair/`, submodule of slot `motion.estimation`). Module side only: bound to `IVectorEngine` at P5a (the renderer item lists do not compile these files yet; the test project does).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

No config key is read. Hand-checked list of the code defaults (`Config`, `SeededSearch.h`): `useNvof = true`, `nvofPreset = 0` (slow), `searchRadius = 6` half px, `maxPending = 2`, `maxResults = 8`, hint grid = output grid = 4 half-luma px, search window 8x8 half px. The service runs only when TD submits a pair, so there is no on/off key; the NVOF-versus-matcher choice is availability-driven and always visible.

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| `VP_LONGPAIR_BENCH_DIR` | directory `<root>\<scene>\longpair` read / written by the `LongPairBenchDump` test (benchmark rows) | unset (test only reports) | test |
| `VP_TEST_GPU` | `hardware` for the real adapter (the dump needs the optical flow engine) | unset | test |

## Products

None. The service returns a `Result` per ticket (`TryTake`, or `RunNow` for tests): `Field` (S10.5 forward / backward vectors in half-luma px, costs u8 higher = less sure, `gridW = ceil(w/4)`), the `Path` taken and a `Why`. No frame history is kept (inputs are copied on submit, results are bounded to 8).

* NVOF: `NvOpticalFlowHinted` is the SECOND session (`enableExternalHints = TRUE`, hint grid 4, `disableTemporalHints = TRUE`: every long pair is independent). `NvOpticalFlow::Engine` (hints off) and its look-ahead / render-time users are unchanged. A pair submitted without a hint runs the same session with `externalHints = null` (path `nvof_unhinted`, not a fallback).
* Hint: `HintField` (TD's composed trail, half-luma px, any block grid; non-finite cell = 0) is resampled bilinearly between cell centres to the 4 px grid and converted to S10.5 (clamped to +-1023 px). The backward direction gets no hint buffer from the engine; the CPU search uses `-hint`.
* The hinted-NVOF capability is checked with `NV_OF_CAPS_SUPPORT_HINT_WITH_OF_MODE`; the supported-grid-size capability is not queried (an init failure carries the driver text and takes the fallback).

## Status and fallbacks

* Fallback = matcher seeded search: the CPU reference (`SeededSearch.h`, law identical to `tools/eval/motion_bench/longpair_bench.py`) or the matcher backend P5a binds through `ISeededBackend`. It runs whenever the hinted session cannot (no driver / no engine / busy, hints unsupported, init or execute failure, 3 execute failures in a row latch, `useNvof = false`).
* Visible, never silent: `VpLongPair::Decision` is a `FallbackCatalog::Decision` with id `motion long pair`, engaged when a pair WITH a hint did not get the hinted session: `motion: long-pair seeded search (NVOF with hints unavailable: <reason>)`; `Service::StatusLine()` carries the same text (and `motion: long pair NVOF + hints` / `... NVOF (no hint for this pair)` otherwise). P5a wires both to the Ctrl+I line and the Config status; moving the builder into `core/FallbackCatalog.h` is an open item.
* Failures with no path (bad sizes, backend error) return `ok = false` with the error; callers fall back to no long-range correction (TD keeps its composed trail and the drift check reports "unavailable").
* A refused submit (queue full or bad input) returns ticket 0 and is counted in `Stats::refused`.

## Tests

* CPU (no GPU touched; `useNvof = false`): `LongPairTests` in `tests/LongPairTests.cpp` (S10.5, hand-computed hint resampling, sub-pel exactness, hint-seeded search finds a 10 / -7 px shift beyond the +-6 range while a zero seed does not, backward uses -hint, visible fallback decision and status, unhinted pair is not a fallback, async ticket, refusals, bound backend).
* GPU (real NVIDIA adapter, lead only): `LongPairBenchDump` (`VP_LONGPAIR_BENCH_DIR`, `VP_TEST_GPU=hardware`) writes the `nohint` / `hint` outputs for the benchmark. Command: `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:LongPairBenchDump /Logger:trx /ResultsDirectory:<dir>` after `python -I tools/eval/motion_bench/longpair_bench.py prep ...`; then `cpu` and `score` (see tools/eval/motion_bench/README.md, "Long-pair rows").
