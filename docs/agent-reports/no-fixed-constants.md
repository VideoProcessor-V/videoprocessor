# No fixed constants: batches B1/B2 (agent report, 2026-10-18)

Branch `feat/no-fixed-constants` from local `integ/2026-10-17` at 74340728 (timing merge),
worktree D:\codex\videoprocessor\fixedconst. Local only (no remote calls). Full inventory:
docs/VP-fixed-constants-inventory-2026-10-18.md.

## Inventory (856 scanned code lines + 25 frame-count constants + 26 throttles)

- Not a rate or resolution: ~650 (UI geometry, levels, code values, counts, ms timeouts).
- Real standard / physical constants (kept): 138 (DeckLink mode tables, 1000/1001 family, PQ,
  BT.601/709 rules, mode names, the new 50 Hz video floor).
- Reference geometry / rate for relative scaling (kept): 13 references + ~15 relative steps.
- Unmeasured-rate fallbacks (kept; logging deferred to B7): 26.
- Already in seconds / ms: ~20.
- Should derive / express in time: 3 converted here (film/video bands, gap-fill span, 50/60 queue
  rule) + 2 bound escapes (B1); 6 verified converted by earlier work; 17 deferred to B3-B7.

## Converted

One switch for the behaviour changes: `general_rate_fixes` (renderer profile, on | off, default
**on**; Config: Fixes under test, "General frame-rate rules"). Pure correctness has no switch.

| Change | Where | Switch |
|---|---|---|
| Film / video class: no fixed bands (<=30.5, 47.5-48.5); film = measured rate below the 50 Hz video floor less 2% (49 Hz) | DejudderPolicy.h:88-112, :69, :164, :204; InterpolationPolicy.h:106-112; renderer :14129, :14159 | yes |
| Gap-fill span as a time: 4 periods at 23.976p = 166.8 ms, whole periods at the measured rate; pair capped by the 16-output budget | InterpolationTimeline.h:44-81, :121; renderer :5832, :14254, :27584-27612, :27981 | yes |
| Fill dropped frames tooltip/label: "gaps up to about 1/6 s (1-3 frames at 24p, up to 4 at 30p, up to 9 at 60p)" | ConfigEditorWindow.cpp:8515-8522 | - |
| Low-latency queue button: `${cadence} >= 49` (all video rates) instead of `== 49-61`; saved 49-61 rules still read as the button | ConfigEditorWindow.cpp:6150-6215 | no (UI; saved rules unchanged) |
| Scene-timing phase accepted 10-240 Hz: now DisplayRefreshBounds (10-500) | CBufferedLiveSourceVideoOutputPin.cpp:1018; DisplayRefreshRatePolicy.h:117 (`AcceptsPeriod`) | no (correctness) |
| Copied 10/500 literals -> DisplayRefreshBounds (identical values) | AlphaPresentationTelemetry.cpp:19-21 | no |
| Literal-bound test also catches `clock / 240` and `expected < 10` shapes | QueueTimingFixesTests.cpp `NoCopiedRateBoundLiterals` | - |

Numerically unchanged at 24 and 60: every standard rate classifies as before (23.976-48 film,
50-240 video); the de-judder and interpolation decisions are identical with the switch on and off
over the full standard source x display matrix (tests); the gap span is 4 periods at 23.976/24/25
as before. The deliberate changes: odd rates 30.5-47.5 and 48.5-49 become film-like; with gap fill
on (default off) a 60p gap may span up to 10 periods (the same 166.8 ms) instead of 4.

## Tests

- New: src/VideoProcessor-Test/GeneralRateMatrixTests.cpp, 28 methods in 8 classes (written by a
  sonnet sub-agent from my brief, then extended with the output-budget cap): 23.976, 24, 25, 29.97,
  30, 47.952, 48, 50, 59.94, 60, 71.928, 100, 119.88, 120, 144, 240 Hz plus 15, 20, 35, 40, 44.955,
  48.8, 49.2; displays 24-360 Hz; switch on and off; resolutions 720, 1080, 1440, 2160, 4096x2160,
  4096x1716, 3996x2160, 1440x1080, 720x576, 720x480 (height-relative helpers; width never matters).
- Updated: RendererCaptureHarnessTests (switch parsed, dumped, fingerprinted, default on),
  ConfigEditorWindowTests (new rule saved; legacy rule reads as the button), QueueTimingFixesTests.
- Results (x64 Release, CPU only, single-threaded vstest with TRX, GpuHardware/GpuOnScreen excluded):
  - Full VideoProcessor-Test suite: 3372 run, 3369 passed, 3 failed. One was my expectation change
    (ConfigFileTests `DejudderPolicyRunsOnlyForUnevenCadenceWithANextFrame`: 40 fps on 60 Hz is now
    film; the test now checks both switch states) - fixed and re-run: passed. The other two,
    `DeckLinkConvertersSustained4K60PerformanceComparison` and `CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest`,
    are CPU-speed assertions on the DeckLink formatters (untouched) that fail under the machine's
    concurrent build load; they failed again alone. Not caused by this change.
  - Targeted re-run after the fix (GeneralRateMatrix, ConfigurationReadCacheTests, the policy,
    timeline, render-ahead and literal tests): all passed.
  - Config tests (off screen, 1 shard): 143 passed, 0 failed, 1 skipped (needs a real window); gate passed.

## Not done / for the caller

- No GPU runs (none needed for these pure-policy changes; the renderer change is plumbing).
- The intent files were not touched: ContentIntentObserver calls `InterpolationPolicy::IsFilmLike`
  with defaults, so it takes the derived class even when `general_rate_fixes` is off.
- Deferred B3-B7 items are listed in the inventory with reasons (queue/timing code just landed,
  files under merge, or owner decisions: #5 mode ranking, #22 refresh-key migration).
- Merge note: LibplaceboVideoRenderer.cpp edits are small and marked "fixed-constant batch
  2026-10-18" (settings field, parse, changed, dump, two policy inputs, the gap-fill span).
