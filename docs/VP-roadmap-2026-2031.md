# VP-V outlook 2026–2031, version 3 (2026-10-26)

Written by the planning agent (read-only) from the owner's goals, the v2 roadmap (2026-10-17), the
current TODO and the 2026-10-26 planner pack. Replaces docs/VP-roadmap-2026-2031.md v2. Dates are
estimates for a hobby project, never deadlines.

## 1. Where VP stands — in plain terms

Think of it as an operation that has been planned in detail and rehearsed many times on a simulator,
but not yet performed on the patient.

**Built and integrated** (on `integ/batch1-wip`): the whole chain from the capture card to the
projector — native RGB capture with GPU unpacking (the 12-bit route exists but is not yet verified end
to end) and the Dolby Vision tunnel; source analysis that detects a stream's real resolution and undoes
the streamer's upscaling; cleaning (debanding, detail stabilisation); motion processing (NVIDIA optical
flow checked by VP's own matcher, de-judder); grain-aware scaling down and up (Stages 8 and 8b); the LED
drive with contrast assist, tied to the picture; the sigma-delta that gets 10-bit smoothness through the
FL35's 8-bit link; below-black and above-white detail kept; crash recording. Learned nets are being
wired (A for compression repair, D to refine the downscale), B-refine is designed, and training runs on
the 3090.

**How it is built** has become as important as what: every package is audited before it is built and
merged, every stage can be switched off with the output exactly as if it were absent, and a programme is
separating VP's code from Bill's so both can keep moving.

**Not yet true:** the integrated chain has not been seen on the FL35; the 3090 has measured only parts of
it; the projector's own numbers (LED timing, screen brightness, display response) are still
placeholders; and VP's code still lives inside 178 of Bill's files.

**What changed since v2:** neural nets, rejected in v2, are now a pillar — always as add-ons whose "off"
is the classical path exactly. Upscaling is B-refine on top of Stage 8b rather than a replacement.
Bill's code is now governed by a rule (registered fixes, either/or with Bill's behaviour, or revert).
12-bit output stays reserved for a future DV display (VPV); FP16 output stays parked.

## 2. The arc, year by year

**Late 2026 — finish the integration, first light.**
- WP-9 chain, nets A and D wired with a generic net host and cost gate, Stage 8b's one-axis fixes, the
  Bill-divergence extraction (N3), chroma measurement (N8), intent audits and efficiency sweeps, then
  **WP-20, the big test**.
- **WP-21, the Bill merge**, qualified as a diff against frozen references.
- **M1, the first FL35 deploy** (owner's timing), with the first cinema session: the ramp test, a pan, a
  dark scene, screen white, the LED and contrast step curves (phone Slo-Mo), the Dolby Vision live check.
- *Unlocks:* real numbers in place of placeholders, and the owner's eye on every default.

**2027 — measure, calibrate, refine.**
- H1: **TrainKit round 3** (A's colour head, B-refine, half-resolution D) and wiring of whatever passes;
  **calibration engine** (Argyll out of process, the ES-1000 for spectral truth, a colorimeter for speed)
  and **display profile v1** — the FL35's constants move out of the code into a measured file; **LED
  driver auto-config** from the measured response; **Stage 13** appearance adaptation on the measured
  screen white; the DV metadata layer with L1 feeding the LED look-ahead; **render-ahead** on top of
  Bill's display clock.
- H2: **capture card upgrade**, bench-verified with a synthetic tunnel first; a **pruning round**
  (section 6); the tier presets (Classic / Low / Mid / Upper) settled from 3090 and laptop costs; the
  design period closes and each module gets a handover file.
- *Unlocks:* a system defined by measurements rather than guesses, and one that could drive another display.

**2028 — new formats, new display.**
- **DV2 and HDR10+** as parsers into the one scene descriptor, when they can be captured.
- **A new display** (if and when): one calibration run plus a profile; **VPV 12-bit** if it is a DV
  display, proven bit-exact by loopback first; non-DV displays stay on 10-bit plus the sigma-delta.
- Nets retrained on current codecs (AV1 film-grain synthesis, newer streaming encoders); light-sensor
  ambient adaptation.
- *Unlocks:* VP outlives the FL35.

**2029–2031 — new capabilities on a stable base.**
- **Edge blending** (multi-projector: geometry warp, linear-light blend ramps, black-level uplift,
  per-projector colour match from the calibration engine) as the major new feature.
- A GPU/API decision when the 3090 is replaced (section 3).
- Maintenance cadence: upstream rebases, driver re-measurement, the nightly matrix, nets refreshed as
  content changes.
- Liftable modules published (sigma-delta, LED drive core, detection and re-base, motion engine,
  appearance adaptation).

## 3. Technology bets and their exits

| Bet | Current position | Exit / trigger |
|---|---|---|
| **libplacebo** | Track Beta 377+ through a minimal local fork (patches 0001–0004+), each patch guarded by a test that fails if it is lost; retire the 7.360 track after WP-21. VP owns the front and output ends; libplacebo stays the colour engine and GPU layer. | Patch count past ~8, a hook limit blocking a needed feature, or adverse upstream direction → replace **one** part (scaler or presenter) inside a liftable module, never the whole. |
| **D3D11** | Stay; libplacebo abstracts it, NVOF and the capture interop work today. | NVIDIA or libplacebo limiting D3D11 (NVOF, interop, a net runtime) → libplacebo's Vulkan backend, with VP's code above the GPU layer kept API-neutral by the module boundary. |
| **Net runtime** | Shader-exported nets on VP's libplacebo runner, fp32 by default, fp16 a labelled manual switch. | A net that cannot fit its budget as shaders → DirectML / tensor cores (P9 spike); the shader path stays as the reference. |
| **Motion** | NVOF as the seed, VP's matcher as verifier and fallback. | NVOF deprecated or absent → the matcher with estimator-lite (already first-class); learned candidates (Stage 11b) as an alternative seed. |
| **Neural vs classical scaling** | Classical is the floor: net off = classical exactly. Nets earn default-on by measured fidelity and the owner's eye. | A net fails its gates or a new GPU makes it unaffordable → off, nothing lost. |
| **Bill** | Extract, register fixes as PR candidates, Original pathway = Bill's behaviour, merge after the big test, weekly rehearsal merges. | Divergence of direction → freeze at a version and cherry-pick fixes; the Original pathway keeps the comparison honest. |
| **Liftable modules** | Each module: small API, own config and tests, no reach into VP internals. | — this is the exit for every other bet. |

## 4. Risks over five years

| Risk | Mitigation |
|---|---|
| **Upstream divergence** (Bill 156 commits ahead; libplacebo churn) | Extraction to call sites; weekly rehearsal merges with a hunk-count trigger; fork-patch tests; the Original-pathway byte-identity test. |
| **GPU and driver change** (NVOF, NVAPI DV mode, D3D11, a new GPU changing net costs) | Measured budgets per slot on two GPUs; the matcher and classical paths first-class; re-measure after each driver update; the timing gate. |
| **Format churn** (DV2, HDR10+, AV1/VVC, Apple TV behaviour) | One scene descriptor, parsers as plug-ins; general code with no fixed rates or devices; recordings as fixtures. |
| **Single maintainer, agent budget, hosting** | Off-machine backups first; a non-GitHub remote (GitLab/Codeberg); handover files per module; batched pushes; schedules that tolerate pauses. |
| **Hardware availability** (FL35 LEDs/DMD, capture cards, the 3090 ageing) | Profile-based design so a replacement is a calibration run; bench-verify before buying; laptop as the reference mid-range GPU. |
| **Complexity** | Pruning rounds; key and pathway counts tracked; "fixes under test" promoted or removed each quarter. |
| **Determinism** (the ±1/16 timing hazard class) | Run-twice tests per slot; references recorded with the drain on; root cause pinned with the debug layer. |
| **Licences** | Argyll out of process (AGPL); NVOF SDK terms; training-data licences (UVG/Xiph local only); GPL-3.0-only headers kept. |

## 5. What to measure each year

- **Fidelity (corpus, every gate):** grain RMS within ±5% and lag-1 correlation not above the source's;
  BTB/WTW survival wherever they exist; edge MTF and a perceptual score against 4K masters for the
  scalers and nets; judder removed and damaged tiles for motion; false-lower readings for detection
  (must stay 0).
- **Timing (laptop and 3090):** p95 cost per slot; dropped or repeated presents per hour; the fixed delay
  constant in every cinema mode; run-twice determinism.
- **Projector:** LED and contrast latency and settle (phone Slo-Mo); content-constant error during
  dimming; screen white and the calibration error (ΔE) from the calibration engine.
- **The owner's eye — an annual screening:** a fixed set of reference scenes (a pan, a dark interior with
  hair, skin in daylight, a fast action cut, a grainy sky, a Crave stream, a DV title) judged on the FL35
  each year against last year's build and against madVR. This is the measure that decides whether the
  bets are paying off.
- **Simplicity:** settings count, pathway count, lines of VP code inside Bill's files, fork-patch count —
  each should fall or hold.

## 6. What to stop or retire

- **Polar scaling**, once the separable one-axis path passes the "no ringing into blanked bars" test on
  the no-net path.
- **Pre-B3 code** (WP-9.6) and the **7.360 test track** after WP-21.
- **Superseded designs:** plain B and the `route_4k` placeholder slot; the D3 two-frame blend branch; FP16
  output (parked → retired unless a display needs it); the early-upload variants now that early upload is
  the rule.
- **Legacy keys:** the Python LED keys (accepted-and-ignored → removed after one release);
  DirectShow-only keys hidden in VP-renderer mode.
- **Duplicate fallbacks:** one no-vector path, one "Fallbacks engaged" pattern, one bypass-reason list for
  nets and stages.
- **Settings:** the owner-facing surface collapses to the tier presets and the motion slider, with every
  detail in the advanced boxes.
- **The MPC filter:** decide — build it with A/V sync on the roadmap, or drop it.
- **madVR as the fallback:** retire after a season of trouble-free use, on the owner's call.

## 7. Decisions for the owner in the next three months

1. **M1 timing:** first FL35 deploy after WP-20, or after the Bill merge (WP-21)? Either way, the first
   cinema session goes with it.
2. **`entrance_gpu_drain`:** default on in cinema profiles if the 3090 cost is small, as adopted —
   confirm once the number is in.
3. **Capture card:** budget and rough timing for a card that takes 4K60 8-bit RGB 4:4:4 untouched and
   12-bit 4:2:2, after the bench test.
4. **Measurement kit:** a used i1Display Pro / ColorMunki Display, and the go for the no-tile Argyll patch
   on the ES-1000.
5. **TrainKit round 3 order:** colour head first (streaming), or B-refine first (1080p sources)?
6. **Hosting and publishing:** where the repositories live after GitHub, and whether liftable modules (and
   the libplacebo fork) are published.
7. **The PR track:** when to offer Bill the registered fixes — after WP-21, or once the first deploy has
   proven them.
