# feat/m2-dv: M2 steps 6, 14, 15 + WP-14 (DV glue), 2026-10-25

Base: integ/batch1 43ebff50 + fix/dv-dolby-style-dark (9c66fdc7). Backup of the pre-rebase
branch: `backup/m2-dv-pre-rebase` (local). No pushes.

## What landed (commits)

- Moves (one module per commit, `git mv`, forwarders at every old header path for the host and
  Bill's includers, manifests / path map / components-master regenerated; every file checked absent
  from origin/v1.4.02-beta):
  - step 6 `colour.sdr_tone_shape`: SdrToneShape.h, DolbyVisionTrims.h;
  - step 14 `input.signal`: HdrInference.*, HdrInferenceSignalBuilder.h, HdrVetoCaptureGate.h,
    DolbyVisionTunnelGate.h, DolbyVisionTunnelHandoff.h, GpuDolbyVisionTunnelUnpack.*,
    DolbyVisionTunnelUnpackReference.h;
  - step 15 `input.capture` (VP files only): GpuCaptureUnpack.*, GpuRgbUnpack.*, UploadReuse.h,
    EarlyUpload.*. Bill's capture code (CaptureInput, AlphaNativeRgbIngress.h, DeckLink, Magewell,
    formatters, VideoConversionOverride) untouched.
- WP-14 adapters (VP-V, header-only): DvHostStage (colour.sdr_tone_shape), DvGpuUnpackSession and
  DvTunnelMetadataExtension (input.signal); the upstreamable IDolbyVisionMetadataExtension hook
  header in video_frame_formatter/.
- Host / Bill-file changes as patches (docs/design/m2-dv-callsites.md):
  m2-dv-callsites.patch (11 hook points, ~244 lines out of the renderer),
  m2-dv-formatter-hook.patch (UPSTREAMABLE), m2-dv-formatter-install.patch,
  m2-dv-fixes.patch (after WP-4's silent-fallbacks patch).
- README-vs-code pass for the three modules; key lists and fallback entries corrected;
  findings F-1..F-12 in docs/design/module-findings-2026-10-25-m2-dv.md with their fix commits.
- Audit follow-ups: own audit (X1, X2, N1, N2), modularity sweep (HIGH dv_tunnel_fixes gate, MED
  slot gate, MED trims tag), dv-dark-fix / merge-1 audit (D-1, M-1, M-2 TODO, M-3).

## Tests and builds

- Builds: Magewell SDK and `/p:VpCiNoMagewell=true`, branch state and with the patches applied
  (logs D:\codex\videoprocessor\m2-dv-build*.log).
- Targeted CPU classes (DV / signal / capture / sdr_tone_shape / PipelineSkeleton / FallbackCatalog /
  R6Baseline / Efficiency / ConfigFile / VideoFrameFormatter) with all host patches applied:
  457/461; the 4 failures are Bill's 4K formatter timing tests under two concurrent builds (3 pass
  on rerun; the sustained-4K60 comparison needs an idle machine). TRX: D:\codex\videoprocessor\m2-dv-trx\.
- Not run (GPU, for the coordinator): DV synthetic suite (incl. `-dolby_` jobs), DV trim shader
  parity, DV autotest / loopback GPU, GpuDolbyVisionUnpack*, the VP_TEST_DV_UNPACK_FAIL harness,
  frozen-b3base byte identity on a DV clip with the patches, capture harness fallback with
  m2-dv-fixes.patch.

## Open

- Compare-capture toggle to WP-11's InvokeModuleCommand once feat/wp11-status is on integ (TODO in
  DvHostStage, m2-dv-callsites.md).
- F-8 (newlldv either/or), F-11 (older VP changes in Bill's formatter): owner / later WPs.
- input.signal / input.capture stay non-switchable host slots (SwitchHonoured); their VP parts are
  switched by their keys and the Original pathway.

## Final verification (2026-10-25, 19:20)

- Branch state, `/p:VpCiNoMagewell=true`, whole solution: rc=0 (build6); DV / skeleton subset
  214/214 (TRX D:\codex\videoprocessor\m2-dv-trx\branch-nomw).
- With the call-site + formatter patches applied, Magewell SDK: rc=0 (build5); targeted classes
  457/461 (patched2; the 4 are Bill's formatter timing tests under two concurrent builds).
- F-4 vs WP-4: WP-4's silent-fallbacks patch is not on integ 43ebff50 (no SyncFallback there).
  m2-dv-fixes.patch does not engage its own entry; it only adds the two releases through
  FallbackCatalog::EarlyUpload (the auditor's preferred option) plus F-3. If WP-4 gains the
  releases itself, drop those hunks.
