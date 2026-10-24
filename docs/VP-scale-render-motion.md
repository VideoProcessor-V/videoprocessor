# VpScaleRender: motion-active picture sequences (fix/vpscalerender-motion)

VpScaleRender (`tools/vp-scale-render`) renders luma pictures through the real VP renderer
DLL and saves VP's rendered-output captures. The TrainKit drives it for the scaling nets
(`trainkit/vpbench.py`, `tools/build_scaling_set.py`) and for D's pan data
(`trainkit/pans.py`, `picture_seq`). This note covers the motion mode added on 2026-10-24 so
D can be trained on renders where VP's motion vectors genuinely run (D_motion).

## Root cause (round 1)

In round 1 VP's motion engine never ran during `picture_seq` renders:

1. **Nothing started the estimate.** The kit's settings set `dejudder_strength: 0`. The
   adaptive downscaler's grain map *uses* vectors when they exist but never starts the
   estimate itself (`DejudderPolicy::Decide`, owner rule 2026-10-14), so the log read
   `Motion ahead: ... idle (no motion estimate runs for this source)` and the grain map ran
   its no-vector path for every frame of D's data.
2. **Every pass broke continuity.** The tool's frame counter was not advanced on the last
   frame of a `picture_seq` (and was advanced twice after a `picture`), so the next picture
   re-sent the previous frame's sequence and capture timestamp: a 0-tick step, logged as
   `capture timestamp discontinuity`, which invalidates motion (`timing_continuity_fixes`).
   With 8 staggered passes per pan, a job had one invalidation per pass (up to ~98). The
   renderer's capture `source=` no longer matched the tool's map either (the `- -1 ... fail`
   rows).
3. **Loading pauses.** Each picture was loaded before it played and the schedule was
   re-anchored afterwards while timestamps continued, so frames grew "older" with every
   picture (stale-delivery gaps after enough pictures).
4. **Render stalls.** The Beta DLL's shader cache was never saved (the tool created only
   `shader-cache\vprenderer\`, not `vprenderer-beta\`), so every job recompiled every shader
   inside its first measured frames; and a GPU slower than the declared rate (the laptop at 4K
   with the estimate and a capture per frame) let the queue grow until the renderer flushed it
   (`backlog flush`, an invalidation each time).

## The fix (tool side; renderer unchanged apart from capture metadata)

- **One session clock.** Frame n of the whole job carries sequence n + 1 and timestamp
  `start + n * period` (exact rational), across pictures; every step is a Normal step.
  A stall of the tool pauses the capture clock (QPC less the stall) instead of sending a
  burst or letting frames age.
- **Back-pressure.** With `motion vectors` / `capture per_frame` the tool holds the next frame
  while the renderer's queue is at its target (4) and while the previous capture request has
  not been taken; the wait pauses the capture clock, so nothing is dropped or flushed and the
  timestamps stay one period apart. The render runs as fast as the GPU allows (wall time can be
  longer than the declared rate; `held_for_queue`, `held_for_capture`, `clock_paused_ms` in the
  summary).
- **Persistent shader cache** for both renderer flavours (`shader-cache\vprenderer-beta\`).
- **Frame-exact naming.** The renderer's `source=` counts accepted frames, not the tool's
  sequence; the sidecar now carries `source_frame_counter` (the VideoFrame counter), which the
  tool uses to name each capture (fallback: the offset its pacing lines show). The tool also
  waits until the renderer is rendering before sending the first frame.
- **Streaming loader.** Consecutive `picture_seq` entries play as one continuous stream; a
  worker thread loads and packs frames a few ahead.
- **`motion vectors`.** The tool writes `<shots>\vp-scale-render-motion.cfg`: the job's
  config with, in every `[vprenderer*]` section, `occlusion_mask: on`,
  `dejudder_strength: 0`, `dejudder_strength_even: 0`, `interpolation: off`,
  `interpolation_fill_gaps: off`, `motion_engine: vp`, `smooth_motion: off` (other keys kept).
  The occlusion mask makes the estimate run alone on a film-like source with no de-judder
  extras: an A->B estimate for every source frame, no refresh ever warped, so every capture
  still shows exactly one source frame. The mask is only applied by a sigma-delta shader;
  the training renders have none (`temporal_dither: off`), so it changes no pixel. The job's
  own config file is not modified.
- **Per-frame captures (`capture per_frame`, default with `motion vectors`).** One capture
  request per source frame, at mid-period; a pan is captured in one pass (no staggered
  passes, no cut between passes). Each result line names the picture and frame the capture
  shows (from its sequence); identification by content still works as before.
- **Settle (`settle <s>`, default 3 s with motion).** The first (up to 8) frames of the
  sequence play forwards and backwards (real motion, valid pairs) while the engine, its shaders
  and the queue start: at least `settle` s, then until 2 s pass with no render stall, no
  invalidation and no slow shader compile, with the estimate running (at most 30 s). It ends on
  frame 0, so the measured frames follow continuously; nothing measured falls in it.
- **Proof.** Each capture sidecar now has (renderer, metadata only):
  `hybrid_motion_mode` (`vectors`: the grain map ran on this picture's vectors;
  `no-vectors`: its scalar fallback; `off`/`map-off`), `hybrid_stabilised_frames`,
  `motion_estimates`, `motion_invalidations` (session counts). The tool copies them to result
  columns 8-10 and logs/prints
  `VpScaleRender: motion summary: ... estimates_after_settle=E over_frames=F invalidations_after_settle=I ... vectors=V no_vectors=N`.
- **Guard (`require_motion <share> [max_invalidations]`).** After the run the tool exits 7
  (`VpScaleRender: MOTION CHECK FAILED: ...`) unless, after the settle, at least `share` of
  the captures were `vectors`, at most `max_invalidations` (default 0) invalidations
  happened and the estimate ran for at least 80 % of the frames. A DLL without the new
  metadata fails the check (rebuild it from this branch).

Legacy jobs (no `motion` line) keep their behaviour: `picture` captures once near the end,
`picture_seq` re-requests after each completed capture, with the continuity fixes above.

## Proof (RTX 3050 Ti laptop, 2026-10-24)

| Run | estimates / frames after settle | invalidations after settle | captures `vectors` | frames captured |
|---|---|---|---|---|
| Round-1 style (smoke job, before) | 1 estimate | 17 (16 timestamp discontinuities) | n/a (no-vectors) | 2 per pass, 8 passes |
| 4K pan 48 frames, 12000/1001, `motion vectors` (3 runs) | 47 / 47 | 0 | 48 / 48 | 48 / 48, one pass, all named right by content |
| 4K pan 48 + zone plate 12 + master pan 12, 24000/1001, one job | 71 / 71 | 0 | 72 / 72 | 72 / 72 |
| Same pan, no `motion` line (round-1 settings, fixed tool) | 0 | 0 | 0 (36 `no-vectors`) | all |

GPU test `VpScaleRenderMotionGpu.PanRendersRunContinuousVectors` (1080p -> 540p): continuous
vectors on a pan and a still; the vectors change the output (pan 0.118 codes mean, still 0.122:
on the pan the map matches the still's, so it follows the motion; without the vectors following
the pan the difference would collapse); consecutive captures align exactly under the pan's
motion (0.010 codes vs 8.37 unshifted: each capture is named after the frame it shows).
The pure rules (`VpScaleRenderSequenceRules`, 8 CPU tests) run on every build.

## Job keys

```
motion vectors                       # run VP's motion estimate (overrides above)
capture per_frame|after_complete     # default per_frame with motion vectors, else after_complete
settle <seconds>                     # default 3 with motion vectors, else 0
require_motion <share> [max_inv]     # exit 7 unless continuous vectors (e.g. 0.9 0)
picture_seq <tag> <raw> <frames> [first]   # [first] is ignored with per_frame
```

Example (one job, two pans streamed back to back):

```
dll D:\...\vprenderer-beta\VideoProcessorVPRendererBeta.dll
config D:\...\render.cfg
shots D:\...\shots
result D:\...\result.txt
log D:\...\vp.log
source 3840 2160 12000 1001
window 1920 1080
shaders standard.ssim_downscaler
require_adapter NVIDIA
require_hook SSimDownscaler.glsl
motion vectors
require_motion 0.9 0
picture warmup D:\...\pan0_seq.raw 8
picture_seq pan0 D:\...\pan0_seq.raw 16
picture_seq zp_h137 D:\...\zp_h137_seq.raw 16
```

Result line: `<tag> <frame> <png> <json> <source_seq> <requested_tag> <ok|fail> <hybrid_motion_mode> <motion_estimates> <motion_invalidations>`
(the first seven columns as before; with `per_frame` the requested tag is the captured
frame's own picture, `settle`/`tail` captures are marked so and never `ok`).

The source rate must be film-like (below 49 Hz; the estimate never runs alone for video
rates). Keep the declared rate the TrainKit used (12000/1001) unless measured otherwise:
the motion per frame is the content's; `adaptive_grain_map_smooth_ms` is a time constant,
so the declared rate changes how many frames the map averages over.

## What the TrainKit changes (pans.py / build_scaling_set.py)

1. Use this branch's `x64\Release\VpScaleRender\VpScaleRender.exe` and its renderer DLL
   (`x64\Release\vprenderer-beta\VideoProcessorVPRendererBeta.dll`, which writes the new
   metadata; the kit's vpbench\vprenderer377 copy does not).
2. `render_config`: accept extra job lines (e.g. `conf["job_extra"]`) and append
   `motion vectors` and `require_motion 0.9 0` for pan renders; treat rc 7 like rc 6 (fail,
   do not train on it).
3. `pans.py`: one `picture_seq` per pan (`PASSES = 1`, no `seqb` file, no `start`); several
   pans may follow each other in one job (they stream continuously; the cut between pans is
   ordinary content). Keep LEAD frames (the matcher's history settles after a cut). Keep the
   identification by content; rows are already attributed by column 6 (= the pan shown).
   Record `hybrid_motion_mode` (column 8) per frame in `pans.json` and use only `vectors`
   frames for D_motion.
4. Real moving footage from masters works the same way: write the clip's consecutive frames
   as the raw (any length; the loader streams it).
