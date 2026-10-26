# Planner notes, pack 3 (2026-10-26): A HDR10+ / DTM · B Argyll no-tile · C test suites

(From the planning agent via the owner, 2026-10-26. Condensed faithfully; tables and constants kept.)

## A. HDR10+ and VP dynamic tone mapping

**Capture reality.** HDR10+ (ST 2094-40) dynamic metadata reaches an HDMI sink in vendor-specific /
extended metadata packets, not the static HDR InfoFrame. The DeckLink SDK exposes static HDR metadata
per frame and, as far as known, no route to arbitrary InfoFrames/EMPs. Check the newest Desktop Video
SDK release notes once; if nothing, treat HDR10+ capture as unavailable on this card. Side channel worth
a 10-minute look: the HDFury Arcana reports incoming metadata on its own interface — if its serial/IP
API exposes the HDR10+ packet, VP could read it beside the video (time alignment is the catch).
Otherwise build the parser now against fixtures and ship VP DTM as the working feature.

**SceneDescriptor additions** (format-neutral; one per scene, held until the next cut):

| Field | Units | From ST 2094-40 | From VP measurement |
|---|---|---|---|
| source | enum | Hdr10Plus | Measured (also Static, DvL1) |
| scene_id, valid_from_frame | — | metadata change | cut generation |
| max_rgb (R,G,B) | PQ/nits | maxscl[3] | per-channel 99.99th pct |
| avg_maxrgb | PQ | average_maxrgb | mean of max(R,G,B) |
| maxrgb_percentiles | PQ at 1/5/10/25/50/75/90/95/99% | distribution_maxrgb | histogram of the analysis store |
| bright_fraction | 0–1 | fraction_bright_pixels | share above the 99th pct |
| min | PQ | — (DV L1 min) | 0.1st pct |
| target_display_peak | nits | targeted_system_display_max_luminance | — |
| curve_hint | knee x/y + Bézier anchors | tone mapping params (per target) | — |
| saturation_weight | 0–1 | color_saturation_weight | — |
| confidence | 0–1 | 1 | sample count / stability |

Consumers (tone curve, LED look-ahead, Stage 12, status line) read only the descriptor.

**Curve (Dolby-style).** Per scene in PQ, on intensity (ICtCp I or max-RGB) with hue-preserving
saturation, as a custom pl_tone_map_function (libplacebo keeps gamut mapping):
- anchors min → display black, avg_maxrgb → avg_target, max_rgb → display_peak × (1 − headroom);
  monotone cubic Hermite in PQ, slope 1 at black, soft shoulder above knee_start;
- avg_target keeps mid-tones where the static curve would (avg_preserve blends keep vs rescale);
- curve_hint near the FL35 peak: interpolate hint vs ours by target distance; else ours.
- Constants: headroom 0.05, knee_start 0.6, avg_preserve 0.8, max_slope 1.6, min_slope 0.25,
  sat_weight_default 1.0.

**Adaptation (scene drift).**
- Per frame: P* = p99.9(maxrgb), A* = avg_maxrgb (frame analysis store).
- Look-ahead over the ~5 queued frames: P*_ahead = max(P*).
- Asymmetric smoothing in PQ: rising τ_up = 0.15 s (immediate when the look-ahead sees it); falling
  τ_down = 3 s; dead band ±0.01 PQ; falling rate limit 0.02 PQ/frame; mid-grey output change < 0.5%/frame.
- Cut: reset to the new scene's stats from the queued frames.
- Shared with LED Dynamic Drive: same descriptor, cut signal, look-ahead window. DTM runs before the
  PRE_OUTPUT LED measurement; DTM's falling rate never faster than the LED's scene rate law.

**Either/or matrix (exactly one dynamic source, shown on the status line):**

| Mode | Metadata | libplacebo peak detection | Notes |
|---|---|---|---|
| Original (Bill) | static HDR10 | as Bill ships | byte-identical to Bill |
| Static (VP default today) | static HDR10 | off | current look |
| VP DTM | Measured | forced off | fallback feature |
| HDR10+ | Hdr10Plus | forced off | falls back to VP DTM if metadata disappears (held to the cut) |
| DV | DvL1 | forced off | existing DV path |
| libplacebo PD | none | on | comparison only |

Precedence: DV → HDR10+ → VP DTM → static.

**Tests.** Parser fixtures (hand-built SEI/EMP payloads incl. edge values, malformed → rejected visibly);
curve (monotone, anchors within 0.001 PQ, slope limits, identity when stats equal the display range,
hint interpolation); drift clips (static, flash, slow sunrise ≤0.5%/frame mid-grey, dark↔bright cuts,
lightning, LED compensate on: mid-tone light constant across a dim); either/or (one source per mode,
PD forced off, Original byte-identical).

**Cost.** CPU µs per frame; LUT rebuild only when the curve moves (≈0.05 ms CPU); GPU = today's tone
mapping. Effort: descriptor + parser + fixtures ~1.5 days; curve + adaptation + matrix + tests ~3–4 days
(human-developer days).

## B. Argyll "no white tile" patch (ES-1000) and i1Display Pro

**Revision first.** The owner identified his ES-1000 as the original i1Pro, UV-cut (not i1Pro 2);
`spotread -?` lists it. Only the i1Pro 2 adds a tile-based wavelength calibration.

**Where the check sits:** the instrument driver (spectro/i1pro.c, spectro/i1pro_imp.c), which decides the
needed calibrations per mode and the "place on white reference" condition; spotread/dispcal/dispread only
relay prompts. Find the spots by the calibration-condition strings and the per-mode "calibration needed"
logic.

**Minimal patch:** env var ARGYLL_I1PRO_NOTILE=1 — emissive modes only: skip the reflective white
calibration and its checks; keep the dark calibration (aperture covered with black flocking/cloth);
reflective modes unchanged; on an i1Pro 2 with tile-based wavelength calibration in emissive mode: stop
and report. Small diff in its own folder; upstream candidate ("emissive-only, no tile").

**AGPL boundary:** out of process only — VP shows the patch through its own pipeline, runs the Argyll tool
as a child process and parses text output; no linking or shared memory. spotread one-shot for single
readings; a script for sequences. Argyll installed separately; VP ships no Argyll binaries. Private use of
a patched build carries no obligation; distribute the diff with any patched binary.

**Validation on the FL35:** (1) dark: covered ≈0, black field small and stable; (2) repeatability: ten white
readings σ(Y) < 0.5%, σ(xy) < 0.0005; (3) cross-check: CCSS for the i1Display Pro from the ES-1000 spectra of
this projector (ccxxmake); white within ≈0.003 xy and ≈2% Y, primaries within ≈0.005; (4) near-black 1–5% ramp
— i1Display the quieter (ES-1000 for spectral truth on bright patches, i1Display for darks and speed);
(5) sanity on a known display. Risks: light leak in dark cal, i1Pro 2 wavelength step skipped (stop rule),
instrument age, re-applying the diff on Argyll updates, narrow-band primaries without a CCSS.

## C. Test strategy — suites

| Suite | What | Where | Duration | Pass | Tier |
|---|---|---|---|---|---|
| 1 Determinism run-twice | frozen r6 fixtures twice per slot config, drain on, byte compare | WARP subset, laptop RTX full matrix, 3090 | WARP ~10 min; RTX ~1.5 h; 3090 ~1 h | byte-identical with drain on; drain off reported, not gated | WARP per merge; RTX nightly; 3090 WP-20 |
| 2 Transitions | 24↔60, 50↔60, SDR↔HDR, HDR10 metadata change, DV↔LLDV, resolution change, profile switch, signal loss/return, device reset | WARP synthetic, RTX full, bench rig live, 3090 | WARP ~15 min; RTX ~1 h; rig ~30 min | no crash; first frame after a transition = fresh start within 1 code; no inherited state; status + fixed delay restored within N frames | WARP per merge; RTX nightly; rig + 3090 WP-20 |
| 3 Soak | looped mixed playlist, nets + de-judder on; per-minute VRAM, pools, queue, presents, memory, handles | laptop RTX, 3090 | 4 h nightly; 8 h overnight | VRAM/pool slope < 1 MB/h, no handle growth, queue ±1, no unexpected drops | RTX nightly; 3090 WP-20 + driver updates |
| 4 Net bypass coverage | A, D, B-refine × each bypass reason | WARP stubs, RTX real nets | ~5 / ~20 min | output = net-off reference; reason in status | WARP per merge; RTX nightly |
| 5 Original = Bill | Bill's tagged build vs VP-V Original, headless | WARP subset, RTX full | ~10 / ~40 min | byte-identical except registered-fix allow-list | WARP per merge; RTX nightly; full WP-20 + after WP-21 |
| 6a Structural settings fingerprint | every key: default, validator, Config control, doc line, fingerprint | CPU | seconds | 100% coverage | per merge |
| 6b CPU defaults fingerprint | hash of every default per profile vs committed golden | CPU | seconds | equal or golden updated in the same commit | per merge |
| 7 Per-slot timing budget | fullscreen per rate and tier; GPU timestamps per slot; DXGI stats | laptop RTX fullscreen, 3090 | ~10 min/rate | p95 within tier budget; no unexpected drops over 10 min | laptop per-merge warn >10%, nightly gate; 3090 WP-20 + driver updates |

Notes: the bench rig (Mini Recorder 4K) tops out at 2160p30 / 1080p60 — 4K 24↔60 live only on the cinema
PC's Quad. WARP is the deterministic reference. Per-merge chain gate target ≤45 min: 6a, 6b, 1 (WARP
subset), 2 (WARP), 4 (WARP), 5 (WARP subset), the existing chain matrix, 7 as warning. Nightly (laptop
≤6 h): 1 full, 2 full, 3 (4 h), 4 real nets, 5 full, 7 gate. WP-20: everything on the 3090, live
transitions, 8 h soak, 5 full, 7 on the 3090; repeated after WP-21 and each NVIDIA driver update.
