# vplog: VideoProcessor log parser

Stdlib-only Python 3 library and CLI. It reads `vp.log`, `vp.log.N`, StateReport
`log-excerpt.txt`, and renderer-harness logs that have no `YYYY-MM-DD HH:MM:SS | `
prefix (`wall` is `None` for those). Only events are yielded: structured `VPEV1`
lines and lines matched by a legacy adapter. Other lines are skipped and counted.

```
python -I tools/vplog/parse.py LOG [--json] [--summary] [--since HH:MM:SS] [--kinds a,b]
python -I -B tools/vplog/test_parse.py
```

Library (other tools: `sys.path.insert(0, "<tools/vplog>")` then `import parse`):

- `Event` dataclass: `line_no, wall, t_ms, kind, fields, source ("vpev"|"legacy"), raw`, property `legacy`.
- `parse_lines(lines)`, `parse_file(path)` return a list subclass; its `.skipped` attribute is the
  number of non-empty lines that matched nothing. `summarize(events)` adds a `skipped` key when
  given that list (or an explicit `skipped=` argument).
- `category(event)` and `summarize(events)` (keys: `counts_by_kind, counts_by_category,
  counts_by_cause, first_wall, last_wall, first_t_ms, last_t_ms, malformed, total`).
- `--since` compares the wall time of day (no midnight rollover); events without a wall time are
  dropped when it is given.

## Grammar "vp-event/1"

One line, prefix token `VPEV1` (may follow the log prefix `YYYY-MM-DD HH:MM:SS | `), then
`t=<ms, 3 decimals>` (steady clock ms), then `kind=<kind>`, then fields `key=value` in emitter
order. Keys `[a-z0-9_]+`. Values bare when matching `[A-Za-z0-9_.:+/-]+`, else double-quoted with
`\"` and `\\` escapes; CR/LF/TAB were replaced by space by the emitter; empty value is `""`. Max
line 1000 chars; if a value was truncated the line ends with `trunc=1`. Closed kind list: session,
resync, fallback, gate, gpu_error, source_change, cadence, repeat, frame_stats, stamp, flag, resync_settle.
Required fields per kind: resync: level cause seq [gap] [detail] mode [injected] [readers]; resync_settle: seq settle_ms settle_frames drops repeats relock relock_frames vectors [cause]; fallback:
state=engaged|released id [reason]; gate: from to reason; gpu_error: api code [what];
source_change: w h rate_num rate_den transfer [dv]; cadence: state pattern [phase]; repeat: cause
count; frame_stats: window_s n p50_ms p99_ms max_ms; session: build_id [commit]; flag: source tag;
stamp: dir index.

Malformed VPEV1 lines (bad token order, unknown kind, bad quoting, missing t/kind, unbalanced
quote, key not `[a-z0-9_]+`) become kind `malformed` with `fields = {"reason": ...}`, are counted,
and are never dropped. Reasons emitted by this parser: `missing_t`, `missing_kind`,
`bad_token_order`, `unknown_kind`, `bad_key` (also a token with no `=`), `bad_quoting` (unknown
escape, stray quote, text after the closing quote), `unbalanced_quote`, `bad_value` (bare value
outside the bare set, or empty bare value), `bad_t` (non-numeric t), `duplicate_key`.
The parser does not enforce the per-kind required-field lists.

Golden lines (the C++ emitter produces exactly these; asserted in `test_parse.py`):

```
G1 VPEV1 t=1234.500 kind=resync level=minor cause=missed_frames seq=42 gap=3 mode=shadow
G2 VPEV1 t=0.000 kind=flag source=hotkey tag="needs space"
G3 VPEV1 t=5.250 kind=fallback state=engaged id="capture GPU unpack" reason="CPU formatters: \"x\" \\ y"
G4 VPEV1 t=1.000 kind=gate from=a to=b reason=""
G5 VPEV1 t=2.000 kind=flag source=s tag="a  b c"
G6 VPEV1 t=60000.125 kind=frame_stats window_s=60.000 n=3600 p50_ms=16.667 p99_ms=17.250 max_ms=33.400
```

## Categories

For `resync` events, by `cause` (tokens from `src/VP-V/core/ResyncCause.h`, function `Token`):

| category | causes |
|---|---|
| drop | missed_frames, overflow_drop, repeat_preserving_drop, drift_drop, kept_run_hole, stale_delivery, gap_other (also timestamp_break, stale_delivery_ended: minor in C++) |
| cadence | any cause starting `pulldown_` |
| timing | rate_step |
| resync | start, start_after_source_change, in_place_source_change, renderer_reset, live_queue_reset, calibration_start, calibration_end, queue_generation_other, file_discontinuity, counter_backwards, timestamp_break_large, source_format_changed |
| other | settings_apply, rebase_decision, graph_rebuild, none, unknown; an unrecognised token falls back on `level` (full -> resync, minor -> drop, timing -> timing, else other) |

Other kinds: `cadence` -> cadence; `repeat` -> drop; everything else (gate, fallback, gpu_error,
source_change, frame_stats, stamp, flag, session, malformed) -> other.

## Legacy adapters

Legacy events have `source="legacy"`, `t_ms=None` and field `legacy=1`.

| line | kind and fields |
|---|---|
| `=== VP LOG SESSION START ===` | session build_id="" |
| `VP build identity: module=.. commit=<url> branch=.. dirty=.. build=..` | session build_id, commit (last URL segment), module, branch, dirty |
| `Alpha queue generation armed: generation=N reason=X ...` | resync level=full cause=X (unknown X: queue_generation_other, detail=X), generation, prefill_target, hard_capacity |
| `Resync (shadow\|on): LEVEL at seq N (description[; gap G])` | resync level cause (reverse of Describe) seq mode [gap] [detail] |
| `Render-ahead gate: A -> B (reason; ...)` | gate from to reason |
| `Render-ahead gate: reset (why; was LEVEL)` | gate from=LEVEL to=reset reason=why |
| `Fallbacks engaged: id (reason); id2 (reason2)` (anywhere in the line) | one fallback per entry, state=engaged id reason; `none` yields nothing |
| `libplacebo [error\|fatal]: text` | gpu_error api=libplacebo code=error what=text |
| `Pulldown: <lock> (was <prev>)` | cadence state=locked pattern=<lock> was, or state=lost pattern=<prev> |

Not matched (no real examples or format strings found): device removed / `DXGI_ERROR_*`
gpu_error lines, source/format change lines (the `Source resolution ...` lines are a different
feature), other `Pulldown:` lines (re-phased, re-basing, rate moved), `Resync (on): FULL at source
frame` and `Resync (on): re-base`. Session start, gate, reset and `Resync (...)` fixtures are
built from the C++ format strings, not from recorded logs; the others use real recorded lines.
