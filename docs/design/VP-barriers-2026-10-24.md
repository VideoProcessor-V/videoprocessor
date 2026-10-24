# VP barrier scan, 2026-10-24 (read-only)

Source: D:\codex\videoprocessor\integ-r4 @ c940e705 (integ/round5). Paths below are relative to `src/` unless they say otherwise.
`LVR` = VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.cpp (31,335 lines).
libplacebo = 3rdparty/libplacebo-beta (PL_API_VER 377; VP fork patches 0001-0004; the hook enum is identical to Stable).
Efforts are in agent wall-clock hours.

## Ranked barriers

### BLOCKERS

**B1. Net A cannot run in its agreed slot as exported (a libplacebo LUMA hook).**
- Evidence:
  - The exporter binds A as `//!HOOK LUMA` at the re-base output (D:\nn-corpus\VP-NN-TrainKit\trainkit\export_glsl.py:13-16, :94).
  - VP's front end is not built from libplacebo hooks. It is a chain of pl_frame-to-pl_frame pl_dispatch passes that run before pl_render: re-base, then deband, then detail pulse, then texture restore (LVR:23524, 23568, 23631, 23650).
  - A LUMA hook therefore runs AFTER deband and texture restore. The agreed order is the reverse: A, then deband, then texture restore.
  - libplacebo never fires LUMA_INPUT for an RGB frame: renderer.c:1457 maps it to PLANE_LUMA only. Native RGB ingest is one rgba16 plane plus a matcher r16 luma plane (VideoProcessor-Lib/vprenderer/GpuRgbUnpack.cpp:183-218), so A would silently never run on RGB capture.
  - The bindings `SCALER_SRC`/`SCALER_OUT` for D (export_glsl.py:19-22) are not libplacebo stages.
- Resolution:
  - Build a VP **NetRunner** that executes the exporter's mpv-format passes standalone through pl_dispatch: a small shim for the DESC/HOOK/BIND/SAVE/WIDTH/HEIGHT/COMPONENTS/COMPUTE directives and the HOOKED_*/name_tex macros.
  - Its inputs are a pl_dispatch and textures, so the same runner serves the front end (A, the 4K refine) and C hooks (D inside AdaptiveDownscale's POST_KERNEL, VideoProcessor-Lib/vprenderer/AdaptiveDownscale.cpp:420-425; pl_hook_params carries a dispatch).
  - For RGB: run A on the existing r16 luma, then add the luma delta to R, G and B.
  - Alternative: change the exporter to emit pl_shader_custom units (the SourceRebase pattern, VideoProcessor-Lib/vprenderer/SourceRebase.cpp:80-130).
- Effort: 8-12 h for the runner and tests; RGB recombine 3-4 h.

**B2. 200 ms is unreachable at 24p even after the 5-frame minimum is removed.**
- Evidence:
  - The queue time target rounds UP: `ceil(ms x rate)` (VideoProcessor-Lib/QueueLatency.h:27-35). 200 ms x 23.976 = 4.8, which becomes 5 frames, which is 208.5 ms of queue alone.
  - On top of that:
    - libplacebo `max_frame_latency = 2` (LVR:10550): up to about 33 ms at 60 Hz;
    - render-ahead (LVR:9708-9713);
    - the capture transfer time of one input frame (41.7 ms at a 24 Hz HDMI signal), which falls outside `captureToPresentationTargetMs` (VideoProcessor-Lib/vprenderer/LibplaceboVideoRenderer.h:386).
  - The forced minimum also ignores the configured motion_ahead_frames: it uses `kDefaultDepth` (VideoProcessor-Lib/UnifiedProfileRuntime.cpp:1237, VideoProcessor-GUI/VideoProcessorDlg.cpp:14935).
- Resolution:
  - Add a **total-delay budget** that queue frames are derived from: frames = floor((budget - fixed overheads) / period), with a 4 / 3 / 2 fallback ladder.
  - Count render-ahead and the swapchain depth inside the budget.
  - Remove the minimum (update the 5 in VideoProcessor-ConfigTests/ConfigFileTests.cpp:782).
  - Make motion_ahead time-based (VideoProcessor-Lib/MotionAhead.h:20-33).
  - See owner decision O1 for what the 200 ms covers.
- Effort: 5-8 h.

**B3. Motion consumers run BEFORE the input side and repeat the front end on every refresh.**
- Evidence:
  - The de-judder phase picture replaces the native planes, and only then come re-base and deband (LVR:23507-23524). The extra path does the same per extra (LVR:16486-16494).
  - A is re-based and debanded a second time just for the hold (LVR:23550-23560).
  - A net placed in this chain would run about 2.5 times per source frame at 24p in 60, plus once more for A.
- Resolution:
  - Make the input side (re-base, repair, deband, texture restore, 4K route, chroma slot) run **once per source frame**, cached by sequence.
  - The warp then reads the cached result, with vectors scaled per axis and mapped through the geometry (already planned).
  - This needs one stage list in place of the three copies: main, extra, crossfade (LVR:5433-5444, 16491-16539, 23505-23686).
- Effort: 10-16 h. This is the core of the skeleton work.

**B4. The real-time proof cannot run this week.**
- Evidence:
  - The laptop "cannot play 4K in real time" (tools/timing-budgets.cfg:36-40).
  - The 3090 trains unattended all week.
- Resolution:
  - On the laptop: gate correctness only, with relative timing.
  - Schedule a 3090 timing window (owner decision O2).

### RISKS

**R1. The calibration LUT is applied in the wrong place.**
- Evidence:
  - It is libplacebo's `target.lut`, PL_LUT_NORMALIZED (LVR:10477-10486). libplacebo applies it during colour management, before PRE_OUTPUT (renderer.c:2432 runs before :2863).
  - As a result, the LED measurement hook at PRE_OUTPUT (VideoProcessor-Lib/led/LedMeasurementHook.cpp:158) already measures projector codes.
  - The agreed order needs the LUT as VP's last PRE_OUTPUT hook.
- Resolution:
  - Apply the LUT with pl_shader_custom_lut inside a VP hook, plus the BTB/WTW late restore.
  - Check the OutputEncodingRepair interplay (VideoProcessor-Lib/vprenderer/OutputEncodingRepair.cpp:27).
- Effort: 4-6 h.

**R2. Hook order is assembled in many places.**
- Evidence: there is no single slot registry; the hook list is built at LVR:5489, 16216-16239, 16266 and 23416.
- Resolution:
  - Build one ordered **SlotRegistry** for both the front end and the output hooks.
  - Register a libplacebo hook only when it is active. A registered no-op hook forces pass boundaries and is not zero-cost.
  - Front-end placeholders return the same pl_frame, with no copy.
- Effort: 4-6 h.

**R3. Intermediate precision is one global switch.**
- Evidence:
  - libplacebo picks rgba16f FBOs (renderer.c:400-411). rgba32f exists only through fork patch 0003 as the global `fbo_float32` (LVR:2681; VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.cpp:451).
  - The exporter requires rgba32f (export_glsl.py:9-10, :84).
  - Per-net fp16 is impossible inside libplacebo. In the front end, VP chooses formats itself (r16 for re-base, with a float fallback: SourceRebase.cpp:205-240).
- Resolution:
  - Run nets in the NetRunner on VP-owned textures.
  - Put the intermediate format in each net's fingerprint.
  - Check VRAM on the 4 GB laptop: one 4K rgba32f texture is 133 MB.
- Effort: inside B1.

**R4. Many code paths assume two planes.**
- Evidence: the phase picture and source-plane checks use `num_planes == 2` (LVR:6285, 15880, 23509, 23621).
- Resolution:
  - The chroma slot output must stay a 2-plane layout: luma plus a full-resolution CbCr plane. libplacebo then does not upsample.
  - RGB ingest de-judder needs its own check.
- Effort: 2-3 h.

**R5. Fallbacks are log-only.**
- Evidence:
  - There is no "Fallbacks engaged" registry anywhere (grep finds nothing).
  - About 59 log-only failure or fallback lines in LVR.
  - The Ctrl+I overlay pulls renderer getters (VideoProcessor-GUI/VideoProcessorDlg.cpp:18156).
  - The state report is VideoProcessor-Lib/StateReport.cpp.
- Resolution:
  - Build a FallbackRegistry (atomic flags plus reason) in the renderer, exposed through one getter that feeds Ctrl+I, the Config status (RPC) and Ctrl+Alt+D.
  - Migrate the sites.
- Effort: 4-6 h, plus 3-5 h for the site audit.

**R6. The Config app is a monolith with hand-built sliders.**
- Evidence:
  - VideoProcessor-Config/ConfigEditorWindow.cpp is 16,281 lines.
  - Each graded slider (motion :7640-7840, picture :8161-8340) is about 200 lines of inline lambdas, and there is no shared preset model.
- Resolution:
  - Build a generic UnitTier widget plus a data table (unit -> tier -> key/value), with Custom and Reset.
  - The global tier writes the unit tiers; profiles keep per-profile positions, as the motion slider does.
- Effort: 8-12 h, plus 3-4 h of tests.

**R7. The build-slot runner allows too many builds and can reclaim a live slot.**
- Evidence:
  - The default is `VP_SLOTS=3` (D:\codex\vp-build-slots-run.sh:9), but the rule is a maximum of 2.
  - A slot older than 75 minutes is reclaimed (:15) even while its LTCG build is still running.
  - The runner does not add /m:4 or CL_MPCount=2 itself.
  - AGENTS.md names an E: SDK path that does not exist on the laptop. The SDK is at C:\Program Files\MWCaptureSDK 3.3.1.1596\SDKv3 (verified present).
  - Stale `.iobj` files are present under VideoProcessor-GUI/x64/Release.
- Resolution:
  - Export VP_SLOTS=2 and pass the C: SDK path in every brief.
  - Delete stale .iobj/.ipdb before an LTCG build.
- Effort: 0.5 h.

**R8. D's light domain is unverified.**
- Evidence:
  - POST_KERNEL may hold linear or gamma light (the Stage 8-1 choice, LVR:23392).
  - The exporter adds the luma delta to RGB, and was trained on neutral-chroma renders.
- Resolution: match the training domain, or lock the domain in the fingerprint.
- Effort: 2 h.

### UNKNOWNS

**U1. The hardware loop may not accept the projector mode.**
- DeckLink Mini Recorder probably rejects 2560x1600@60 (queue item 58).
- The loop needs a Magewell USB 4K Plus class device, or a broadcast mode for the bench.

**U2. Delay gate infrastructure is partial.**
- There is no end-to-end constant-delay test.
- `captureToPresentationTargetMs` exists and can drive the 4 / 3 / 2 gate.
- The DV tunnel corpus injection already exists (VideoProcessor-Test/RendererCaptureHarnessTests.cpp:3008-3011).
- There is no pairwise generator (grep finds none). Writing one is about 2-3 h.

**U3. Fingerprint plumbing is greenfield.**
- No net loader or fingerprint check exists. The only "fingerprint" is the settings hash (LVR:1454).
- About 3-4 h, inside B1.

**U4. Stage 13, adaptive gamma and black level have no pixel hook yet.**
- No CAM16 code exists.
- These are pure new placeholders.

## Owner decisions

- **O1.** What does "200 ms" measure? Signal arrival to light on screen? Or VP's own capture-to-present time? The answer decides whether 4 frames fit at 24p.
- **O2.** The 3090 is training all week. When can it pause for the real-time 24p everything-on check? Before you leave, or at a planned break?
- **O3.** A fixed delay, or one that is constant per frame rate? An exactly fixed 200 ms at every rate needs padding, because whole frames do not divide evenly.
- **O4.** OK to move the calibration LUT out of libplacebo into VP's last output hook? It changes what the LED measurement sees.
- **O5.** OK to keep rgba32f for nets, at extra cost? Or require fp16 versions from round 2 onward?
- **O6.** Buy or borrow a capture device that accepts 2560x1600 for the hardware loop?
