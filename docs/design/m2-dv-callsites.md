# WP-14 DV glue: renderer call sites (patch for the integrator)

Branch `feat/m2-dv` (integration batch 1, WP-14 paired with M2 steps 6, 14 and 15). Status: the
adapters are committed and CPU-tested on the branch; the renderer side is **not** committed. It is
[m2-dv-callsites.patch](m2-dv-callsites.patch), for the integrator to apply on `integ/batch1`
(host = call sites only; one writer of the host file).

```
git apply --check --cached docs/design/m2-dv-callsites.patch   # clean against this branch's tip
git apply docs/design/m2-dv-callsites.patch                    # then build VPRenderer + VPRendererBeta
```

The patch only touches `src/VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp`: about
243 lines out, 32 in. It is a **behaviour-free move** (two-commit rule, commit 1): the moved code is
textually the same apart from member renames (`dvTrims*` -> `m_trims*` and so on), the host's
`LedDisplayGamma(target.color.transfer)` passed in as a value, and one comment put back on the
member it describes. No setting, log line, status text or fallback entry changes. UPSTREAMABLE: no
(VP-only glue). Plugin API: unchanged (`ToggleDvCompareCapture` keeps its entry).

## Adapters (VP-V)

| Adapter | Module | Owns |
|---|---|---|
| `src/VP-V/modules/colour/sdr_tone_shape/DvHostStage.h` | `colour.sdr_tone_shape` (uses `input.signal` headers) | trims hook + state, Dolby-style curve switch + status, L1 log, reference conditions, compare capture recorder, tunnel deband-skip flags |
| `src/VP-V/modules/input/signal/DvGpuUnpackSession.h` | `input.signal` | the GPU unpack object (per device), first-success log, de-judder failure count + 5 s log, the one-time session fallback entry |

Both are header-only (the renderer is compiled by two projects, VPRenderer and VPRendererBeta, and
the CPU tests compile them too; no project-file change is needed for the host).

## Hook points (one call each)

| # | Where in the host (function) | Before | After |
|---|---|---|---|
| 1 | includes | `GpuDolbyVisionTunnelUnpack.h`, `DolbyVisionTrims.h`, `DvTrimSelection.h`, `DvCompareCapture.h`, `DvTunnelPolicy.h`, `DvDolbyStyle.h` | `DvGpuUnpackSession.h`, `DvHostStage.h` (the latter inside the libplacebo `#pragma warning` block, where `DvDolbyStyle.h` was) |
| 2 | Impl members (capture unpack group) | `unique_ptr<GpuDolbyVisionTunnelUnpack> dvUnpack`, `dvGpuLogged`, `dvDejudderFailures`, `dvDejudderFailureLogTick` | `DvGpuUnpackSession dvUnpack` (the atomics `dvGpuWanted` / `dvGpuFailed` stay: the frame preparation reads them on another thread) |
| 3 | `UnpackDolbyVisionPayload` | 45 lines: create, unpack, retry, log, count, fallback | `dvUnpack.Unpacker(...)`, the early-upload slot handling (host's), `dvUnpack.Finish(...)` |
| 4 | Impl members (output hooks group) | 22 DV members (trims hook, statuses, selection, compare recorder, tone switch, flags) | `DvHostStage dvStage{ GlslHookLoader(), engage, release }` |
| 5 | device teardown | `dvTrimsHook.Clear(); dvTrimsConfigured = false;` and `dvUnpack.reset(); ... dvGpuLogged = false;` | `dvStage.ReleaseHooks();` and `dvUnpack.Reset();` |
| 6 | `RenderLocked`, tunnel state | DV metadata fallback + tunnel releases (13 lines) | `dvStage.OnTunnelState(tunnelActive, dm)` |
| 7 | `RenderLocked`, ingress status | `"; " + dvTrimsStatus + "; " + dvToneStatus + StatusSuffix + DebandStatus` | `dvStage.IngressSuffix(dm)` |
| 8 | `RenderLocked`, VP deband | skip + debandState + fallback (10 lines) | `const bool dvVpDebandSkipped = dvStage.SkipVpDeband(...)` |
| 9 | `RenderLocked`, PRE_OUTPUT after the SDR tone shape | Dolby-style curve, trims, reference conditions, compare capture (~130 lines) | `dvStage.OnPreOutput({...}, colorMapParams, hookTemplates, frameHooks)` |
| 10 | `RenderLocked`, libplacebo deband | plane deband skip + one log line (11 lines) | `if (dvStage.SkipPlaneDeband(...)) frameParams.deband_params = nullptr;` |
| 11 | `ToggleDvCompareCapture` | `m_impl->dvCompare.Toggle()` | `m_impl->dvStage.ToggleCompareCapture()` |

Order inside the frame is unchanged: OnTunnelState (6) before the ingress line (7); the VP deband
decision (8) before the PRE_OUTPUT hooks (9); the curve before the trims (the trims read whether the
Dolby-style curve runs) and the compare capture last, as before; the plane deband (10) at the same
place in the libplacebo parameter set-up. Fallback entries go through the host's `EngageFallback` /
`ReleaseFallback` (callbacks), so "Fallbacks engaged" and its log lines are identical.

Threading: every DvHostStage call is on the render thread (`RenderLocked`), as the code it replaces.
`DvCompareCapture::Recorder` keeps its own mutex (`Toggle` comes from the GUI through the plugin API).

## Not moved (and why)

- The DV GPU unpack **routing** (`dvGpu` / `dvRaw` predicates in the ingress route text and in the
  frame preparation) is part of the capture upload path shared with `GpuCaptureUnpack` /
  `GpuRgbUnpack` and early upload; it stays with the capture glue (a later input.capture adapter).
- The DV settings readers (`dv_trims`, `dv_tone_mapping`, `dv_compare_capture_folder`,
  `dv_tunnel_*`) stay in the renderer settings until WP-18 (VP settings readers, last).
- The SDR tone shape block at PRE_OUTPUT (delta 13, same slot) is not DV glue; it moves with the
  colour/output glue (WP-13) or a follow-up.
- Bill's DV code (`CR210DolbyVisionTunnelFormatter.*`, `HDRData.cpp`, `ColorSpace.cpp`, `EOTF.cpp`,
  his HDR/tone set-up) is untouched.

## Verification

- Branch (no patch): x64 Release solution build with the Magewell SDK; DV CPU classes.
- Patch applied in the working tree (not committed): VPRenderer, VPRendererBeta, Lib, Test built
  with `/p:VpCiNoMagewell=true`; the patch is reverted afterwards.
- CPU tests: `DvHostStageTests` (new: fallback entries per hook point, ingress tail, deband skips,
  curve switch and its LUT fallback, trims states incl. the shader-missing carry, GPU unpack
  session rules) plus the existing DV classes.
- For later (GPU, not run by agents): DV synthetic suite 16/16 (`run_dv_synth.py`, with and without
  `dv_tone_mapping=dolby_style`), DV trim shader GPU parity (`DolbyVisionTrimShaderMatchesItsReference`),
  the DV autotest / loopback (`test/dv-tunnel-auto`), `GpuDolbyVisionUnpackMatchesTheCpu*`, the
  `VP_TEST_DV_UNPACK_FAIL=1` renderer-harness fallback check, and the frozen-b3base byte identity
  on a DV clip (the patch must be byte-identical).

## Fix patch (separate, after the move): m2-dv-fixes.patch

Findings F-3 and F-4 ([module-findings-2026-10-25-m2-dv.md](module-findings-2026-10-25-m2-dv.md))
need host changes, so they come as their own patch (behaviour change, NOT part of the move):

- F-3: the `capture GPU unpack` entry reads "CPU formatters for the session: <error>" (it claimed
  4:2:0; the CPU formatters keep P210 4:2:2 for V210 / UYVY / HDYC unless P010 is forced).
- F-4: WP-4's `silent-fallbacks-callsites.patch` already engages `early upload: the render upload
  (<error>)` (`FallbackCatalog::EarlyUpload`) when early upload cannot open, but never releases it.
  This patch adds the releases through the same builder: when the pool opens, and when early upload
  is no longer wanted. (A failed open is not retried while it stays wanted - `earlyOpenFailed`,
  pre-existing - so after a device loss the entry is released only once early upload stops being
  wanted or the device is null at a raw frame.)

**Order: apply WP-4's `silent-fallbacks-callsites.patch` first** (it defines `SyncFallback` and the
early-upload engage); this patch was cut against that state (audit X2).

Stacks with the call-site patch in either order (checked: WP-4, then this, then the call-site patch). Tests to run with it (GPU, coordinator): the renderer capture harness fallback test
(prefix "capture GPU unpack (CPU formatters" still matches), and an early-upload open failure
check (none exists yet; candidate: force `EarlyUpload::Open` to fail via a test environment switch).

## Pending dependency: the compare-capture toggle (audit M-2)

`IRenderer::ToggleDvCompareCapture` (plugin API 43, a dedicated virtual in Bill's interface) is
for a WIP diagnostic. WP-11 (`feat/wp11-status`, `docs/design/wp11-callsites.md`) adds the generic
`InvokeModuleCommand(module, command, reply)`; its plan already routes `dv_compare_capture` through
it. Not merged into `integ/batch1` at this branch's base, so hook 11 keeps the dedicated virtual.
TODO after the rebase onto the final `integ/batch1` (if WP-11 is in): the renderer's
`InvokeModuleCommand("vp", "dv_compare_capture")` calls `dvStage.ToggleCompareCapture()`, the
GUI shortcut dispatches through it, and the dedicated virtual leaves `IRenderer.h`,
`LibplaceboPluginVideoRenderer` and the GUI (`OnCommandDvCompareCapture`).

## Formatter hook: VP's DM additions out of Bill's formatter (audit M-1)

feat/dv-l8 (merge 1) put VP logic into Bill's `CR210DolbyVisionTunnelFormatter.cpp/.h`: the L9 /
L10 parse calls, the bounded DM hold (`DvMetadataHold::Tracker`, the static / black transitions
and their log lines) and a VP-V include in his header. They move into
`src/VP-V/modules/input/signal/DvTunnelMetadataExtension.h` behind
`video_frame_formatter/DolbyVisionMetadataExtension.h` (committed; UPSTREAMABLE, no VP-V
dependency; no extension = his formatter's behaviour).

Two patches, in this order:

1. [m2-dv-formatter-hook.patch](m2-dv-formatter-hook.patch) - UPSTREAMABLE, Bill's files only
   (`CR210DolbyVisionTunnelFormatter.cpp/.h`): `SetMetadataExtension`, and five calls
   (OnSignal in `OnVideoState`, ParseExtensionBlock for unhandled levels in `ParseMetadata`,
   KeepDmPastLossLimit at the loss limit, OnPicture after the loss handling, Decorate in
   `GetDolbyVisionTunnelMetadata`). Removes the VP-V includes, the tracker member and ~45 lines
   of VP logic; adds ~36 generic lines. The DM data fields VP added to `IVideoFrameFormatter.h`
   (L9/L10 targets, hold phase, rejected count) stay: they are the data the hook fills.
2. [m2-dv-formatter-install.patch](m2-dv-formatter-install.patch) - VP-only: the renderer's
   `CreateAlphaFormatter` (host, one call) and `DvLoopbackAnalyzer.cpp` install a
   `DvTunnelMetadataExtension` on every tunnel formatter they create.

Behaviour with both applied: identical to merge 1 (same code, same order, same log lines; the
extension sees exactly the values the inline code read). Behaviour with patch 1 only: Bill's
formatter as upstream (no L9/L10, no hold) - which is what his own formatter tests exercise.
Tests: `DvTunnelMetadataExtensionTests` (CPU, the extension alone, on the branch now);
`DvSignalL8HoldTests::FormatterParsesL8AndL10AndHoldsThenStatics` and `HoldFollowsTheTunnelFixesSwitch`
(end to end through the loopback analyzer, which installs the extension) need both patches;
`DolbyVisionTunnelFormatterTests` (Bill's) run on his behaviour.

Out of scope here (finding F-11): the formatter still differs from origin/v1.4.02-beta by about
1100 lines of earlier VP changes (bug-hunt fixes, DV input fixes, AVX2 work); they predate this
batch and need their own inventory and hook plan.
