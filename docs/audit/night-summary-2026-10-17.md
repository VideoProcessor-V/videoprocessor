# Overnight summary (2026-10-16 evening -> 2026-10-17 morning)

Branch `merge-v1.4.02`, pushed to the fork after each batch. Nothing deployed (deploy only when asked).
Agents ran on Opus 5.5; GPU tests run by me (or one agent with exclusive GPU), CPU within 16 threads.

## Committed tonight (newest last)

| Commit | What |
|---|---|
| 4e2ae402 | Scaling slider removed (superseded by the shader add-ons list) |
| 10a65b56 | Config + runtime audit fixes (editor buttons/inheritance, low_latency_path not inherited, live apply, case) |
| 542a03ee | Config: VP settings in signal order |
| f25d4159 | Whole-build bug hunt fixes (producer, precision, capture, crash state, LED, editor intent) |
| 317fcac2 | Pulldown D1/D3/D4 fixes + reliability test, mixed-screen guard, precision plumbing, BTB/WTW on by default |
| c4b49782 | Config app fixes, rule expressions, shader fixes, packaging (-NoUpdater), fbo_float32 Auto on everywhere |
| 9662ccc4 | BTB/WTW in all cases (extensions on), SDR tone shape keeps WTW, LED second pass, capture readiness, analysis luma range, mixed screens (one-row UI, dense grid), regression fixes |
| 392cc2ec | DV tunnel: robustness, faithful unpack across timings and R10 packings, L1 min, L2/L8 parsing |
| cb682f09 | DV tunnel GPU unpack (switch `dv_tunnel_gpu_unpack`, off): CPU 2.6 ms -> 0.007 ms at 4K, GPU pass 0.41 ms, bit-exact on hardware; fixes DV showing black with native RGB ingest on |
| 37b68faa | GUI: crash state kept on fatal errors, no key auto-repeat, Enter no longer restarts the renderer, display recovery never blocks startup |
| (merge) | 50/60p interpolation: Stage A + Stage B (any ratio, e.g. 60p on 50 Hz) + gap fill + one vectors-valid state; `interpolation` OFF by default; reviewed + fixed; GPU 24/24 |
| c0ae8edf | Interpolator second-review fixes (OFF path byte-identical) + DV trims review fixes (no near-black specks, continuous at black, corrupt codes rejected) |
| (merge) | DV next: L1 scene minimum on the DV card / JSON (plugin API 32: deploy EXE + DLL together); source resolution + re-base on DV; L2 trims (non-certified, `dv_trims`, off) |

Test status at c0ae8edf: CPU 1100/1100, Config 122/0/1, GPU smoke 80/80 incl. interpolation and DV. Earlier, at 392cc2ec: CPU ~890 targeted tests pass; Config 121 pass / 0 fail / 1 skip (needs a real
window); GPU smoke 57/57 (pixel identity, stream faults, re-base chain, sigma-delta, LED compensation,
extended range, SDR tone shape, bars, look-ahead, low latency).

## Merged branches

Both feature branches (worktrees `C:/dev/vp-5060` and `C:/dev/vp-dv`) are merged into merge-v1.4.02;
the worktrees can be removed.

## Docs written

- `docs/VP-fidelity-roadmap-2026.md` - five-year roadmap, 90-day sequence, owner questions.
- `docs/audit/dv-extension-ranking-2026-10-16.md` - DV: what exists, 12 extension projects.
- `docs/audit/capture-card-readiness-2026-10-16.md` - adding a card, bugs, first-day checklist.
- `docs/audit/bug-hunt-2026-10-16.md`, `docs/audit/pulldown-review-2026-10-16.md`,
  `docs/VP-interpolator-50-60-plan.md` (Built + Stage B sections).

## Decisions taken in your absence (please confirm or reverse)

1. `extended_range_more_cases` ON (BTB/WTW for BT.1886/sRGB, wide-gamut SDR, calibration LUT, HDR
   passthrough) - 40/40 GPU tests; in-range within 1 code of off.
2. fbo_float32 Auto = 32-bit float on the low-latency path too (your rule: higher precision default).
3. `source_rate` in rules now rounds (59.94 -> 60, 23.976 -> 24) and rules use three-valued logic for
   unknown variables - **check your own deployed rules** that compare `${source_rate}` (e.g. `== 23`,
   `<= 59`); `${cadence}` (exact) is unchanged and is what the shipped config uses.
4. SDR tone shape off while a calibration LUT is attached (it acts after the LUT).
5. LED: LDMM 0 at every start; `led_idle_restore_s` 10 s (full light after no video; 0 = off).
6. Pulldown fixes on (switch `pulldown_fixes`); pulldown detection itself still off by default.
7. Mixed-screen guard on (menus with previews, overlays, PiP never re-based).

## Questions for you

- Roadmap questions 1-8 (12-bit output route, deploy before/after 50/60p, ML upscaler, colorimeter,
  non-certified DV trims, interpolator defaults, pulldown default-on criteria, hardware budget).
- Interpolator: gap fill max 3 missed frames? Keep Stage A as the warm-up before Stage B?
- Display link: FL35 is 8-bit (noted); capture card: none chosen yet, cost-effective (noted).

## Still open

- LED: stopping the old module under the state lock on an Apply that changes the connection (needs
  reordering start/stop; untested without hardware).
- Pulldown D2 (queue target at the film rate while locked) - needs a renderer->GUI channel.
- DV: L2/L8 trims not applied yet (parse first, verify on a live title); L1-driven FL35 tone curve;
  re-base on DV; tunnel on more devices.
- Capture: automatic restart after errors; native limited-range ARGB route; Magewell Eco/USB.
- 3090 timings for everything new (Stage B re-renders the chain per refresh).
