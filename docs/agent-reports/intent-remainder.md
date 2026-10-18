# Intent interpreter remainder (items 2-5), queue-fed observer, fade model

Branch `feat/intent-remainder` (local only; the GitHub account is suspended), worktree
`D:\codex\videoprocessor\intent2`, from `integ/2026-10-17` f4722145. Design:
docs/VP-intent-interpreter-design.md (owner decisions 2026-10-17; status section at its end).
Lead plus three sub-agents (motion/integration: opus, core logic: opus, Config/docs: sonnet), each in
its own worktree (`intent2-motion`, `intent2-core`, `intent2-config`), merged by the lead.

## Functions, switches and defaults

All are renderer profile keys (`[vprenderer]` and every `[vprenderer.<name>]`) under the Config heading
"Experimental content-adaptive settings", with a checkbox per function, sub-settings greyed beneath it,
and a tooltip that names the key. Everything is off by default.

| Function | Keys (default) | What it does |
|---|---|---|
| Motion intent (Authentic Motion analog) | `intent_motion` off; `intent_motion_policy` display / ease_fast / ease_slow / visibility / metadata (display); `intent_motion_min` 0.5; `intent_motion_min_shot_s` 2 | Per-shot pan speed (picture heights/s), steadiness, edge term, J = band(v) x edge. The scale multiplies `dejudder_strength` within [min, 1] and never raises it. Video-class sources get 1. ease_fast defers to `dejudder_motion_rolloff` when that is on. The de-judder's valid vectors replace the CPU projection measurement pair by pair; invalid pairs fall back. |
| Ambient adaptation (Light Sense analog) | `ambient_adapt` off; `ambient_adapt_strength` 1.0; `ambient_screen_reflect` 0.01 cd/m2 per lux; `ambient_max_black_nits` 1.0; `ambient_tau_up_s` 15; `ambient_tau_down_s` 90 | Lux comes from the [ambient] detector (manual: lit = lux_lit, dark = 0). Median of 5, asymmetric log-lux EMA, hold then decay on sensor loss. B_eff = B + strength x lux x reflect, capped, in about 1/6-stop steps, applied at cuts (slew below 1 %/s without cuts) through the BT.1886 target black. Off or strength 0 gives sdr_black_nits exactly. The LED share floor follows B_eff. |
| Black protection (Precision Black analog) | `intent_black` off / report / led_protect (off) | Reports the shadow steps between DV L1 min and the measured p10 that fall under display black + one 8-bit step. led_protect caps LED dimming so p5 stays one code above black after compensation. L1 min is never given to libplacebo's min_luma; code 0 and BTB never move. |
| Display mapping readout | `intent_display` off / report (off) | PQ(L1 max) - PQ(display peak) and stops, with no expansion, plus the nearest L2/L8 trim. DV-card line such as "scene 1000 nit -> 48 nit (-4.4 stops), trim L2 100 nit". |
| Fade model (owner decision, this branch) | `intent_ease_s` 0-5 s (1.5; 0 = instant at the cut) | Provisional value at the cut: the policy's value when the queued evidence covers 0.5 s, otherwise the previous value carried over. It then eases toward the better-informed estimate (rate-limited to the full range per intent_ease_s, no overshoot), settles and holds. Fades and known short shots keep the previous value. Used by motion intent and led_protect's share. |
| Automatic `active_picture_lookahead_frames` (owner decision) | absent = auto | Normal = min(target - 1, queue_size - 1, 8), Low latency = 0. An explicit value, including 0 and including one inherited from the default queue section, overrides. The Config control shows "Auto"; the queue presets no longer write the key. |

Two defaults differ from the design doc's phase-A text, and the coordinator approved both:
- `intent_motion_min` is 0.5 (doc: 1.0);
- `ambient_adapt_strength` is 1.0 (doc: 0).

The checkbox is now the off switch. Strength 0 stays bit-identical, and a test covers that.

Precedence is Dv2 > DvL11/DvL1 > Inferred > Off. Inferred values are always logged next to the
metadata values ("motion scale <policy> metadata=... inferred=...").

## Files (main additions)

- `src/VideoProcessor-Lib/MotionIntent.h`: policies, shot accumulator, applier, and the fade model
  (`ShotEase`, `OnCutEased`, `Progress`).
- `IntentInterpreterObserver.h`: shot observer. It finds shot boundaries, fades and near-black
  frames, collects per-shot motion and shadow statistics, decides at the display side and eases
  within the shot.
- `IntentInterpreterSupport.h`: ambient step and LED-limit helpers.
- `QueueLookahead.h`: automatic look-ahead.
- `AmbientAdaptation.h`, `BlackIntent.h`, `DisplayIntent.h`: pure logic.
- `IntentDescriptor.h`: plain-data status structs.
- `IRenderer.h`: `GetIntentStatus` and `SetAmbientLux` (plugin API 36; the coordinator renumbers
  at merge).
- `led/LedDriveModule.*`: `SetIntentLimits(shadowShare, ambientBlack, displayBlack)`.
- `vprenderer/LibplaceboVideoRenderer.cpp`:
  - keys parsed, fingerprinted and dumped (around 1150, 1421, 2490, 10540, 28765);
  - motion hook, one multiply after `DejudderPolicy::EffectiveStrength` (around 14237);
  - ambient target black (`IntentTargetBlackNits`, around 14054, used at around 17518);
  - black/display evaluation with the eased LED share (around 14084-14160);
  - queue-fed walk in the render loop (around 27258-27330). This is the only change in timing/queue
    code: a narrow change to depth and condition that adds the intent observer's wanted depth next
    to the LED's.
- `VideoProcessorDlg.cpp` / `StatsOverlayWindow.*` / `VideoProcessorApp.*`: lux feed once a second;
  Motion, Ambient, Black and Display lines on the toggled overlay and in the crash state; automatic
  look-ahead.
- `ConfigEditorWindow.cpp`: real controls in place of the four placeholders, `intent_ease_s`, and
  the Auto look-ahead.
- `RendererProfileConfig.h`: validation.
- `VideoProcessor.cfg` and `docs/cinema/VideoProcessor-test.cfg`: comments only.
- `CONFIGURATION.html` and the design doc.
- Tests:
  - `MotionIntentTests.cpp` (including `MotionIntentEaseTests`)
  - `IntentInterpreterTests.cpp` (observer, queue-fed walk, QueueLookahead, key parsing through the
    renderer DLL, benchmark)
  - `AmbientAdaptationTests.cpp`
  - `BlackIntentTests.cpp`
  - `DisplayIntentTests.cpp`
  - `LedIntentLimitsTests.cpp`
  - `ConfigFileTests.cpp`
  - `ConfigEditorWindowTests.cpp`

## Tests and results

Full x64 Release solution build through `D:/codex/vp-build-slots-run.sh`: msbuild rc=0, no errors,
and no new warnings in the new or changed files. Magewell is built with the SDK.

**CPU suite.** vstest, single-threaded, TRX, filter `FullyQualifiedName!~RendererCaptureHarnessTests`:
3333 run, 3329 passed, 4 failed.
- `RendererParsesEveryIntentKey` was a test-harness bug. The test rewrote one cfg file within the
  same second with a same-length value, so the configuration cache served the stale copy. It now
  writes a new file per call. After the rebuild, the intent / motion / ambient / black / display /
  LED-limits / content / ConfigFile classes ran 237 of 237 passed. That run includes
  `RendererParsesEveryIntentKey`, the fade-model tests, the plugin API test (36) and the inheritance
  test.
- Known timing-flaky tests (real-time LED threads), flagged separately and not regressions:
  - `LedDriveDelta1Tests::EffectiveLookaheadIsClampedToTheFramesActuallyQueued`
  - `WithoutLookaheadTheGainLandsLedLatencyAfterTheCommand`
  - `WithLookaheadTheCommandPrecedesItsFrameByLedLatency`
  - Three re-runs of the class failed a different single test each time (52 of 53). The same class
    on a base DLL without these changes (dv-autotest, integ + hunt2-timing) also flaked:
    `LandingIsTimedFromTheWriteInMilliseconds` failed in one of 2 runs.

**Config tests.** `tools/run_config_tests.ps1`, off screen, `-Shards 1`: 143 passed, 2 failed,
1 skipped (needs a real window).
- Passed: the intent group tests ("content-adaptive group", "intent remainder group" including the
  `intent_ease_s` control) and "active-picture lookahead automatic when absent".
- The 2 failures were the remote-connection tests ("remote target switch keeps window responsive",
  "remote-only client opens its sole discovered VP"). Re-run alone, all 3 remote tests passed: they
  are flaky with loopback timing.

## CPU cost (laptop, CPU micro-benchmark, VP_TEST_BENCH=1)

Per source frame, intent observer step plus the walk's CPU analysis:

| Resolution | v210 | P010 |
|---|---|---|
| 3840x2160 | 0.72 ms | 0.97 ms |
| 1920x1080 | 0.60 ms | 0.85 ms |

At 24 fps the intent-only walk (each frame analysed once) costs:

| Resolution | v210 | P010 |
|---|---|---|
| 4K24 | 17 ms/s | 23 ms/s |
| 1080p24 | 14 ms/s | 20 ms/s |

The active-picture walk of the current frame plus 4 ahead, uncached (5 analyses per render), costs
86 / 117 ms/s at 4K24 and 72 / 102 ms/s at 1080p24 (v210 / P010).

## Gated GPU work for Claude (not run here: no GPU runs by agents)

- **GPU filter:** `FullyQualifiedName~RendererCaptureHarnessTests`. The renderer harness checks that
  with every intent key off the output stays identical.
- **Cost measurement:** run on the laptop at 4K24 and 1080p24, Normal queue profile. Compare the
  `lookaheadMs` render-loop timings in `C:\Videoprocessor\vp\logs\vp.log`:
  - intent functions all off with `active_picture_lookahead_frames: 0` (the old default);
  - all off with the key absent (auto look-ahead on);
  - `intent_motion: on` plus `intent_black: report`.
  The GPU side is expected to be unchanged, since the observer is CPU only. Confirm with GPU time
  per frame and dropped-frame counts, then repeat on the 3090.
- **On-screen checks:**
  - a 24p film with motion intent in visibility and ease_slow (read the log's per-shot "motion scale"
    lines and the settle lines);
  - ambient adaptation with the sensor (lights on and off, steps at cuts);
  - led_protect on the FL35 (the LED floor holds, CNTR is never above need);
  - the display readout on a DV title.

## Still open

- The L11 frame-rate-conversion hint's decoding is an assumption until a live DV packet confirms it.
- The optional BT.2390 surround gamma and the VP-owned black curve are not built (the design defers
  them).
- The default-section "configured" flag for the look-ahead is read at startup, like the other queue
  defaults: removing the key from the default section takes effect after a restart.
- `integ/2026-10-17` has moved on since this branch started (fix/hunt2-timing merged, ca7e3670). This
  branch touches the render loop only in the walk depth/condition (around LVR 27258-27330), so expect
  a small conflict there when merging.
- Pre-existing timing-flaky LED and remote Config tests (see results).
