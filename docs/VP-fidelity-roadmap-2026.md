> SUPERSEDED 2026-10-17 by docs/VP-roadmap-2026-2031.md (rebuilt against the owner's updated goals). Kept for history.

# VP fidelity roadmap 2026-2031 (draft for the owner, 2026-10-16 night)

Owner goal: scaling software that earns its keep for 5+ years (DV2, a new display) - highest fidelity
and highest-end features; 12-bit throughput later. Drafted by a planning agent from the memory
files and docs. Owner confirmed: the FL35 link is 8-bit - the reason the temporal sigma-delta exists
(it carries VP's 16/32-bit internal precision onto the 8-bit link); the work must still earn its place
on a future DV2-capable display, and the capture card is the current limiting factor.

## Plain summary

VP already captures 12-bit RGB, finds the real master resolution and rebuilds from it, removes film
judder with NVIDIA optical flow, dithers with motion guidance, keeps below-black / above-white and
drives the laser + CNTR with the picture. Two gaps: (1) the last week's work has not run on the FL35 /
3090 yet; (2) the projector is wired into the code (gamma, peak, link, refresh, LED law) - a new
display means code changes, not a profile. Next 90 days: finish the queue, build 50/60p, deploy and
measure on the 3090, then two foundations that make the next five years cheap: a **display profile**
(a new display = a measured file) and a **format-neutral metadata layer** (DV / DV2 / HDR10+ become
parsers feeding one tone-mapping path).

## Targets per pillar (1-2 years / 5 years)

- **Capture:** DV tunnel on the GPU + more timings (M); zero-copy upload via D3D12 interop or a
  libplacebo patch (L) / a 4K60 4:4:4 12-bit HDMI 2.1 card behind a format-agnostic capture layer.
- **Source analysis:** pulldown D1-D4 then default-on per the review's criteria (M); 3:2 / per-axis
  kernel inverse from the Apple TV captures (M); 2K-master detection (L, research) / ~~learned source
  classification as a hint behind the trust gates~~ - rejected 2026-10-17 (owner: no neural).
- **Scaling and detail:** ~~an ONNX-DirectML stage (onnxruntime-directml is already in 3rdparty) for a
  1080->4K upscaler and a temporal denoiser, gated by PSNR/SSIM vs FSRCNNX, flicker and the 3090 budget
  (L-XL)~~ - rejected 2026-10-17 (owner: no neural); detail accumulation retried as MC grain averaging
  (M) / ~~ML re-base later~~ - rejected 2026-10-17 (owner: no neural); GLSL stays the path.
- **Motion:** interpolator Stage B (per-vsync, 60p on 50 Hz) and gap fill (L); low-latency backward
  vectors (M) / multi-reference occlusion-aware interpolation, higher refresh when a display takes it.
- **Precision / BTB-WTW:** a precision contract in the chain gate (every intermediate >= 16-bit float,
  sign-preserving, one final quantisation; BTB 4-63 / WTW 941-1019 survive every cell) (S-M); the SDR
  full-strength roll-off fix (S); perceptual dither (M) / 12-bit output via a DeckLink output card or
  FP16 for full-range displays (owner question 1).
- **HDR and DV:** DV level check (S); L1 min + FL35 peak/black curve (S-M); L2/L8 trims if they arrive
  (M-L); per-scene dynamic tone mapping with queue look-ahead (M) / DV2 and HDR10+ as metadata parsers.
- **LED/CNTR:** projector validation first; DV L1 / scene statistics into the look-ahead (M); measured
  LED response (M) / a generic "dynamic light device" profile.
- **Config:** layout, shortcuts, display-profile and metadata pages, a GPU budget readout (M) / a
  calibration wizard (meter -> profile -> LUT).
- **Tests:** telecine corpus, new chain-gate axes (display profile, metadata source, interpolator,
  ONNX), pairwise per change + nightly full matrix, a 3090 timing gate / automated off-screen scoring of
  banding, detail, judder and levels per release.

## Architecture for the long run

1. Data-driven display profile (measured EOTF/LUT, black/peak, primaries, link depth/range/chroma,
   refresh set, optional dynamic-light device); renderer and LED read only the profile.
2. Format-neutral per-frame metadata (static MDCV/CLL, scene statistics measured or from the stream,
   trims keyed by target nits, source tag); tone mapping, LED and stats consume it.
3. A stage registry (needs vectors/metadata, minimum precision, cost, switch/default, fallback,
   chain-gate axis); MotionValidity is the first instance.
4. A measured performance budget per stage / GPU / format; warn, never auto-degrade.
5. Chain-gate growth: every new profile / parser / stage adds its axis before merging.
6. Generalise the fixed constants (IsFilmLike, 10-240 Hz bounds, 2160/1080 references, per-frame holds)
   behind `source_class: measured`.

## 90-day sequence

1. Weeks 1-2: chain checks (re-base / deband / CNN), pulldown fixes + telecine gate.
2. Week 3: settings layout, shortcut table, key retirement, tooltips.
3. Weeks 4-7: 50/60p (Stage A built on branch interp-5060 tonight; Stage B next), chain-gate axis.
4. Weeks 8-9: gate, audit, deploy to the FL35; 3090 timing table; BTB/WTW, sigma-delta, LED checks on
   the projector; DV level check; Apple TV captures (Mini Recorder).
5. Weeks 10-11: display profile v1 (FL35 constants moved, byte-identical test); metadata layer v1 (DV
   L1 incl. min); precision contract assertion.
6. Weeks 12-13: per-scene tone curve tuned to the FL35; L1 into the LED look-ahead; retest on the
   captures; ~~ONNX interop timing spike on the 3090~~ - rejected 2026-10-17 (owner: no neural).

## Questions for the owner

1. ~~12-bit output when the display changes: DeckLink output card, FP16 surface (full-range displays), or
   neither?~~ Answered 2026-10-17 (owner, FINAL): no DeckLink output card; FP16 output PARKED; 12-bit
   only via VPV (VP sends a DV tunnel through the NVIDIA GPU) for DV displays; non-DV displays stay on
   10-bit 4:4:4 + sigma-delta.
2. Deploy before 50/60p (validate on the FL35 sooner) or after?
3. ~~ML upscaler / denoiser acceptable if optional, gated and measured (it can invent plausible detail)?~~
   Rejected 2026-10-17 (owner: no neural).
4. Buy a colorimeter (e.g. Calibrite Display Pro HL) so display profiles are measured?
5. A non-certified DV L2/L8 trim reimplementation acceptable?
6. Interpolator: extras-only first, non-integer ratios only, fallback hold vs blend?
7. Pulldown: accept the review's pass criteria for default-on?
8. Hardware budget/timing for a 4K60 4:4:4 capture card and the next display?

## Updates 2026-10-17 (owner decisions)

- Built: 50/60p interpolator + gap fill; pulldown fixes; DV GPU unpack and trims; metadata layer (SceneDescriptor/DisplayProfile/MappingPolicy, no behaviour change).
- Rejected by the owner: neural/ONNX upscaler, denoiser and motion estimators (invented detail, poor value); detail accumulation as grain averaging (grain rule: never average grain away). Only detail-pulse stabilisation survives (docs/audit/detail-retrieval-research-2026-10-17.md).
- New priorities: render-ahead (budget per source frame, docs/VP-render-ahead-design.md) is top; the RTX 3050 Ti laptop is the reference mid-range GPU (4K30 goal); full-precision optimisation before any half-precision path; features designed to be default-on when faithful and the 3090 runs them; interpolation off for the owner (kept for others); hiccup repair separate and wanted.
- New work: intent interpreter (DV2 analogs, Experimental content-adaptive settings, docs/VP-intent-interpreter-design.md); report-a-problem hotkey; per-component Claude handover files at the end of the design period; pruning of superseded options (fallbacks for low latency and non-optical-flow paths kept).
- libplacebo: public maintained fork vmcpherson2012-dev/libplacebo (vp-fl35, CI build), rebasing onto upstream over time; output_fbo_float32 is fork-only and must be carried forward. Zero-copy upload via a libplacebo patch is now realistic.
- DV: tunnel delivers bit-exact 12-bit 4:2:2 through any card that passes 8-bit RGB 4:4:4 untouched; 60 Hz DV needs 2160p60 8-bit RGB capture (Quad HDMI Recorder likely 30p only).
- Questions answered: Q3 (ML) no; Q5 (non-certified trims) yes; Q6 interpolation off for the owner, hiccup repair separate; Q1 closed for the FL35 (revisit with a new display). Open: Q4 colorimeter, Q7 pulldown criteria, Q8 card/display budget.
