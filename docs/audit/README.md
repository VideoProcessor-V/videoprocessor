# Audit process for this project

Two Claude Code sessions work on this checkout, never at the same time:

- the **implementer**, which writes code, tests, builds and commits;
- the **auditor**, a separate session with no memory of the implementer's
  reasoning, which reads the specification and the diff, builds, runs the
  tests, and writes findings. It never modifies source.

The owner relays findings between them. This file lives at
`docs/audit/README.md` and both sessions read it.

---

## 1. Folder layout

```
docs/
  VP-dither-judder-plan.md          project brief (authoritative design)
  VP-led-drive-module.md            stage 7 guide
  VP-led-drive-answers-1.md         decisions; overrides the two above
  reference/
    beta_22.py                      the Python script being ported
  audit/
    README.md                       this file
    stage7-step1-protocol.md        one audit file per implementation step
    stage7-step2-controller.md
    stage7-step3-measurement.md
    stage7-step4-compensate.md
    stage7-final.md                 optional whole-diff pass at the end
```

Later stages follow the same pattern: `stage5-step1-….md`, etc.

## 2. Commit convention (implementer)

- One commit at the end of each implementation step, after the full test
  suite passes. Message format:

  ```
  Stage 7 step 2: controller, script mode
  ```

- Fixes in response to an audit are a separate commit:

  ```
  Stage 7 step 2: audit fixes (stage7-step2-controller.md)
  ```

- Never leave a step uncommitted when handing over to the auditor; untracked
  files do not appear in a plain `git diff`.

## 3. Audit file format (auditor)

Each file has four sections, each a list of items. Every item has: file
and line (or function), a one-line statement of the problem, and a one-line
reason tied to the specification. No style comments unless they hide a bug.

```
# Audit — Stage 7 step 2: controller

Diff audited: <previous commit>..<step commit>
Build: OK / FAILED (reason)
Tests: N passed, M failed (names)

## Blockers
(contradicts the spec, or would misbehave on the projector)
- src/VideoProcessor-Lib/led/Controller.cpp:142 — up-step for distance 10–24
  is 2; beta_22.py uses 1. Answers file §2.

## Should fix
- …

## Notes
- …

## Verified OK
(short list of the spec points checked and found correct — so the owner
can see what was covered, not only what was wrong)
```

## 4. General checks (every audit)

1. Every numbered decision in `VP-led-drive-answers-1.md` that the step
   touches is implemented as written.
2. Renderer touch points stay within the guide's limits (three call sites
   for the LED module — exact stats out, gain in, generic queued-frame
   statistics observer in the lookahead walk — plus one hook-chain entry;
   blanking/hook-chain/saturation for stage 2). The observer interface and
   the walk code must not reference the LED module by name or purpose.
3. Tests exercise the behaviour the owner tuned, not merely that code runs.
4. The `Low_Latency` queue profile is unchanged.
5. The build is clean and the full suite passes on the auditor's own run.
6. Every GPU test that renders through hooks or dispatches its own passes
   requires a **clean libplacebo log** (`AssertCleanLibplaceboLog`), not only
   successful return values. libplacebo 7.360.1 logs, but does not report,
   some failed passes. The only accepted exception is the known PRE_OUTPUT
   message that `OutputEncodingRepair` repairs
   (`AssertCleanLibplaceboLogExceptPreOutputBug`). This is a standing rule
   since design review 4 (N-6).
7. No key-only behaviour: every setting a key can change is also a saved
   config key, and key overrides are shown on the overlay (constraint 12,
   `docs/VP-delta-6-settings-first.md`).

## 5. Step-specific checks

**Step 1 — protocol layer**
- No path can transmit when `protocol: none`, `enabled: false` or
  `mode: log_only`.
- `SerialPort` is separate from `BarcoAsciiDriver`.
- ACK parsing, `ack_timeout_ms`, `min_command_gap_ms`, coalescing of stale
  targets, clamping to the reported range.
- Shutdown cannot block on the port (bounded timeout).
- Startup sequence: ECHO on, range query, read back `LPW1` and `CNTR`,
  CNTR restored to 50 once if needed, then never written again.
- Command terminator handling as specified (try `\r`, then literal `cr` +
  `\r`; log which is ACKed).

**Step 2 — controller**
- Curve, `steps_up_dark` / `steps_up_bright` / `steps_up_split_apl`,
  `steps_down`, contrast bands, both fast dumps, anti-pump band — each
  matched line for line against `beta_22.py`.
- `sudden_flag` path omitted.
- `gain_encoded = contrast_actual / 50` in `script` mode.
- Deterministic given inputs; unit tests drive it from a synthetic APL
  sequence and assert the exact level sequence.
- `mode: compensate` not yet required here unless the step included it.

**Step 3 — measurement and renderer integration**
- Statistics computed from the picture *before* the compensation hook
  (measurement pass precedes it in the chain at PRE_OUTPUT).
- Active-picture rectangle from the fork's own tracking, not a fixed crop.
- Code-scale conversion `code = 16 + 219 × value`.
- `apl_weights` default `0.114, 0.587, 0.299`, configurable.
- Highlight percentile, `highlight_area` headroom limit, `fade_frames`,
  `fade_min_step`, `cut_threshold` as defined in the answers file §8.
- Async readback; the render thread never waits on it.
- Exactly three renderer call sites, the third being the generic
  queued-frame statistics observer inside the existing lookahead walk.

**Step 4 — compensate mode and the hook**
- Hook at `PL_HOOK_PRE_OUTPUT`; value domain verified against
  `renderer.c` (full-range normalised, target transfer, pre-encode) and
  stated in the audit.
- `compensation_eotf` (default 2.2) used for linearisation; `gain_domain`
  switch honoured; knee engages continuously near the headroom limit.
- `light_actual` follows the modelled LED trajectory (one-pole lag until
  measured), never the raw target.
- Highlight policy: persistence gate, area blend, continuous response,
  faster engage than relax, `highlight_tolerance_stops`.
- Fade and cut handling per the guide §4.4 and the answers file §8.
- Exit/disable/mode-change/startup state handling per answers file §6.
- Config defaults in `VideoProcessor-test.cfg`: `enabled: false`,
  `mode: log_only`.

## 6. Prompts

### Setting up (implementer, once)

> Create docs\audit\ and copy C:\dev\handoff\AUDIT-PROCESS.md there as
> README.md, then read it in full. Adopt the
> commit convention in section 2 from now on: commit at the end of each
> completed step with the message format shown, after the full test suite
> passes. Confirm the current working tree is committed and tell me the
> commit for the most recent completed step.

### Audit (auditor, per step — replace N and <name>)

> You are auditing another session's implementation, not writing code.
> Read docs\audit\README.md, docs\VP-dither-judder-plan.md,
> docs\VP-led-drive-module.md, docs\VP-led-drive-answers-1.md and
> docs\reference\beta_22.py in full. Then audit Stage 7 step N only: use
> git log to find the step-N commit and the previous step's commit, and
> review the diff between them including all new files. Apply the general
> checks in README section 4 and the step-N checks in section 5. Build x64
> Release and run the full test suite yourself. Write your findings to
> docs\audit\stage7-stepN-<name>.md in the format of README section 3,
> including the Verified OK list. Do not modify any source file. Be
> specific and brief; I am not a developer.

### Responding to an audit (implementer, per step)

> Read docs\audit\stage7-stepN-<name>.md. Address every blocker and
> should-fix, or explain in one line each why you disagree. Rebuild, run
> the full test suite, and commit as "Stage 7 step N: audit fixes
> (stage7-stepN-<name>.md)". Then list what you changed in plain terms.

### Re-audit (auditor, if the first audit had blockers)

> Re-audit Stage 7 step N: review only the audit-fixes commit against your
> previous findings in docs\audit\stage7-stepN-<name>.md. Append a
> "Re-audit" section to that file stating, for each prior item, resolved /
> not resolved / disputed-and-I-agree / disputed-and-I-disagree, with a
> one-line reason. Build and run the tests. Do not modify source.

## 7. Rules

- Never run both sessions on the folder at the same time.
- The auditor never edits source; the implementer never edits audit files.
- Design questions raised by an audit (not code questions) go to the owner,
  who brings them to the design conversation; the answer comes back as a
  new `VP-led-drive-answers-N.md`.
- Two audit rounds per step is the norm; a third means the specification
  is unclear — stop and fix the document first.
