# Integration round 5 (2026-10-18): bug hunt 3, efficiency, fixed constants B3-B7, test infrastructure

Branch `integ/round5` in D:\codex\videoprocessor\integ-r4. It starts at `integ/round4` 3164dda2.
Everything was local: no remote calls and no GPU runs. Each branch's agent report was read before it was merged.

## Merges (all --no-ff, in order)

| # | Branch @ tip | Merge commit | Conflicts and how they were resolved |
|---|---|---|---|
| 1 | fix/gate-r3 @ 0677a10e | 0f991a4f | none |
| 2 | test/eff-gpu @ de54c404 (supersedes feat/efficiency b7d09db3, which was not merged) | 3ff71fc6 | The efficiency_fixes switch sat beside round 4's general_rate_fixes in the settings struct, fingerprint, parse, change list, dump, Config control and defaults table, and the harness key tests. Kept both. AnalyzeActive walk: the efficiency `WantsFrame` gates now apply to round 4's per-frame `statisticsWant` / `contentWant`. A frame is skipped only when none of statistics, content or intent wants it. |
| 3 | feat/no-fixed-constants-2 @ 014b4ff3 | d40ce1c2 | general_rate_fixes tooltip: took the B3-B7 text and kept the efficiency control. |
| 4 | fix/h3-signal @ 12041c7f only (eaa4e9ab, the parked MPC work, excluded) | 52c78323 + 908c7ef2 | Capture callback: DV take-back hold first, then the C2 veto measured before the frame on an armed veto, then the frame, then the hand-off probe. Processing note: recorded only outside a veto preview. 908c7ef2: the veto preview (BuildHdrVetoSdrState) no longer touches the DV hand-off probe state. |
| 5 | fix/h3-picture @ 11cf28e0 | 77736dd1 | none |
| 6 | fix/h3-timing @ 09667112 | 0dc83f0e | IRenderer.h: SetActivePictureLookaheadAutomatic is appended after calibration's two methods. Kept both gap members (vpFillMaxSpan, vpFillGapFrames) and both includes. **Plugin API 39 -> 40.** H3TimingTests F2 now uses gate r3's floor `kLockedQueuePairFrames` (3), so target 5 at 3:2 with an automatic look-ahead gives 3, not 2. |
| 7 | fix/h3-led-config @ 90e764ec | 3e678e3e | The AppendConfigBoolOption calls use ConfigBoolSwitches. `dv_tunnel_handoff` was added with defaultOn = true, and the test asserts it. The detail bools are renderer-profile keys, not this helper. Merged the tsv and Config placeholder lists, and both test sets (a missing brace was repaired). CFG9: [general] queue_size is read as the branch does. RetiredSettings had no queue_size entry, so there was nothing to remove. |
| 8 | fix/h3-shutdown @ 8771c155 | 3523d159 | ShortcutDefaults: modifier-only table plus round 4's reserved-chord array. Config load: refreshConfigToggleShortcut plus round 4's CFG7 fold. TryParseShortcut stays on StateReportSettings::TryParseChord, which now parses through ShortcutChord.h, giving one grammar (SH10). OnClose: RS-232 cancel plus the calibration shutdown. |
| 9 | fix/h3-state-report @ 6655c795 | 46182bd7 | WaitForShutdownWorkers waits for the SH3/SH5 joins and the SH11 report worker, and logs one combined line. Kept both header members. |
| 10 | test/real-footage-suite @ 8ceaaa33 | 58c00b17 | Only the vcxproj conflicted. The two fixes (maximally even lock, stream-start cut judge) merged cleanly. |
| 11 | test/queue-stress @ 93f20fe1 | 79775aa1 + 6ba64910 | vcxproj. The Q1 repros were marked TEST_IGNORE / KnownFailing "Q1 pending fix/q1-stale-burst". |
| 12 | fix/q1-stale-burst @ 7efcf7f0 (coordinator addition) | b29fa7f2 + e62f4673 | none. The ignore markings were removed, and both repros pass. |
| 13 | perf/detail-pulse @ c106339b (coordinator addition; base 11cf28e0) | 462a95bc | Harness fingerprint change list: both keys. detail_pulse_fast is covered by the Config coverage test. |
| 14 | fix/h3-latch @ 61288d09 (coordinator addition; base 8ceaaa33) | 7d217dde + ea7ae114 | Config defaults table and LVR fingerprint: both keys inserted beside source_resolution_latch_fixes. Detector header: kept SnapRatio and FrameEvidence. Render-ahead extras: kept the crossfade mix and added the start-up hold clear. **Real failure (merge 14):** `SourceResolutionMonitorDeclaresAutoEngagesAtACutAndDisengagesAtOnce`, "the fade made no cut". With the cut fixes, the held shot's own cut (moment 10.0 s) is confirmed at the first fade frame (10.5 s). The judge is right, so the test was fixed: it now counts a new switch moment, or a confirmed cut whose moment is in the fade (ea7ae114). It passes. |

RefreshKey migration (merge 3): ConfigFile::Load rewrites `m_sections` in memory only, and ConfigFile has no save. In the Config editor, `migrateRefreshKeys` sets `dirty_` and `hasPendingMigrations_`. runLoadMigrations then sets `requiresMigrationBackup` whenever the document changed, so the save keeps a backup.

## Verification

- **Build.** x64 Release through the shared runner. The first build hit LNK1103/LNK1318 (stale .iobj/.ipdb and Lib pch.obj); 18 files were deleted, and the rebuild passed. The builds after each later merge: rc 0.
  - After the h3-latch merge, two builds hit LNK1104 on `vprenderer\VideoProcessorVPRenderer.dll`. A transient lock not held by any of my processes; the retry built.
  - Final build (ea7ae114): rc 0.
- **Build identity.** 28/28 on the final build (ID f3536a635215a3c20c55adf276141010).
- **CPU suite.** Single-threaded vstest with TRX (`D:\codex\vp-r5-logs\trx\cpu.trx`), on the build with merges 1-12. Excluded: RendererCaptureHarnessTests, NlsHookFailureRetry..., DvAutotestGpu*, DolbyVisionGpuUnpack....
  - 3899 total: 3897 passed, 1 failed, 1 not run (LongTakeCameraMoveIsNotACut, ignored by its branch).
  - The real-footage suite ran on D:\testclips (about 90 min).
  - Q1 repros and StaleBurstFilmSoak pass.
- **After the pulse merge.** DetailPulse*, TextureRestore*, DetailRestoreConfigTests, ConfigFileTests and QueueStressRepros: 208/208 (`pulse.trx`).
- **The failure.** DeckLinkConvertersSustained4K60PerformanceComparison. It also failed alone twice, as did CDeckLinkRGBToP010VideoFrameFormatter4KSmokeTest.
  - Total CPU load was 100 % from other agents. v210 averaged 7-8 ms, against 4.4 ms in the same suite earlier.
  - No converter changed in this round. The only formatter edit is a log-only fallback notice in CR210DolbyVisionTunnelFormatter.
  - Environmental; rerun them on an idle machine.
- **After the h3-latch merge** (`latch.trx`): every SourceResolution*, RealFootage*, StartupCropHold, MixedScreen, ActivePicture*, AlphaSourceCrop, crop and layout, DetailPulse / DetailRestore / TextureRestore and ConfigFileTests test, plus EveryProfileKeyChangesTheLiveSettingsFingerprint and IntegrationKeysReachTheSettingsAndTheirDefaultsAreFaithful.
  - 946 run: 945 passed, 1 failed (the fade test above).
  - On the final build the fade test and the two key tests pass: 3/3 (`fade2.trx`).
- **Config tests.** Off screen, 1 shard, gate passed every time: 154 passed, 0 failed, 1 skipped (needs a real window).
  - Run on merges 1-12, after the pulse merge, and after the h3-latch merge.
  - The final commit changes only a CPU test file.

## GPU tests for the coordinator (VP_TEST_GPU=hardware)

`/TestCaseFilter:"Name=StreamFaultsKeepTheDejudderSafeAndRecovering|Name=DetailRestoreGpuSourceMonitorUnchanged|Name=UploadReuseRoutesAreBitIdentical|Name=EfficiencyFixesArePixelIdentical|Name=EfficiencyStageCostsOnThisGpu|Name=FileSourceVpRendererSurvivesRateSeekAndPauseOrders|FullyQualifiedName~DetailPulseGpu|FullyQualifiedName~TextureRestoreGpu|FullyQualifiedName~CrossfadeGpu|FullyQualifiedName~AmbientSdrGpu|Name=H3IdleMotionEngineKeptAcrossHiccups|Name=H3LookaheadCostCropSubtitle4K|FullyQualifiedName~RendererCaptureHarnessTests"`

- FileSourceVpRendererSurvivesRateSeekAndPauseOrders also needs VP_TEST_FILTER_CLIP.
- H3LookaheadCostCropSubtitle4K also needs VP_TEST_H3_LOOKAHEAD=1.
- `tools\test_state_report_onscreen.ps1` now expects plugin_api 40.
