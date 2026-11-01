# triage: bug-ticket intake

Stdlib-only Python 3. Turns one input into `BUG-NNNN.md` in a bugs directory kept outside the repo.

```
python -I tools/triage/intake.py INPUT --bugs-dir DIR [--tag Picture|Judder|Colour|Sync|Other|Auto] [--title TEXT]
python -I -B tools/triage/test_intake.py
```

Inputs (exactly one): a StateReport bundle `vp-report-*.zip`, a soak triage seed JSON
(`"source":"soak"`), or a loop-rig failure JSON (`"source":"looprig"`).

Loop-rig failure schema: `{"source":"looprig","scenario":"..","row":"..","measure":"..","value":<n>,
"threshold":<n>,"first_frames":[..],"log_path":"..","build_id":".."}`. `log_path` (if it exists) is parsed;
otherwise `first_frames` strings are parsed as log lines.

Bundle fields used: `manifest.json` (`trigger`, `build_id`, `commit`, `version`, `renderer`, `created_local`),
`log-excerpt.txt`, `crash-state.txt` (a line `source...: value`), `live-settings.json` (first key containing
`source`), `config/*`.

`--bugs-dir` is required and is refused (exit 2) if it lies inside the git repo containing this script.

Exit codes: 0 written (prints id), 2 usage / bugs-dir refused, 3 unreadable input, 4 input already ingested
(prints the existing id, writes nothing; detected by `input_sha256:` in tickets).

Ticket: header `key: value` lines (read back by the tool), then excerpt and summary. Missing values are `unknown`.
`severity: S?` and `unit: ?` are placeholders for a later classifier. Config fingerprint: sha256 hex [:16] of
`config/*` (name + bytes, sorted by name); soak/looprig: sha256 of build_id + check/scenario.

Auto tag: gpu_error or "gpu/device" text -> Picture; drop/cadence/repeat -> Judder; source_change or colour
words -> Colour; gate or resync-category -> Sync; else Other.

Excerpt: parser events within +/-30 s of the trigger wall time (bundle `created_local`, soak `trigger_wall`),
nearest 80 at most; without a wall time or events with wall times, the last 80 lines.

Duplicates: signature = set of `kind:cause-or-id` of the window events + `tag:` + `family:`. Build family = build
id text before the last `+` or `-` (unchanged if neither). Jaccard >= 0.6 against tickets with `status: open`
writes `Possible duplicate of BUG-XXXX` and `dup_of:`; nothing is merged or closed.

`INDEX.md` is regenerated from the ticket files after each intake (deterministic bytes).
