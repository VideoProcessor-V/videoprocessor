# Agent report: DV input fixes (2026-10-18)

Branch `fix/dv-input-1018` from local `test/dv-tunnel-auto` (048b8477), worktree
`D:\codex\videoprocessor\dv-fixes`. Local only (no remote calls). Four bugs found by the
DV input test system (`docs/VP-dv-tunnel-autotest.md`), each reproduced first by a test
through the harness (loop, reference encoder, LLDV / HDR10 / SDR modes) and then fixed.
No DV decode maths changed; BTB/WTW untouched (no range or level changes anywhere).

## Root causes (at 048b8477)

1. **10-bit RGB LLDV stays black (tunnel switch on).** `DolbyVisionTunnelGate.h:36-40`
   opens the tunnel formatter for every r210 capture reported as SDR; on 10-bit content the
   RGB8 placement probe (`CR210DolbyVisionTunnelFormatter.cpp:1169-1211`) never resolves, so
   `AnalyzeFrame` returns BLACK for good (`:1218-1219`), and `DolbyVisionTunnelOwnsSignal`
   (`DolbyVisionTunnelGate.h:44-48`) makes newlldv refuse it (`HdrInference.cpp:46-47`).
2. **8-bit RGB LLDV shown as SDR (tunnel switch on).** Same gate; the placement resolves, so
   the renderer's tunnel SDR passthrough shows it as BT.709 / BT.1886
   (`LibplaceboVideoRenderer.cpp:17132-17137`), and newlldv refuses it twice (tunnel owns it,
   and `HdrInference.cpp:56-57` "below 10-bit").
3. **Truncated tunnel frame decoded.** `CR210DolbyVisionTunnelFormatter.cpp:1218-1219`
   returns PICTURE whenever the metadata is valid; nothing checks that the picture arrived.
4. **Hotkey clash.** `VideoProcessorDlg.cpp:631` and `ConfigEditorWindow.cpp:1104` gave
   `raw_capture_toggle` Ctrl+Alt+D, the `[shortcuts] state_report` (report-a-problem)
   default on feat/state-report and integ/mpc-state; VP and Config each kept their own
   copy of the defaults, so nothing could catch it.

## Fixes

- **Tunnel hand-off** (fixes 1 and 2; `[general] dv_tunnel_handoff`, default on, Config >
  DV profile > DV Tunneling: "Hand LLDV / HDR10 sent as RGB to the normal path";
  `/dv_tunnel_handoff`). `src/VideoProcessor-Lib/DolbyVisionTunnelHandoff.h`: a probe on the
  capture thread (GUI, after the renderer has the frame) applies the formatter's own rules
  through new static probes (`HasTunnelHeader`, `ClassifyRgbContent`,
  `LooksLikeTunnelContent`; the member code moved into shared helpers unchanged). Tunnel
  evidence = a CRC-valid header or tunnel-like content (so a damaged or range-converted
  tunnel stays black, never packed pixels). With none for ~1 s of source pictures (derived
  from the measured rate; drops count) and either 10-bit content (any colorimetry) or RGB
  with BT.2020 and no HDR InfoFrame, the signal is handed off: the gate closes for it
  (`DolbyVisionTunnelGateInput::handedOff`), the tunnel no longer owns it, newlldv infers PQ
  (also under a profile that forces the tunnel). A CRC-valid header takes it back; while
  handed off the probe runs before the renderer and holds frames back until the tunnel
  state is in place. A new capture state starts a new probe generation. 8-bit BT.709 RGB
  (SDR menus) is never handed off.
- **8-bit LLDV decision.** Capture cards report neither the Dolby VSIF (DeckLink) nor
  anything else separating 8-bit LLDV from 8-bit SDR sent as BT.2020. Known: RGB 8-bit,
  BT.2020 AVI colorimetry, no HDR InfoFrame, and (probe) no tunnel. A TV-led tunnel has
  CRC-protected metadata in every picture and RGB-default colorimetry, so on a DV input
  BT.2020 RGB without a tunnel is LLDV or HDR10 without its InfoFrame: PQ. newlldv's 10-bit
  floor is waived for exactly that signal (`HdrInferenceSignal::dvTunnelHandedOff`) and for
  an explicit indication (`lldvIndicated` from the new `VideoState::dolbyVisionVsif`;
  LowLatency wins with or without the switch and hands off at the first picture; TvLed
  never hands off). The SDR veto still protects real 8-bit BT.2020 SDR; with the switch off
  nothing changes. No capture device fills the VSIF yet (Magewell could, via
  `MWCAP_HDMI_INFOFRAME_ID_VS`; untested, not wired).
- **Truncated frames** (fix 3; under `dv_tunnel_fixes`): a picture that would be decoded
  (own CRC-valid header, valid DM) whose last row is all zero - impossible for real tunnel
  content - is REJECTED (the renderer keeps the previous picture), logged with a running
  count, metadata kept. `AnalyzeFrame` now also reads the last row (the GPU-path memory test
  allows it). A tail left holding stale bytes is not detectable.
- **Shortcuts** (fix 4): `src/VideoProcessor-Lib/ShortcutDefaults.h` is the one defaults
  table for VP's accelerators and Config's Shortcuts page/clash check. Raw capture is now
  `[shortcuts] raw_capture`, default **Ctrl+Alt+W**. Ctrl+Alt+R was free among defaults but
  is a user binding for `reapply_rules` in the Config tests, which a default would have
  taken over. `kReservedShortcuts` keeps Ctrl+Alt+D for state_report until it merges.

## Follow-ups (coordinator, same day)

5. **DV Tunneling profile claimed LLDV YCbCr.** `DolbyVisionTunnelOwnsSignal` returned true
   for any capture under a profile whose DV mode is DV Tunneling (`profileForcesTunnel`), and
   `BuildPushVideoState` / the SDR veto excluded the profile outright
   (`m_profileDolbyVisionTunnelOverride != 1`), so Apple TV LLDV as YCbCr 4:2:2 and HDR10
   without its InfoFrame were never inferred as PQ while such a profile matched. The tunnel is
   only ever 8-bit RGB 4:4:4, so YCbCr (V210 / UYVY / HDYC) is now declined
   (`DolbyVisionTunnelImpossibleEncoding`, `ProfileTunnelClaimsSignal`) under
   `dv_tunnel_handoff`; the gate never opened for YCbCr anyway. Test:
   `DvTunnelingProfileDeclinesYCbCrLldvAndHdr10` (10/12-bit 4:2:2 BT.2020 with and without
   the InfoFrame, 8-bit UYVY, and the tunnel still the profile's; switch off = old result).
6. **Truncation check proof.** `DvRpuAnchorTests.TruncationCheckNeverRejectsALegitimateFrame`:
   an all-zero word is I = 0 with chroma 0; under all 1374 real RPU DMs (46 files: fixtures +
   the 37-RPU corpus; all full range) and the synthetic DMs (IPT, rich, limited-range YCbCr) it
   decodes to a negative PQ-domain value (at most -0.151), which no light produces, and a
   limited-range DM puts code 0 below black. Packing PQ black (each DM's own black codes), the
   deepest intensity code, letterboxed pictures (black bottom bars) and a fade 2% from black
   with the independent reference encoder, in both RGB8 placements: 10984 of 10992 frames
   decoded, 0 rejected (the 8 others are the profile-4 14-bit DM VP rejects as documented).
   No stronger check was needed; the tunnel metadata carries no frame-size field to use.

## Tests (CPU, single-threaded, TRX; no GPU)

New: `DvIncomingHdrModeTests.TenBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAndInferredPq`,
`.EightBitRgbLldvWithTheTunnelSwitchOnIsHandedOffAsPq` (high8 and low8),
`.DolbyVsifDecidesEightBitLldvExplicitly`; `DvTunnelLoopbackTests.TruncatedFrameWithIntactMetadataIsRejectedNotDecoded`,
`.HandoffNeverTakesATunnel` (healthy, range-converted, damaged), `.TunnelTakesAHandedOffSignalBack`;
`DvTunnelHandoffTests` (4); `ShortcutDefaultsTests` (4: no two defaults or reserved chords
collide, raw_capture, LiveApply constants, shipped cfg). Each switch-off branch asserts the
old behaviour. Failing first: with the four fixes reverted in place (handoff probe
disabled, 8-bit waiver removed, truncation check off, raw_capture back on Ctrl+Alt+D) the
new tests ran 37 / 27 passed / **10 failed** (`dv-fixes-logs\trx\prefix.trx`).
Two existing formatter tests embedded valid metadata into an all-zero picture (the
truncated-frame signature); they now restore a tunnel near-black picture first.

Results on the final build (x64 Release, full solution rc=0):
- CPU (vstest, single-threaded, TRX `dv-fixes-logs	rxinal3.trx`, after the follow-ups):
  every DolbyVision*, Dv*, HdrInference*, Lldv*, Shortcut*, ConfigEditorCore, ConfigFile,
  MainConfigSchema, RawCapture class: **492 / 492 passed**.
- Config tests after the follow-ups (shortcut / DV / LLDV / control coverage): 24 / 24 passed.
- Config tests (off screen, 1 shard): 142 passed, 1 failed, 1 skipped (needs a real window).
  The failure (`remote target switch keeps window responsive`) ran while a build was
  linking; rerun on the final build with the shortcut / DV / LLDV tests: 11 / 11 passed.
- GPU: not run (no GPU in this task).

## Owner notes

- Docs updated: `docs/VP-dv-tunnel-autotest.md` (mode table, section 4 fixes, owner steps:
  Ctrl+Alt+W), `docs/dv/LLDV-vs-tunnel-check.md` (Ctrl+Alt+W; RGB conversion no longer black),
  `docs/agent-reports/dv-tunnel-autotest.md`.
- Not GPU-verified: the renderer path after a hand-off is VP's ordinary r210 path; a
  hand-off or take-back restarts the renderer (it rejects a tunnel-flag change in place).
