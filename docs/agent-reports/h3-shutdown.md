# Bug hunt 3, hunter D: shutdown, shortcut and crash-state fixes (SH1-SH12)

Branch `fix/h3-shutdown`, from `fix/dv-input-1018` (7d164c22). Local only.
SH11 (the problem-report worker) is on `fix/h3-state-report` (6655c795),
from `feat/state-report` (4cfe0a0c), because the report code is not on this
base; merge it with the state-report feature.

## Fixes

| Item | Fix | Test |
|---|---|---|
| SH1 | `WM_QUERYENDSESSION` is answered TRUE and tears nothing down; `WM_ENDSESSION(TRUE)` runs the bounded close path in place: cancel the RS-232 test, bounded LED restore on every reachable renderer (LDMM 0, full light, CNTR 50), topology restore under a timed lock, crash state marked clean, log flushed, process ends (exit 0). `WM_ENDSESSION(FALSE)` changes nothing. `SessionEndShutdown.h` `SessionEnd::Handle`; `VideoProcessorDlg.cpp` `OnQueryEndSession`/`OnEndSession`. | `BugHunt3ShutdownTests::SessionEndRestoresTheLightTopologyAndCrashState` sends both messages to a message-only test window driving a ProjectorDriver on a fake port, and expects "light restored to 255", LDMM 0, CNTR 50 and the step order. `CancelledSessionEndKeepsVpRunning`. |
| SH2 | `OnCancel` override: IDCANCEL (Esc, Shift+Esc, an unbound Esc) goes through `OnClose` (deadline, watchdog, restores), never `EndDialog`. | `CancelIsRoutedThroughTheClosePath` (policy). |
| SH3 | `WaitForShutdownWorkers` now also joins, under the same bound, the liveness watchdog, the unified action workers and a running RS-232 test. The App owns the dialog in a `unique_ptr` and destroys it under `ShutdownDeadline::ExitGuard` (10 s; on overrun: topology restore, crash state kept, terminate) before `CrashState::Stop(clean)`; the log line "Application teardown complete: crash state marked clean" precedes it. | `ExitGuardFiresOnlyWhenTheTeardownOverruns`. |
| SH4 | Topology restore mutex is a `std::timed_mutex`. `RestoreDisplayTopologyBounded` (forced exit, dialog-destroy guard, session end) waits 2 s, then skips the step (pending record kept for the next start). | `TimedLockSkipsTheTopologyStepWhenTheMutexIsHeld`. |
| SH5 | `ConnectionTestPlan::cancel`: the 5 s hold ends at once and the high step (light back up) always runs; a test cancelled before it starts sends nothing. VP sets it in `OnClose`, waits for it (bounded) in `WaitForShutdownWorkers`, the forced exit and the session end, and does not resume the LED drive while closing. | `ConnectionTestCancelledDuringTheHoldRestoresTheLight`, `ConnectionTestCancelledBeforeItStartsSendsNothing`. |
| SH6 | The low-level hook observes only explicitly global bindings: `[shortcuts] global_actions` (comma list of action keys, default empty = none) or `global_hotkeys: true` (every binding). By default the hook is not installed at all. Decision in `ConfigurationLiveApply::DecideBackgroundShortcut`: VP acts in another app only for a global binding and never swallows a key it does not act on. AltGr (Right Alt reported with Ctrl, no Left Alt) never matches a Ctrl+Alt chord (`IsAltGr`, `KeyboardShortcutMatches`) in the hook, the dialog and the fullscreen route; the documented AltGr+Enter fullscreen relaxation stays. `global_actions` is validated (`MainConfigSchema::ShortcutGlobalActionsValid`) and has a Config field. MPC file playback: no action is global by default; Alt+Enter, Ctrl+1..4 and Ctrl+I are MPC keys too, so leave those out of `global_actions`. | `BackgroundShortcutsActOnlyWhenExplicitlyGlobal`, `AltGrIsNotCtrlAlt`, `GlobalActionsListIsValidated`. |
| SH7 | Defaults moved to modifier combinations (below). Esc stays the `fullscreen_exit` default, flagged `fullscreenFocusOnly`: it acts only while fullscreen is on and a VP window has the keyboard (fullscreen host, renderer window in it, or VP's own window - the existing behaviour, since Windows can raise the topmost output without moving the foreground). In VP's window without fullscreen it is consumed and does nothing, so it cannot become IDCANCEL. Explicit user bindings are untouched. | `ShortcutDefaultsTests::DefaultsUseModifierCombinations` (new), `NoTwoDefaultShortcutsCollide`, shipped-config test. |
| SH8 | Config field "Automatic display rule (legacy layout only)" for `display_rules_auto`; locked in a unified configuration (RendererProfileConfig rejects the key there and VP ignores it). VP reads the key from the main file's `[shortcuts]` first, the renderer file as fallback. | Covered by the Config tests run. |
| SH9 | Config's own `config_editor` QShortcut is rebuilt from the document after every save, remote apply and reload, using the shared chord grammar (empty disables it). VP's editor detection and Config's single-instance check accept "VideoProcessor Configuration · <target>" (`IsConfigurationEditorTitle`). | `RemoteConfigTitlePassesTheFocusCheck`. |
| SH10 | One grammar: `CanonicalizeKeyChord` takes F plus one or two digits (1-24), as VP did; VP parses through it (`ShortcutChord.h`, `TryParseShortcut`). An invalid fixed chord is now logged ("Shortcut ignored: ... using the compiled default"). | `ConfigAndVpShareTheFunctionKeyGrammar`. |
| SH11 | (`fix/h3-state-report`) The report worker signals done; `WaitForShutdownWorkers` cancels its thumbnail wait and waits for it (bounded). Retention removes `vp-report-*.zip.partial` older than 5 min (`RemoveStalePartials`). `MaybeAutoReport` and `AutoReportLimiter::Allow(now, busy)` spend no slot while a report is being saved. | `StateReportTests::AutoReportLimiterBusySpendsNoSlot`, `RetentionRemovesStalePartialWrites`. |
| SH12 | Live files per instance: `crash-state-<pid>.txt/.tmp/.dmp`, plus `crash-state-<pid>.lock` held exclusively (delete-on-close) for the instance's life. A start keeps only leftovers whose instance is gone (and the old single-name file), oldest first, so a running instance is never renamed into "crash state (N)" and only real unclean exits count against the ten kept. | `CrashStateFilesArePerInstance`, `KilledInstanceLeftoverIsKeptButALiveOneIsNot`; existing CrashState tests updated to `LivePath()`. |

## Default shortcuts (ShortcutDefaults.h)

auto_set Ctrl+Shift+A; fullscreen_exit Esc (fullscreen + VP focus only);
fullscreen_toggle Alt+Enter; toggle_stats_overlay Ctrl+I;
capture_rendered_output Ctrl+Alt+S; raw_capture Ctrl+Alt+W;
show_profiles Ctrl+Alt+I; pq_set Ctrl+Shift+P;
renderer_restart Ctrl+Alt+Shift+R (was Shift+R); renderer_reset Ctrl+Shift+R
(was R); capture_1..4 Ctrl+1..4; video_conversion_off Ctrl+Alt+V (was V);
video_conversion_p010 Ctrl+Shift+V (was Shift+V); config_editor Ctrl+Shift+S;
toggle_noui Ctrl+Shift+U; display_rules_auto Ctrl+Shift+F4 (was F4);
reapply_rules and room_lights none. Global actions: none.

## Verification

- Full x64 Release build, rc=0 (both branches). LTCG needed stale `.iobj/.ipdb`
  under `x64\Release\obj` removed once (LNK1103/1318).
- CPU tests, single-threaded, TRX: Led|Config|CrashState|Shortcut|BugHunt|
  Retired|Hiccup|Lifecycle|Shutdown - 648 run, 648 passed.
- Config tests off screen, 1 shard: 143 passed, 0 failed, 1 skipped (needs
  a real window), gate passed (1258 s).
- `fix/h3-state-report` (SH11, 6655c795): StateReport|DiagnosticHistory|
  StoreZip|BugHunt2Gui - 47 run, 47 passed.
- No GPU runs. Windows shutdown was not triggered; the session messages were
  sent only to a test window.

## Notes for the owner

- Behaviour change (SH6): with the defaults, no VP shortcut acts while
  another application has focus (Ctrl+Shift+S no longer reveals Config from a
  browser). Add `global_actions: config_editor` (or others) to keep one.
- An unbound Esc / Shift+Esc in VP's window still closes VP, now through the
  orderly close path (SH2), as before but bounded.
- The shipped VideoProcessor.cfg's own profile shortcuts (F2, F3, F5, F6, N,
  Shift+N, Shift+P, Shift+Q, Shift+L, Shift+A) are user bindings and were not
  changed; the owner rule may want them revisited.
