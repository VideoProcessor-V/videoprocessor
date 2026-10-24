# VP test structure (settings agent, 2026-10-24)

The owner's testing rules (queue item 46, 2026-10-20; the delay decision 2026-10-24) and the
files that implement them.

## 1. Per slot (generated from slots.json)

`tools/vpv_tests/slot_matrix.py` writes `docs/design/slot-test-matrix.md` and
`tests/vpv/slot-matrix.json`. It records, for each slot:
- **placeholder:** byte-identity with the slot on and off, and zero cost. Required for
  placeholder and partial slots.
- **isolation:** fixed inputs give the expected outputs, and the cost is measured.
- **contract:** formats, geometry, and the exact "Fallbacks engaged" entry text.
- **chain:** the end-to-end delay at the 4/3/2-frame budgets.

`--check` fails when the matrix is stale, when a non-control slot has no fallback entry, or when
an id or order repeats. The byte-identity tests themselves run on the skeleton's `PipelineSlots.h`
/ `EntranceStages.h`, in PipelineSkeletonTests.

## 2. Combinations

The full product is too large to test exhaustively. It covers units x tiers x toggles x signal
(SDR/HDR10/LLDV/tunnel) x rate x sizes x geometry.

| Layer | File | How |
|---|---|---|
| Named presets | the VP-V global tiers Classic..Upper | the Config tests check that they write and read back |
| Pairwise (all pairs) | `tools/vpv_tests/pairwise.py` -> `tests/vpv/pairwise.txt` | about 41 lines cover every pair of 27 dimensions; `--verify` proves the coverage |
| High-risk | `tests/vpv/high-risk.txt` | explicit lines: nets + low latency; DV tunnel + repair/re-base; A-lens/NLS + re-base + vectors; LED compensation + Stage 13 + LUT + black level (`planned=` marks the parts not built yet); render-ahead + 24p; short queue + every look-ahead consumer |
| Random overnight | `tools/vpv_tests/random_combos.py` | seeded; failures are logged verbatim for replay |

The option model is `tests/vpv/dimensions.json`, generated from `docs/design/vpv-tiers.json` +
`slots.json` plus the signal, rate, queue, size and geometry dimensions.

**Runner:** `tools/run_combinations.ps1 [-Set all|pairwise|high-risk|random] [-Count N] [-Replay failures.txt]`.
- It writes `combos.txt` and runs `CombinationRunner` (VideoProcessor-Test) single-process.
- Failing lines are appended to `failures.txt` with the reason.
- It is CPU-only. `-Gpu` refuses to run on battery and sets `VP_TEST_ADAPTER=NVIDIA`; the GPU
  combinations are the BenchKit's.

`CombinationRunner` runs three checks on each combination:
1. VP's own config reader must accept the implied renderer profile.
2. VP's real queue/cadence/render-ahead logic (QueueStressHarness) must run 90 simulated seconds
   with no invariant failure. rnd-* lines get their seed's faults.
3. The delay plan must give the combination's frames at a constant total.

## 3. End-to-end delay gate

Owner 2026-10-24 (ADR-0020):
- ONE total delay, the same at every frame rate (padded);
- 4 queued frames recommended (about 250 ms at 24p);
- lower settings fall back to 3/2 frames with every feature working.

`DelayGateTests.cpp` (VideoProcessor-Test, CPU tier) has two tests:
- **PlannerLadderConstantTotalAtEveryRate:** the 4/3/2-frame budgets and any configured budgets,
  at 23.976/24/25/29.97/30/50/59.94/60. Each budget gives its frames at every rate, and the total
  is identical across rates.
- **QueueHoldsThePlannedDelayAtEveryRate:** the real queue logic holds the planned frames at every
  rate, with render-ahead off and on. The measured steady queue time is within one source period
  plus two refreshes of the plan, and no invariant fails.

Parameters, because the exact value is the owner's (O1/O3):
- `VP_DELAY_BUDGETS_MS` (e.g. `200,250`);
- `VP_DELAY_DISPLAY_HZ` (default 60);
- `VP_DELAY_PROCESSING_MS` (default 8).

The fixed costs are 2 swapchain refreshes + one frame of capture transfer + processing. That puts
4 frames at 23.976p at 250 ms. ADR-0020 leaves the capture transfer open (O1), so it is a
parameter.

**Next tier (GPU, not built yet):** the live gate reads the renderer's
`captureToPresentationTargetMs` (`GetPresentationTargetTiming`) in the renderer capture harness.
It uses the same budgets and rates, on mains only, with `VP_TEST_ADAPTER=NVIDIA`. It follows the
skeleton wiring `DelayBudget` into the queue (ADR-0020 consequences).

## 4. Chain gate

`tools/run_chain_gate.ps1` runs the CPU checks:
- `gen_vpv_tiers.py --check`;
- `slot_matrix.py --check`;
- `dimensions.py --check`;
- `pairwise.py --verify`;
- the DelayGate and CombinationRunner classes (built-in set).

The GPU parts stay mains-only.
