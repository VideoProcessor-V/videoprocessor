# Soak runner and report

`run_soak.ps1` (Windows PowerShell 5.1) launches VideoProcessor, samples that PID only, collects
the logs and runs `report.py` (Python 3, stdlib only; uses `tools/vplog/parse.py`).

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\soak\run_soak.ps1 `
    -Exe C:\Videoprocessor\vp\VideoProcessor.exe -Hours 8 -Out D:\soak\run1 [-IntervalSeconds 10] [-Arguments "..."] [-WorkingDir DIR]
python -I tools/soak/report.py --out D:\soak\run1 [--thresholds FILE] [--warmup-min 30]
python -I -B tools/soak/test_report.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools\soak\test_run_soak.ps1
```

## Runner rules

- Exit 2 (refused, before any file is written) when `-Out` is inside `-WorkingDir` or under
  `C:\Videoprocessor`, `-Exe` is missing, or a process from that exe path is already running (its PID
  is reported and it is never touched). Exit 4 when python is missing; otherwise the report exit code.
- Never copies, deploys, edits or deletes anything in `-WorkingDir`. It launches with
  `Start-Process -WorkingDirectory` and reads. `-WorkingDir` defaults to the exe's folder.
- Stops at `-Hours` (fractional allowed), when the process exits, or on Ctrl+C: `CloseMainWindow()`,
  wait 15 s, then `Stop-Process -Id <PID>` only. Never by name.
- Then copies (never moves) `logs\vp.log`, `logs\vp.log.0`, new `logs\reports\*.zip` and
  `crash state (*).txt` newer than launch into `<Out>\collected\`.
- `soak-exit.json` is always written: `pid, exit_code, crash, reason, launch_utc, end_utc`.
  `crash` is true when the process exited before `-Hours`; reasons: `hours_elapsed`,
  `process_exited`, `interrupted`.
- `soak-notes.txt` gets `stale log` when `logs\vp.log` is missing or not modified since launch.

## soak.csv

`utc, elapsed_s, private_bytes, working_set, handles, threads, gdi_objects, user_objects,
gpu_dedicated, gpu_shared, nvidia_util, nvidia_temp, nvidia_clk_gr, nvidia_clk_mem, log_offset`.
GPU memory comes from the `GPU Process Memory` counters (summed over the PID's instances) and the
nvidia columns from a read-only `nvidia-smi --query-gpu`; both are empty when unavailable.

## Checks (thresholds.json, overridable with `--thresholds`)

Slopes are least-squares fits over samples after the warm-up (default 30 min).

| check | FAIL when | NO DATA when |
|---|---|---|
| private_bytes_slope | > 2 MB/h | no csv, fewer than 3 post-warm-up samples |
| vram_slope (gpu_dedicated) | > 4 MB/h | as above, or column empty |
| handles_slope | > 50 /h | as above |
| thread_flat | post-warm-up max-min > 8 | as above |
| gpu_errors | any gpu_error event, `libplacebo [error]`, or a device-removed line | no collected log |
| drops_repeats | > 1 /h in steady time (after warm-up, outside +/-10 s of source_change or full resync) | no log, no steady time |
| resync_causes | a full resync with cause none/unknown/empty (counts per cause are listed) | no log |
| resync_settle | settle_ms p95 > 5000, any event with `vectors=invalid` (never settled), or any `relock=no` (all limits in thresholds.json: `settle_p95_ms_max`, `settle_unsettled_max`, `settle_relock_fail_max`) | never NO DATA: PASS with a "not evaluated" note when the run has no `kind=resync_settle` lines |
| fallback_notice | never FAIL | any fallback engaged: a visible-notice event does not exist yet, so it cannot be verified |
| frame_time_creep | frame_stats p99, last hour vs first hour, > +10% | no frame_stats, run < 2 h |
| crash | soak-exit.json says crash | file missing |

Overall: FAIL if any FAIL; INCOMPLETE if any NO DATA and no FAIL; else PASS. Exit 0 / 1 / 3.
Event times are placed on the run clock from their wall prefix (events before the run start, such as
an older `vp.log.0` session, are ignored); events without a wall prefix use the t value relative to
the last wall-stamped event of the same file.

Outputs in `<Out>`: `soak-YYYYMMDD.md`, `report.json`, `plot-memory.svg`, `plot-vram.svg`,
`plot-handles.svg`, `plot-frametime.svg` (a plot is skipped when it has fewer than 2 points), and for
each FAIL `triage\<check>.json` with keys `source, check, verdict, detail, build_id, out_dir, log_path,
trigger_wall, first_events` (up to 10 `{line_no, raw}`).

## Required deployment config and sessions

- Drops/repeats and resync-cause checks need resync instrumentation: set `resync_event shadow` (or `on`) in the TEST deployment's own config (done by the coordinator or owner, never by the runner). Without any in-run `kind=resync` / `Resync (shadow|on)` line these checks report NO DATA.
- Only the run's own session counts: `vp.log.0` is collected only when written after launch, and the report drops every event before the run's `=== VP LOG SESSION START ===` marker from all checks. A FULL resync with cause `queue_generation_other` counts as uncaused.

## Resync fault-injection schedule (`resync_schedule.py`)

```
python -I tools/soak/resync_schedule.py --seed N --hours H [--mean-interval-s 90] [--scenarios seek,reset,...] [--dv] > schedule.txt
python -I -B tools/soak/test_resync_schedule.py
```

Writes the text read by `ResyncInject::LoadSchedule` (`src/VP-V/modules/input/signal/ResyncInject.h`; the VP
test deployment is armed with env `VP_RESYNC_INJECT=1`): a `seed N` header, then `at <sec>s <kind> [arg]` lines in
non-decreasing time (`#<n>` / `seq <n>` source-sequence times are also accepted by the loader). Deterministic for
a seed (own SplitMix64, no clock, no `random` module): the same arguments give the same bytes. Gaps between
scenarios are exponential-ish around `--mean-interval-s` (clamped to 0.25x..4x). Scenarios (default: all but
the DV ones; `--dv` adds `dv_switch`, `rpu_loss`, `tunnel_handoff`): `seek reset inplace rebuild gap rate stall
capture_restart signal_loss storm overflow minor full`. `rate` steps are +-300..3000 ppm and return to `rate 0`
20..120 s later; `rpu_loss n` is followed by `rpu_return`, `signal_loss ms` by `signal_return`; `dv_switch`
targets rotate tunnel, lldv, hdr10, sdr; a `storm` is 4-6 events within 8 s. Exit 2 on a usage error or more than
100000 entries.

## Per-event settle metrics (`resync_settle`)

`ResyncEventMetrics.h` emits one `VPEV1 ... kind=resync_settle seq settle_ms settle_frames drops repeats relock
relock_frames vectors [cause]` line per FULL resync once it settled (or was abandoned: `vectors=invalid`, the
"did not settle" marker). `resync` lines may also carry `injected=0|1` and `readers="a,b"`. The report's
`resync_settle` check prints median/p95/max settle_ms, settle_frames, drops/repeats per cause, relock failures
and injected vs real FULL counts; thresholds `settle_p95_ms_max` (5000), `settle_unsettled_max` (0),
`settle_relock_fail_max` (0). A run without settle lines passes with a "not evaluated" note.
- `VP_RESYNC_SCHEDULE=<path>` (read only when `VP_RESYNC_INJECT=1`): the loop test tier hands the stress schedule (`tools/soak/resync_schedule.py` output) to `ResyncInject`, which loads it at construction (`LoadScheduleFile`); a missing or invalid file is shown in `StatusText` ("NOT loaded"), never silent. Unarmed: the variable is not read.
