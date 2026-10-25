# WP-24: captured-repeat render reuse (DRAFT, revised after audit)

Branch `feat/repeat-reuse`, rebased onto integ/batch1 **dc149284** on 2026-10-25 (worktree
`D:\codex\videoprocessor\repeat-reuse`). The branch adds VP files and the upstream contract.
`RenderedWarp.h` (a VP file) gains `Cache::Armed` and a comment fix. Every edit to the host and test
project is in `repeat-reuse-callsites.patch`, which passes `git apply --check` on dc149284.

Audit: `skeleton-notes/audit-feat-repeat-reuse.md` (HIGH 2, MED 6, LOW 8). Section 8 maps each
finding to its fix.

## 1. What it saves

A captured repeat is a film frame sent again: 1 refresh in 5 at 24-in-30, 1 in 2 at 30-in-60.
Today each repeat re-runs the whole libplacebo render. When nothing that feeds the picture changed
since the frame's first present, the repeat draws the cached PRE_OUTPUT picture (the de-judder
extras' 6d cache) through the OUTPUT stage only:
- output encoding;
- the OUTPUT hooks (sigma-delta, occlusion/grain map, encoding repair);
- dither;
- target overlays.

There is still exactly one `pl_render_image` per refresh, so the dither advances as before.

**The cost side (audit H1).** The cache copy is armed only for a first present the queue already
holds a repeat of, and only with a key that could be reused. The copy is rgba32f where the GPU can
render to it.
- 24-in-30: 6 copies per second buy 6 saved renders per second.
- 30-in-60: 30 copies buy 30 saved renders.

R3 must still measure copy cost against saved cost on the laptop and the 3090, at both cadences.

## 2. Pieces

| File | Class | What |
|---|---|---|
| `src/VideoProcessor-Lib/vprenderer/RepeatRenderReuse.h` | **UPSTREAMABLE** (std + libplacebo public headers) | `RepeatFrameKey`; `KeyFrom()` (generic key from the loop's frame: image overlays, LUT *generation*, temporal render state); `Signature`; ordered refusals (`HookStateOpaque` is persistent); `Stats` + status line; `IRepeatReuse` with the three calls; `NeverReuse` (upstream default = today) |
| `src/VP-V/modules/output/presentation/RepeatReusePolicy.h` | VP-only, pure | switch / Original pathway, VP temporal stages, `VpExtension`, hook-state signature with named opaque refusal, arming rule + back-off, counters |
| `src/VP-V/modules/output/presentation/TemporalShaders.h` | VP-only, pure | mpv shader text scan (`frame`, `random`, `//!BUFFER`), registry noted by the loaders, shader-set generation |
| `src/VP-V/modules/output/presentation/RepeatReuseAdapter.h` | VP-only, libplacebo | `ClassifyHooks` (pure: multi-stage and temporal hooks are opaque), `DrawFromCache` (6d path, zero warp) |
| `src/VP-V/modules/output/presentation/RepeatReuseGlue.h` | VP-only | **VP's `IRepeatReuse` implementation**: all logic the host used to carry (audit M5); fallback entries (M4) |
| `src/VP-V/tests/RepeatReusePolicyTests.cpp` | CPU tests | 21 tests |
| `src/VP-V/tests/gpu/RepeatReuseGpuTests.inc` | GPU, written, not run | R1-R3 byte-identity/timing design |
| `docs/design/repeat-reuse-callsites.patch` | host patch | `LibplaceboVideoRenderer.cpp` + test vcxproj |

## 3. Call points in Bill's loop (three, not one: audit M6)

| # | Where | Host text |
|---|---|---|
| 1 | after hook assembly, before the encoding-repair check | `SetOptions(...)`; `if (Active()) { SetContext(RepeatReuseContext(image, target)); PrepareFirstPresent(KeyFrom(...), frameHooks); }` |
| 2 | **first statement of the draw callback** passed to `NlsHookRecovery::RenderOwned` (Bill's file, untouched) | `if (repeatReuse.TryDrawRepeat(repeatReuse.Key(), renderer, destination, parameters)) return true;` |
| 3 | after the draw | `repeatReuse.NoteRendered(repeatReuse.Key(), rendered && !hookRender.retried);` |

**Feeding the key.**
- The render loop sets `repeatNextQueued` after the dequeue (one statement): the queue already
  holds a repeat of this frame.
- The subtitle composition calls `NoteSubtitleComposition(move, background, donorReady)`.
- The two mpv parse sites call `TemporalShaderRegistry::Note`.

**The host's own state.**
- `RepeatReuseContext()` (~40 lines) only reads host members into a `VpContext`: temporal flags,
  cache, the known-hook table, re-base, LUT identity, the fallbacks heading.
- `DisplayLutSignature()` returns the LUT's observed file version and parsed state.

**Upstream PR.** It would contain `RepeatRenderReuse.h`, the three calls, a
`std::unique_ptr<IRepeatReuse>` defaulting to `NeverReuse`, and `KeyFrom` at call 1. The
`repeatNextQueued` statement is upstreamable too: it is generic queue knowledge.

## 4. What must still run in Bill's loop on a repeat

Everything before the draw callback runs on every refresh, as today:
- **Queue, timing, render-ahead.** Delay totals are unchanged.
- **Crop and subtitle presentation** (21c). Holds, animations and caption timers advance per
  refresh; their results feed the key (geometry, image overlays, composition signature).
- **The subtitle composition.** It still composes on every repeat (L6: a later step could reuse it
  per source frame). Its inputs, including the temporal-fill donor, now feed the key (H2).
- **Overlay and hook assembly.** Target overlays are drawn again by the reuse draw, so they are not
  in the key (L3). Every pre-OUTPUT hook's parameters are in the key.
- **LED measurement.** Picture-neutral, so it is skipped on a reused repeat, as on the de-judder
  extras; the previous result stands. Keep R2's "LED drive identical" check mandatory (L7: confirm
  LedDrive smoothing is time-based).
- **Telemetry, Present, sigma-delta `static_refresh`.** Unchanged.

## 5. Decision, arming, visibility

**Refusals, in order:**
1. not a repeat;
2. other frame;
3. hook state not known: an opaque hook, named, e.g. "AdaptiveDownscale has no registered state";
4. temporal state: VP phase pictures, extras, crossfade, startup black, deinterlacing, mixing, peak
   detection, deband grain;
5. no cache;
6. stream, settings, geometry, colour (LUT by generation), overlays (image), hook state or extension
   (re-base factor/kernel, LUT identity, subtitle composition, engaged fallbacks) changed;
7. draw failed.

**Hook classes:**
- picture-neutral: LED measurement, the cache copy;
- static: encoding repair, Extended Range carry/restore/late (a function of frame, colours, LUT and
  settings, all keyed);
- generation: VP calibration LUT stage (LUT identity);
- mpv: pointer + every parameter value;
- opaque: unregistered native hooks, AdaptiveDownscale (grain-map smoothing), any hook with an
  OUTPUT pass plus an earlier stage (M1), and mpv shaders using `frame`/`random`/`//!BUFFER` (M2).
- The occlusion mask is no longer excluded (M3). It is OUTPUT-only, so it stays out of the key but
  runs in the reuse draw, carrying the grain-tolerance map.

**Arming (H1):**
- only when `nextIsRepeat` holds and the complete key is neither opaque nor temporal;
- after 3 persistent refusals in a row it backs off for 2 s;
- no copy is made without a possible reuse.

**Visible (M4):**
- The status line shows "Repeats reused: N of M (last refusal: reason: hook)".
- **"Fallbacks engaged"** carries "repeat reuse: reuse draw failed; repeats render in full" (sticky
  per stream), or "repeats render in full (hook state not known: AdaptiveDownscale has no
  registered state)" when a switched-on reuse backs off. The GUI's existing one-time notice and the
  Ctrl+Alt+D bundle follow.

**Setting:** `repeat_render_reuse` on|off, default **off** until R1/R2 pass. The Original pathway is
always off. When off, the host runs only `SetOptions` and the `Active()` test each refresh (L5).
Not done yet: the settings fingerprint, the live-apply list and the Config page entry.

## 6. Byte identity (GPU, written, not run)

- **R1** (unit): reuse versus full re-render, 0 differing bytes; the dither phase is the only
  allowed difference.
- **R2** (capture harness, 24-in-30 and 30-in-60): frames identical with the setting on and off;
  at least 95% of repeats reused on steady content; LED drive identical.
- **R3** (timing): copy cost versus saved cost.

Known R1 risks:
- fractional target crop edges;
- L1: with libplacebo 7.360, a limited-range output encodes through the repair shader on the full
  path but natively on the reuse draw. The fallback rule if they differ is to refuse reuse while
  the repair is active.

The cases still to build are listed in the .inc (L8).

## 7. Commit plan

| Commit | Content | Expectation |
|---|---|---|
| upstream contract (M6) | header | no behaviour |
| M5 glue, M1/M2, H1, H2, M4 | VP files + tests | no behaviour (not included) |
| patch | host calls | setting off (default): byte-identical (no key, no arm, no probe); on: R1-R3 |

## 8. Audit resolution

| Finding | Fix |
|---|---|
| H1 copies without reuse | arm only on `nextIsRepeat` + complete key (not opaque/temporal) + back-off; Extended Range registered static; AdaptiveDownscale opaque → never armed |
| H2 key gaps | `VpExtension` (re-base factor X/Y + kernel, LUT identity, subtitle composition incl. donor, fallbacks heading) in `extension`; LUT by generation in `colour`; calibration stage = Generation |
| M1 multi-stage hooks | opaque in `ClassifyHooks` |
| M2 temporal mpv shaders | `TemporalShaders.h` registry, noted at both parse sites; opaque |
| M3 occlusion excluded | removed from the exclude list and the neutral list |
| M4 not in Fallbacks | `EngageFallback` / `ReleaseFallback` through the glue; named persistent reason |
| M5 VP logic in host | `RepeatReuseGlue` (IRepeatReuse); host = calls + read-only context fill |
| M6 contract unusable / "ONE hook" | non-template interface that can draw; three documented calls; VP goes through it |
| L2 deband grain | temporal (refuse) |
| L3 overlays | image overlays hashed, target overlays out |
| L4 hook ABA | shader-set generation in the hook state |
| L5 off cost | host guarded by `Active()`; no per-refresh allocation of tables (fixed arrays in `VpContext`; the glue still copies them into vectors once per refresh when on) |
| L8 docs/tests | stale rgba16f comment fixed; R1 overlay case wired; missing cases listed |
| L1, L6, L7 | open, listed (R1 limited-range case, composition reuse later, LED smoothing check) |

## 9. Verification state

See the report for the build and CPU test results on dc149284. No GPU tests were run.
