# Audit — merge of 1.4-beta-rc2.2 into the FL35 work

Diff audited: merge commit 4b5a3c3f on stage7-rc2 (parents 146de2a8 ours,
19d74440 rc2.2; common base 1.4-beta-rc1). The other Claude session was idle
throughout.
Build: OK — full x64 Release /t:Rebuild at 4b5a3c3f, 0 errors, no new warnings.
Tests: native 2463 passed, 2 failed (ShellStylePlacementWithoutSizingHintsIsNotReverted,
SameSizeEditWithRestoredWriteTimeInvalidatesCache — both known); VideoProcessorConfigTests
97 passed, 2 failed (the two known RPC tests).

Method: for each of the 15 files both sides changed, I compared each side's own
changes (rc1 → that side) with what the merge adds on top of the other side.
Every file matches exactly, except the deliberate conflict resolutions listed
under (a).

## Blockers
- None.

## Should fix
- docs/cinema/VideoProcessor-test.cfg, `subtitle_fit: false` in
  `[vprenderer.viewport.cinema]` and `[vprenderer.viewport.no_blanking]` — rc2.2
  ignores every `subtitle_*` key outside a `[vprenderer.subtitles]` section
  (RendererProfileConfig.h `IsLegacySubtitleKey`), and with no subtitles
  profile it switches the "classic" subtitle fit **on**
  (UnifiedProfileRuntime.cpp, LibplaceboVideoRenderer.cpp ~2592). So the test
  config's "subtitle fit off" now silently means "on". It only acts when
  automatic crop is on (renderer ~4167), i.e. with the "Black bars removed" /
  "fill" zoom profiles: the crop will now open up to keep subtitles in the
  bars visible. Owner to choose: keep it (it is upstream's default) or add a
  `[vprenderer.subtitles]` profile with `type: off`; either way remove the
  two dead `subtitle_fit` lines so the file says what VP does.

## Notes
- (d) New subtitle features with the test config as it stands: classic
  subtitle fit **on** (see above; only matters with a zoom profile active);
  black-backed / generated-gray caption relocation **off** (it needs a
  `[vprenderer.subtitles]` profile with `type: black` or `generated_gray`, or
  the Ctrl+Shift+T preview key — that key is keyboard-only, but it is an
  upstream diagnostic, not one of the FL35 controls).
- (c) rc2.2 adds subtitle detection inside the queued-frame walk. It runs
  before the statistics observer and only adds CPU time; it adds no new
  `continue`/`return` before the observer (only a state reset on the two
  existing skips), and the walk is still called under the same condition
  (look-ahead frames > 0).
- When rc2.2's caption relocation is used, it is composed into the source
  before the main render, so the LED measurement sees the relocated caption.
  It is measured only inside the trusted active picture, so a caption moved
  into the black bars is not counted. Nothing to change; worth knowing when
  judging the LED with relocation on.
- rc2.2's subtitle preview mode clears `renderParams.hooks` (NLS off for that
  view). Because our custom hooks and the LED hooks are added per frame
  outside `renderParams`, they keep running in that mode — as intended.

## Verified OK
- (a) ConfigEditorWindow.cpp: Subtitles keeps page 19 (navigation map
  `{"Subtitles", 19}`); the LED drive page is the 21st page added (index 20)
  and its navigation leaf points to 20. `refreshLiveToggles` and rc2.2's
  subtitle HDR fallback refresh are both present. ConfigEditorWindowTests.cpp:
  only the page count (21), the LED page index (20) and the message wording
  changed; all other test lines from both sides are present.
- (a) LibplaceboVideoRenderer.cpp: the only lines dropped are rc2.2's new way
  of finding the NLS hook (a loop over `renderParams.hooks`). I checked rc2.2:
  it only ever sets `renderParams.hooks` to the NLS hook alone, or to nothing.
  Our test (`num_hooks == 1 && hooks == &nlsHook`) gives the same answer, as
  the commit message says. Includes and shutdown cleanup from both sides are
  kept. rc2.2's subtitle composition is its own pass (`ComposeSubtitleBeforeCrop`).
- (b) Auto-merged files — VideoProcessorDlg.cpp/.h, RendererProfileConfig.h,
  UnifiedProfileRuntime.cpp, LibplaceboVideoRenderer.h, both vcxproj files and
  filters, AlphaSourceCropPolicyTests.cpp, ConfigFileTests.cpp,
  LibplaceboLutParserTests.cpp: every line from both sides present, nothing
  else added. The queue apply path still turns `target_latency_ms` into frames
  (QueueLatency) next to rc2.2's subtitle-key handling. Profile change
  detection compares both our `target_latency_ms` fields and rc2.2's subtitle
  fields. Per-frame hook submission is unchanged from our side: LED
  measurement, LED compensation, custom hooks, NLS, dither suppression.
- (c) The walk still runs the active-picture analysis and calls
  `BeginWalk`/`ObserveFrame` on the neutral observer once per visited frame.
  Renderer touch points for the LED drive are unchanged: gain in
  (`BeginFrame`), stats out (`SubmitMeasurement`), the walk observer, and the
  compensation hook-chain entry. No LED reference was added to the walk.
- (d) The shipped test config still validates and resolves (the cinema
  test-config tests, which load the real file, pass). rc2.2 removed or renamed
  no key the file uses. Apart from `subtitle_fit` (above), nothing changes
  meaning.
- (e) Low_Latency section and its 1-frame target unchanged. Stage 5,
  LED drive, edge blanking, the shader chain and saturation: code unchanged by
  the merge (zero-difference comparison above), and their tests all pass.
