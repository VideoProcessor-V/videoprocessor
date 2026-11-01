# Open items (merge train 2, 2026-11-01)

Pending fixes merged with SHOULD/minor findings (feature off by default or inert; no MUST open). One line per item: branch, finding, file:line.

- feat/camera-two-layer: continuity-chosen minority layer keeps a high symmetric scoreAmb (lo/hi); use s_other/s_chosen; Ly.a from refShare still scales scoreAmb. Must be fixed before twoLayer default ON. GlobalModelFit.cpp:1283-1290 (also :1210, :1241, :1257)
- feat/camera-two-layer: twoLayer default stays OFF: two-layer is more lenient than single-layer when one layer touches the border (ci==1 takes scoreAmb, ignores ambA); decide default ON on real clips after the fix above. GlobalModelFit.cpp:1289
- feat/camera-two-layer: store the reference picture size in TwoLayerState and drop haveRef on a size change. GlobalModelFit.cpp:1159
- feat/camera-two-layer: post-cut seeded record does not copy layerFlags/layerRisk/objectLayer (diagnostics missing). GlobalModelFit.cpp:~1663
- feat/camera-two-layer: add a test that a held/continuity-chosen minority layer gets ambiguity < 0.35, and a no-more-lenient test with one border-touching layer. modules/motion/estimation/tests
- feat/source-identity: enable-order race: setter turns on before the held reset/reset flag; set sourceIdResetPending first and do the held reset in the worker's reset branch. SourceResolutionMonitor.cpp:~455-458 (worker branch ~969)
- feat/source-identity: no tests for the max() gate (consensus deeper than classical -> classical wins, AutoFactor and AutoFactors) or for a pre-off/on held record not reading as fresh. identity/tests/SourceIdPipelineTests.cpp
- feat/source-identity: detector windows and referenceMinimum=8 assume ~2 analysed frames/s; derive from the measured analysis rate or record as a known limit. SourceIdPipeline.h:39-46, FrameQuality.h:172
- feat/dither-need-map: WARP test (d) checks the amplitude only at grey 0 (later greys bound-only; the band does not catch a wrong amplitude); render each grey with a fresh hook and compare exactly. modules/output/dither/tests (DitherNeedWarp test d)
- feat/dither-need-map: sigma-delta catalog row lists only P3; add P10/P11/P12 inputs, and add a VerifyUnit(kP12LightLevel) check in dither need. core/ProductCatalog.h:277; modules/output/dither/DitherNeedGpu.h
- feat/dither-need-map: if light_level_out becomes part of P12, raise p12.size.fixedBytes 16384 -> 32768. core/Products.h:628
- feat/dither-need-map: BlockPass::Step (external linkage) calls internal-linkage Supported() (formal ODR nit). modules/output/dither/DitherNeedGpu.h
- feat/m2-hooks: unwired re-shape sites #4-7, #15-16, #21-23, #27 and the settings/Config hand-offs are not yet routed through the hooks (not blockers); they move with the resync Q1 / P3 / P7 packages. docs (M2 P0 call-site document), LibplaceboVideoRenderer.cpp call sites
- feat/camera-k1-k4 (re-check SHOULDs): CameraConsumerBinding binds m_dummy that can be null (SetDummyTexture only in tests); refuse the binding while null. CameraConsumerBinding.cpp:43-46
- feat/camera-k1-k4: field-era change only resets m_lastRequestedField; clear the ring, sealed mark and fitter history (NoteReset). CameraLinkCore.cpp:217-226
- feat/camera-k1-k4: Guide gets ctx.nextPairSeconds; if 0 the governor cool-down never runs down; fall back to measured time. GuidanceScheduler.cpp:369
- feat/camera-k1-k4: failed native create/start is silent and m_dropped not counted (D3D11 falls back to the failing buffer path); count and show it. GuidanceCountsRing.cpp:390-405
- feat/camera-k1-k4: m_seenReadbackDropped unused (CameraLinkCore.h:164); mis-indented line (CameraLinkCore.cpp:242)

## Camera K1+K2+K4 (module side)
- Subtask 13, the offline benchmark (motion_bench on D:\testclips, EPE no-camera vs camera vs RAFT, narrow/escalated/skipped shares), is
  NOT run: limp L2 (coordinator, 2026-11-01). Run it after the usage reset, after the verification build; tools are in
  tools/eval/motion_bench/ (run_motion_bench.ps1, score_flow.py, offline_adapter.py).
- Wiring still to do: H1 adapter (CameraExtension), items.props (H7), K1-9 LaneRunner registration, p7g in SkeletonProducts()/ProductRows().
- feat/dither-need-map: tools/check_test_categories.py flags DitherNeedTests::StateTrackerDecidesResetOffAndMissingWithoutPlanes as GPU-using with no category; categorise it (GpuWarp) or confirm CPU-only. src/VP-V/modules/output/dither/tests/SigmaDeltaHookTests.cpp
- feat/activate-skeleton-camera (2026-11-01): seal hooks (provider/subscriber) are appended again when a host state is rebuilt for a reused renderer address (old ones hold weak refs and do nothing; FrameSeal cannot drop hooks) - documented in modules/control/skeleton/README.md. Not fixed: GpuAllocatorTests::RegionEvidenceAdoptsTheCoreGrainSplitProduct expects 20 products, gets 19 (Products.h untouched by this branch; present in the base).
- feat/activate-skeleton-camera (re-check SHOULDs): on a stale generation Reconcile reuses the dead renderer's pl_gpu from Aux (Remove(host,false) keeps aux) until GpuCreated; clear aux on a stale generation. CameraExtension.cpp:137-145; same pattern SkeletonModule.cpp:322-327
- feat/activate-skeleton-camera: constructor reads the tap generation before the epoch; read the epoch first. SkeletonModule.cpp:142, CameraExtension.cpp:52
- feat/activate-skeleton-camera: Reconcile now also runs on the frame path; settings re-apply may run on the UI thread -> two concurrent Reconcile calls possible (narrow); state the thread contract or serialise Reconcile. SkeletonModule.cpp:478-489, CameraExtension.cpp:254-264
