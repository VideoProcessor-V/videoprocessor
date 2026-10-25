# VP settings reference guide

For the owner: what each setting does, why it exists, when to change it, its default, what it
costs and what it interacts with. Written 2026-10-24 from the code, the design documents and the
Config tooltips, and used to audit those tooltips (section 11).

**How to read it**
- The settings are grouped by the pipeline's **units**, in the order the picture passes through them:
  Input, Motion, Repair, Scaling, Colour, Dynamic Drive, Output. See
  [`docs/design/VP-pipeline-final.md`](../design/VP-pipeline-final.md) for the full design.
- **Key** is the name in `VideoProcessor.cfg`. **Default** is what VP does when the key is absent.
- **Cost** is GPU time per frame unless it says otherwise: *none*, *tiny* (under 0.2 ms), *low*
  (under 1 ms), *medium* (1-3 ms), *high* (over 3 ms at 4K on the laptop's RTX 3050 Ti).
- "Under test" means a fix or feature that ships behind a switch until it is proven.
  Most "fixes under test" are default ON and their switch exists only so an A/B is possible.
- This guide covers VP's own (FL35 fork) settings in full. Bill's upstream settings are only listed
  in section 9, with a one-line summary each.

**Contents**
1. [The VP-V page: tiers in one place](#1-the-vp-v-page-tiers-in-one-place)
2. [Input](#2-input)
3. [Motion](#3-motion)
4. [Repair](#4-repair)
5. [Scaling](#5-scaling)
6. [Colour](#6-colour)
7. [Dynamic Drive](#7-dynamic-drive)
8. [Output](#8-output)
9. [Upstream (Bill's) settings, in brief](#9-upstream-bills-settings-in-brief)
10. [Similar names, different jobs](#10-similar-names-different-jobs)
11. [Tooltip audit](#11-tooltip-audit)
12. [Settings to retire (for your approval)](#12-settings-to-retire-for-your-approval)

---

## 1. The VP-V page: tiers in one place

Config now has one **VP-V** page that holds everything VP adds.

| Part | What it does |
|---|---|
| **Pathway** | *VP-V pathway* (default): the VP-V page is the one place for VP's settings. *Original pathway*: every VP addition is off in the renderer, so the whole VP-V page is disabled (it says so); Bill's settings stay on his pages, exactly as upstream. Stored as `[vpv] pipeline_original_pathway` (on = Original; written explicitly either way). In the renderer, Original runs the mixed stages in their upstream form (ADR-0019). |
| **Profile** | The renderer profile the tiers edit. A named profile inherits everything it does not set from the first (default) profile, and profile rules pick the profile, for example per frame rate. |
| **Global tier** | Classic / Low / Mid / Upper for every unit at once. If a unit has no step at that level, it uses its nearest lower step. Change one unit afterwards and the global tier shows *Custom (per unit)*. |
| **Fallbacks engaged** | The running VP's list of automatic downgrades, for example "Fallbacks engaged: capture GPU unpack (CPU 4:2:0 ...)", or "none". It is the same line as Ctrl+I and Ctrl+Alt+D. |
| **Unit tabs** | Each tab has two sub-pages. **Main**: the unit's tier slider, its taste slider where it has one, and small cards of switches. **Advanced**: the settings a master on Main sets (greyed "Set by ..." until you tick *Custom* on that card; unticking puts the master's values back), tuning used only while its feature is on ("Used only when ... is on"), the measured internals (collapsed), the planned settings and the unit's stages with their fallback text. The layout map is `docs/settings/organisation.md`. |
| **Pipeline (expert)** | For testing only: where the calibration LUT runs (`calibration_lut_stage`: libplacebo today, or VP's last colour step after Dynamic Drive), the module order (`pipeline_order`, checked with the renderer's own rules; an order it would reject is shown with the reason and not saved), and stages switched off entirely (`pipeline_slots_off`). |
| **Proposed retirement** | The section-12 candidates below, by group, with where each control is and the value in use. The same note is added to each control's tooltip. Nothing is removed until you approve. Each unit tab also ends with "More settings of this unit", naming the pages that hold the rest (coverage audit: `docs/settings/coverage-audit.md`). |

**One control per key.** Every VP setting has exactly one live control. VP's own settings that used
to be on the VP Renderer > Rendering page moved to the VP-V page (the Rendering page keeps Bill's:
quality, tone mapping, Processing); the old duplicate controls are hidden. The proposed retirements
have their controls on the Proposed retirement tab.

**Masters and what they set** (full table in `docs/settings/organisation.md`):
- each unit's **tier** sets its tier keys (Advanced > Set by the tier);
- the **de-judder amount** slider (Motion > Main) sets its eight fields (Advanced > Set by the de-judder
  amount); the de-judder's other shaping is a separate card and is not set by it;
- the **picture slider** (Scaling > Main) sets the re-base evidence (Advanced > Re-base evidence);
- one slider per unit: the Classic Motion tier greys the de-judder amount, and below the Mid Scaling
  tier the picture slider is greyed (re-base is off) and never offers its own Off;
- a feature switch (render ahead, per-shot easing, SDR tone shape, room light, content labels) frees
  its tuning on Advanced only while it is on;
- **Proven fixes: all on** (Proposed retirement) sets the 14 renderer fix switches;
- on the Dynamic Drive page (Main | Advanced): the **LED speeds preset** sets the nine speeds,
  **Advanced LED values** sets the five advanced LED groups, **LED fixes under test** sets the nine
  LED fix switches.

**Where VP-V keys live.**
- The global VP-V keys are in a `[vpv]` section: `pipeline_original_pathway`, `pipeline_order`,
  `pipeline_slots_off` and `calibration_lut_stage`. Config renames and reorders renderer profiles;
  it never touches `[vpv]`.
- The renderer reads `[vpv]` first. The same keys in `[vprenderer]`, where the skeleton first
  put them, still work.
- These keys keep their names: they have no `vpv_` prefix, because the section is the namespace.
- The tier keys are the existing per-profile keys, so their names are unchanged. ADR-0023 lists
  how they would map into `[vpv.*]` later.

**What the tiers mean.**
- **Classic** = no advanced features: straight to the projector with classical scaling.
- **Low**, **Mid** and **Upper** add measured features within a growing GPU budget.
- A tier is only a **preset of existing settings**. No new key stores it: VP reads the tier back from the values. Change one of the tier's settings and the tab shows *Custom*. *Reset to tier* puts the values back.

| Unit | Classic | Low | Mid | Upper |
|---|---|---|---|---|
| Motion | VP's basic matcher (whole-pixel, nothing ahead) | VP's matcher at quarter-pixel | NVIDIA engine + matcher, 4 ahead (shipped) | + split poorly matched blocks |
| Repair | libplacebo deband only | + breathing reduction (today's default) | VP's grain-safe debander | + texture restore |
| Scaling | no re-base, plain scaling | re-base, plain scaling (shipped) | re-base + hybrid SSIM downscale (w 0.2) | *not yet: refine net / D (uses Mid)* |
| Output | libplacebo dither | as shipped: the shader list decides (the cinema list loads the sigma-delta) | always VP's sigma-delta | *same as Mid* |
| Input, Colour, Dynamic Drive | not tiered (paths and set-up, not quality steps) | | | |

The motion taste slider (the de-judder amount, Motion > Main) is never part of a tier and no tier
writes it: it is your taste, not a cost choice (As shot = no motion processing). The shipped values
are a tier of every unit (Motion Mid, Repair Low, Scaling Low, Output Low); when values match no
tier the unit shows Custom and no tier is written unless you choose one.

---

## 2. Input

*Takes the picture from the capture card at full precision, works out the signal type and holds it
for the fixed delay.*

### 2.1 Total delay (Queue page: `target_latency_ms`)
- **What:** the time from the picture arriving to it appearing on screen. Set it to match the
  audio delay in your AVR.
- **Why:** the AVR delays the sound by a fixed amount. If VP's delay changed between titles, lip
  sync would drift. VP therefore holds ONE total delay, the same at every frame rate. Faster rates
  are padded.
- **Recommended: about 267 ms** (measured: 4 queued frames at 23.976p, the slowest rate, on a 60 Hz
  display, including the display's phase; the earlier estimate was 250 ms; a faster display needs less). The queued
  frames are what the look-ahead features (motion ahead, Dynamic Drive's look-ahead, crop and
  subtitle look-ahead, hiccup repair) work from.
- **When to change:** only to match an AVR or soundbar that cannot delay the audio by 250 ms plus
  the projector's own lag. A shorter value still works: the look-aheads fall back to 3 or 2 frames,
  and the VP-V page's Input tab shows the frames and padding at every rate.
- **Default:** not set. The queue then uses *Target frames*.
- **Cost:** none in GPU time. It sets the delay.
- **Interactions:**
  - Below about 2 frames at 24p, the delay cannot be met, and VP says so.
  - Render-ahead's lead counts inside this budget, never on top of it.
- **Caveat:** until the total-delay budget is wired into the renderer (ADR-0020, barrier B2), VP
  still treats this value as queue time only, so the real delay is slightly longer.

### 2.2 Queue mechanics (Queue page)
| Key | What / why | Default | Change it when |
|---|---|---|---|
| `target_frames` | The queue length in frames, used when no delay is set. | profile's value | Prefer the total delay; frames give a different delay at every rate. |
| `queue_size` | The queue's capacity (an upper bound, not the delay). | auto | Never, normally. |
| `lead_frames`, `startup_preroll_frames`, `active_picture_lookahead_frames` | How many queued frames the start-up and the crop detector use. | auto (follows the queue) | Diagnostics only. |
| `reset_queue_too_large_percent`, `reset_after_render_restart_seconds` | When an over-full queue is reset, and how long after a renderer restart. | 5 s / profile's value | Only if the queue keeps resetting. |
| `low_latency_path` | Games / 50-60p live. **Why:** the de-judder and motion look-ahead are switched off and frames upload early to get the lowest delay. | on only for a queue named Low_Latency | For a gaming queue profile. |
| `queue_timing_fixes` (Proposed retirement > Proven fixes) | Drops frames to catch up only when really behind. | on | Never; A/B only. |

### 2.3 Capture (VP-V page, Input tab)
| Key | What it does | Why it exists | Default | Cost |
|---|---|---|---|---|
| `capture_path` | Unpacks DeckLink v210/UYVY on the GPU (`gpu`) or the CPU (`standard`). | The GPU path is byte-identical and about twice as fast at 4K, with no CPU conversion. | gpu | saves CPU |
| `capture_upload` | Uploads each frame on its own thread as it arrives (`early`), or at render (`render`). `auto` = early only in low latency. | It takes about 5 ms per 4K 12-bit frame off the render thread. | auto | none |
| `native_rgb_ingest` | DeckLink 12-bit RGB (R12B/R12L) and 10-bit r210 reach the renderer as full RGB 4:4:4, unpacked on the GPU. | The old path squeezed RGB to 10-bit 4:2:0 on the CPU, which loses colour detail. | on | low |
| `dv_tunnel_gpu_unpack` | Unpacks the Dolby Vision tunnel's 12-bit picture on the GPU (bit-exact with the CPU). | It saves CPU time. | off (under test) | low |
| `source_range` | The range the source sends (auto / full / limited). | Some sources mislabel their range. | auto | none |

**When to change:** never, unless the "Fallbacks engaged" line names capture. That line means the
GPU unpack failed and VP fell back to the CPU path.

### 2.4 Signal (DV / HDR detection, General and DV pages)
| Key | What / why | Default |
|---|---|---|
| `dv_tunnel_handoff` | A tunnel with no metadata for about 1 s (LLDV/HDR10 sent as RGB) goes to the normal path. | on |
| `dv_tunnel_fixes` (under test) | Tunnel menu/letterbox probe, metadata-loss counting, L2 before L8. | on |
| `dv_trims` (under test) | VP applies the DV Level 2 trims for your display's peak after its tone mapping. | off |
| `dv_tone_mapping` (under test) | Dolby Vision pictures: libplacebo's curve, or `dolby_style` (approx.): a Dolby-style curve from the scene metadata (L1) with the trims in ICtCp. Not certified; shown as "Dolby-style (approx.)". Colour unit, Dolby Vision card. | libplacebo |
| `dv_compare_capture_folder` (diagnostic) | A folder on a large drive; the DV comparison capture shortcut (Ctrl+Alt+Shift+W) records a short labelled clip there. Empty = off; the Original pathway clears it. Input unit, Readouts (diagnostics). | empty (off) |
| `newlldv`, `newlldv_sdr_veto` | BT.2020 without an HDR flag is treated as HDR after 1.5 s; real SDR returns to SDR. | off / on |

### 2.5 Source analysis (detected on the untouched picture)
- **`source_resolution`** (*Scaling* tab, because it drives the re-base): see 5.1.
- **Subtitle detection and placement:** Bill's subtitle page, plus VP's guards
  `source_resolution_text_guard` and `source_resolution_mixed_guard` (5.1).
- **`source_coding_fixes`** (under test, on): reads the real bit depth and coding damage, which
  drive the auto deband and low-bitrate handling.
- **Planned:** the label-detector net (codec family, film grain) for the repair net's labels. Not
  yet implemented.

---

## 3. Motion

*Measures motion once on the untouched picture and feeds the de-judder, hiccup repair, the dither
and (optionally) interpolation. One "vectors valid" switch governs every user. At a cut, drop,
repeat or resync all of them fall back together.*

### 3.1 Motion engine (tier)
| Key | What it does | Why | Default | Cost |
|---|---|---|---|---|
| `optical_flow` | NVIDIA's optical-flow hardware computes the vectors. Off = VP's own matcher only. | The hardware engine is fast and runs off the render thread; the matcher takes over whenever the engine has no vectors. | on | low (engine) |
| `motion_ahead_frames` | Vectors are computed ahead of the picture, off the render path. Since the r6 fixes the depth is a TIME (about 67 ms: 2 frames at 24p, 4 at 60p), so it no longer forces a longer queue. | The picture never waits for its motion. | 4 (on) | none on the render thread |
| `dejudder_est_split` | A poorly matched block is searched again as four quarter blocks. | It gives small objects on moving backgrounds their own motion. | off (Upper tier: on) | low-medium |
| `dejudder_estimate_ahead` | With the frame after next queued, the next frame's motion is worked out in spare time. | It evens out the render thread's load. | on | none |
| `optical_flow_global`, `optical_flow_global_fallback` | Camera pan/tilt is read from the engine's vectors; the search is used when the engine is unclear. | It saves about 0.4 ms. | on / on | saves time |
| `optical_flow_cost_trust` | The engine's own match cost lowers trust. | An idea that made no measurable difference. | off | none |
| `optical_flow_fb_check` | The engine's forward and backward vectors must agree. | About 6% less damage for 1-2% less judder removed. | off | low |

**Measured internals: leave them alone.** `dejudder_match_tolerance`, `dejudder_frame_trust`,
`dejudder_global_prior`, `dejudder_global_two`, `dejudder_est_gate`, `dejudder_vector_smooth`,
`dejudder_est_candidates`, `dejudder_est_lambda`, `dejudder_est_qpel`, `dejudder_est_split_sad`,
`dejudder_search_range`, `dejudder_texture_tolerance`, `dejudder_consistency_px`,
`dejudder_regional_hold`, `dejudder_cut_histogram`, `dejudder_pixel_trust(_lo/_hi)`,
`dejudder_landing_check`, `dejudder_verify(_share)`, `dejudder_low_res`,
`dejudder_hold_feather_px`, `dejudder_hold_hysteresis`, `dejudder_motion_scale_lo/hi`,
`dejudder_trust_scale`.

These were tuned on real footage, and Config already marks them "measured - leave alone". Each
one decides when a block's motion is trusted, how far the search looks, and how held and moved
areas meet. Change one only to reproduce a bug report.

### 3.2 De-judder (the motion taste: VP-V > Motion > Main, "De-judder amount"; its fields on Advanced)
| Key | What / why | Default |
|---|---|---|
| **Motion slider** (As shot, Film, **Cinema**, Natural, Smooth, Max) | One question: how much should motion change from what was shot? Each step sets the keys below. | Cinema |
| `dejudder_strength` | How far the 3:2 hitch (24p on 60 Hz) is eased toward even motion. 0 = as shot, 1 = full frame-rate conversion. | 0 if unset (the slider writes 0.25) |
| `dejudder_strength_even` | The same for even cadences (30p on 60 Hz, 25p on 50 Hz). | 0 |
| `dejudder_even_rates` | Whether the even strength also applies to 23.976 / 29.97. | whole rates only |
| `dejudder_object_strength` | How much things moving within the frame are smoothed (the camera's motion is always smoothed). Lower is less "video". | 1 (slider: 0.5 at Cinema) |
| `dejudder_motion_rolloff`, `dejudder_roll_lo/hi/floor` | Fast action keeps its strobing: the strength falls above a brisk pan. | on at Cinema |
| `dejudder_fade_match`, `dejudder_fade_floor` | Motion is followed through smooth fades, at a reduced strength. | on / 0.25 |
| `dejudder_pulldown` | Finds film sent with repeated frames (24p as 2:3 in 60 Hz) and shows it at its own rate. | off (experimental) |
| `pulldown_fixes` (under test) | Keeps that lock through a missed frame or an edit. | on |

- **When to change:** move the slider. Per-rate behaviour comes from profile rules: a 24p profile at
  Cinema and a 30p profile at Natural.
- **Cost:** medium. The in-between pictures are warps of the rendered picture.
- **Interactions:** VP's de-judder and libplacebo's smooth motion are one or the other, chosen per
  profile by the Motion page's *Motion handling*. The de-judder is off in low latency, for
  interlaced input and for 50/60p video.

### 3.3 Hiccup repair, interpolation and render-ahead
| Key | What it does | Why it exists | Default | Cost |
|---|---|---|---|---|
| `interpolation_fill_gaps` | **Hiccup repair.** When a stream loses up to about 1/6 s of frames, the missing frames are filled from the frames either side. | Streams drop frames. A filled gap reads as smooth; a repeated frame reads as a stutter. | on | low, only on a gap |
| `interpolation` | VP's own frame interpolation for video sources, from the motion vectors. *Auto* = only where the rate does not divide the display (50p on 60 Hz). | It is HDR-safe, unlike frame blending. | off | medium |
| `interpolation_fallback` | Where motion is not trusted: *hold* the nearest frame (never a double image) or *blend*. | hold keeps the picture clean. | hold | none |
| `interpolation_grain` | An interpolated picture keeps the grain of the nearer frame, rather than averaging two grains away. | Faithful grain. | preserve | none |
| `source_class` | Which sources count as film (de-judder) and which as video (interpolation): the fixed-rate rule or the measured one. | Odd rates need the measured rule. | legacy (fixed rates) | none |
| `render_ahead` | The de-judder's in-between pictures are queued straight after their frame, so the next frame starts rendering at once. | It gives a slow GPU more time per frame. It runs only inside the delay budget, and is listed under "Fallbacks engaged" when it cannot fit. | off (under test) | none (more latency) |
| `render_ahead_queue_credit` | Shortens the input queue by render-ahead's lead, so the total delay stays the same. | Lip sync. | off (under test) | none |
| `render_ahead_utilisation`, `dejudder_gate` | How much of a source frame's time the GPU may use, and how the de-judder steps down when it does not fit. | Keeps frames on time on a slow GPU. | 0.85 / auto | none |

### 3.4 Content-adaptive (experimental, off)
| Key | What it does | Default |
|---|---|---|
| `intent_content`, `intent_content_hold_seconds` | Labels film, video, sports, animation or a still menu, so profile rules can follow it. | off |
| `intent_motion`, `_policy`, `_min`, `_min_shot_s` | May scale your de-judder strength DOWN per shot (never up). | off |
| `intent_ease_s` | How the intent features ease in at a cut. | 1.5 s |

---

## 4. Repair

*Cleans streaming damage at the picture's real (encoded) size: encoding repair (planned), deband,
then breathing reduction and texture restore. Grain is never removed or averaged away.*

### 4.1 Encoding repair (planned)
- **`encoding_repair`**: net A, or VP's own restore module. Not yet implemented.
- It is kept only if it gains at least about 0.5-1 dB on low-bitrate streams with the grain kept
  (round-2 measurement).

### 4.2 Deband (tier)
| Key | What it does | Why it exists | Default | Cost |
|---|---|---|---|---|
| `deband_vp` | VP's own debander, before any scaling. *Auto* = only as strong as the source needs. | It is safe on HDR (libplacebo's leaves black specks there) and leaves texture and grain alone. | off (Mid tier: on) | low |
| `deband_vp_grain_guard` | Splits the picture into a smooth layer and the grain on top, and debands only the smooth layer. *Veto* = skip grainy areas entirely. | Removes banding even where the stream kept noise in the bands. | on | tiny |
| `deband_vp_exclusive` | While VP's debander runs, libplacebo's automatic deband is turned off. | libplacebo's deband thins fine grain. An explicit Light/Default strength is kept. | on | saves time |
| `low_bitrate_handling` | How strongly VP's debander reacts to compression: by the measured bitrate (auto), bit depth only (off), or always (on). | Poor streams band more. | auto | none |
| `deband_strength` | **libplacebo's** debander strength (Auto / Default / Light / Off). | Upstream's deband, used in Classic and Low. | auto | low |
| `deband_grain` | Whether libplacebo's debander adds synthetic grain. | VP's dither does that job, and the film's own grain is kept. | off | none |

### 4.3 Breathing reduction and texture restore (tier)
| Key | What it does | Why it exists | Default | Cost |
|---|---|---|---|---|
| `detail_pulse_stabilise` | Fine detail and grain "breathe" with the encoder's group of pictures. This raises each frame's own detail toward its best-coded neighbour (never beyond it). | Starved streams pulse visibly. | on | low |
| `detail_pulse_cap` | The most a frame's detail is raised (x1.00-1.50). | A safety limit. | 1.35 | none |
| `detail_pulse_fast` | The same picture in about half the GPU time. | Efficiency. | on | saves time |
| `texture_restore` | Separates grain from scene texture per area, and raises both toward the best-coded reference along the motion (about 2 s memory). | Recovers texture that the stream smeared. | off (Upper tier) | **high** (9.8 ms at 4K on the laptop until its efficiency pass) |
| `texture_restore_cap` | The most texture is raised (x1.00-2.00). | A safety limit. | 1.50 | none |

---

## 5. Scaling

*Undoes the player's upscale (re-base), brings the picture to 4K (planned), and scales it to the
projector with the hybrid SSIM scaler.*

### 5.1 Re-base (`source_resolution` and friends; tier)
| Key | What it does | Why it exists | Default |
|---|---|---|---|
| `source_resolution` | Many 4K streams are 1080p underneath, upscaled by the player. VP shrinks the picture back to its real size and then upscales it with its own path. *Auto* detects it; *native* = as captured; a size forces it. | Fewer halos and cleaner edges than scaling the player's upscale. | auto |
| `rebase_kernel_match` | Identifies the player's upscale filter and undoes it exactly, rather than with a generic downscale. | +8 to +15 dB closer to the decoded picture on real Apple TV output. | on |
| `source_resolution_auto_min_confidence`, `_evidence_seconds`, `_evidence_shots` | How sure Auto must be (score, seconds, separate shots) before it re-bases. | One soft shot of real 4K must never decide alone. | 0.8 / 10 s / 3 |
| `source_resolution_engage_max_wait_s`, `_crossfade_s`, `_release_fade_s` | When Auto is sure, it switches at the next cut or dark moment, else crossfades; the return to native is quick. | The change must never be visible. | 20 s / 1.5 s / 0.25 s |
| `source_resolution_text_guard` | Ignores the player's own sharp 4K subtitles when judging. | Text would block or fake the detection. | on |
| `source_resolution_mixed_guard` | Never re-bases a mixed screen (a menu with a small video, picture-in-picture). | The re-base is one size for the whole picture. | on |
| `source_resolution_latch_fixes`, `_cut_fixes`, `active_picture_startup_hold` | Latch and cut-judge fixes, and holding the start-up picture until it is cropped. | Under test, default on. | on |

- **When to change:** never for normal viewing. *native* is the A/B reference.
- **Cost:** low (one 1-D pass per axis).
- **Interactions:** the Scaling tier: Classic/Low = native, Mid = auto.

### 5.2 4K route, refine net and chroma upscale (planned)
- `entrance_route_4k`, `refine_net`, `chroma_upscale_vp`: reserved slots, not yet implemented.
  The Scaling tier's *Upper* step waits for the refine net.

### 5.3 Output scaling: the hybrid SSIM scaler (tier)
| Key | What it does | Why it exists | Default |
|---|---|---|---|
| `adaptive_downscale` | Blends the SSIM downscaler with a faithful Lanczos downscale (luma only): hard edges and grain lean faithful, texture keeps SSIM. Needs the SSimDownscaler shader. | SSIM alone sharpens grain into noise and haloes edges. | off (Low/Mid tiers: on) |
| `adaptive_w_base` | How much SSIM is used where nothing argues against it. The design value is **0.2** (hybrid). | Measured: 0.2 keeps detail without grain harm. | 1.0 (tiers write 0.2) |
| `adaptive_faithful` | The faithful filter: *auto* (polar where both axes shrink alike, separable when only one does) or *ortho* (separable always). | Separable keeps grain's character at about a quarter of polar's cost. | auto |
| `adaptive_grain_map`, `_smooth_ms` | Measures grain per block along the motion vectors. | Grainy areas lean faithful. | on / 500 ms |
| `adaptive_edge_lo/hi`, `adaptive_grain_lo/hi/floor/smooth_ms/shadow_bias` | Thresholds for where edges and grain lean faithful. | Measured; leave alone. | see Config |
| `ssim_bitrate_aware`, `ssim_bitrate_floor` | Gentler sharpening on poor streams. | SSIM would sharpen blocking. | on / 0.4 |
| `ssim_residual_stabilise`, `cnn_residual_stabilise`, `residual_stabilise_grain_guard` | Averages the scalers' added detail along trusted motion, but not on grain. | It stops shimmer without freezing grain. | on |
| `adaptive_chroma_ssim` | Applies SSIM to chroma too. | It can fringe colour. | off |
| `adaptive_upscale`, `adaptive_upscale_w_base`, `adaptive_upscale_superres_scale`, `upscale_light` | The same idea for sources smaller than the screen (CNN vs faithful upscale). | CNN upscalers sharpen grain. | off / 1 / on / linear |
| `downscale_light` (Scaling page) | The light domain for downscaling (linear or gamma). | Gamma matches madVR's SSIM. | linear |

- **Planned:** the D refine add-on under the hybrid scaler (`hybrid_refine_d`).
- **Cost:** low-medium.

### 5.4 Geometry (crop, A-lens, NLS, zoom: Bill's Screen/Zoom pages + VP's NLS)
- **`nls_sampling`** (under test): the NLS stretch reads the picture with Catmull-Rom, which keeps
  grain. The alternative, bilinear, is the old sampling.
- If the NLS hook fails, VP falls back to the linear safe fit ("Fallbacks engaged").

---

## 6. Colour

*libplacebo does the tone and gamut mapping in every tier. Below-black and above-white are always
kept (owner rule). Stage 13 appearance is planned.*

| Key | What it does | Why it exists | Default |
|---|---|---|---|
| `preserve_extended_range` | Keeps SDR detail below video black and above video white through scaling and colour conversion. | Upstream libplacebo clips it; the owner requires both kept in all cases. | on |
| `extended_range_more_cases` (under test) | The same for BT.1886/sRGB curves, wide-gamut SDR, a LUT and HDR. | Covers the remaining paths. | on |
| `sdr_tone_shape` | SDR on an SDR projector: highlights roll off softly toward peak (the shape tone-mapped HDR has). Nothing is created above the master's white. | SDR highlights clip harshly on a projector. | off |
| `sdr_white_headroom`, `sdr_knee_start`, `sdr_knee_strength`, `sdr_contrast_recovery` | Where reference white sits below peak, and the knee's shape. | Tunes the tone shape. | 0 / 0.80 / 0.5 / auto |
| `custom_tone_mapping` (beta) | Gamut overflow correction after gamut mapping. | Experimental. | off |
| `dynamic_target` | Trades some scene brightness for highlight headroom. | Upstream beta option. | on |
| `fbo_float32` | 32-bit float intermediates (exact highlights, more bandwidth). | Half-float rounds values above white. | auto |
| Ambient (`ambient_adapt` and 5 more) | Renders to the black that room light leaves visible. | For rooms that are not dark. | off (experimental) |
| `intent_black`, `intent_display` | Shadow-loss measure, LED shadow protection, a mapping readout. | Experimental diagnostics. | off |

**Planned:**
- `combined_hdr_lut`: a cheap static HDR->SDR LUT for low-end cards (Classic/Low).
- **Stage 13 appearance** (`appearance_adaptation` and its settings): CAM16 dim-screen adaptation.
  It stays off until the i1Pro screen-white measurement.

---

## 7. Dynamic Drive

*Projector LED and contrast (CNTR) control from the picture. It is set up on the Dynamic Drive
page, not tiered.* The page has **Main** (Dynamic Drive on/off and its command source, the LED
profile, the LED speeds preset, the projector connection) and **Advanced** (the speeds' values, set
by the preset until Custom; Contrast and cuts; the Advanced LED masters and its five groups, each a
small card set by "Advanced LED values" until Custom; projector response and limits; Experimental).
Cards a command source does not use stay hidden.

**Command source** (`command_source`):
- *internal_script*: VP's copy of your script, used as an intent reference rather than for its
  coding limits.
- *compensate*: VP dims the LED and lifts the picture to compensate, using look-ahead.
- *none*: no LED control.

Key groups (Dynamic Drive and Projector pages):

| Group | Keys | What / why |
|---|---|---|
| Switch | `enabled`, `lookahead`, `led_lookahead_ms` | Dynamic Drive on; look-ahead lets the LED land with the picture. |
| Measurement | `led_peak_full_coverage`, `led_picture_only`, `highlight_percentile`, `highlight_persist_ms`, `apl_persist_ms`, `highlight_area_small/large`, `highlight_release_ms`, `led_highlight_grain_suppress`, `apl_weights` | What counts as the picture's level and its highlights: the picture only (not subtitles), every pixel, grain ignored. |
| Script | `script_pacing`, `script_lookahead`, `script_measure_ms`, `script_echo_ms`, `script_highlight_code`, `script_resend_failed_writes`, `tick_ms`, `curve`, `steps_*`, `rate_*` | Reproduces the original script's rhythm and decisions. |
| Compensate | `adaptive_gamma`, `compensation_eotf`, `knee`, `deadband`, `anti_pump_band`, `gamma_boost(_knee/_width)`, `shadow_lift_ansi*`, `cntr_assist(_max)`, `software_first` | How the picture gain makes up for the dimmer LED. |
| Limits | `led_floor`, `led_floor_reactive`, `led_floor_black`, `led_ceiling`, `black_prelift_max`, `highlight_tolerance_stops` | The light range VP may use. |
| Cuts and fades | `cut_jump`, `bright_cut_dump`, `cut_threshold`, `cut_cntr_dump`, `cut_cntr_level`, `shaped_recovery`, `max_cut_dip`, `recovery_bias_pct`, `early_ramp`, `fade_ms`, `fade_min_slope`, `cut_lead`, `ftb_*` | Behaviour on cuts, fade-ups and fades to black. |
| Timing | `led_latency_ms`, `cntr_latency_ms`, `led_landing_ms`, `cntr_pair_predict`, `correction_weight_by_time` | The picture gain is timed to land with the light change (about 80 ms). |
| Pass-through | `led_btb_passthrough`, `led_wtw_passthrough`, `preposition_black_only` | Below-black and above-white untouched under a boost. |
| Projector | `protocol`, `read_back`, `ack_timeout_ms`, `min_command_gap_ms`, `max_levels_per_command`, `burst_*`, `echo`, `terminator`, `light_min/max`, `cntr_max_usable`, `dmd_clip_cntr`, `flux_table`, settle times | The RS-232 link and the projector's measured response. |
| Idle | `led_idle_restore_s` | Full light after the video stops. |

**Rules that shape the defaults:**
- CNTR is never left above the level the picture needs.
- The script is the intent reference.
- Read-back is off: send only, like the original script.

**Planned:** `black_level_correction`, which compensates the projector's black floor as the LED and
CNTR change. Not yet implemented.

---

## 8. Output

*The calibration LUT, the dither for the 8-bit projector link, and the presentation.*

| Key | What it does | Why it exists | Default | Cost |
|---|---|---|---|---|
| `temporal_dither` | *sigma_delta*: VP's temporal sigma-delta, which spreads the finer-than-8-bit steps over time along the motion. *libplacebo*: its own dither. *auto*: VP decides. | The FL35 link is 8-bit, and the sigma-delta is what keeps 10-bit smoothness through it. | auto (Low tier: sigma_delta) | low |
| `dithering` | libplacebo's dither pattern (blue noise etc.), used when libplacebo dithers. | Upstream option. | auto | tiny |
| `display_bit_depth` | Dither target depth (8/10). | Clamped to the surface depth. | auto | none |
| `occlusion_mask` | Tells the sigma-delta where new picture appears, so it does not carry a residual into it. | No smears at edges of moving objects. | off | low |
| `dither_grain_tolerance_scale` | The sigma-delta's motion match tolerates grain this many times more. | Keeps the 10-bit averaging alive on grain. | 3 | none |
| `dither_vectors` | The dither takes the de-judder's motion on in-between pictures (auto) or always searches itself (own). | Saves about 1 ms per in-between. | auto | saves time |
| Calibration LUT (Color / Output page) | The projector calibration LUT. | Today libplacebo applies it before VP's output hooks; the design moves it to VP's last step (planned `calibration_lut_stage`). | | low |
| `render_ahead` and its companions | See 3.3. | | off | |

---

## 9. Upstream (Bill's) settings, in brief

These belong to the upstream program and keep their own pages. VP-V does not reformat them.

- **General:** capture device and input, start fullscreen / minimized / video-only, the statistics
  overlay, the frame queue and its auto reset, refresh-rate switching, the monitor, profile updates,
  scene detection, file A/V offset.
- **Screen and zoom:** crop, aspect, A-lens, zoom/viewport, subtitle fit.
- **Subtitles:** detection, placement and style.
- **Motion page:** *Motion handling* (VP de-judder vs libplacebo smooth motion), smooth motion
  mode, mixing filter, deinterlacing (mode, algorithm, field order, cadence).
- **Scaling page:** upscaler, downscaler, chroma upscaler, sigmoid, downscale light.
- **Tone mapping:** curve, gamut mapping, peak detection, contrast recovery, saturation, spline and
  peak-detection tuning, highlight response (beta).
- **DirectShow, LLDV, Shortcuts, Actions, Logs, Updates, Calibration (meter, patches, wizard).**

---

## 10. Similar names, different jobs

**`render_ahead` vs `motion_ahead_frames` vs `dejudder_estimate_ahead`**
- `motion_ahead_frames`: the NVIDIA engine computes motion vectors **ahead of the picture**, from
  queued frames. This is about *when the vectors exist*.
- `dejudder_estimate_ahead`: VP's own matcher works out the **next frame's** motion in the render
  thread's spare time. This is load balancing.
- `render_ahead`: the **finished in-between pictures** are queued for display ahead of time. This is
  about *presentation*. It uses part of the delay budget, and nothing about motion changes.

**`deband` vs `deband_strength` vs `deband_vp`**
- `deband_strength`: libplacebo's debander (Auto / Default / Light / Off).
- `deband`: its legacy on/off key. Config reads it and replaces it with `deband_strength`.
- `deband_vp`: VP's own grain-safe debander, which runs before scaling at the source size.
  With `deband_vp_exclusive` on, it switches libplacebo's *automatic* deband off.

**`detail_pulse_stabilise` vs `texture_restore`**
- Both raise a frame's detail toward a better-coded neighbour, but at different scales.
- **Detail pulse stabilise** fixes the short **breathing** (frame to frame, within a group of
  pictures). It is cheap and on by default.
- **Texture restore** separates grain from texture and rebuilds smeared texture over about
  **2 seconds** along the motion. It is expensive and belongs to the Upper tier.
- Neither invents detail the stream never had.

**`source_resolution` vs re-base vs `rebase_kernel_match`**
- `source_resolution` decides **whether and to what size** the picture is shrunk back (auto,
  native, or a size).
- The **re-base** is the act of shrinking it back to the encoded size.
- `rebase_kernel_match` decides **how**: by undoing the player's exact upscale filter (on), or with
  a generic Lanczos downscale (off).
- The planned `entrance_rebase` key will be the re-base slot's own switch.

**`capture_path` vs `capture_upload` vs `native_rgb_ingest`**
- `capture_path`: **where** YCbCr frames (v210/UYVY) are unpacked: GPU or CPU.
- `native_rgb_ingest`: the same for **RGB** frames (r210, R12B/R12L): full RGB on the GPU, or the
  old CPU squeeze to 4:2:0.
- `capture_upload`: **when** a frame is copied to the GPU: on arrival, or at render.

**Smooth motion vs interpolation vs de-judder vs hiccup repair**
- **Smooth motion** (libplacebo): blends neighbouring frames by time. It is simple, but makes double
  images on motion.
- **De-judder** (VP): film only. Moves the in-between pictures along the motion vectors to ease the
  3:2 hitch, by the amount the motion slider sets.
- **Interpolation** (VP): video only (50/60p and other video rates). Builds true in-between
  frames from the motion vectors where the rate does not divide the display.
- **Hiccup repair** (`interpolation_fill_gaps`): fills frames a stream **lost**, whatever the
  motion settings.
- Smooth motion and VP's motion are one or the other per profile, never both.

**LED compensate vs Dynamic Drive vs CNTR**
- **Dynamic Drive** is the whole unit that controls the projector's light from the picture.
- **Compensate** is one of its command sources: VP dims the LED and raises the picture gain so the
  image looks unchanged, with deeper blacks.
- **CNTR** is the projector's own contrast control. Dynamic Drive uses it as an assist
  (`cntr_assist`), and the rule is that it is never left above what the picture needs.

**`chroma_upscaler` vs the CfL shader**
- `chroma_upscaler` (Scaling page): **libplacebo's** filter for upsampling the colour planes
  (4:2:0 / 4:2:2 to full resolution).
- The **CfL shader** ("chroma from luma", a user shader in the cinema configuration) predicts colour
  detail from the luma instead. When it is loaded it takes over that job inside libplacebo.
- The planned **VP chroma upscale slot** (`chroma_upscale_vp`) would do it in VP's front end, so
  libplacebo would receive 4:4:4.

---

## 11. Tooltip audit

The guide was checked against every Config tooltip. Corrections applied in this change:

| Key | Problem | Fix |
|---|---|---|
| `motion_ahead_frames` | Said On "makes the queue hold at least 5 frames". The forced minimum is gone (r6 fixes) and the depth is now a time. | Rewritten: time-based depth, never lengthens the delay, degrades on a short queue. |
| `target_latency_ms` | Called it a queue delay; did not state the recommended value or the one-total-delay rule. | States one total delay, the recommended 250 ms (4 frames at 24p) and the 3/2-frame fallback; points to the VP-V Input tab. |
| `deband_strength` | Had no tooltip. | Added: libplacebo's debander, and its relation to VP deband. |
| `adaptive_w_base` | Did not say the design value. | Adds "the hybrid design value is 0.2". |
| `dejudder_strength` | The field had no tooltip; only the slider did. | Added: 0 = as shot, 1 = full conversion; normally set by the slider. |

Checked and found correct: `temporal_dither` (its buttons carry the tips; the drop-down only holds the value), `adaptive_faithful` (only the explicit `polar` value is retired; Auto still uses polar where both axes shrink alike), the capture keys, the deband guard and exclusivity, detail pulse,
texture restore, source resolution and its guards, interpolation and hiccup repair, render-ahead,
the dither keys, extended range, the SDR tone shape, and the optical-flow keys.

---

## 12. Settings to retire (for your approval)

You said 124 keys was too many and that you hate a "bazillion ambiguous options". These are the
candidates. Retiring a key keeps the behaviour of its default and keeps reading old files.

**A. Fixes under test that are proven: fold the fix in, drop the switch.**
`pulldown_fixes`, `timing_continuity_fixes`, `cadence_class_fixes`, `motion_history_fixes`,
`queue_timing_fixes`, `general_rate_fixes`, `efficiency_fixes`, `source_coding_fixes`,
`source_resolution_latch_fixes`, `source_resolution_cut_fixes`, `active_picture_startup_hold`,
`extended_range_more_cases`, `dv_tunnel_fixes`, `residual_stabilise_grain_guard`,
`magewell_recover_faults`, `capture_display_fixes`, the LED "(fix under test)" switches.
Retire each after one clean chain gate with it on.

**B. Experiments that lost.**
- `optical_flow_cost_trust`: no measurable difference.
- `dejudder_warp_stage=source`: the old path, kept only for A/B.
- `dejudder_occlusion_fill`, `dejudder_block_taper`, `dejudder_taper_sad`: off, experimental.
- `interpolation_grain=blend`, `dejudder_source_warp_filter=bilinear`, `nls_sampling=bilinear`:
  options that lose grain.
- `deband_grain`: VP's dither does that job.
- `adaptive_faithful=polar` (the explicit value): already runs as Auto; remove it from the list.

**C. Superseded by the new design.**
- `render_ahead_queue_credit`: render-ahead now always counts inside the total delay.
- `motion_ahead_frames`: becomes time-based (planned `motion_ahead_ms`).
- `target_frames` for normal profiles: one total delay replaces it.
- `detail_pulse_fast`: the picture is identical, so it can always be on.
- `dither_vectors`: auto always does the right thing.

**D. Keep, but hide as diagnostics.**
The "measured - leave alone" motion estimator internals (3.1), the adaptive-scaler thresholds
(5.3), and `dejudder_verify(_share)`.
