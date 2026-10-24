# VideoProcessor pipeline: final design (frozen 2026-10-24)

Status: FROZEN for the skeleton restructure. The open items are listed in section 4. Each one
is a measured decision, so none of them changes the slot order except the re-base/repair
order (marked PENDING).
Machine-readable slot list: [`slots.json`](slots.json). Decision records: [`../decisions/`](../decisions/).
Module folders: `src/VP-V/modules/<unit>/<slot>/`.

---

## 1. One-page overview (for the owner)

VP takes the Apple TV picture from the capture card, cleans it, scales it, colours it for the
projector and shows it **at a fixed, known delay**, so the AVR's fixed audio delay always matches.

The picture travels through **eight units**. The pipeline runs top to bottom:

| Side | Unit | What it does, in plain words | Status |
|---|---|---|---|
| **INPUT SIDE** | Input | Takes frames from the card without losing precision; works out the signal type (SDR/HDR10/LLDV/DV); holds the frames for ONE total delay you set to match your AVR (recommended: enough for 4 frames, about 267 ms at 24p on a 60 Hz display, measured), the same at every frame rate | built |
| | Input (analysis) | Studies the **untouched** frame: how the player scaled it, where the picture is, cuts/fades, subtitles | built |
| | Motion | Measures motion on the untouched frame at its own size (cheap, accurate); one "vectors valid" switch every user obeys | built |
| | Scaling: **re-base** | Undoes the player's upscale, axis by axis, so we work on the real picture | built (order pending) |
| | Repair | Removes codec damage (net A, or VP's own restore module, or off), then deband, then the texture-restore add-on | A = slot only |
| | Scaling: **4K route + refine** | Brings the picture to 4K with fixed 1-D steps; one refine net polishes the result | slots only |
| | Scaling: chroma | A reserved spot for VP's own colour-detail upscale | slot only |
| **PROCESSING** | Motion consumers | De-judder, hiccup repair, optional interpolation | built |
| | Scaling: output | The hybrid SSIM scaler to the projector size (+ the optional D net) | built (D = slot) |
| | Colour | libplacebo tone/gamut mapping (or the combined LUT for low-end cards); below-black/above-white always kept | built |
| **OUTPUT SIDE** | Colour | SDR tone shape → Stage 13 appearance (dim screen) | Stage 13 = slot |
| | Dynamic Drive | LED measurement → LED compensation → black-level correction | black level = slot |
| | Output | Projector calibration LUT (the last colour step) → dither for the 8-bit link → present | LUT moves to VP |
| **CONTROL** | Control | Config tiers, "Fallbacks engaged" status, nets' fingerprints, diagnostics | partial |

**What "slot" means.** The skeleton gives every stage a fixed place in the order now, even if
its real component comes later. An empty slot costs **nothing**: VP skips it entirely, and the
picture is byte-for-byte the same as without it. Components then drop into their slots one at a
time, each tested alone and in the whole chain.

**Tiers.** Each unit has a Classic / Low / Mid / Upper slider (fewer steps where a unit lacks the
granularity) plus a global one. The unit's toggles are listed under its slider.

**Nothing degrades silently.** If anything falls back (for example a net is bypassed or the GPU
unpack fails), the status shows "Fallbacks engaged: ..." with the reason. It appears in the Ctrl+I
overlay, in Config and in Ctrl+Alt+D, plus a one-time notice.

**Still to decide (from measurements, not opinion):**
1. Does the re-base run before or after repair? Re-base first is proposed. The round-2 "A diagnostic" measures both orders.
2. Is net A worth keeping? It must gain at least ~0.5-1 dB on low-bitrate streams with the grain kept. Otherwise the repair stage becomes VP restore (an optimised texture restore + breathing).
3. Is the deband still needed when A is on?
4. Refine net: one net told whether A ran, or two versions? Your lean is one net told, if it costs the same.
5. D net: real cost in VP, and the grain gate.

---

## 2. Principles that shape every slot

- **Modularity** (ADR-0017): each slot has a clear input/output contract. It is measured in
  isolation and can be improved without a teardown. Any fused or optimised version must be
  bit-identical to the modular one.
- **Zero-cost placeholders** (ADR-0015): an off slot adds no pass and no copy, so its output is
  byte-identical. Timing and delay tests stay true.
- **Fixed delay** (ADR-0003, timing decided in ADR-0020):
  - The user sets ONE total delay to match the AVR. It is identical at every frame rate (padded),
    so the AVR setting never changes between titles.
  - The pipeline is built for 4 queued frames: this is the RECOMMENDED setting, about 267 ms (measured) at
    24p.
  - A lower total walks the 4 -> 3 -> 2 ladder, and every feature keeps working at the lower
    margin.
  - Look-aheads share the queue (they never add to it) and are time-based where possible.
- **Analysis and motion on the original frame** (ADR-0004, ADR-0006). This is forced: A_motion
  needs vectors before it runs.
- **BTB/WTW kept in all cases** (ADR-0002). **No silent fallbacks** (ADR-0001). **General, not
  fixed**: no hard-coded rates, cadences or resolutions (ADR-0018).
- **libplacebo = the colour engine and GPU layer in the middle** (ADR-0016). VP owns the front
  end (capture, re-base, repair, 4K route, nets) and the output end (Stage 13, dynamic drive,
  black level, calibration LUT, dither) as VP passes/hooks.
- **Net dependency rule** (ADR-0008): train upstream to downstream. Fingerprints carry the
  upstream versions and states, and VP bypasses a net (visibly) on a mismatch.

---

## 3. The slots in detail

Each slot lists purpose/why, contract (formats, geometry, delay share, look-ahead), settings/tier,
fallback entry, fingerprint/upstream, and open items. `slots.json` holds the same data plus
the tests. **slots.json is authoritative for ids, order and keys; this text explains them.**

Hook-point legend: **capture** (ingest), **host** (CPU), **prepass** (VP's own GPU passes on the
source planes before libplacebo samples them; today the source-stage lambda in
`LibplaceboVideoRenderer.cpp`), **libplacebo** (inside `pl_render_image`), **PRE_OUTPUT / OUTPUT**
(VP hooks handed to libplacebo, in order), **present**.

### 3.1 INPUT SIDE

**input.capture (10), built, capture.** Frames from DeckLink/Magewell come in at full precision.
v210/UYVY go to P210 4:2:2 via the GPU unpack, and r210/R12B/R12L via native RGB ingest. Upload
reuse and early upload apply. Output: GPU planes at the delivered size, no crop. Keys:
`capture_path`, `capture_upload`, `native_rgb_ingest`, `dv_tunnel_gpu_unpack`. Fallback: "Capture:
CPU 4:2:0 (GPU unpack failed: reason)". This is log-only today and must become visible (item 44).
Open: the CPU fallback should keep 4:2:2; the RGB-capture luma path for A.

**input.signal (20), built, host.** Detects SDR / HDR10 / LLDV / DV tunnel and their metadata, and
unpacks the DV tunnel. Output: a signal descriptor on every frame. Fallback: DV unpack on the CPU
(log-only today). Rule: nets never run on raw DV IPT.

**input.queue_timing (30), built, host.** Holds ONE user-set total delay (`target_latency_ms`; ADR-0020) and runs cadence/pulldown
detection, the film clock and stale-burst handling.
- **Recommended total:** enough for 4 queued frames at the slowest source rate. At 24p that is
  4 x 41.7 = 167 ms, plus the swapchain, render-ahead and processing: about 267 ms (measured, 23.976 on 60 Hz).
- **The same total at every frame rate:** faster rates are padded, so the AVR delay never
  changes between titles.
- **A lower total:** the queue depth = floor((total - fixed costs) / frame period). It walks the
  4 -> 3 -> 2 ladder, shown as a "Fallbacks engaged" entry. Config shows the resulting frames and
  margin.
The queue depth is the deepest single look-ahead plus the hiccup spare. On a short queue
(2-3 frames) every consumer shortens its look-ahead or falls back, and the delay never changes.
Fallback: "Queue: short look-ahead (n of 4 frames)". Being fixed on `fix/r6-baseline`: the forced
5-frame minimum, a time-based `motion_ahead`, and render-ahead inside the budget.

**input.source_analysis (40), partial, host.** Runs on the **original delivered frame**: per-axis
source resolution + kernel class, active picture/crop, cut/fade/black, subtitles, and A's labels
(label schema v1: bit depth, per-axis ratio, kernel, bitrate tier, codec family + AV1 FGS, and a
state flag re-based / native / un-reversed). Low confidence makes the labels "unknown" (A trains
with ~12% unknown). Open: blind kernel estimation (item 18); the identification/label-detector net
(round 2; the owner's idea is an identification net feeding the kernel-matched re-base).

**motion.estimation (50), built, prepass.** NVOF seed + VP matcher on the original luma at
**source size**, with a time-based look-ahead. ONE vectors-valid state, invalidated at
cuts/drops/repeats/drift/resyncs. Output: source-size vectors + validity. Fallback: "Motion:
matcher only (NVOF unavailable)". If the vectors are invalid, every consumer falls back.

**scaling.rebase (60; alternative position 75), built, prepass. PENDING ORDER.** A 1-D per-axis
re-base: the kernel-matched inverse, or Lanczos when the kernel is unknown, H then V, r16
intermediate, back to the **encoded grid**. Output: planes at the encoded size, where x and y
factors may differ.
*Why first:* the kernel inverse relies on the delivered frame being an exact kernel upscale.
Texture restore, breathing and deband add or move fine-band energy, which folds into aliasing on
inversion. Repair at the encoded size is also ~3x cheaper (A 1080 1.4 ms vs 4K 4.4 ms).
*Status:* code today already runs re-base -> deband -> detail pulse -> texture restore. Round 2
measures BOTH orders. The skeleton keeps the order data-driven, so the swap is one line of data
(ADR-0005).
Fallback: a failed pass renders the native frame (in code); an unknown kernel uses Lanczos; off
goes straight to output scaling. Keys: `source_resolution`, `rebase_kernel_match`; planned
`entrance_rebase`.

**repair.encoding (70), placeholder, prepass.** Encoding repair at the **encoded resolution**.
Choices: net A (A_motion with neighbours + vectors / A_single for low latency) / **VP restore**
(VP's own add-on module, not "classical") / off. Contract: luma only, active area with mirror pad,
bounded residual; same size and format out. Tiers: Classic off; Low VP restore or a small A;
Mid A; Upper A_motion. Fallback: "Repair: bypassed (fingerprint mismatch / missing weights / DV
IPT input)". Fingerprint: label schema, pass order H->V, r16, re-base shader identity, VP commit.
**Open:** round 1 showed GATE COLLAPSE (A learned to skip everything). The round-2 DIAGNOSTIC
(gate off, re-based domain) is the upper bound: keep A only at >= ~0.5-1 dB on low-bitrate
streams with grain kept. Breathing comes later as a fine-tune, only once A demonstrably repairs.

**repair.deband (80), built, prepass.** VP's debander at source resolution, after repair, with a
grain guard. Classic uses libplacebo's deband. Fallback: libplacebo deband on temporal frames.
**Open:** is it still needed with A on (measure)? The x52 real-pan shimmer on 1080->1600
(queue item 1).

**repair.texture_restore (90), built, prepass.** The long-memory add-on on A's output: texture
restore (~2 s peak-hold along vectors) + breathing reduction (`detail_pulse_stabilise`). Upper
tier, after an **efficiency rebuild** (9.8 ms at 4K on the 3050 Ti vs a 1.2-1.4 ms estimate).
It is never folded into A (net dependency rule).

**scaling.route_4k (100), placeholder, prepass.** Processing resolution =
`min(source, max(4K, effective output active size after display mode + A-lens + NLS))`, reached
with fixed 1-D x2 shaders per needed axis (H then V, r16). It is recomputed on an output-profile
change (clean reconfigure, vectors invalidated). The 8K placeholder: an 8K source to a <=4K
display is downscaled to 4K first; only an 8K display keeps native 8K, with all nets bypassed and
motion at 1/4 per axis. Off goes straight to output scaling (Classic/Low). There is no
passthrough option otherwise, including 1080->1080.

**scaling.refine (110), placeholder, prepass.** ONE refine net at the end of the 4K route,
trained to the truth. Inputs: picture + continuous per-axis scale (sx, sy) + A-ran flag (+ path
re-based/native). Texture restore and deband are unlabelled robustness augmentation. Per-axis C
is DROPPED (round 1: harmful, ringing 0.22->0.8). Results are reported per combination group,
and VP bypasses only an underperforming group (visibly). Upper tier; off in low latency unless a
cheap variant exists. **Open:** one net told vs two versions (owner: one told, if the cost is
identical).

**scaling.chroma_upscale (120), placeholder, prepass.** A reserved VP-owned chroma upscale. When
active, libplacebo receives 4:4:4 and must not re-upsample. **Open:** verify what libplacebo does
with 4:2:2/4:2:0 chroma in our path; chroma siting vs crop/A-lens/NLS; BTB/WTW; DV tunnel;
subtitles. Candidates: CfL-style luma-guided or separable Lanczos.

**Chroma decision (research/chroma c7564afa, 2026-10-24).**

- **The slot stays EMPTY for now.** libplacebo's chroma upscaler ('auto' = catmull_rom) is the
  default for every tier.
- **CfL is opt-in** (graphics/subtitles). Its bundled `chroma_offset_x` is now -0.5, matching
  VP's LEFT-sited input (synthetic edge dE 2.17 -> 0.88).
- **Siting fixes, applied:**
  - the re-base scales each plane's shift with its factor (`VP-V/core/ChromaSiting.h`);
  - the legacy CPU P010 path (even chroma lines only) is tagged TOP_LEFT.
- **The contract for a future chroma slot:**
  - 2 planes: luma plus full-size CbCr, >= 12-bit or float, shift 0;
  - no clamp (BTB/WTW kept);
  - the full frame including bars;
  - DV tunnel handling defined;
  - the `(w+1)/2` chroma-width maths audited for odd sizes;
  - any VP stage that resamples chroma scales the plane shift (`ShiftAfterResample`).

### 3.2 PROCESSING

**motion.vector_delivery (130), partial, host.** Hands source-size vectors to each consumer
**per axis**: x scaled by the x factor and y by the y factor (re-base and the 4K route give
different factors; the same mechanism as the 8K 1/4 plan). They are mapped through
crop/A-lens/NLS. Precision consumers (temporal dither, scaler refinements, texture restore) get
an optional cheap +-1 px refinement. If the geometry can't be mapped, the vectors become invalid.

**motion.consumers (140), built, prepass.** De-judder, hiccup repair, optional interpolation
(exclusive with libplacebo smooth motion by profile rules), dither vectors. All degrade on a
short queue.

**scaling.geometry (150), built, libplacebo.** Crop/active picture, A-lens, NLS,
zoom/viewport. NLS falls back to the linear safe fit on a hook failure (in code).

**scaling.output (160), partial, libplacebo.** Hybrid SSIM downscale (`adaptive_w_base` 0.2)
inside libplacebo + the optional **D** refine add-on (its own switch under the hybrid scaler).
Lanczos is the fallback; polar is retired (separable over polar). **Open:** D's real cost in VP
(the PyTorch fp16 upper bound is 35.8 ms at 4K) and D's grain gate (0.75 vs >= 0.9). D counts
only without sharpness/detail/grain loss (MTF, HF energy, grain, pan test).

**colour.tone_gamut (170), built, libplacebo.** libplacebo tone/gamut mapping, OR the combined
static HDR->SDR 3D LUT (an option for low-end cards, in the same slot; the owner decided it is
not the default). Catch: the combined LUT outputs projector codes, but LED compensation needs
linear target light. Decide when building the LUT option (ADR-0013).

**colour.extended_range (175), built, cross-cutting.** BTB/WTW preservation: VP relabels the
source and its hooks do the conversion. The probe codes must be identical with every output slot
on and off.

### 3.3 OUTPUT SIDE: the corrected hook order

The hybrid scaler runs **inside libplacebo, before its PRE_OUTPUT hooks**, so every VP output
hook runs at the **output size**. Order (ADR-0013):

| # | Slot | Stage | Status today |
|---|---|---|---|
| 0 | (plumbing) | libplacebo output-encoding repair: first, when any PRE_OUTPUT hook runs | built |
| 200 | colour.sdr_tone_shape | Delta 13 SDR tone shape + DV trims (part of the Stage 13 design: the shape runs first) | built |
| 210 | colour.appearance | **Stage 13 CAM16** (off by default) | placeholder |
| 220 | drive.led_measure | LED measurement (pre-compensation content) | built |
| 230 | drive.led_compensation | LED adaptive gamma / compensation (CNTR never above need) | built |
| 240 | drive.black_level | **Black level correction** | placeholder |
| 250 | output.calibration_lut | **Projector calibration LUT, applied by VP** (last colour op) | today libplacebo's target LUT, which libplacebo applies BEFORE PRE_OUTPUT (`renderer.c` colour mapping) = wrong order |
| - | (plumbing) | user custom shaders; adaptive-downscale blend; de-judder rendered-picture cache (last PRE_OUTPUT) | built |
| 260 | output.dither | Sigma-delta / libplacebo dither (OUTPUT stage, quantiser last) | built |
| 270 | output.presentation | Swapchain, refresh, render-ahead inside the budget, subtitles | built |

**Two LUTs.** (1) The projector calibration LUT is the default path: SDR target -> projector
codes, applied last by VP. (2) The combined HDR->SDR LUT is the low-end option in libplacebo's
tone-map slot.

**colour.appearance (210), placeholder.** CAM16 forward under reference conditions, then inverse
under screen conditions. It uses soft hue-preserving M compression and a strength blend, and is
static per viewing setup. When on, it replaces the gamma boost + saturation. True BTB/WTW codes
pass through. Keys (planned): `appearance_adaptation` (off), `appearance_strength` 1.0,
`appearance_reference_white_nits` auto, `appearance_reference_surround` dim,
`appearance_screen_white_nits` 48 (until the i1Pro measurement), `appearance_screen_surround` dark.
Open: one owner per correction with the ambient light sensor.

**drive.black_level (240), placeholder.** Compensates the projector black floor as LED/CNTR
change (delta 11 toe / ANSI lift). It runs after compensation, before the calibration LUT. The
design is not yet specified beyond its position.

### 3.4 CONTROL (outside the pixel path)

- **drive.projector_control (900)**: RS-232, LED + CNTR, script intent / internal script / compensate.
- **control.net_runtime (910)**, placeholder: net packages (shader + parameters + fingerprint),
  fingerprint check, per-net bypass with a visible entry. fp16 intermediates preferred.
- **control.diagnostics (920)**: the single **"Fallbacks engaged: (list)"** heading ("none" when
  clean) in Ctrl+I, Config status and Ctrl+Alt+D, plus a one-time transient notice; crash ring;
  logs.
- **control.config (930)**: per-unit tiers + global, toggles under sliders, unbuilt slots shown as
  "not yet implemented"; it is generated from `slots.json` by the settings agent.

### 3.5 Tier map (initial; the settings agent refines it)

| Unit | Classic | Low | Mid | Upper |
|---|---|---|---|---|
| Repair | off | VP restore or small A | A | A_motion + texture restore |
| Scaling (entrance) | off (straight to output) | off | re-base + 4K route | + refine |
| Scaling (output) | Lanczos | hybrid | hybrid | hybrid + D |
| Motion | off | de-judder (matcher) | NVOF + matcher | full |
| Colour | libplacebo or combined LUT | libplacebo or combined LUT | libplacebo | libplacebo |
| Output | libplacebo dither | sigma-delta | sigma-delta | sigma-delta |

Stage 13 is available in every tier when on, because it costs about nothing.

### 3.6 Structural rules from the barrier scan (docs/design/VP-barriers-2026-10-24.md)

These rules change HOW the slots run, not their order.

- **B3: the input side runs ONCE per source frame.** re-base -> repair -> deband -> texture
  restore -> 4K route -> refine -> chroma run once per source sequence. The result is cached by
  sequence (the "entrance cache").
  - The motion consumers (de-judder phase pictures, extras, crossfade, hold) WARP or READ the
    cached result. They do not re-run the front end.
  - Vectors reach them per axis, through `motion.vector_delivery`.
  - Today the stage chain is copied three times in `LibplaceboVideoRenderer.cpp`: main ~23505,
    extra ~16491, crossfade ~5433. The skeleton replaces the copies with ONE stage list driven by
    the slot registry.
  - Consequence: the phase picture is made from the processed picture. Today it is made from the
    native planes, before re-base.
  - A net then runs once per source frame. In the current chain it would run ~2.5x at 24p-in-60.
- **R2: one SlotRegistry** covers the front-end passes and the output hooks, in `slots.json`
  order.
  - A libplacebo hook is REGISTERED ONLY WHEN ITS SLOT IS ACTIVE. A registered no-op hook still
    forces pass boundaries, so it is not zero-cost.
  - A front-end placeholder returns the same `pl_frame`: no pass, no copy.
- **B1/R3: VP-side NetRunner** (`control.net_runtime`). It executes the exporter's mpv-format
  passes on VP-owned textures through `pl_dispatch`: a shim for DESC/HOOK/BIND/SAVE/WIDTH/HEIGHT/
  COMPONENTS/COMPUTE and the HOOKED_* macros.
  - It does not use libplacebo LUMA hooks. A LUMA hook would run after deband/texture restore,
    and never on RGB.
  - Intermediates are fp16 by default. The intermediate format is part of each net's
    fingerprint.
  - The same runner serves the front end (A, the refine net) and D inside `AdaptiveDownscale`'s
    POST_KERNEL hook. `pl_hook_params` carries a dispatch.
  - **RGB capture:** repair the existing r16 matcher luma, then add the luma delta to R, G and B.
  - D's light domain (linear vs gamma at POST_KERNEL) must match the training domain or be locked
    in the fingerprint (R8).
- **R1: the calibration LUT becomes VP's LAST output hook.** It uses `pl_shader_custom_lut` plus
  the BTB/WTW late restore. The owner agreed that VP applies it after LED compensation.
  - Consequence: the LED measurement then sees target light, not projector codes. Today it
    measures projector codes.
  - Check the OutputEncodingRepair interplay.
- **R4: the chroma slot output stays 2 planes:** luma + a full-resolution CbCr plane. libplacebo
  then does not upsample, and the many `num_planes == 2` paths keep working.
- **R5: "Fallbacks engaged"**: the bug-fix agent builds the initial registry mechanism on
  `fix/r6-baseline`. The skeleton reuses it and does not build a second one.
  - Each slot's `fallback.entry` in `slots.json` is the text it reports.
- **B2: the queue is a TOTAL-DELAY BUDGET.**
  `queue frames = floor((budget - fixed costs) / frame period)`, with a 4 / 3 / 2 ladder.
  - Fixed costs = swapchain depth (`max_frame_latency`), render-ahead, processing, and the
    capture transfer time where the budget covers it.
  - The rounding is DOWN. Today `QueueLatency.h` rounds UP: 200 ms x 23.976 = 4.8, which becomes
    5 frames = 208 ms.
  - DECIDED (owner 2026-10-24, ADR-0020):
    - ONE user-set total delay. 4 frames is RECOMMENDED (`DelayBudget::RecommendedBudgetMs`:
      4 frames at the slowest source rate, about 267 ms (measured, 23.976 on 60 Hz) at 24p).
    - The SAME total at every frame rate: faster rates are padded (`Plan::paddingMs`).
    - The ladder is 4 -> 3 -> 2. Config shows the resulting frames, degraded state and margin.
    - The budget stays configurable.

### 3.7 The "Original pathway" baseline (owner 2026-10-24, ADR-0019)

Switching every VP addition off must reproduce **Bill's original pathway**. This proves the VP
additions are modular add-ons and gives a known-good reference.

- Every slot has an `origin`: **upstream**, **mixed** (it exists upstream, with VP options
  added) or **vp** (a VP addition).
  - Upstream: tone/gamut.
  - Mixed: capture, signal, queue/timing, source analysis, geometry, output scaling,
    calibration LUT, dither, presentation, config, diagnostics.
  - Everything else is vp.
- `Registry::ApplyOriginalPathway()` (and later a Config profile "Original pathway") turns every
  vp slot OFF. Every mixed slot runs in its upstream form via the overrides in `slots.json`
  `original_pathway.settings`, for example `capture_path standard`, `source_resolution native`,
  `adaptive_downscale off`, `sigma_delta off`, `render_ahead off`.
  - In this mode the calibration LUT stays libplacebo's target LUT, as upstream does. The VP
    LUT hook (R1) is a VP-mode change.
- **Golden test (planned, gate item).** The renderer capture harness renders synthetic fixtures
  (our own patterns; no film frames) in original mode. It compares them byte-for-byte with
  goldens recorded from an upstream-equivalent build (the bill-v1.4.02 line, with the SAME
  libplacebo build). A timing check compares frame order, repeats and drops for the same input
  rate.

**Documented exceptions.** Each is justified and gets a tolerance in the test:

| Exception | Why it cannot be identical | Tolerance |
|---|---|---|
| libplacebo version / VP fork patches (Beta 377 + clamp patches) | VP needs the fork for its own stages; the goldens use the same libplacebo build to remove this difference, else +-1 code | byte-identical with the same libplacebo; +-1 code otherwise |
| OutputEncodingRepair | It runs only when a PRE_OUTPUT hook is present; original mode has none, so it is inactive | identical |
| VP bug fixes in upstream code (queue/timing fixes, capture resync hardening, crash fixes) | Fixes that cannot be switched off without reintroducing crashes/stalls | timing only; listed per fix |
| Config migration (retired/renamed keys) | Upstream configs are migrated on load | n/a (settings mapping test) |
| Chroma siting fixes (2026-10-24) | Re-based frames scale the plane shift; the legacy CPU P010 path is tagged TOP_LEFT; VP corrections of upstream behaviour; the Original pathway keeps upstream's LEFT tag and has no re-base | none in the Original pathway |
| "Fallbacks engaged" status / diagnostics | Status only, not in the pixel path | n/a |
| LED drive (Dynamic Drive) | The LED module is created by the host, not the renderer. Original mode turns its hooks off (slot switches), but the projector commands follow the LED config, so the golden test runs with the LED drive off | test precondition |

### 3.8 How "once per source frame" (B3) preserves the pathway intent

The owner approved B3 on condition that it keeps the same order and meaning, with only the
redundant per-refresh repeats removed.

- **Order unchanged.** The entrance stage list runs the input-side slots in the `slots.json`
  order (re-base -> repair -> deband -> texture restore -> 4K route -> refine -> chroma). The
  registry is the only source of order.
- **Meaning unchanged.** Each stage still sees exactly one source frame and produces that
  frame's processed picture.
  - Today every refresh of a 24p frame at 60 Hz re-runs re-base + deband on the same input. The
    duplicate runs produce the same picture, or a phase picture that the design says should be
    made AFTER the input side. Removing them changes no stage's job.
  - Stateful stages already advance once per source frame: the debander hold (slot 0) and the
    detail pulse/texture restore history advance on A. They keep exactly that.
- **Motion consumers move AFTER the input side**, as designed (processing order 130-140). They
  warp/read the cached processed picture, with vectors delivered per axis (`VectorScale`).
  - This is the one intended visible change: today the phase picture is built from native
    planes, BEFORE re-base/deband.
  - It is measured: a byte-identical check on non-motion frames, and a quality/shimmer check on
    de-judder frames.
- **Crossfade.** The crossfade reads the cached results of both paths: the path being left keeps
  its cache entry for the fade.
- **Cache keys.** The cache is keyed by the source sequence + an epoch. The epoch is bumped on
  settings, re-base factors, processing size or format, so a stale picture is never reused.
  Sequence 0 (unknown) is never served from the cache.

### 3.8b B3 status and the hard gate (ADR-0024)

**The default since 2026-10-24 (`pipeline_entrance_once`, owner accepted; `off` = the per-picture
diagnostic path).** With it on:

- **The ring.** `EntranceStages::Ring` keeps each source frame's processed picture as its own
  copy (a plain copy pass, exact for the 16-bit and float planes), keyed by the unique source
  sequence, the frame's structure (crop, range, colour, re-base, active stages) and a role.
  - A: the frame drawn as itself, the stateful stages (debander hold, detail history) advanced
    on it once.
  - B ahead: the next frame, processed **as its own A** before it is shown. With stateful
    stages, B's own vector field is estimated ahead (the de-judder's estimate ahead, (B, C),
    while A's remaining pictures read the snapshot of (A, B)). So the picture B shows at its own
    draw is the very texture made ahead: exact continuity at the source-frame boundary. When
    B's field cannot be made ahead (no C queued yet, an estimate failure), B is processed per
    picture for that frame and "Fallbacks engaged: entrance once (B's vectors not ready ahead
    ...)" says so.
  - The per-frame readings (bitrate quality, level step) of B ahead are A's: a one-frame lag of
    those readings, by design (a frame's processed picture is made once).
- **The consumers.** Timeline pictures between frames (interpolation, hiccup gap pairs) and the
  source-path extras (the warp, Stage A interpolation) warp or blend the processed planes. The
  shaders map the field (estimate pixels) onto the processed size (`MotionWarp::WarpProcessed`,
  `FrameInterpolator::Run(estimate size)`); at scale 1 they are byte-identical to before.
- **Visible fallbacks:** a source-resolution crossfade, a composed (subtitle) picture, a copy
  failure. Those pictures take today's per-picture stages.
- **Cost (measured, real film clip, 24p in 60 with interpolation).** Input-side runs per
  source frame: 2.43 with B3 off, 1.05 with B3 on (about 2.3x lower). The default 24p-in-60
  path (the de-judder's rendered warp) already runs the input side once per source frame with
  B3 off; B3 does not change it.
- **Measured acceptance so far** (`EntranceOnceRealClipAcceptance`,
  `EntranceOnceInterpolatorIsExactOnStillOrInvalidVectors`): continuity (B ahead is B's A,
  fallbacks only at start-up), grain 96-101% of B3 off, B3 on byte-identical to B3 off without
  motion pictures, the interpolator exact on stills and invalid vectors at plane scales 1.5
  and 2. The delay gate's ladder passes with the held-backlog shedding.
- **A held queue sheds a backlog** older than its hold by more than two frames
  (`AlphaQueuePolicy::ShouldShedHeldBacklog`): without it, a start-up or render stall left the
  delay 60-120 ms above the budget.
- OFF is byte-identical to today's code (GPU test against the r6 build). The Original pathway
  never uses it.
- **Audit 2026-10-24 (fixed):** after B is processed as its own A, a late fallback picture of A
  never runs the stateful stages again (the ring's processed A or the per-picture path; a
  "history breaks" counter proves it); a fallback stays shown for its source frame; ring
  pictures are drawn with the frame's own descriptor (crop, per-frame HDR metadata); occlusion
  fill off on processed planes is shown; held sheds drop exactly the late frames.
- **Timestamps mean the frame's start** (Magewell stamps the start; DeckLink's arrival stamp is
  moved back by the frame duration), and the age gate waits the transfer, then the hold: the
  transfer is counted once for every card.

**The hard gate.** B3 passes ADR-0024's nine real-clip criteria and becomes the default before
any deploy, motion training or dependent net integration. The net runner and net A come after
it. Until then `motion.consumers` stays fixed in the order and outside `UpstreamKey`.

### 3.8c KNOWN GAPS (the single list; nothing is dropped silently)

| Gap | Where it shows | What closes it |
|---|---|---|
| B3 criterion 3 (pans A/B) not run | owner waived it 2026-10-24 (B3 default on) | an owner A/B on real pans whenever convenient |
| B3 crossfade keeps the old order | "Fallbacks engaged: entrance once (source-resolution crossfade)" | version 2: both paths' processed pictures in the ring |
| render_ahead total across rates | "Fallbacks engaged: delay budget (render-ahead ... PENDING-RA)"; delay gate PENDING-RA | render-ahead's lead folded into the queue hold |
| 120 Hz displays hold ~4 film frames more than planned | delay gate PENDING-HZ | investigate the 120 Hz harness case |
| A queue_size below the hold | "Fallbacks engaged: delay budget (queue_size N holds X of Y ms; raise queue_size)" | the operator raises queue_size (32, the deployed value, holds 267 ms at 60p) |
| LED and subtitle consumers at 2 queued frames | look-ahead ladder | a projector/subtitle fixture to test the shorter look-ahead |
| Nets and tiers | slots are placeholders (zero cost) | the net runner after B3, then net A |
| Mid-stream rate change in the delay gate | not yet a test | a harness case switching 23.976 -> 59.94 |
| Steering settle time after a reset | 1-2 minutes for a large sub-frame rest at 30p | a faster initial trim |
| Detail look-ahead for B ahead | B3's B ahead skips the optional detail look-ahead | measure C's bands when C is uploaded |
| B3 needs C queued for B's field ahead | "Fallbacks engaged: entrance once (B's vectors not ready ahead (no frame after next ...))" at start-up or on a short queue | the delay budget's 4-frame look-ahead keeps C queued; shorter queues fall back per picture |
| Timeline pictures vs plain draws at near-black | was ~2500 pixels of a still 1080p frame at +-1 code (B3 on and off): sub-pixel noise vectors resampled at each phase - fixed in the interpolator (unchanged pixels with under half a pixel of motion drawn as themselves); residual ~15 pixels at codes 0-3 | identify the residual (candidates: the A-aligned consumers skipped on pictures between frames - grain map, residual hold, occlusion mask); `NearBlackTimelineVsPlainProbe` switches stages by environment |
| Closed-loop de-judder extras on this laptop | its panel (iGPU-driven) gives no DXGI statistics, windowed or fullscreen: the extras hold ("timing not known: display rate") | B3's source-path extras are verified through render-ahead's open loop (asserted); closed loop needs a display with statistics (the 3090 / projector PC) |
| Delay total: the timeline (interpolation) path | the plain path's age gate holds transfer + hold from the frame's start; the timeline steers by frame count, its total is covered only by the DelayGate model | measure the timeline's total in the harness the way the ladder test does |
| Delay total: swapchain and display phase | modelled constants in the ladder test ("hold + transfer + render measured") | an input-to-light bench (the hardware loop) |
| Start-up dark-level settling | a still picture's near-black codes change once ~3 s after start (plain draws too) | identify the settling stage; tests capture after it |

### 3.9 Real-time proof on the laptop (B4)

The 3090 trains this week. The real-time proof uses the **laptop RTX 3050 Ti**, projected to
the 3090:

- **Per-slot GPU timestamps.** D3D11 timestamp queries bracket each slot's passes/hooks, using
  the existing `AlphaD3D11FrameGpuTimer` pattern. They feed `SlotTiming::Accumulator`.
- **Every-frame mode.** The analysis mode processes every frame in order, with no dropping. It
  is fine even at ~1 s/frame, so each slot's cost is measured on the real content.
- **Projection.** `SlotTiming::Project` multiplies each slot's laptop mean by its measured
  3090/laptop ratio (from BenchKit or paired runs), and compares the sum with the frame budget.
  Slots without a measured ratio use a fallback ratio and are COUNTED as unmeasured in the
  report.
- The laptop timings also anchor the Low/Mid tiers (the laptop 4K30 goal).

### 3.10 Reorderable modules (owner 2026-10-24, ADR-0021)

The overarching control program places modules in the designed order. Because it is modular, it
can move a module to another position.

- **The order is data.** The setting `pipeline_order` lists slots that take each other's
  positions, for example `repair.encoding, scaling.rebase`. An empty setting gives the designed
  order.
- **Each module declares a contract** (`src/VP-V/core/SlotContracts.h`):
  - the resolution domain it needs and gives: delivered / encoded / processing / output;
  - the light domain: source / target light / projector codes;
  - the plane layout;
  - vectors needed or produced;
  - whether it must see the original frame;
  - its zone: VP front end / libplacebo-internal (fixed) / VP output hooks.

  On top of the contracts there are hard precede rules: LED measurement before compensation,
  compensation before black level, SDR shape before Stage 13.
- **The validator** (`OrderValidator.h`) rejects an invalid order with a plain reason, for
  example "repair.deband needs the delivered or encoded resolution, but at that position it gets
  the processing resolution (after scaling.route_4k)".
  - An invalid order never runs: the designed order stays, under "Fallbacks engaged".
- **Net fingerprints include `UpstreamKey`**: the running slots before the net, in order. A move
  bypasses the dependent nets visibly.
- **Uses:** the A-order experiment, deband position experiments, future modules, and order
  variants in the pairwise test generator.

---

## 4. Decisions and open items

Decisions: see `docs/decisions/` (ADR-0001 .. ADR-0024).

| Open item | Decided by | Affects |
|---|---|---|
| Re-base before or after repair (A order) | round-2 A diagnostic, both orders | `scaling.rebase` order 60 vs 75 |
| Keep net A? | diagnostic: >= ~0.5-1 dB low-bitrate gain, grain kept | `repair.encoding` component |
| Deband need with A on | round-2 results | `repair.deband` default |
| Refine: one told vs two versions | round-2 cost/quality | `scaling.refine` package |
| D cost and grain | VP measurement + harm checks | `scaling.output` D default |
| Combined LUT vs LED compensation | when building the LUT option | `colour.tone_gamut` |
| Chroma upscale need | libplacebo chroma verification | `scaling.chroma_upscale` |
| Black level design | later spec | `drive.black_level` |
| ~~O1/O3 timing~~ DECIDED: one user-set total, the same at every rate, 4 frames recommended (ADR-0020) | owner | `input.queue_timing` budget |
| 3090 real-time check (O2): after the owner returns; laptop per-stage timings scaled meanwhile (3.9) | owner | chain gate timing |
| Nets: rgba32f vs fp16 (O5); fp16 default proposed | owner + round 2 | `control.net_runtime` |

---

## 5. How the skeleton implements this

All VP-V code lives under **`src/VP-V/`** (owner 2026-10-24). Bill's code stays in place, with
a few clearly commented `#include <VP-V/...>` hooks. Read `src/VP-V/README.md` first.

- **`src/VP-V/core/`**: header-only, C++14, CPU-tested. One job per header:
  - `PipelineSlots.h`: the slot table, mirroring `slots.json` (drift-checked).
  - `SlotContracts.h` + `OrderValidator.h`: reorderable modules (ADR-0021).
  - `SlotRegistry.h`: the ONE registry, covering the zero-cost rule, slot switches, the Original
    pathway, the order and the fingerprint key.
  - `EntranceStages.h`: the stage list and the per-source-frame cache.
  - `OutputHookOrder.h`: the hook order.
  - `VectorScale.h`, `DelayBudget.h`, `SlotTiming.h`, `FallbackStatus.h`.
- **`src/VP-V/modules/<unit>/<slot>/`**: module docs, manifests and tests. Migrated code so far:
  `output/calibration_lut/CalibrationLutStage.*`.
- **`src/VP-V/nets/`, `config/`, `tests/`**: the net runner (planned); the VP-V Config section
  (settings agent); the unit tests (`PipelineSkeletonTests`, compiled by `VideoProcessor-Test`
  under the filter "VP-V").
- **Renderer wiring** (`LibplaceboVideoRenderer.cpp`):
  - `UpdatePipelineSlots` builds the registry on every settings apply.
  - `RunEntrance` is the one entrance stage list, used by the main draw, the de-judder extras
    and the crossfade's path being left. The three copied chains are gone.
  - `RunMainEntrance` reuses a cadence repeat's entrance result.
  - `TagSlotHooks` + `OrderFrameSlotHooks` put the output slot hooks in registry order.
  - `CalibrationLutStage` is the VP calibration LUT (`calibration_lut_stage = vp`).
  - **Settings** (global, in `[vpv]`; `[vprenderer]` is still read as a fallback, ADR-0023):
    - `pipeline_original_pathway`;
    - `pipeline_order`;
    - `pipeline_slots_off`;
    - `calibration_lut_stage`.

### 5.1 Skeleton integration status (feat/skeleton)

1. **Done:** the core library and its tests.
   - Reorderable modules: valid reorders work, invalid ones are rejected with a reason, and a
     move changes the downstream fingerprints.
   - Original pathway tests.
2. **Done:** the main, extra and crossfade copies replaced by one registry-driven stage list
   (`RunEntrance`).
   - Each path keeps its own module slots and rules, so the designed order renders as before.
   - A cadence repeat of a picture drawn as itself reuses its entrance result: the only
     redundant per-refresh repeats removed (3.8).
   - The debander hold and the detail history still advance once per source frame on A, which
     is processed by the stages before them, whatever the order.
3. **Default since 2026-10-24 (`pipeline_entrance_once`, 3.8b):** phase pictures and extras warp the
   ring's processed pictures; the shaders map the field to the processed size; B is processed
   ahead as its own A with B's field estimated ahead. Default OFF until ADR-0024's acceptance
   passes on real clips (`EntranceOnceRealClipAcceptance`, and the existing harness tests with
   `VP_TEST_ENTRANCE_ONCE=1`).
4. **Done:** output slot hooks tagged and ordered by the registry (unchanged for the designed
   order), and the VP calibration-LUT hook (R1) available.
   - The LUT runs before the de-judder rendered-picture cache, so the extras show the LUT-mapped
     picture too.
   - The default stays `calibration_lut_stage = libplacebo` (byte-identical) until the owner's
     measurement.
   - BTB/WTW in the VP stage are carried at slope 1 in code space (libplacebo mode uses the LUT
     edge slope): a documented difference.
5. **Config UI** for the tiers, slot switches and order list: the settings agent, from
   `slots.json` (`src/VP-V/config/`).
   - Fallbacks report through `core/FallbackStatus.h`.
   - The Original pathway also needs the LED drive off. The LED module is owned by the host,
     not the renderer, so this is listed as an exception for the golden test.
