# Hunt 2 fix group: config (Config editor, coverage test, docs)

Date: 2026-10-17. Lead branch `fix/hunt2-config` on the fork (vmcpherson2012-dev/videoprocessor).
Base `bbbd2797` = merge-v1.4.02 `cd544707` + ci/github-actions `bc0ed443`.
Inputs: scratchpad hunt2/11-config.md (M1-M6, L1-L9, I1-I2) and the Config/docs items of 15-intent.md (C11, D1-D13, missing docs).

## Branches

| Branch | Tip | Content |
|---|---|---|
| fix/hunt2-config-a | c0103d64 | migrations, backups, wheel guard, greyed gaps, inherited Picture fields |
| fix/hunt2-config-b | bc08b127 | missing controls, MainConfigSchema rule lists, coverage test |
| fix/hunt2-config-c | 70461890 | docs |
| fix/hunt2-config | this commit | merge of a, b, c + lead sweep fixes |

## Fixes

### Migrations and backups (a)
- M1: the legacy queue fold is `migrateLegacyQueueOwnership()` in `runLoadMigrations()` (ConfigEditorWindow.cpp ~2911-2962). It sets dirty, `hasPendingMigrations_` and `requiresMigrationBackup`. The page-build call stays as a fallback and also sets the flags.
- M2: LUT slash normalisation, retired downscaler -> AUTO, and removal of a malformed crop aspect limit now run once at load (`migrateProfileValueNormalisations()`, ~2979-3027). `loadDetails` only displays. The removal now marks the editor dirty.
- The former renderer display name is also a load migration (`migrateLegacyRendererName()`, ~2964-2977).
- L3: switching target keeps the dirty flag that `loadConfiguration` set (~2130).
- L9: removing an empty `[vprenderer]` or `[vprenderer.input]` header counts as a visible migration (~2270, ~2877).
- Safety net at save (~4915): any pending migration on an existing file forces the backup.
- ConfigEditorCore.cpp:
  - L1: `GetLastError` is read only after a failed `GetFileAttributesW`.
  - L2: Overwrite after a conflict first copies the file on disk to `<cfg>.before-overwrite.<local time>.bak`.
  - L8: migration backups are named `<cfg>.before-config-migration.<local time>.bak` (`BackupTimestamp`).
  - The status line names whichever backup was made.

### Wheel guard, greyed state, number fields (a)
- M3: `UnfocusedWheelGuard` also covers `QAbstractSlider`, which includes the Motion and Picture sliders. Scroll bars are excluded (~616-630).
- L4: these controls are greyed from the effective values while they have no effect (~8552-8598): the optical-flow extras, render ahead's gate/credit/share, the source-resolution Auto evidence fields, `interpolation_grain` and `deband_vp_grain_guard`.
- L5: an inherited Picture field counts as not set, and leaving an inherited Picture or Motion field writes nothing.
- L6: `render_ahead_utilisation` and `intent_content_hold_seconds` are clamped when the field is left (`clampNumberFieldOnLeave`).
- I2: the stale comment is fixed.

### Controls and coverage test (b)
- MainConfigSchema.h exposes every rule list that `Validate` uses (no behaviour change).
- New Startup controls (~5433-5490, ~5562-5634):
  - `[decklink] rgb_8bit_packing`, `rgb_10bit_packing`, `rgb_12bit_packing`
  - `[directshow.conversion] conversion_method`, `chroma_downsampling`, `min_core_count`, `max_core_count`
  - `[directshow.ppm] ppm`
  - `[general] bounded_invalid_capture_recovery`
- Choices start at "Not set", so opening Config writes nothing.
- Subtitles profiles get `subtitle_hold_ms` (0-1000, default 250) and `subtitle_near_bar_px` (0-200, default 20) (~9605, fallback table ~10119).
- M5/M6/L7/I1, coverage test:
  - The scanner matches `key\s*==\s*"`.
  - It walks every schema list.
  - A control covers a key only in its own section.
  - The allow-list is keyed per section, and stale entries fail the test.
  - The two live subtitle keys are off the allow-list.
  - `subtitle_detection_optimization` is allow-listed with its real reason: it is accepted only in renderer/Screen sections and reset before the Subtitles profile applies, so it never takes effect.
- Sample VideoProcessor.cfg comments no longer say "file-only".

### Docs (c + lead)
- D1-D13: the docs now say VPV is the only 12-bit route (for DV displays), FP16 output is parked, and the DeckLink output card is rejected. Covered: roadmap, the 12-bit and DV/FP16 audits, fidelity roadmap Q1, and the matcher follow-up item (lead).
- Also corrected:
  - NVOF extras defaults
  - `dejudder_gate auto`
  - GPU capture defaults
  - hiccup repair standalone
  - detail accumulation and the Stage 11b learned candidates rejected
  - LED script = intent reference
  - read_back recorded as an open owner question
- BTB/WTW have no meaning after HDR->SDR tone mapping, by design: stated in the BTB/WTW answers, the investigation, the dither-judder plan, BUILD-STATUS and the roadmap.
- Added: VP-auto-calibration-design.md, VP-projector-profiles-design.md, VP-stage12-latch-review.md, VP-renderer-mpc-filter-design.md, VP-fixed-constant-sweep-2026-10-17.md.

## Lead sweep (whole merged result)
- Found: the a branch's CI build failed with C2001. Two new test fixtures (`testInheritedPictureFieldsAndGreyedRenderAhead`, `testProfileRewritesAreBackedUpLoadMigrations`) had real line breaks inside string literals. Fixed in 434c6ca7. A quote scan of every changed C++ file finds no other cases.
- Checked, no interaction found:
  - The new Startup combos fall under the extended wheel guard.
  - a's L9 empty-header migration does not dirty the fixture of b's "no write on open" test (no `[vprenderer]`).
  - a's `greyed_state_refresh` QAction name does not affect b's coverage test, which checks only schema keys.
  - The legacy-queue allow-list entries match a's load migration.
  - Every renderer-level legacy subtitle key on the allow-list is reset in LVR ("Discard legacy subtitle settings").
  - merge-v1.4.02 has moved on since the base (render-ahead), but it does not touch Config files.
- The sub-branch runs for b and c were cancelled by the coordinator because they were redundant after the merge. The lead-tip run is the reference.

## Switches
None. These are pure correctness and coverage fixes. New controls default to "Not set", so the runtime defaults are unchanged.

## Tests added or extended (Config UI)
"hunt2 startup controls write parser spellings", "hunt2 subtitle detection fields", "profile rewrites are backed-up load migrations", "inherited Picture fields and greyed render ahead"; extended: "every schema key has a Config control", "unfocused number boxes ignore the wheel" (sliders), the overwrite test (backup), "unrelated content remains exact" (chroma control). No GPU tests.

## Deferred
- The `ppm` and min/max core fields can store Intermediate validator input (Save is then refused by the parser; nothing is corrupted). Same class as L6.
- The base predates ci/github-actions 550f3f0e/951e920b, which exclude the 4 runner-bound tests, so those 4 fail on this branch's CI by design.
- Not built locally: both CPU slots were taken by other groups. CI is the build of record.
- ConfigEditorWindow.cpp is touched only in the ranges above. The organisation pass is not done.

## CI
- Cloud run 39067248048 (tip 434c6ca7): https://github.com/vmcpherson2012-dev/videoprocessor/actions/runs/39067248048
  - Build passed and the CPU-test step completed. The Test summary step failed; the failing names could not be read.
  - The GitHub account was then suspended (HTTP 403), so neither the log nor the Config-test step could be read.
  - The base predates the exclusion of the 4 runner-bound tests, so this summary failure is expected. It is not confirmed.
  - Earlier runs: the a branch's run 39064201153 failed to build with C2001, which is fixed in 434c6ca7. The b and c runs were cancelled as redundant.
- Local verification (laptop) of 434c6ca7, in the detached worktree D:\codexideoprocessor\hunt2-config-b, slot1:
  - Built VideoProcessor-ConfigTests, VideoProcessor-Test and VideoProcessor-VPRendererBeta (x64 Release, /p:VpCiNoMagewell=true; this machine has no Magewell SDK at E:\).
  - CPU tests: vstest with the CI filter (FullyQualifiedName!~RendererCaptureHarnessTests): 3094 run, 3094 passed, 0 failed. The 4 tests excluded on the runner pass here.
  - Config tests: `toolsun_config_tests.ps1 -Shards 1`, off screen. 141 passed and 1 failed. The failure was "config keys activate the renderer and matcher settings": the Beta renderer DLL was not yet built in that worktree. After building it, that test passes, so 142 of 142 pass. One test is skipped because it needs a real window.
