# VP-V test plan: synthetic → loop → shadow (2026-10-26)

Owner order: close every gap that can be closed synthetically first, then the bench-rig loop, then the
shadow deploy, then switch over. Builds on the planner's suite tiers (planner-notes-pack3.md, part C)
and today's baseline (CPU 4283/4284, Config passed, GPU 19/19, WARP-capable graphics on the RTX 219/219,
r6 byte identity passed twice, chain gate explained). Principles that every phase checks:
graceful visible fallback (no silent fallbacks), the frame queue as one continuum (0 = low latency,
1-2 = Sports preset, 5 = film), fewer pathways, Original pathway = Bill's output, slot off = byte-identical,
no hard-coded rates/resolutions (8K basis), full precision by default.

## Phase 1 — synthetic (laptop, off screen and WARP; no capture hardware)

Runs per merge (≤45 min tier) or nightly (≤6 h tier) as marked. Inputs: the 4K masters and their
encoded deliveries (D:\testclips), the DV synthetic corpus, synthetic frames, recorded raw captures.

| # | Suite | Method | Pass | Tier |
|---|---|---|---|---|
| 1.1 | Determinism run-twice | every slot configuration rendered twice, entrance_gpu_drain on; WARP subset + RTX full | byte-identical (drain off: report only) | WARP per merge; RTX nightly |
| 1.2 | Original = Bill / slot off | r6 + Bill-tagged builds vs VP-V Original and each slot off | byte-identical, or a registered-fix allow-list entry | per merge (subset); nightly (full) |
| 1.3 | Settings fingerprints | structural (every key: default, validator, Config control, doc, fingerprint) + CPU defaults golden | 100% / golden equal | per merge |
| 1.4 | Mid-stream input changes | scripted replay: 24↔60, 23.976↔24, 50↔60, 59.94↔60, SDR↔HDR, HDR10 metadata change, DV↔LLDV (recordings), resolution change, rule-driven profile switch, signal loss/return, HDCP-style blackout (frames stop/black) | no crash; first frame after a change = fresh-start render within 1 code; no inherited state; status + fixed delay restored within N frames; fallbacks visible while recovering | WARP per merge; RTX nightly |
| 1.5 | Queue continuum | every look-ahead feature (drift correction at cuts, interpolated hiccup repair, DTM/HDR10+ look-ahead, LED look-ahead, motion estimate-ahead, B3 entrance ahead, render-ahead) at queue depth 0,1,2,3,5 | each feature runs at its declared depth and steps down gracefully and visibly below it; queue depth 0 really holds 0 frames and adds no delay | nightly |
| 1.6 | Clock drift | synthetic 59.94-vs-60 and 60-vs-59.94 over 30+ min; queued modes defer the slip to a cut (bounded wait), low latency slips when the clocks cross | one slip per ~16.7 s at 0.1%; queued: at cuts or least-visible moment, interpolated hiccup repair; low latency: plain repeat (prediction option off); every slip in diagnostics | nightly |
| 1.7 | Low latency judder | timestamp-scheduled presentation at 60p/59.94p on 60/59.94 Hz | presented-time error within ±1 refresh of source timing; 1:1 cadence; latency ≤ 1 refresh | nightly |
| 1.8 | Live changes under load | Config edits and profile switches while playing; tier slider preview + Apply | no stutter beyond the documented re-init; values applied exactly; Apply-only writes | nightly |
| 1.9 | System events (simulated) | forced device loss/driver reset, VP restart restoring state, display reconnect (simulated adapter change) | recovers to the same state; visible notice; no leaks | nightly |
| 1.10 | Net bypass coverage | A, D, B-refine × every bypass reason (stub nets on WARP, real nets on RTX) | output = net-off reference; reason shown | WARP per merge; RTX nightly |
| 1.11 | Content variety | clips with burned-in subtitles, changing letterbox mid-film, credits scrolls, logos, heavy grain, low bitrate, 50 Hz sports | detection/placement correct; grain kept (RMS ±5%); no false re-base | nightly |
| 1.12 | HDR10+ / DTM | synthetic ST 2094-40 fixtures (incl. malformed); drift clips (flash, sunrise, cuts, lightning); LED compensate on | descriptor exact; curve monotone, anchors within 0.001 PQ; mid-grey change ≤0.5%/frame; one dynamic source per mode | per merge (parser) / nightly |
| 1.13 | Chroma study | 4K masters → 4:2:0 → simulated player upsampling (kernels, 4:2:2/RGB, siting); undo+redo vs keep+fill vs mix | identification accuracy; quality vs true chroma; cost per branch | one-off study, then regression |
| 1.14 | Upscale chain | VpScaleRender rebuilt from integ; 1080p/1440p/720p; 8b route vs direct fallback; one-axis + anamorphic/A-lens/NLS geometry | aspect exact; the 8b/B-refine route ≥ direct; per-axis cases correct | nightly |
| 1.15 | Soak (laptop) | 4 h looped playlist, nets + de-judder on | VRAM/pool slope < 1 MB/h; no handle growth; queue within target ±1; no unexpected drops | nightly |
| 1.16 | Per-slot timing | GPU timestamps per slot, laptop fullscreen session | p95 within tier budget; warn on >10% regression | per merge warn; nightly gate |
| 1.17 | Install / overlay | clean-machine install (VM); overlay install onto Bill's version, uninstall restores Bill's files by hash; config migration from Bill's settings; Bill's build loads our settings file after uninstall | exact restore; no lost user settings | before the loop |
| 1.18 | Diagnostics | report-a-problem dump and crash-state ring exercised (forced fault) | the dump contains what a diagnosis needs | before the loop |

## Phase 2 — the bench-rig loop (laptop + DeckLink Mini Recorder 4K in the Ugreen enclosure; Apple TV → Integral 2 (custom EDID) → capture, direct; the EZCOO only to watch at the same time or for the shadow deploy; laptop output via USB-C → HDMI 2.0). The same suite reruns on the HTPC + Quad for 4K50/60.

Limits: the Mini Recorder captures up to 2160p30 / 1080p60; laptop ports are Intel-driven (no NVIDIA DV
output mode). 4K50/60 and DV output go to the HTPC step.

| # | Test | Pass |
|---|---|---|
| 2.1 | Bring-up: each Apple TV mode (SDR, HDR10, LLDV, DV tunnel via the custom EDID, 4:2:2 12-bit, RGB, 23.976/24/25/29.97/30, 1080p50/60) captured and rendered | correct format detection, levels, colour; status line right; no fallbacks except stated limits |
| 2.2 | Capture frame path: VP's GPU raw upload + early upload vs Bill's CPU conversion (render upload) A/B | identical pictures; early upload saves time; no extra short-queue fallbacks (after the readiness fix) |
| 2.3 | Live mid-stream changes: switch content on the Apple TV (frame-rate match on/off, SDR↔HDR, DV↔LLDV), unplug/replug, input switch on the EZCOO | same criteria as 1.4, on a real signal |
| 2.4 | Queue continuum live: film (5), Sports preset (1-2), low latency (0) | features step down visibly; low latency adds no frames |
| 2.5 | Glass-to-glass latency per mode (phone Slo-Mo + on-screen marker) | equals the stated fixed delay per profile (lip sync) |
| 2.6 | Chroma study live leg: grab bursts per Apple TV mode | Apple TV chroma kernel/siting identified (or not) per mode |
| 2.7 | HDR10+ source check: EDID with HDR10+ (verify in the HDfury editor first) → does the Apple TV send HDR10+? DeckLink frame-flag/metadata log; Diva in parallel via the EZCOO: does its API return the raw HDR10+ packet live? | documented yes/no per device; if the Diva works, metadata path wired |
| 2.8 | Real content session: an evening of normal viewing through the loop (subtitles, letterbox changes, sports, grainy film) with logging | no unexplained fallbacks or stutters; issues filed with dumps |
| 2.9 | Soak live: overnight loop on a looping source | as 1.15, plus capture/display clock drift behaviour (1.6) on real clocks |
| 2.10 | Desktop interference: notifications, focus changes, Windows HDR toggle, another app using the GPU | recovers; visible notices |

### Phase 2 order and content (owner 2026-10-27)
1. Output loop first: laptop HDMI out → Mini Recorder (Ugreen), patterns AND real video (masters, deliveries,
   sports clips) captured back and compared; 1080p60 and 4K30.
2. On-the-fly toggles during real playback in every loop: slot on/off, presets (queue depth incl. Sports/
   Competitive), motion engine / camera model, deband, scalers/blend, dither, LED log-only, profile rules —
   each: no crash or stutter beyond the documented re-init, the effect visible in the capture, a clean return
   when switched off (byte-identical to never-on where that rule applies), status shown.
3. HDMI resyncs during real playback: output mode/refresh changes from the laptop (resolution, 59.94↔60,
   24/30/60, SDR↔HDR, 4:2:2↔RGB / bit depth where the adapter allows), cable unplug/replug, capture re-lock,
   EDID re-read — each checked against the unified resync event (FULL stamp, clock re-latch, all resets, no
   inherited state, recovery time within budget, visible reason).
4. Re-base changes during real playback: spliced clips whose source resolution or player kernel changes
   (1080↔4K↔720, H-only and V-only upscales, anamorphic, native↔upscaled, letterbox changes altering the active
   area) — checked against the per-axis re-base stamp: only the changed axis re-latches and crossfades, per-axis
   caches reset, no false re-base on native, motion reset only on geometry change, A-lens/NLS/aspect correct.
   Crossfades must be minimally noticeable: per-frame change of detail (high-band energy) and mean luminance
   through the crossfade within the surrounding content's natural frame-to-frame variation (no pop, no visible
   step), no ghosting/double image, bounded duration; switch aligned to a nearby scene cut when the queue sees
   one; a few judged by the owner on the FL35 during the shadow phase.
   Test reels: segments long enough for each latch to settle and be observed (detection + latch time, crossfade,
   motion/camera-model warm-up, cadence lock, DTM adaptation) — ≥ 30-60 s per segment; minutes-long segments and
   30+ min continuous runs for drift and soak; reels spliced from the masters/deliveries (short sources looped
   seamlessly) with documented segment boundaries.
   Streaming quality drops (ABR): reels simulating adaptive-bitrate ladders (4K→1080→720→back, bitrate-only
   drops, short blips vs sustained drops, switches at segment boundaries not scene cuts): re-base re-latches
   correctly with hysteresis (no thrash on quick up/down); the stream-quality-drop notice appears once after a
   sustained drop (transient message + Ctrl+I line + log, suggesting a stream restart) and clears on recovery;
   no notice for content that is genuinely low-res from the start.
5. Then the Apple TV → Integral 2 → capture input tests (2.1-2.10).

## Phase 2b — HTPC overnight test pack (optional, before shadow; RTX 3090 + DeckLink Quad; nothing installed, madVR untouched)

Self-contained zip (VP build + test DLL + portable vstest + scripts + small fixtures), one
`run-overnight.cmd`, results zip for the morning. Runs: 3090 timings (nets, debander, texture restore,
every slot — confirmation only), the GPU suites, live loop via a spare 3090 output → EZCOO → a spare Quad
input (4K50/60, 12-bit 4:2:2, DV tunnel, live 24↔60), 8 h soak, the timing gate fullscreen with DXGI
statistics. Scheduled around TrainKit runs.

## Phase 3 — shadow deploy (cinema; madVR keeps playing)

The EZCOO splits the Apple TV signal: one copy to the normal madVR chain (what the owner watches), one to
VP on the HTPC rendering to a spare output or off screen with logging. Nothing changes for viewing.

| # | Test | Pass |
|---|---|---|
| 3.1 | Real titles, real hours: VP runs alongside every viewing session for a week or two | no crashes; fallbacks explained; logs + periodic output snapshots reviewed daily |
| 3.2 | Side-by-side checks on chosen scenes (reference set: pan, dark interior with hair, daylight skin, fast cut, grainy sky, Crave stream, DV title) — VP output on a second display or captured snapshots vs madVR | owner judges; issues filed |
| 3.3 | Projector control dry run: LED/CNTR commands logged but not sent (or sent with the owner present), then live | timing and values as designed; CNTR never above need; full drive when measurement is off |
| 3.4 | Delay: VP's stated delay vs the AVR lip-sync setting | in sync |
| 3.5 | Rollback rehearsal: overlay uninstall restores Bill's version / madVR in one step | exact restore |

Then switch over (M1, after the Bill merge), keeping madVR as the fallback until a season of trouble-free
use (owner's call).

## Order and tooling
1. Phase 1 suites first, in this order: 1.4 (transitions), 1.5 (queue continuum), 1.6/1.7 (drift and low
   latency), 1.1-1.3 (determinism, Bill identity, fingerprints), then the rest.
2. The chain gate runs every phase-1 per-merge tier after each merge group; nightly tier unattended.
3. Phase 2 when the rig is built; phase 2b when convenient; phase 3 after phase 2 passes.
4. Every failure: diagnosis note in skeleton-notes, fix with an audit, the test kept as a regression.

## Planner adjustments (pack 4, adopted)
Order: 1.3 and 1.1 first, then 1.4-1.7; stabilise the EntranceOnce fallback flake before 1.5.
Criteria: 1.6 bounded wait ≤10 s else the lowest-global-motion frame, run on a virtual clock; 1.7 every
source frame presented exactly once except scheduled slips, presented-time error ≤ ½ refresh (needs DXGI
stats → HTPC); 1.8 re-init budget per change type; 1.9 simulated teardown vs one real driver reset on the
3090; 1.14 not worse than direct by >0.05 dB PSNR, no added overshoot, plus a perceptual score; 1.15 slope
by regression over hours 1-4 with CI < 1 MB/h; 2.5 within ±1 frame of the stated delay; 3.1 every
fallback event carries a reason code.
Added: thermal hot-run variant of 1.16 (laptop + 3090); sleep/resume and display off in 1.9; FL35 RS-232
emulator on a USB-serial loopback before phase 3; blind A/B (randomised toggle, hidden label logged);
grain/bypass checks on real phase-2 captures; Bill's installer/updater over an overlaid VP-V in 1.17.
Shadow phase: watch madVR's dropped/repeated counters (VP-V shares the 3090); lighten or stop VP-V if they rise.

## Resync / discontinuity rule (owner 2026-10-26)
Every resync is one discontinuity event that all stateful parts honour. FULL (format, rate, resolution,
HDR/DV mode, signal loss/return, device reset): clock re-latched to the new source; queue rebuilt; B3 ring,
repeat-reuse cache and analysis history cleared; motion history and vectors invalid until a fresh pair;
cadence/pulldown detector reset; DTM/LED look-ahead reset as at a cut; render-ahead re-anchored. MINOR (one
drift slip): vectors across the slipped pair invalid or time-corrected; no clock re-latch; queue kept.
1.4 and 1.6 assert it: no inherited state after any event, no vector used across a discontinuity, the reset
and its reason visible in the status.
