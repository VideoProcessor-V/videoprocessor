# Pruning superseded options, phase 1 (feat/prune-options)

Branch `feat/prune-options` from local `integ/2026-10-17` (93369c39), worktree
D:\codex\videoprocessor\prune. Local only (no remote calls). Full inventory and the owner's unsure
list: docs/VP-prune-inventory-2026-10-17.md.

## Removed (superseded) and their replacements

| Removed | Replacement (always runs now) |
|---|---|
| `fix_scene_cooldown` (Fixes under test) | 2 s scene-cut cooldown at the content rate (off was a unit bug: 12.5 s at 24p) |
| `dejudder_picture_area` (Fixes under test) | matcher statistics over the active picture only, bars held |
| `dither_vectors_new_frame` (Fixes under test) | a new frame's dither takes the de-judder's field when valid; own search stays the automatic fallback |
| `adaptive_faithful: polar` (value) | `auto` (polar where both axes shrink alike, 1D otherwise) |
| `dejudder_even_rates: 30`, `30_and_29.97` (values) | `whole`, `whole_and_1001` |

## Migration

`src/VideoProcessor-Lib/RetiredSettings.h`. `RendererProfileConfig::ValidateProfileSetting` accepts a
retired key with any value. Retired values stay valid and are migrated at the renderer read
(`RetiredSettings::Migrate`). The renderer logs one line per retired key or value per section
(`LogRetiredSettings` in LibplaceboVideoRenderer.cpp: "profile settings: [section] ... is retired").
Config has no control for the retired keys. It keeps them in the file as written. Its hidden
`adaptive_faithful` value holder labels polar "retired: runs as Auto".

## Code

- LibplaceboVideoRenderer.cpp: the three settings fields, their parse, fingerprint, live-change and
  describe entries are removed. The scene-detector call loses the old-units branch. The new-frame
  dither condition is `!ditherVectorsOwn && ...`. `Dejudder::Settings::pictureArea` is set true and
  stays an internal field.
- ConfigEditorWindow.cpp: the three Fixes-under-test controls and their default entries are removed.
- Docs: BUILD-STATUS.md rows, audit-scope.md entry, this report, the inventory.

## Tests

- New: `RetiredRendererSettingsMigrate`, `OldConfigurationWithRetiredSettingsLoads`,
  `OlderShippedConfigurationsLoad` (loads every .cfg in `VP_TEST_OLD_CONFIGS`; skipped when unset),
  validation cases for retired keys/values. Config: "retired renderer settings survive config save and
  reopen".
- Updated: Config "every new renderer control reaches VP" / "config keys activate the renderer and
  matcher settings" (retired edits and describe keys dropped), "inherited buttons and reorganised
  sections" (now checks that the retired controls are gone).
- Old configs: 28 files loaded with no error (VP_TEST_OLD_CONFIGS run). They are VideoProcessor.cfg
  from Bill's v1.3.005 / v1.4.00 / v1.4.01 / v1.4.02-beta, fl35-stage4 / stage5b / rc2 and
  merge-v1.4.02. Also 6 earlier docs/cinema/VideoProcessor-test.cfg versions plus the current one,
  docs/examples/*.cfg, test-fixtures/*.cfg, and the cinema test cfg with every retired key and value
  added.
- Results: see the final section.

## Kept on purpose

Low-latency paths (`capture_upload`, `fbo_float32` off, `low_latency_path`), non-NVOF fallbacks
(`optical_flow` off = matcher, `optical_flow_global_fallback`, all matcher keys), CPU fallbacks
(`capture_path: standard`, `preserve_extended_range` off), the motion slider's keys, all of Bill's
keys. Timing/queue, intent, detail, calibration and MPC areas are untouched (phase 2 notes in the
inventory, section 6).

## Results (2026-10-18)

- Build: full solution, x64 Release, rc=0 (build runner slot).
- CPU suite, single process, `/Logger:trx`: 3303 run, 3300 passed and 3 failed. The 3 failures were
  capture-format timing tests (`CV210toP010VideoFrameFormatter4K60PerformanceSmokeTest`,
  `DeckLinkConvertersSustained4K60PerformanceComparison`, `CR210toRGB48VideoFrameFormatter4KSmokeTest`),
  which ran while the Config tests loaded the CPU. All 3 pass when run alone. No GPU (WARP; VP_TEST_GPU unset).
- Config tests off screen: 144 passed, 0 failed, 1 skipped (updater shutdown needs a real window).
- Old configs: 28/28 load (MainConfigSchema, RendererProfileConfig::Read, UnifiedProfileRuntime).
