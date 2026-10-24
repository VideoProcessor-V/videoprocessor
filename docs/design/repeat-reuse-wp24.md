# WP-24: captured-repeat render reuse (DRAFT)

2026-10-24. Branch `feat/repeat-reuse` from d0212534 (worktree `D:\codex\videoprocessor\repeat-reuse`).
Not integrated: the branch adds new files only, and every edit to an existing file is in
`repeat-reuse-callsites.patch` (`git apply --check --cached` on d0212534).

Sources: integration-batch-plan.md WP-24 and efficiency audit #4.

## 1. What it saves

A captured repeat is the same film frame sent again inside the signal: 1 refresh in 5 at 24-in-30,
1 in 2 at 30-in-60. Today each one re-runs the whole libplacebo render:
- scaler and tone/gamut;
- adaptive downscale;
- every PRE_OUTPUT hook.

The audit estimates ~2-6 ms per repeat refresh on the laptop at 4K, uncertain. That is 20% of
renders at 24-in-30 and 50% at 30-in-60.

When nothing upstream changed, the picture at the end of PRE_OUTPUT is the same as on the source
frame's first present. The repeat can draw that cached picture through the OUTPUT stage only:
output encoding, the OUTPUT hooks (sigma-delta, occlusion mask, encoding repair), libplacebo's
dither and the target overlays. The dither keeps advancing per refresh because there is still
exactly one `pl_render_image` per refresh.

## 2. Pieces

| File | Class | What |
|---|---|---|
| `src/VideoProcessor-Lib/vprenderer/RepeatRenderReuse.h` | **UPSTREAMABLE** (std only) | `RepeatFrameKey`; `Signature` (FNV-1a, exact on floats); `RepeatReuseRules::Compare` (ordered refusals); `Stats` + status line; `IRepeatReuse<Frame>` hook interface; `NeverReuse` (upstream default = today's behaviour) |
| `src/VP-V/modules/output/presentation/RepeatReusePolicy.h` | VP-only, pure | the switch and Original pathway, the arming window, VP temporal stages, the hook-state signature with opaque refusal, counters |
| `src/VP-V/modules/output/presentation/RepeatReuseAdapter.h` | VP-only, libplacebo | hook classification (mpv hooks recognised by libplacebo's own mpv hook function); `DrawFromCache` (the extras' 6d path with zero warp) |
| `src/VP-V/tests/RepeatReusePolicyTests.cpp` | CPU tests | 12 tests |
| `src/VP-V/tests/gpu/RepeatReuseGpuTests.inc` | GPU tests, written, not run | byte-identity design R1-R3 |
| `docs/design/repeat-reuse-callsites.patch` | host patch | `LibplaceboVideoRenderer.cpp`, `RenderedWarp.h` (`Cache::Armed`), test vcxproj |

## 3. The ONE hook point in Bill's loop

The hook is the **first statement of the draw callback** that `RenderLocked` passes to
`NlsHookRecovery::RenderOwned`. `NlsHookRecovery.h` is Bill's file; it is not edited. In his
v1.4.02 tree that callback ends in `temporalRenderer.Render(...)`; in ours the entrance runs first.
The hook:

```cpp
if (repeatDecision.reuse) {
    if (repeatReuseAdapter.DrawFromCache(...destination...)) return true;   // OUTPUT stage only
    repeatDrawFailed = true; repeatReuse.NoteDrawFailed();                  // visible; full render below
}
```

- The upstream PR would contain only `RepeatRenderReuse.h`, these lines (with a
  `std::unique_ptr<IRepeatReuse<pl_frame>>`, defaulting to `NeverReuse`), and the key fill.
- Our policy, the adapter and the 6d cache plug in from the fork.
- Because the callback is inside `RenderOwned`, a reuse draw keeps its contract. The draw result
  is checked the same way. No NLS retry is needed, because the NLS hook (not OUTPUT stage) is not
  in the reuse draw.

VP's own code has two more touches:
- the key and decision just before the draw (the host knows the state);
- arming the cache for a first present while repeats are being seen, next to the de-judder's own
  `dejudderCache.Arm`.

## 4. What must still run in Bill's loop on a repeat

The reuse replaces the draw only. Everything before the callback runs on every refresh, as today:

- **Queue, dequeue, presentation timing, render-ahead pacing.** Repeats are real refreshes. Delay
  totals are unchanged, and no queue decision depends on the reuse.
- **Crop and subtitle PRESENTATION** (`RenderLocked` 21c: viewport/crop decision, caption holds,
  subtitle relocation/cut-paste, startup crop hold). The analysis already skips repeats
  (`!cadenceRepeat` guards). The presentation state machines advance per refresh: holds expire,
  viewport animations step, captions appear. Their result feeds the key (`geometry`, `overlays`
  including the relocation rects), so a change on a repeat refuses the reuse and renders in full.
- **Overlay assembly** (`assembleOverlays`, OSD and subtitle overlays on the target). Overlays are
  drawn again in the reuse draw (they are on `destination`); a changed overlay refuses by key.
- **Hook assembly** (every hook, every parameter value set). The key's `hookState` is computed from
  the assembled list, so a hook whose parameters change per refresh refuses the reuse. Examples:
  LED compensation ramping, intent/ambient easing, tone shape.
- **LED measurement.** On a reused repeat the measurement hook does not run: it is picture-neutral
  and excluded, exactly as on the de-judder extras today. The LED drive therefore sees no new
  measurement for that refresh, and the previous result stands. The repeat's picture is identical,
  so a re-measure would only repeat the same numbers. The audit's "feed the cached result on
  `cadenceRepeat`" is this, by construction. The LED compensation **output** is still per refresh:
  a ramp changes its parameters, the key refuses, and the repeat renders in full.
- **Telemetry and diagnostics** (frame records with `cadenceRepeat`, presentation evidence,
  post-stall advisor) and **Present**: unchanged. GPU stage timers (`gpuFrameTimer`) measure the
  smaller draw; that is the point.
- **The sigma-delta** keeps `static_refresh = 1` on repeats, set during hook assembly, and runs in
  the OUTPUT stage of the reuse draw.

## 5. Decision and refusals (visible)

Refusals, in order:
1. not a repeat;
2. other source frame;
3. no cached picture;
4. stream changed;
5. temporal per-render state: VP phase pictures or interpolation, de-judder extras, a
   source-resolution crossfade, the startup black hold, calibration, upstream deinterlacing,
   smooth-motion mixing, peak detection;
6. settings, geometry, colour, overlays, hook state or extension changed;
7. reuse draw failed.

- **Status (Ctrl+I, on the de-judder/output line):** "Repeats reused: N of M (last refusal: …)".
  A failed reuse draw is rendered in full on the same refresh and counted; nothing is log-only.
- **Opaque hooks.** A pre-OUTPUT C hook the policy cannot see into refuses the reuse; that is the
  safe default. The draft registers these:
  - picture-neutral: LED measurement, the cache copy, the occlusion mask (first present only);
  - static: calibration LUT stage, output encoding repair.
- **Follow-ups so the reuse also works on the higher tiers:**
  - AdaptiveDownscale (grain-map smoothing over time) needs a state generation;
  - Extended Range hooks need a state generation, or a "static per source frame" registration.
- **Arming:** the cache copy costs one output-size copy per source frame (~0.2 ms on the 3090,
  ~1-2 ms on the laptop at 4K). It is armed only while captured repeats are being seen: one within
  the last second, a time window with no fixed cadence list. So 24p on a 24 Hz output pays nothing.
- **Setting:** `repeat_render_reuse` on|off, draft default **off** until the R1/R2 gates pass; then
  the owner's default-on rule applies. The Original pathway is always off (upstream behaviour). Not
  in this draft: the settings fingerprint, the live-apply "changed" list, the Config page entry
  (slots.json: `output.presentation`), and `Policy::Reset()` on resync (the key's frame generation
  covers it).

## 6. Byte-identity design (the GPU test file has the detail)

A reused repeat must equal a full re-render of the same refresh, 0 differing bytes at the output
bit depth. Both paths call `pl_render_image` once per refresh, so libplacebo's temporal dither and
VP's sigma-delta advance identically; the temporal-dither phase is the only allowed difference.

- **R1** (unit, one renderer): cases SDR, HDR→SDR (peak detection off), target LUT, fractional
  target crop, overlay, sigma-delta, fp16 FBO, plus BTB/WTW probes.
- **R2** (capture harness): 24-in-30 and 30-in-60 recordings with reuse on vs off give identical
  frames; at least 95% of repeats reused on steady content; LED drive identical.
- **R3** (timing gate): GPU time per repeat refresh drops; delay totals are unchanged.

**Risk to check in R1:** the cached picture is copied at the end of PRE_OUTPUT, and the reuse
renders it 1:1 into the rounded target rectangle, as the 6d extras do.
- With a fractional target crop, a full render's edge pixels are partial coverage. The cache holds
  exactly what the full render had at PRE_OUTPUT, but if the rounded rectangle and the fractional
  crop disagree on edge pixels, R1's fractional case shows it.
- The fallback is to refuse while the target crop is not integral. The 6d extras accepted this
  after their capture-harness check.

## 7. Commit plan and expectations

| # | Commit / patch | Expectation |
|---|---|---|
| 1 | upstream contract header | no behaviour |
| 2 | VP policy + adapter + CPU tests + GPU test design + this note | no behaviour (not included) |
| P | `repeat-reuse-callsites.patch` | with `repeat_render_reuse` off (default): **byte-identical**, because the policy refuses before the hook (Disabled) and the cache is never armed by it; the CPU overhead is the key fill, ~5-15 µs per refresh, which could be skipped when off (follow-up). With it on: R1/R2/R3 above |

## 8. Verification state

With the patch applied: `VideoProcessor-Test` + `VideoProcessor-VPRenderer` x64 Release built (shared slot runner); `RepeatReusePolicyTests` 12/12 (single-threaded vstest, TRX). Not run: GPU R1-R3 (no GPU today), the renderer harness, the full CPU suite. The policy + adapter are unverified on the GPU: WIP.
