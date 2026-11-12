# Skeleton hub (M2 P0)

`ExtensionHub` is the single `RendererExtension::IRendererExtension` that VP-V registers with the generic
hook layer (`src/VideoProcessor-Lib/vprenderer/RendererExtension.h`). It fans every callback out to
`ISkeletonModule`s in a fixed order and keeps per-host state keyed by the renderer identity.

- P0: no module is added, so the hub is inert (registering it changes nothing).
- Registration: `HubBootstrap.cpp`, kept by `/INCLUDE:VpvHubBootstrapAnchor` in
  `src/VP-V/build/VideoProcessor-VPRenderer*.items.props`. No line in the host files.
- Modules are added at start-up only (`AddModule`); the list is read-only afterwards and dispatch is lock-free.
- PHASE 1 of the host seams plan (refs/107 s2.4 item 1; owner E3 2026-11-11): the hub keeps ONE pathway fact per host
  (`OriginalPathway(host)`, lock-free `HostSlots`), recorded at `SettingsApplied` (`code != 0` = Original) AFTER the
  modules saw that call (a module's own handler compares the previous fact with `f.code`) and dropped with the host.
  Modules whose callbacks must do nothing in the Original pathway read it first (`// VPV-GATED` early return, verified by
  `tools/check_hook_gating.py`): `VectorEngineExtension::OnQueue`, `DejudderExtension::OnQueue` / `OnPlanOutputs`. The
  ingress host key is the renderer, not the Impl, so `input.queue_timing` keeps its own fact on its binding. No fact =
  VP-V (as before). Tests: `OriginalPathwayGateTests`.
- T-AR3-N (AUDIT-R 1fa78cdb9 N2/N4): the same per-host fact carries the six ingress fix switches as one word (`NoteFixSwitches(host, bits)` from `UpdatePipelineSlots`, render thread; `FixSwitchBits(host, bits)` lock-free for the capture / file threads, false = nothing published, the caller keeps its switches). A pathway fact or switch word the 16-host table cannot record is counted (`PathwayRefusals()`), and the SettingsApplied one is also reported as a `WorkerFault` status line ("hub" / "pathway table full"); the switch word's refusal engages the "ingress switches" fallback at the settings seam. Tests: `OriginalPathwayGateTests`.
- Not a module folder: no `MANIFEST.json` (core/ has none); the module checker does not scan it.
- Later packages add modules (resync/seal first, vector engine, entrance, motion, presentation, capture, camera/LED).
- M2 P10: `input.capture` (`modules/input/capture/CaptureExtension`), registered right after resync; it reaches its
  renderer through its own host binding (`CaptureHost.h`), not through the hub's host map.

## M2 P0b: hook amendment R10-R20 (doc 59 section 1.1)

Declarations, hub dispatch and inert defaults only; the call sites belong to the packages that move the code.
Phase payloads and the one-line helpers are in `RendererExtension.h`; `ISkeletonModule` has one typed method per
phase and the hub decodes `FrameContext::phaseData` for them. With nothing registered, or no module overriding the
method, every helper returns the host's own value (or `false` = "the host's code runs").

| # | Helper (RendererExtension::) | Phase | Rule | Default | Call site by |
|---|---|---|---|---|---|
| R10 | `PlanOutputs(host, OutputPlan&)` | PlanOutputs | take-over | host's output list untouched | P5b |
| R11 | `PlanPresent(host, PresentPlan&)` | PlanPresent | take-over | `interval == 1` | P6 |
| R12 | `AfterPresent(host, seq, gen, PresentContext&)` | AfterPresent | fan-out, outcome only added to | nothing ran | P5c |
| R13 | `AdjustPresentationSample(host, sample, native)` | PresentSample | fan-out, chained | sample unchanged | P6 |
| R14 | `ResolveTime(host, TimeKind, billQpc)` | ResolveTime | fan-out, chained | `billQpc` | P5c |
| R15 | `DecideIngress(IngressContext&)` | DecideIngress | take-over | host's values stand | P10 |
| R16 | `OnConvertedFrame(host, ConvertContext&)`, `AfterPublish(host, PublishView&)` | Convert, AfterPublish | take-over, fan-out | `false` (host's formatter) | P10 (P13 uses it) |
| R17 | `TryUpload(host, UploadContext&)` | Upload | take-over | `false` (host's upload) | P10 |
| R18 | `std::shared_ptr<void> extension` in the host's `ConvertedPayload` | - | member | null | P10 moves the VP members into it |
| R19 | `ResolveQueueTarget(host, QueueTargetContext&, billTarget)` | QueueTarget | fan-out, chained | `billTarget` | P10 |
| R20 | `kTimedFileFramesIid`, `ExternalTiming`, `IQueueAdmission` | - | declarations | - | P10 (interface text: P2) |

Phase rule (`RendererExtension::PhaseFansOut`): the fan-out phases are AfterPublish, QueueTarget,
AfterPresent, PresentSample and ResolveTime (ResolveTime is new; the other four existed and now fan out, see the
note below); every other phase keeps the first-wins rule it had. A module writes its payload only once it has
succeeded: a contained throw leaves what it wrote, except for AfterPresent (R12), where the hub discards a throwing
module's writes. R12 is experimental in the Bill PR text (doc 59 D-10).

Semantics note (deliberate, inert): the phases AfterPublish, QueueTarget, AfterPresent and PresentSample already
existed in the enum and were first-wins by default. P0b makes them fan-out. Nothing called or handled them before
this package, so no behaviour changes today; the change is recorded here because it alters the rule of declared
phases. R12 outcomes are merged per module by the hub (OR / max / sum); a throwing module's partial writes are
discarded. R19 results are clamped to capacity by the helper. The earlier unused payload structs `UploadJob`,
`ConvertJob` and `QueueTargetView` were removed and `OutputPlan` was reshaped (no users).

M2 P4 (70b section 9): Entrance, CollectHooks and AfterRender are fan-out observer phases too (`PhaseFansOut`). Handler check before the change: the only handlers that see them are `SkeletonModule` and `CameraExtension` (AfterRender, both return false: observation only), `ResyncExtension` (Begin only) and the test modules; none returned true for these phases and no test relied on the first handler stopping the chain (`HookLayerTests` uses the default Begin phase). Entrance is the render thread's GPU-work point (`NotifyEntrance`: renderMutex held, before the frame's hook list is built); handlers never write the frame's textures, params or hooks. `DrawJob` gained the inert fields `renderErrors` (AfterRender), `source`/`frame`/`destination`/`params`/`result` (Draw). Tests: `EntranceHookTests`.
