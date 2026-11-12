# P9 inventory: ConfigEditorWindow.cpp hunks against Bill (a591ad3b5)

Package P9 (refs/110 BRIEF P9, doc 60 s4.1-4.4). Baseline a591ad3b5 (PR #238 merge); branch r2/p9. Classes: H = calls a hook or provider (stays), U = unmovable here (reason given), M = moved in this package, fix = behaviour fix (own fix: commit).

## Moved (byte for byte, no new CEW line)

| What | Where it went | Row |
|---|---|---|
| sandboxPreview, queueLatencyNoteText (canonicalShortcutText stays in the .cpp, forward-declared by Helpers.h), showCanonicalShortcutSpelling, snapNumberFieldToStep, clampNumberFieldOnLeave, retiredChoiceTarget, addChoiceUnsetButton, markChoiceButtons, GlslShaderParameter, readGlslShaderParameters | control/config/editor/Helpers.h | i, h |
| migrateRefreshKeys, migrateLegacyQueueOwnership, migrateLegacyRendererName, migrateProfileValueNormalisations, foldLegacyInputPolicy | control/config/editor/Migrations.h | a |
| setRemoteApplyBusy | control/config/editor/ApplyEffects.h | d |

All three headers reach ConfigEditorWindow.cpp through the existing VpvRenderingDefaults.h include, so the file gains no include line and no hook line (hook gate: 0 new, ConfigEditorWindow.cpp 32 <= 32).

## Stays, with the reason

* Row b liveProfileCommand, addLiveToggle, refreshLiveToggles: all VP, but they use ActiveProfileRole (CEW constexpr) and controlName (unnamed namespace), both defined after the single VP-V include point, so a header reached from there cannot name them. A second include line in the Bill file is not allowed (rule 31). Follow-up once those file-scope names relocate.
* Row c shortcut validation and refreshConfigToggleShortcut: U by the brief (DIV-CFG-03 stays).
* Row e [vpv] apply classifier: lives outside the Config editor file; its fix: commit is separate and not part of this slice.
* Row f fallbacks heading: the editor shows the string ActiveProfileStatus::fallbacks already built by the producer; CEW holds only the label (createShell, config.live.fallbacks) and its setText. The GUI heading is StatusExtension::FallbacksHeading(const Snapshot&, NarrowFn) (StatusExtension.h:43) and is not called from CEW; nothing to move.
* Row g Startup page VP keys: createStartupPage and outputTransportSelections stay (S1 parts stay out).
* Row k close prompt (discardEdits, resolveUnsavedEdits, requestExit) and row l (ConfigCore, RPC, AppMain): U by the brief.
* Migrations through MigrateOnLoad(formApi_): that needs a document accessor on ConfigFormApi; the brief forbids new hook lines, so it is flagged to the lead and not added.

## Remaining differences by enclosing symbol

178 hunks in 36 symbols after the moves above.

| Symbol | Hunks | Added | Removed | Class | Note |
|---|---|---|---|---|---|
| (file scope) | 8 | +19 | -1 | U | includes and constexpr at file scope; include lines are Bill-file lines (rule 31) |
| effectiveChoiceValue | 1 | +4 | -0 | H | VP key list calls a Vpv helper; one call site stays |
| controlName | 1 | +199 | -0 | M/U | the VP-only helpers moved (editor/Helpers.h); the rest of the unnamed namespace is shared with Bill code |
| ConfigEditorPlacement::ClampFrameToWorkArea | 7 | +113 | -25 | U | window placement fix, fix: class; not a move |
| ConfigEditorWindow::finishLocalTargetSwitch | 1 | +5 | -2 | U | row l target switch |
| ConfigEditorWindow::selectAnotherTarget | 3 | +11 | -11 | U | row l target switch |
| ConfigEditorWindow::selectPage | 2 | +2 | -2 | U | page lookup by name |
| ConfigEditorWindow::loadConfiguration | 2 | +7 | -40 | U | row a/l load; calls runLoadMigrations |
| ConfigEditorWindow::runLoadMigrations | 1 | +105 | -0 | U (mixed) | row a: Bill call sequence plus the VPV-HOOK line; order stays in the .cpp; the VP-only steps moved (editor/Migrations.h) |
| ConfigEditorWindow::migrateUnifiedColorOutputProfiles | 1 | +5 | -0 | U | comment-only hunk (the B7 refresh-key convention note, row a); no code line differs |
| ConfigEditorWindow::migrateProcessingToScaling | 2 | +2 | -1 | U | Bill-original migration; two VPV-HOOK call lines (HideDitherAutoFromPlan, RestoreDitherAuto), already counted in the hook gate |
| ConfigEditorWindow::migrateSharedRefreshRate | 1 | +7 | -2 | U | Bill-original migration; VP edit makes removing an empty root a visible (dirty) migration, hunt 2 L9 |
| ConfigEditorWindow::migrateRefreshRateSwitchMode | 1 | +10 | -15 | U | Bill-original migration; VP edit rewrites the legacy boolean spellings (row a); stays beside runLoadMigrations |
| ConfigEditorWindow::migrateOutputDisplayProfiles | 1 | +8 | -0 | U | Bill-original migration; VP guard so an existing file with no legacy monitor is not dirtied by opening (DIV-CFG-05) |
| ConfigEditorWindow::installProfileContextMenu | 1 | +18 | -0 | U | row b profile live-command menu |
| ConfigEditorWindow::liveProfileCommand | 5 | +54 | -15 | U | row b/l: out-parameter shape of our helper plus the ProfileGroups lookup; uses ActiveProfileRole and controlName, both after the VP-V include point |
| ConfigEditorWindow::runTargetCommand | 3 | +7 | -0 | U | row l: settings-preview guard (sandboxPreview) and live-toggle refresh after a target command |
| ConfigEditorWindow::refreshActiveProfileIndicators | 2 | +8 | -1 | U | row b/f live status |
| ConfigEditorWindow::applyActiveProfileIndicators | 2 | +3 | -1 | U | row b live status |
| ConfigEditorWindow::validationErrors | 1 | +7 | -0 | U | row c: shortcut issues block Apply (audit 2026-10-16); validation shares Bill list |
| ConfigEditorWindow::updateValidationState | 3 | +27 | -2 | U | row c: marks and clears invalid shortcut fields in Bill validation loop (audit 2026-10-16) |
| ConfigEditorWindow::notifyVideoProcessor | 1 | +3 | -2 | U | row l RPC |
| ConfigEditorWindow::applyChanges | 1 | +2 | -0 | U | comment-only hunk (remote Apply lock note, row d); no code line differs |
| ConfigEditorWindow::rebuildConfigurationShell | 1 | +1 | -0 | U | one-line VP edit: pathwayRefreshers_.clear() when the shell is rebuilt |
| ConfigEditorWindow::saveChanges | 18 | +272 | -48 | U | row d apply/save; no seam without new hook lines |
| ConfigEditorWindow::createShell | 5 | +48 | -10 | U | row j/l shell and page order |
| ConfigEditorWindow::addNavigationButton | 1 | +1 | -0 | U | one VPV-HOOK line (pageName property), already counted in the hook gate |
| ConfigEditorWindow::createStartupPage | 7 | +260 | -12 | U | row g Startup VP keys (S1 parts stay out) |
| ConfigEditorWindow::createProfilePage | 78 | +2745 | -111 | U | VP rows inside Bill profile pages; owned by the provider-card packages, not P9 |
| ConfigEditorWindow::createNlsShadersPage | 2 | +2 | -0 | U | two lines |
| ConfigEditorWindow::createStandardShadersPage | 7 | +202 | -1 | U | row h shader PARAM editor UI (calls the moved readGlslShaderParameters) |
| ConfigEditorWindow::createShortcutsPage | 3 | +18 | -28 | U | row c, DIV-CFG-03: shortcut field table replaced; stays by the brief |
| ConfigEditorWindow::refreshConfigToggleShortcut | 1 | +22 | -0 | U | row c, DIV-CFG-03: config toggle shortcut refresh; stays by the brief |
| ConfigEditorWindow::setupTray | 2 | +3 | -2 | U | row k: no tray in the settings preview; exit routes through requestExit |
| ConfigEditorWindow::reveal | 1 | +1 | -0 | U | row l |
| ConfigEditorWindow::closeEvent | 1 | +7 | -0 | U | row k: close with unsaved edits asks Save / Discard / Keep editing (audit 2026-10-17) |

Generated from git diff -U0 a591ad3b5 HEAD on src/VideoProcessor-Config/ConfigEditorWindow.cpp, grouped by the nearest preceding function definition in the current file.
