<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Loop-rig analyser and resync stress schedules

Offline Python (3.14, numpy for the image paths). Self-test from any directory:

    python -I -B tools/looprig/test_looprig.py

## Pieces

- `analyze.py` scores a captured loop recording per scenario (`scenarios.json`): pacing (cadence
  lattice per steady segment, p99 phase error), drops/repeats split by cause (source = never sent,
  event = inside a resync event window, vp = unexplained), latency per mode, cadence lock and false
  lock, levels incl. BTB/WTW and dE (reuses `tools/levels`), capture integrity (output-stamp counter),
  unexplained FULL/TIMING resyncs, and per-resync-event rows (below).
- `stress_schedule.py` writes, per scenario group and input mode, `schedule.txt` (the
  `ResyncInject::LoadSchedule` format), `commands.txt` (`resync.inject` module-command lines) and
  `expected.json` (scored by the analyser). Groups: random, atv_switch, capture, drift, overflow,
  seek, storm, dv, replug. Inputs: sdr, hdr10, lldv, dv_tunnel. Seeded and deterministic.
- `scenarios.json`: scenario list; every rate is an `[num, den]` output mode, the source rate comes from
  `sent.csv`. Latency budgets (output periods per mode) are first guesses to tune after the first rig run.

## Per-resync-event rows (aligned with the VPEV1 fields of feat/resync-q0-q3)

Read from `VpEventLine.h` / `ResyncInject.h` / `ResyncEventMetrics.h`:
`resync level cause seq [gap] [detail] mode [injected] [readers]` and
`resync_settle seq settle_ms settle_frames drops repeats relock vectors`. Per event:
`logged` (cause and level accepted), `settle` (settle_ms against the limit, vectors must end `valid`),
`picture_settle` (last disturbed source frame in the recording), `drops`/`repeats` (recording, within the
event allowance), `relock`, `consumers` (readers field, scenario `required_readers`), `stale_vectors`,
`dv_relatch` (last `source_change` dv token against the expected mode; token map in `dv_tokens`),
`storm_notice` (fallback id `resync_storm` engaged). A missing field is NO DATA, not a pass.
`parse.py` in tools/vplog gained the `resync_settle` kind for this.

Assumptions to confirm when the emitter is final: the injected-kind to ResyncCause mapping
(`EXPECT_TABLE` in stress_schedule.py), the `dv` token values in `source_change`, the readers names.

## Running a scored session (the owner does the cabling once)

1. Generate: `python -I tools/looprig/stress_schedule.py --out D:\codex\looprig\sched --seed 1 --duration 3600 --inputs sdr,dv_tunnel`
2. Play a stamped clip (`tools/framestamp/make_stamped_clip.py`, writes `sent.csv`) into capture, run VP with
   `VP_RESYNC_INJECT=1` and `VP_DEBUG_OUTPUT_STAMP=1`, load `schedule.txt`.
3. Record VP's output with the Mini Recorder (10-bit v210 / Y4M C422p10, standard mode).
4. `python -I tools/looprig/analyze.py --scenario resync_random --recording rec.y4m --sent sent.csv
   --log vp.log --expected sched\random_sdr\expected.json --out report`

Exit 0 PASS, 1 FAIL, 3 INCOMPLETE (some row had no data).
