# Audit — merge of 1.4-beta-rc3 into stage7-rc2 (c7e11957) + 6c-2 note fixes (48b1b830)

Reviewed: `git diff 48b1b830 c7e11957`, `git show --cc c7e11957`, `git diff f4c4f920 48b1b830`
(source). RC3 = 8186661b; merge base = 1.4-beta-rc2.2 (19d74440).
The other Claude session was idle throughout; the checkout stayed clean.
Build: OK — full x64 Release /t:Rebuild at e7b0a969 (= c7e11957 + docs), 0 errors, no new warnings.
Tests: native 2485 total, 2484 passed, 1 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted —
known). New/RC3 tests pass: PeakDetectionOffWithTuningAtAutoKeepsTheStaticCurve,
ToneTuningMapsEveryNativeParameterAndPreservesPresetDefaults, ToneTuningValidatesBoundsAndInheritedRelations,
OcclusionMaskFollowsTheNlsMap (now 4 profiles), NlsMaskMappingMatchesTheShadersCentreScales.
VideoProcessorConfigTests 101 passed, 3 failed: the two known RPC tests and "remote target switch
keeps window responsive" (known intermittent).

## Blockers
- None.

## Should fix
- (2) **One RC3 page index was missed.** `ConfigEditorWindow.cpp` ~line 4403
  (Bill's b9073917, auto-merged, so not shown as a conflict):
  `button->setEnabled(button->property("pageIndex").toInt() == 20);`
  When Config has no target yet (the "Checking for VideoProcessors…" empty
  screen), RC3 leaves only **Updates** clickable. Here page 20 is our **LED
  drive**, so the LED drive entry is enabled instead and Updates is greyed
  out. Change it to 21. No test covers this screen. It does not affect
  playback.

## Notes
- (4) With the updater missing, VP checks for it 30 s after start and then
  every 6 h, but does nothing because the file is not there (no message, no
  network). VP and Config also check at start for an update lock file; it
  can only exist while setup is running, so with no updater they never stop.
- (6) The NLS-V case in the GPU test uses ratio 1.33; the owner's NLS-V
  profile allows up to 1.22. It is still a valid test of the vertical map.
- Build warnings in RendererProfileConfig.h are the old "value hides…"
  warnings; they only moved because RC3 added lines above them.

## Verified OK
- (1) **Conflicts:**
  - Renderer settings: both sides kept, nothing duplicated or lost.
    - RC3's `toneTuning` and our saturation, de-judder strength, match
      tolerance, cut threshold and occlusion_mask are all in the settings,
      the fingerprint, the parsing, the copy to the render parameters, the
      transport copy and the live-change list ("tone_mapping_tuning" plus
      ours).
    - The tone fingerprint ends with its own '|', so the fields stay
      separated.
    - Validation in RendererProfileConfig merged cleanly: RC3's tone keys
      and checks between profiles, plus our keys.
  - Config editor: our de-judder box and preset sync are kept, and RC3's
    three tone-mapping sections and their refresh call are added after
    them. No line was lost.
- (2) Pages: the LED drive page is 20; Updates is 21 in all of these: the page
  list, the sidebar entry (`config.openUpdates`), `showUpdates()` and the
  page-count test (22). Our LED test uses page 20 as before. RC3's Updates
  tests open Updates by its entry or `showUpdates()`, not by number. The only
  other 20 is the Should fix above.
- (3) Static HDR→SDR:
  - With `peak_detection: off` libplacebo gets no peak-detection settings,
    and RC3 applies the five dynamic tuning keys only when peak detection is
    on. So none of them can have an effect.
  - With every key on Auto, the tone constants are left as libplacebo's
    defaults. The new test compares them byte for byte with
    `pl_color_map_default_params`. Clamping the fallback knee changes
    nothing at the defaults (0.1 ≤ 0.4 ≤ 0.8).
  - The cinema test config has `tone_mapping: spline`,
    `peak_detection: off`. Note: the nine curve keys (knee, slope, contrast)
    would still change the static curve if someone sets them; on Auto they
    do nothing.
- (4) Updater absent:
  - VP's "Check for updates..." (window menu) shows "This copy does not
    include the updater…" and stops there.
  - Config's Updates page shows the same message and starts no helper.
  - Nothing is downloaded, installed or blocked; all networking is inside
    VideoProcessorUpdate.exe, which is not deployed.
- (5) Everything else from rc2.2..rc3 comes in unchanged. I compared each
  file's changes with RC3's own: 35 of 40 are identical. The other 5
  differ only by the page 20→21 / 22 changes, the new test, and one line's
  indentation (tab kept) in the fingerprint.
- (6) Note fixes:
  - **Exact file names:** the folder part is removed, then the name must
    equal `NLS.glsl` / `NLSPlus.glsl` (any letter case), so `MyNLS.glsl` no
    longer counts.
  - **Geometry:** a fraction is cut off, as in the shaders (0.7 → 0; 1 and
    above → 1). Unit-tested.
  - **Stale NLS reason:** "mask off (NLS shader not known)" is cleared as
    soon as NLS is off.
  - **NLS-V** is in the GPU test (NLS.glsl, vertical axis, the profile's
    protected/0.35/curve 2), with both crops, and it passes.
