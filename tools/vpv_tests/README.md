# VP-V combination test tooling

All commands run from the repo root with `python -I`. Outputs are committed.

| Tool | Purpose | Output |
|---|---|---|
| `slot_matrix.py` | per-slot test matrix from `docs/design/slots.json` | `docs/design/slot-test-matrix.md`, `tests/vpv/slot-matrix.json` |
| `dimensions.py` | option model from `vpv-tiers.json` + fixed dims (signal, rate, queue_frames, low_latency, source_size, output, geometry) | `tests/vpv/dimensions.json` |
| `pairwise.py` | deterministic all-pairs generator (greedy, `--seed`, default 1) | `tests/vpv/pairwise.txt` |
| `random_combos.py` | seeded random sampler | file given by `--out` |
| `../run_combinations.ps1` | overnight runner (vstest, `CombinationRunner`) | `%TEMP%\vp-combinations\<ts>\` |

`tests/vpv/high-risk.txt` is hand-written (owner's list a-f); lines with a `planned=` field are pending.

Regenerate:

    python -I tools/vpv_tests/slot_matrix.py
    python -I tools/vpv_tests/dimensions.py
    python -I tools/vpv_tests/pairwise.py

Check (exit 1 when stale/inconsistent):

    python -I tools/vpv_tests/slot_matrix.py --check
    python -I tools/vpv_tests/dimensions.py --check
    python -I tools/vpv_tests/pairwise.py --verify tests/vpv/pairwise.txt
    python -I tools/vpv_tests/pairwise.py --selftest      # toy model; --stats for counts

Combination line format: `name=pw-0001;dim=value;dim=value;...`. High-risk lines
list only the dimensions that matter; others stay at defaults. Constraints go in
`CONSTRAINTS` in `pairwise.py` (partial-dict predicates, use `.get`); currently empty.
Rerun `pairwise.py` after `dimensions.py` changes, then `--verify`.

Overnight run:

    powershell -File tools\run_combinations.ps1 -Set all -Count 500 [-Gpu]
    powershell -File tools\run_combinations.ps1 -Replay <OutDir>\failures.txt

Without `-Gpu`, `VP_COMBO_GPU=0`. With `-Gpu` the script refuses on battery and
sets `VP_TEST_ADAPTER=NVIDIA`. The C++ `CombinationRunner` reads `VP_COMBO_FILE`
and appends failing lines plus ` # reason` to `VP_COMBO_LOG`.
