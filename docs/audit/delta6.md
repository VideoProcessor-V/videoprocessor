# Audit — stage10-6d fixes (0d547734) and Delta 6 settings-first (460939a5, 02f4d03a)

Range c85e9a9c..7bf8a0f1 (7bf8a0f1 is docs only).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at 7bf8a0f1, 0 errors. VideoProcessor-BetaTests built: OK.
Tests:
- native 2532 total, 2530 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
  SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known). KeyProfileChoiceLastsUntilTheRulesChange,
  UnifiedProfileRuntimeReloadsEditedViewportAndKeepsSelection, DejudderRenderedWarpMatchesTheSourceWarp
  and the ExtendedRange tests pass; RendererMetadataCycles… passed in this run.
- VideoProcessorConfigTests: first run 102 passed, 3 failed (the two known RPC tests plus
  "remote-only client opens its sole discovered VP"); rerun 103 passed, only the two known RPC failures.
  So the third is intermittent (same RPC family).
- VideoProcessorBetaTests (377): all PASS. It ran this time: 6d cache max 1, hold max 0, extra vs
  source 0.165; extended range greys 4…1019 kept, edges max 1 (ewa mean 0.034).

## Blockers
- None.

## Should fix
1. **`use_frame_queue` live apply only changes the check box.**
   - Clicking the main window's "use queue" box sets
     `m_wantToRestartRenderer` so the renderer picks it up.
   - A Config Apply that changes `use_frame_queue` only calls `SetCheck`,
     which does not run that handler. The running renderer keeps the old
     queue mode while the box shows the new one, until something else
     restarts it.
   - Either take the same restart path on a changed saved value, or say in
     the tooltip that it applies at the next start.
   - (`queue_auto_reset` is read live, so it is fine.)
2. **"Key override" entries that stay after the key's change is undone:**
   - The automatic display-rule key (`OnCommandDisplayRuleAuto`) does not
     clear "display rule (DirectShow)".
   - "shader rule manual" is never cleared, except at restart.
   - "fullscreen" and "video only" are recorded on every toggle, so pressing
     the key twice (back to the saved state) still shows an override.
   - The overlay should show only overrides that are still active. The
     stats overlay entry already handles this correctly (it compares with
     the saved value).

## Notes
- **What releases a key override.**
  - `Refresh` releases **every** live key override when the rules'
    combined result changes in **any** group.
  - So a Shift+Q queue choice is dropped when, say, the source goes from
    HDR to SDR and only the rendering group's rule result changes. A
    per-group release (only the groups whose own rule result changed)
    would match "until a profile change" more closely. The planner should
    confirm which is meant.
  - A transient change in the source values while the context stays valid
    (e.g. a rate reported differently for a moment) also releases them
    permanently. Signal loss is safe: it is deferred.
  - A renderer change re-evaluates the rules, but releases only if a rule
    actually uses `${renderer}`.
- **Config Apply.**
  - A reload keeps a live override while its profile exists. Reload keeps
    the last rules signature, so an Apply that leaves the rules' result
    unchanged keeps it. Tested.
  - An Apply that adds or removes a group, or changes a rule's outcome for
    the current source, changes the signature and releases the overrides
    on the next refresh. That is defensible as "a profile change".
- **`persist_profile_selection: true` is not exactly as before.**
  - The saved choice still survives restart and still acts as a fallback.
  - But a live key override is now released when the rules' result changes.
    Before delta 6 it stayed in force across source changes.
  - The Config tooltip and the delta doc call `true` "the earlier
    behaviour". Either keep live overrides under `true`, or reword it.
- **The default flip affects existing installations.** A deployed config
  without the `persist_profile_selection` key loses persistence silently.
  Mention it in the deploy notes.
- **global_hotkeys.**
  - Only the background observer is filtered. The in-focus accelerator path
    and the fullscreen window's key forwarding
    (`WM_MESSAGE_EXTERNAL_SHORTCUT`) are unchanged, so every key still
    works while VP (including its fullscreen window) has focus.
  - Esc (a bare key) is focus-only, which is right.
  - Alt+Enter (Alt) stays global.
  - The Config key Ctrl+Shift+S is a `RegisterHotKey` hotkey, outside the
    observer, so it stays global.
  - Treating Shift+letter as typing is a sensible refinement, and it is
    flagged in the doc.
- **Remaining key-only behaviour.** The subtitle preview cycle
  (Ctrl+Shift+T) has no saved setting and is not on the overlay. It is a
  diagnostic, like the output sweep's Space (documented). Everything else
  left without a setting is an action (reset, restart, reapply rules, show
  profiles, capture output), not a state. Otherwise constraint 12 holds.
- **Sample config edited.** The portable ZIP's `VideoProcessor.cfg.example`
  must be regenerated byte-identical at packaging
  (tools\test_portable_config_contract.ps1).
- **Stage 10 guard.**
  - The carry is limited to ±0.5 and the restore ignores |alpha| > 0.5009.
    Real BTB/WTW offsets are at most about 0.1, so the margin is ample.
  - A hook that writes some other alpha *within* ±0.5 would still be added,
    but the common `vec4(rgb, 1.0)` case is closed and GPU-tested.

## Verified OK
- **Blocker 1 (stage10-6d):** `ReleaseDejudder()`, which `RetireOutput`
  calls before `pl_d3d11_destroy`, now releases the cache texture (and
  clears its gpu) and destroys the rendered warp (texture and dispatch).
  `~Cache` calls the same `Release`, so it is safe after an earlier release.
- **Item 2:** `DejudderRendered::Rounded` is the same arithmetic as
  libplacebo's renderer.c in both 7.360.1 and 7.377:
  - `roundf` of the destination rectangle clamped to the framebuffer;
  - the source moved by the same scale.
  - The extra's target crop is the rounded rectangle, the size of the cache
    texture, so the render is exactly 1:1. GPU test: fractional crop, hold
    max 0.
- **Item 3:** the cache is armed only with `dejudderExtrasAllowed` (not in
  mask-only mode).
- **Delta 6:**
  - `stats_overlay`, `use_frame_queue`, `queue_auto_reset` and
    `[shortcuts] global_hotkeys` are validated and read at start (an invalid
    value keeps the default, logged). Each has a Config control in the right
    section with the right default, all covered by the Config test.
  - The stats overlay starts from its saved value (timer in
    `OnInitDialog`). A live change applies only when the saved value
    changes, and ends the key override.
  - `persist_profile_selection` defaults to false in both readers, and the
    tooltip explains `true`.
  - The overlay's "Key override" line lists the groups a key holds plus the
    session entries. The unified-profile keys return before the DirectShow
    display-rule entry.
  - `IsGlobalSafeShortcut` (Ctrl, Alt or F1–F24) is unit-tested.
  - The DirectShow-only labels are on the Shortcuts page, and the sample
    comments are corrected (scope on F2; shipped keys are A/B backups).
- **02f4d03a:** the viewport test now rewrites at a different size, so it
  avoids the known same-size cache weakness.
