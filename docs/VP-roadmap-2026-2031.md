# VP roadmap 2026-2031, version 2 (rebuilt 2026-10-17)

Rebuilt from scratch from the memory files, today's design and audit docs and `git log` (HEAD
3e0f82eb on merge-v1.4.02). It replaces docs/VP-fidelity-roadmap-2026.md; it does not patch it.

## 1. Summary for the owner

**Where VP stands.** The chain is largely built: native 12-bit RGB ingest with GPU unpack; source
resolution detection with a kernel-matched, per-axis re-base; VP's debander; NVIDIA optical flow
running ahead of display, with the matcher as fallback; de-judder (frame A, rendered warp); the
sigma-delta onto the 8-bit FL35 link; BTB/WTW kept in all cases where they exist (after HDR->SDR
tone mapping they have no meaning, by design; docs/VP-btb-wtw-answers.md); grain-faithful switches on by
default; LED/CNTR drive; the DV tunnel (robust, GPU unpack, L1 min, L2 trims off); 50/60p
interpolation and gap fill (off); pulldown detection (off); crash state; and a metadata layer
skeleton. Three gaps remain:
1. None of the last week's work has run on the FL35 or the 3090.
2. On mid-range GPUs the de-judder holds, because of how it is scheduled, not because the GPU is
   too slow.
3. The projector is still wired into the code: its constants, LED timings and gamma are guesses,
   not measurements.

**The five biggest bets for durability**
1. **Render-ahead plus measured budgets.** Cost per source frame, with the laptop as the reference
   GPU. This decides whether most people can run VP at all (docs/VP-render-ahead-design.md).
2. **Display profile plus auto-calibration.** A new display becomes a measured file, not a code
   change. The LED's response is measured, not assumed (scratchpad VP-auto-calibration-design.md).
3. **The metadata layer as the only door for HDR facts.** DV1, DV2, HDR10+ and measurement each
   feed one scene descriptor. The intent interpreter and the LED look-ahead read it.
4. **A faithfulness contract in the chain gate.** Precision, BTB/WTW and grain RMS are asserted on
   every change. This is what lets features become default-on safely.
5. **Owned infrastructure.** A maintained public libplacebo fork that tracks upstream, the
   report-a-problem bundle, and per-component handover files, so the work survives years of
   upstream churn and many agents.

**What changed from the old roadmap**
- **Rejected:** all neural methods, and detail accumulation by averaging.
- **New top priority:** render-ahead. The RTX 3050 Ti laptop is now the reference mid-range GPU.
- **Built since the old draft:** 50/60p, the pulldown fixes, the metadata skeleton, and the DV
  tunnel work.
- **12-bit output:** back as a future need. The DeckLink route is rejected. ~~The candidate is the
  measured FP16 PC-levels path; VPV covers a DV display.~~ **Owner decision 2026-10-17 (FINAL):** 12-bit output only via
  VPV, for DV displays. FP16 half-float output is PARKED (rounding/dither unknowns). Non-DV displays
  stay on the 10-bit 4:4:4 surface + sigma-delta.
- **New work:** auto-calibration, the intent interpreter, the detail-pulse stabiliser and texture
  restorer, the report hotkey, pruning, the efficiency round and the libplacebo fork.
- **LED:** the script now defines intent, not its coding limits.

## 2. Pillars

Status key: B = built, P = in progress, D = designed, I = idea.

**Capture & DV (B/P).** Built: native RGB, GPU unpack, early upload (experimental), DV tunnel with
GPU unpack (off). 15 capture bugs are listed (docs/audit/capture-card-readiness-2026-10-16.md).
- 6 months: cinema DV session; `dv_tunnel_gpu_unpack` on after its gates; the generalisations
  (mode from IDeckLinkDisplayMode, rowBytes, range/depth on VideoState, an error/restart policy);
  bench-verify a candidate card with a synthetic tunnel before buying it.
- 5 years: a format-agnostic capture layer; a card that takes 2160p60 8-bit RGB 4:4:4 untouched
  (60 Hz DV) and 10/12-bit 4:2:2; zero-copy upload.

**Source analysis (B/P).** Built: detection, VBR readings, per-axis and kernel-matched re-base,
pulldown with its fixes.
- 6 months: pulldown D2 and default-on from the Apple TV recordings; the G19 items (3:2/1440p
  inverse, per-axis inverse, chroma); fixed-constant batches B1/B2 (measured source classifier).
- 5 years: classification driven entirely by measurement; the 2K-master gap stays a known limit
  unless a new signal appears.

**Scaling & detail, faithful (B/D).** Built: re-base, debander (grain guard), shader add-ons list.
Designed: `detail_pulse_stabilise`, then `texture_restore` with a grain/scene split and a defocus
gate (docs/audit/detail-retrieval-research-2026-10-17.md).
- 6 months: build both, default off, in the order: detection on the original frame -> debander
  -> stabiliser -> texture restore -> re-base/upscale -> sharpening. Judge on the 3090.
- 5 years: default-on where the gates pass. No invented structure, ever.

**Motion (B/P).** Built: NVOF seed and producer, de-judder, Stage A/B interpolation (off), gap
fill, one vectors-valid state.
- 6 months: apply the NVOF corpus defaults; standalone hiccup repair (gap fill and dropped-frame
  repair without interpolation); interpolation grain preserve verified; low-latency backward
  vectors for 50/60p.
- 5 years: an interpolation option at parity with smooth motion (off for the owner); forward
  prediction as an experiment; higher refresh when a display takes it.

**Precision & output (B/D).** Built: rgba32f caches, the sigma-delta, BTB/WTW, the 10-bit
surface. 12-bit: no API offers a 12-bit swapchain (docs/audit/12bit-output-paths-2026-10-17.md).
- ~~6 months: loopback measurement of the FP16 PC-levels mapping on the laptop with the UltraStudio
  (1-2 days; scratchpad dv-output-and-fp16-2026-10-17.md);~~ the precision contract in the gate.
- 5 years: ~~a measured FP16 path for a 12-bit display if the measurement allows it;~~ VPV (VP sends a
  DV tunnel through NVAPI) for a DV display, proven bit-exact by loopback first.
- Owner decision 2026-10-17 (FINAL): FP16 output PARKED; 12-bit only via VPV for DV displays; non-DV displays stay
  10-bit + sigma-delta (docs/audit/dv-output-and-fp16-2026-10-17.md).

**HDR/DV mapping & metadata layer (B/D).** Built: SceneDescriptor, DisplayProfile and
MappingPolicy (no behaviour change); L1 min; L2 trims (off). Designed: the intent interpreter.
- 6 months: L1 into the LED look-ahead; the interpreter's content, display and black-report
  functions; motion intent in display-only mode.
- 5 years: DV2 and HDR10+ as parsers; a VP curve only if the projector shows a shortfall; the
  interpreter's policies promoted after judging.

**LED/projector (B/P).** Built: internal_script, compensate, cntr_pair_predict, read-back off,
resend only on failure.
- 6 months: highlight batch (measure the picture only, full-coverage peak detection, CNTR never
  above need); measured LED flux, latency and settle; `script_light_smoothing`; compensate
  `rate_domain: flux` (docs/VP-led-light-smoothing-plan.md,
  docs/audit/VP-led-compensate-review-2026-10-17.md).
- 5 years: a generic "dynamic light device" in the display profile.

**Calibration & display profiles (D).**
- 6 months: buy the colorimeter; build the calibration (about 25 days: Argyll spotread out of
  process, any meter Argyll supports, LUT plus profile plus LED characterisation); move the FL35
  constants into profile v1, byte-identical.
- 5 years: a new display is one calibration run; ambient adaptation for living rooms.

**Usability & Config (P).** Every key now has a control; signal order is in.
- 6 months: Config organisation pass; the shortcut table (modifier keys, toast, undo); the
  "Experimental content-adaptive settings" group; pruning.
- 5 years: a small, stable key set with old-config migration.

**Reliability & diagnostics (B/D).** Built: crash state ring, renderer retry, the hardened test
gate. Designed: report hotkey (DiagnosticHistory, Ctrl+Alt+D, about 5 days).
- 6 months: build the hotkey; bug-hunter rounds at each merge point.
- 5 years: user reports Claude can triage on its own.

**Performance, laptop reference (D/P).**
- 6 months: render-ahead Phase 1 plus gate and ladder; the efficiency round (full precision
  first); the laptop 4K30 profile; 3090 timing table
  (docs/audit/laptop-4k30-budget-2026-10-17.md).
- 5 years: every feature carries a measured cost on both GPUs; the half-precision laptop path only
  if it saves a lot.

**Engineering (P/I).**
- 6 months: public libplacebo fork with CI and a test that fails if output_fbo_float32 is lost;
  handover template drafted.
- 5 years: regular upstream rebases gated by the Beta GPU tests; a handover file per component at
  the close of the design period, with its change log fed back.

## 3. Sequence

### Next ~12 weeks (as currently queued)

| Wk | Work |
|---|---|
| 1 | Fix batch merged: OneAxisRebase GPU failure, LED stop-under-lock, pulldown leftovers. GPU gate on Beta 377, then GeForce. NVOF defaults (cost_trust off, fb_check off, global on) plus a fault test with motion-ahead engaged. |
| 2 | LED highlight batch, plus the compensate before-deploy items. |
| 2-3 | Fixed constants B1 (rate bounds, the 20-120 Hz DWM gate, the 15 fps floor) and B2 (measured classifier; thresholds per source frame; 25/30p as film; one refresh-key convention with cfg migration). 8-11 days. |
| 4 | Bug-hunter round, then a merge point (interaction agent, gate, auditors). |
| 5-6 | Render-ahead Phase 1, gate and ladder, tests (7-8 days). Standalone hiccup repair. |
| 7 | Report-a-problem hotkey (plugin API bump; EXE and DLL deployed as a pair). |
| 8 | Config organisation, shortcut table. |
| 9 | Intent interpreter, all five behind checkboxes; motion as display-only first. |
| 10 | detail_pulse_stabilise, then texture_restore (both off), with the whole-chain gate on and off. |
| 11 | Efficiency round (area agents; measured on the laptop and the 3090). |
| 12 | Pruning (owner approves the list; fallbacks kept). Calibration build starts. |

**Owner-dependent, slotted where possible**
- **Cinema DV session** (about 40 min, docs/audit/dv-live-verification-checklist.md): before the
  first deploy. Best after week 1's gate or week 6's render-ahead (see question 1).
- **3090 timings:** in the same visit.
- **UltraStudio loopback:** ~~FP16/10-bit mapping and~~ the VPV pixel test, any evening after week 4
  (FP16 parked 2026-10-17).
- **Apple TV recordings** (Mini Recorder, including a Crave 24-in-30 case): as early as possible.
  They gate pulldown default-on, G19, and the texture/stabiliser retests.
- **Colorimeter:** order by week 10 so it is on hand at week 12. Test it with Argyll 3.5; buy
  returnable.
- **Capture card choice:** after the bench tunnel test. Not before Q2.

### Next year by quarter

- **Q1 2027.**
  - Calibration finished. LED measurement leads to light smoothing and the flux-domain rate.
  - Display profile v1 (the FL35 constants moved, byte-identical); L1 into the LED look-ahead.
  - Pulldown default-on decision; G19 kernel work.
  - Capture generalisations, ahead of a card.
- **Q2 2027.**
  - Capture card integration (first-day checklist).
  - 50/60p low-latency vectors; zero-copy upload via a fork patch.
  - First upstream rebase of the libplacebo fork.
  - Default-on decisions for the stabiliser and the interpreter functions, from the judging.
- **Q3 2027.**
  - Close the design period and write the handover files.
  - VP curve or DV trims only if the projector shows a need.
  - Ambient adaptation tuned with the sensor; second efficiency pass.
- **Q4 2027.**
  - Maintenance cadence (upstream rebase, driver re-measure, nightly matrix).
  - Prepare for a new display: profile, calibration, and VPV ~~or FP16~~ as its link needs (a non-DV
    display stays 10-bit + sigma-delta; FP16 parked 2026-10-17). DV2
    parser when DV2 can be decoded.

## 4. Architecture principles for durability

1. **Display profile.** The renderer, the LED and the calibration read peak, black, EOTF, gamut,
   link depth/range/chroma, refresh set and the light device only from the profile. A new display
   is a file.
2. **Metadata layer.** Every HDR fact enters through SceneDescriptor with a source tag (measured,
   DvL1, DvL11, Dv2). Consumers never parse a format themselves. DV-derived behaviour sits behind
   its own switch and is described as "VP's interpretation".
3. **Stage registry.** Each stage declares what it needs (vectors, metadata), its minimum
   precision, cost, switch and default, fallback, and chain-gate axis. MotionValidity and the
   VectorStore are the pattern.
4. **Measured budgets.** Cost is per source frame, as p95 per slot type, on the laptop and the
   3090. The degrade ladder drops extras, never precision.
5. **Precision contract in the chain gate.** Intermediates are 16-bit float or better and
   sign-preserving, with one final quantisation. BTB 4-63 and WTW 941-1019 survive every cell where they exist (not after HDR->SDR tone mapping, by design).
   Grain RMS stays within ±5% and lag-1 correlation no higher than the source's. Fork-only patches
   have a test that fails if they are lost.
6. **Generality.** Rates, cadences, distances and resolutions are derived from measurement, never
   from lists. Distances are a share of picture height; rates are per second.
7. **Faithful by default.** A new feature ships off or behind "Fixes under test" with a
   byte-identical restore switch. It becomes default-on when it is faithful and the 3090 runs it.
   Fallbacks (the matcher, low latency) stay first-class and tested.
8. **Handovers.** Unit boundaries follow the stage registry. Each unit gets a handover file that
   points at the report-bundle fields for debugging.

## 5. Risks and dependencies

- **Unconfirmed hardware behaviour**
  - FL35 LED latency, settle and flux are placeholders.
  - Quad HDMI Recorder 4K60 RGB is unverified.
  - DXGI statistics on the laptop's internal panel are unconfirmed.
  - NVIDIA's FP16-to-link conversion and dithering are undocumented (the reason FP16 output is
    parked, owner 2026-10-17).
  - 3090 timings for everything new are unmeasured.
- **Driver changes:** NVOF ships in the driver; the NVAPI DV mode is "experimental" and has a
  stuck-black report; FP16 mappings would need re-measuring after each update (FP16 is parked).
- **libplacebo:** each rebase can bring back the 375 integer clamp, flip the PRE_OUTPUT repair,
  or drop fork-only patches. Bill may adopt master on his own schedule.
- **Upstream merges from Bill:** plugin API bumps and conflicts in the renderer, settings and
  Config.
- **DV ecosystem:** DV2 may not be decodable through the tunnel; tvOS or player changes (LLDV vs
  TV-led); no certification.
- **Capture card availability and cost:** the card is still the fidelity limit on the FL35 path.
- **Owner time:** the cinema sessions, recordings, measurements and judging gate most default-on
  flips.
- **Complexity:** the key count and parallel paths grow unless pruning keeps pace.
- **Agent cost and pace:** the 20x plan is this month only.
- **Licences:** ArgyllCMS (AGPL, so out of process only); the NVOF SDK licence before shipping;
  the CroqueMr packer before any upstream PR.
- **Laptop variance:** clocks vary 20-30% between runs, so budgets need p95/p99 margins.

## 6. Open questions for the owner

1. **First FL35 deploy:** right after week 1's GPU gate, or after render-ahead (week 6)? The
   cinema DV session goes with it either way.
2. **Pulldown default-on:** is the bar the 8 corpus criteria plus 98% locked on your Apple TV
   recordings with zero false locks?
3. **Capture card and next display:** budget and rough timing?
4. **Colorimeter:** OK to order the Calibrite Display Pro HL (returnable) around week 10?
5. **Script step tables:** the +7/-5 LPW1 steps and the CNTR 50/51 toggle exceed visibility.
   Leave the tables as your script has them (smoothing only), or allow finer steps?
6. **Design period:** when should it close, triggering the handover files? Proposed: Q3 2027.

## 7. Obsolete or rejected from the old roadmap

| Item | Reason |
|---|---|
| ONNX-DirectML upscaler and denoiser, ML re-base, learned source classification, ONNX timing spike and gate axis | Owner: no neural methods (they invent detail and are poor value) |
| Neural motion estimators (RIFE/RAFT) as a primary path | Same; only reconsidered if real footage shows an occlusion shortfall |
| Detail accumulation, including "retry as MC grain averaging" | Prototype +0.01-0.09 dB against a +1 dB gate; averaging breaks the grain rule. Replaced by the pulse stabiliser and texture restore |
| 12-bit via a DeckLink output card | Owner rejected it; a DV display gets VPV instead |
| FP16 scRGB surface as a second output path (2026-10-15 form) | Dropped as a parallel-path hassle |
| FP16 half-float PC-levels output (any form) | PARKED by the owner 2026-10-17 (FINAL): rounding/dither unknowns; 12-bit only via VPV for DV displays; non-DV stays 10-bit 4:4:4 + sigma-delta |
| D3D12/Vulkan rebase for 12-bit | No API has a 12-bit swapchain; months of work for no gain |
| Interpolator Stage B and gap fill as future targets | Built (off by default) |
| Question "deploy before or after 50/60p?" | Moot: 50/60p is built |
| The old 90-day sequence | Done or superseded by the current queue |
| Per-scene VP tone curve tuned to the FL35 as a planned step | Planner order: libplacebo's curve informed by L1 stays default; a VP curve only if a shortfall shows |
| "Warn, never auto-degrade" budgets | Replaced: the render-ahead ladder drops extras, never precision |
| Light meter as a "future maybe" | Replaced by auto-calibration with an Argyll-supported colorimeter |
| Answered questions Q1, Q3, Q5, Q6 | Answered 2026-10-17 (Q1 closed for the FL35; non-certified trims yes; interpolation off for the owner) |
| Python LED source, relay and listen host | Removed 2026-10-15; the harness validates the script copy |
| Scaling, Dither and Light sliders | Removed or dropped (the shader add-ons list replaces Scaling) |
| `dejudder_motion_rolloff` as a standalone control | Becomes the "Ease on fast pans" policy under motion intent |
