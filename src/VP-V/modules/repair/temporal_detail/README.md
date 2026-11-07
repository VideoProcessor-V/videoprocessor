# TemporalDetail (`repair.temporal_detail`, DRAFT)

Unit **repair** | replaces `repair.texture_restore` (order 90) | status **draft**: CPU model + GPU passes (WARP-matched), no call sites | hook: prepass

## Purpose

I/reference frames carry more real fine structure than the B-frames around them. TemporalDetail
**transfers** the best recent frames' real structure forward along the motion, warped once. It works on the shared
grain-free picture (`repair.grain_split`) and adds the current frame's own grain back exactly, so it never invents
detail, never averages, scales, freezes or replaces the current grain, and never undoes intended softness (focus, motion blur).
Owner grain bounds (asserted): grain RMS within 5%, whole-field grain temporal bands within 0.95-1.05, frozen-grain lag-1
correlation increase <= 0.05.

## Contract

- Input: luma after net A (where on) and the re-base, before scaling (re-based plane). Chroma passes through.
- Motion: an `IMotionSource`: today's vector field, or the camera model through an adapter compiled only when its header exists.
- Output: same size and format, delay 0; look-ahead optional (queued modes).
- Off (`temporal_detail = off`): the module does not run, output = input byte-identical.
- Active picture (owner 2026-10-31, after the detail pulse's bar halos): `FrameIn::active` / `Gpu::Params::active` is the
  renderer's trusted black-bar / picture rect scaled to the plane. Everything (split, statistics, gates, templates,
  transfer, boost, cleaner, grain guard, add-back) runs on that rect with its border as the plane edge (no tap reaches
  into a bar; bar blocks do not exist for the statistics); bar pixels are copied bit-exact. Moving the rect at equal size
  resets the store (geometry). Motion grids stay in plane coordinates (`MotionPair::originX/Y`).
- Motion sources are bounded (long-run hunt 2026-10-31): `FieldMotionSource` keeps a 128-frame window behind its newest
  pair, `Prune(presented)` drops everything older than the presented frame (the model never re-reads an older pair:
  composition is incremental), `Clear()` at resets; the camera adapter prunes its records with them.
- Measurement knobs (`Model::Tuning`, never user settings): grain budget, cleaner, guard, the gate thresholds
  `Tuning::gates` (`strongerMargin`, `baseNoiseCodes`, `baseGrainSigmas`, `uncertaintyPx`; defaults = the constants) and
  `forceWeight` (the transfer ceiling, measurement only). `tools/td-cli` drives the CPU model on real encodes with them
  (test/temporal-detail-real; its own mirrored active-picture wrapper was superseded by `FrameIn::active` at integration).
- Measurement ablation (test/td-real-vectors; `Model::Tuning`, never user settings): `gatesOff` (gate ablation bits
  `Tuning::GateBits`: each set bit forces that gate open; 0 = the designed model, byte-identical) and `typeAdmission`
  (admission from the TRUE coded frame type `FrameIn::codedType`, offline only; false = byte-identical). `td_cli` reads
  both from its header and the FRM1 flags (4 I, 8 P, 16 B); per-block `Diagnostics::refSeq` names the reference frame; the gate bits also change the template cleaner's member selection, `kGateAge` opens the age gate only up to 4 x `maxAge`, and only `--stdio` takes the knobs (stats JSON ends with `q_mean` and `refs`).

## Algorithm (CPU reference model `TemporalDetailModel.h`; shaders mirror it)

Scale `s = GrainScale(planeLines)` (1 at <= 2160 lines, lines/2160 above). Block = 16 px x s. Every
frame count follows the measured content rate (`Frames(seconds, hz)`); no GOP, cadence or resolution is assumed.

1. **Split (shared product `grain.split`, repair.grain_split).** `base = G(2.5 s) Y`, `D = Y - base`, fine band
   `D1 = Y - G(1 s) Y`, mid band `D2 = D - D1`, block grain level `N`, Wiener share `f = clamp(1 - kappa N / e3x3, 0, 1)`:
   **grain-free structure `S = f D` (bands `S1 = f D1`, `S2 = f D2`), `clean = base + S`, grain `G = Y - clean`.**
   Computed once per frame and shared; TemporalDetail reads it, never recomputes it.
2. **Block measures.** Structure energies `E1`, `E2` (fine, mid), focus/acutance `F = E2 / (E2 + E3)`
   (E3 = the low-mid band), quality `q = E1 + e` (fine structure energy), and
   optional hints (per-block bitrate quality); absent = neutral.
3. **Coding-pattern evidence (FOCUS GUARD).** Transfer is allowed only where the current softness
   looks like coding:
   - *high-band-only loss*: against the warped reference, `E2cur / E2ref >= rho2` (coding keeps edges)
     while `E1cur < E1ref`; focus and motion blur lower the mid band too, so they fail here;
   - *focus gate*: `Fcur >= kPull Fref` (texture_restore's);
   - *pulse evidence* (measured): a per-block rebound counter. A rebound is a rise of
     `q` by >= tau after a fall; counts decay with half-life `max_age / 2`. Evidence =
     smoothstep(0.5, 1.5, rebounds). A region whose detail only falls has no evidence;
   - *referee* (queued modes): if the best future frame in the queue is also soft in the block
     (`qfut < kSoft qref`) there is no transfer;
   - *retire*: `q` and `E2` both falling for `Frames(0.2 s)` consecutive frames, or the guard vetoing the
     reference for `Frames(0.2 s)`, retires the
     block's reference (gradual fade, below).
4. **Reference store** (product `td.refstore`). A ring of `K = 5` unwarped past slots (the grain-free bands S1/S2 and the
   half-resolution base of a selected frame) plus one future slot in queued modes. Per block: the slot
   index, `qref`, age, accumulated uncertainty `u`, retire flag and crossfade state. Per slot: a composed
   motion map (block grid) from the current frame back to that slot. A block **refreshes** to the current frame
   when `q >= qref decay(age)` and the current block is not soft by the guard; the refresh frame
   transfers nothing and the weight holds. A new slot is
   admitted when at least one block selects it; eviction takes the slot with no live blocks, else the
   oldest whose blocks have faded. A refresh **crossfades** from the old slot to the new over
   `Frames(0.1 s)` (no pop at each I/P refresh).
4a. **Multi-frame template cleaner** (once per admitted slot). The single-frame Wiener split leaves the grain of dense texture
   in `S` (f near 1 there); a slot carrying it would freeze that grain into every frame it serves. At admission the new slot's
   bands are cleaned by a motion-aligned spatio-temporal Wiener in local DCT patches (P = 8 px x s, sine window, half-patch
   step): grain is temporally independent, structure coherent along motion. Per coefficient, `X0` = the new slot's `S`,
   `Xm` = each other slot that aligns there (uncertainty x base match x correlation gates >= 0.5, within max_age), sampled
   exactly as the template samples it (warped once to the current frame, lighting gain) and used as evidence only. The cross
   terms `X0 Xm` estimate the structure power without grain bias:
   `P = max(0, (X0^2 - V0 + sum_m X0 Xm) / (1 + M))`, `W = P / (P + kappa V0)` (kappa 4), `V0 = N hs mean(window^2 f^2)` with
   `hs` the D band's grain spectrum shape (N = the split's grain level). The cleaned bands `c1`, `c2` are stored beside the raw
   ones (the gates keep reading the raw bands; the template transfers the cleaned ones), with a per-block residual grain share
   `rho2 = sum W^2 V0 / sum N hs mean(window^2)` (residual grain energy over one full frame's grain). Patches are zero outside
   the plane (replicated edge pixels would read as structure). Nothing is re-warped; future sources stay uncleaned (rho2 = 1).
5. **Composed motion, warp once.** Backward vectors `Vb_n` (frame n to n-1). Composition on the block grid:
   `C_n(p) = Vb_n(p) + C_{n-1}(p + Vb_n(p))` (bilinear at the grid). Camera-labelled blocks with a usable
   camera model use the composed homography instead (exact, no grid error). Each hop adds
   uncertainty `(1 - trust) * block + |Vf + Vb|` (forward/backward disagreement when forward vectors exist).
   The structure is sampled **once** from the unwarped slot with a 4-tap Catmull-Rom filter at `p + C(p)`; nothing
   is ever re-warped.
6. **Validation** (per block, all smooth gates multiply): age `<= max_age` with a linear fade over its last
   half; `u <= 0.5 px * s`; warped base matches (`mean |base - base_w| <=` coding-noise + grain allowance;
   this is the occlusion/ghosting veto); structure positively correlated (`corr(S, S_w) >= 0.3`) and the
   reference stronger (`E1(S_w) > (1 + m) E1(S)`) with fine energy 2-5 x the grain level N (grain-only blocks
   never serve); coding-pattern evidence; cut/fade/stamp reset.
7. **Smoothed weight (FLICKER-FREE).** Soft gates (strength, pulse evidence, age, retire, reference stronger)
   give the target `t`. Per block a weight with hysteresis (enter above 0.15, leave below 0.05) and a rate limit
   (rise `<= 1 / Frames(0.15 s)`, fall `<= 1 / Frames(0.3 s)` per frame). Age-out and retire fade through the
   limiter, never a step. Hard vetoes (base match/occlusion, uncertainty, correlation, reference weaker than the
   current block, high-band loss, focus, referee) multiply the result: instant fall, limited rise.
8. **Transfer on the grain-free picture.** Per block a **template** `T_b = sum_k c_k g_k S_b,k(p + C_k(p))` over every
   valid stored slot (hard gates >= 0.5, within max_age; the selected reference always, crossfading from the previous one)
   or the future source alone; each member is sampled once from its unwarped slot (warp once), `g_k = clamp(base / base_w,k,
   0.8, 1.25)` (lighting), `c_k` the q-weighted best linear estimate of a common structure under independent grain, scaled to
   the selected reference's amplitude (`c_k = a_sel a_k / sum a_j^2`, `a_k = sqrt(E1 of slot k)`). The current frame enters
   only as its per-band **projection** `lambda_b = <S_b, T_b> / <T_b, T_b>` (block sums, clamped 0..1) and coherence
   `rho_b = corr(S_b, T_b)`: `tau_b = (1 - lambda_b) smoothstep(0.3, 0.6, rho_b)`. Then
   `delta = beta sum_blocks om w (tau_1 T_1 + tau_2 T_2)` (weights bilinear between block centres).
   **Template grain budget:** the template keeps `sum c_k^2 rho2_k` of one member's grain (step 4a's residual);
   `beta = min(1, kGrainBudget / sum om w max(tau) sqrt(sum c^2 rho2))` keeps that foreign grain <= 0.08 x the current grain
   RMS (Model::Tuning::grainBudget = 0 runs the designed strength and templateClean = false the single-frame bands, for measurement).
   **Amplitude limit** on the grain-free prediction: `|P + delta| <= 1.1 max(|P|, |T|)`, `P = lambda T` (the current
   pixel's own value is never clamped). **Fallback boost** where no valid reference exists but the guard (referee included)
   holds: `+ b S`, `b <= boost_max` (0.15), with `b f <= kBoostGrainShare` (0.04) so the current grain scales by at most 4%.
9. **Recompose: add the current grain back exactly.** `Y_out = TD(clean) + G = Y + delta`. A pixel with nothing to add is
   copied unchanged (bit-exact). Diagnostics: `structureOut = S + delta`, `grainOut = G` (plus any guard add-back).
10. **Output-vs-input grain guard** (safety net on top of 8-9). On the same grain map, per block (pooled 3 x 3, EMA over
   `Frames(0.25 s)`), over grain-dominated pixels (`f < 0.2`): `G_out = G_in + (1 - f) delta`, the level ratio, the
   projection `<(1 - f) delta, G_in> / <G_in, G_in>` (current grain removed) and the lag-1 correlation of `G_out` / `G_in`
   against the previous frame's, one hop back along the backward vectors (measurement only). The guard reads `delta` less the
   template's structure transfer at pixels where the blended (cleaned) template carries structure (`T^2 > 0.25 N`): the map's
   `G` holds some structure at low-f pixels, and restoring it there is not grain damage (it tripped the guard on legitimate
   transfer and cost 0.6 dB with no effect on the bounds); the boost, the add-back and any other change stay visible
   everywhere. (a) A measured loss of current grain above 1% is added back at once (`delta += k G`). (b) A violation (loss
   > 0.04, rise > 0.05, lag-1 increase > 0.03, made possible by the cleaner statistics) trips the block (hysteresis: released
   below half), its weight factor falls to `guard / v` (rate `1 / Frames(0.1 s)`) and recovers over `Frames(2 s)` (no
   trip/release cycling under damage, which the two-seed isolate reads as a low-band rise). Counted (`guardTrips`, `addBackBlocks`) and visible in the status
   (`; grain guard: N blocks, add-back M`). The guard cannot see grain hidden in dense texture (no spatial map can); the
   template budget covers that.

Two-sided: in queued modes the future slot is the best frame in the queue window, warped once by composed
forward vectors; it serves as referee (step 3) and as a reference for the last B-frames of a group and for
revealed regions (the better-validated of past/future, never both summed).

## Indicator stamps and resets

Each frame carries a `StampView` (full, continuity, rebase H/V, settings[repair]) that the
host fills, later from `core/Indicator` (doc 25 §A2). Policies: `full`, `continuity` (pairs spanning a slip are
invalid) and `rebase[H|V]` reset; `settings[repair]` resets only for
mode/max_age changes (strength/boost change the target); `timing` is ignored. Cuts and fades reset the store.

## Declared products (core/Products.h)

The module's former private descriptors (TemporalDetailProducts.h) were retired: its products are registered in
`core/Products.h` (`AppendTemporalDetailProducts`; ids `kTdAnalysis`, `kTdRefstore`, `kTdAdmission`) and listed in
`docs/design/product-graph.md`. `td.out` (the output luma plane) is a render-scope temporary, not a product.

| Product | Inputs | Backend | History | Size at 4K |
|---|---|---|---|---|
| `td.analysis` (block maps, rebounds, softening run) | P10 grain split, P11 block stats, P13 source ring | GpuCompute (T1) | 1 | ~1.5 MB |
| `td.refstore` (K past slots, per-block state) | td.analysis, P10, P3, P13 | GpuCompute (T1) | K = 5 | declared 10 B/px per slot = 83 MB at 4K (what the code allocates today; the SA6 compact form targets 4.5 B/px) |
| `td.admission` (cleaner / templates on admission frames) | td.analysis, P10, P13 | GpuCompute (T2 gap) | 0 | arena (SA1) |

Cleaner cost (estimate, RTX 3090 at 4K, not dispatched): only on admission frames (a refresh, e.g. 2 of 8 frames in IBBBPBBB):
~0.52 M half-step 8 x 8 patches x (2 + M <= 4 forward, 2 inverse) separable DCTs ~ 4 GMAC plus one sample of each aligned
slot per pixel (the template's own sampling), ~0.5-1 ms per admission frame, ~0.2 ms averaged; nothing on other frames.
Memory +33 MB per slot (+166 MB for K = 5); folding the gates onto the cleaned bands (raw energy = cleaned + a per-block
removed-energy grid) would drop the raw bands and make it ~0.

## Settings

Declared, not wired (no renderer call site yet; `TemporalDetail::ParseSettings` validates them). No key was added by the
scratch-arena work (SA6): the arena is passed by the host in `Gpu::Params::arena`, not set by a key.

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| `temporal_detail` | profile | enum | off, steady, auto | off | not in Config (module not wired) | n/a (VP-only) | declared: "Temporal detail: inactive (<reason>)"; `Status()` line | budget-governed: the VRAM governor keeps fewer reference slots first (5 down to 1, `Params::slots`), then drops the scratch-arena temporaries (T1), then the module; nothing is fixed to a GPU model |
| `temporal_detail_boost` | profile | bool | on, off (the fallback boost) | off | not in Config (module not wired) | n/a (VP-only) | declared: off unless re-validated on real encodes (owner 2026-10-31) | same as `temporal_detail` |
| `temporal_detail_strength` | profile | float | 0.0 .. 1.0 | 1.0 | not in Config (module not wired) | n/a (VP-only) | declared | same as `temporal_detail` |
| `temporal_detail_max_age` | profile | float | 0.25 .. 4.0 (seconds) | 2.0 | not in Config (module not wired) | n/a (VP-only) | declared | same as `temporal_detail` |
| `temporal_detail_boost_max` | profile | float | 0.0 .. 0.3 | 0.15 | not in Config (module not wired) | n/a (VP-only) | declared; used when the boost is on | same as `temporal_detail` |

**auto** (owner 2026-10-31): `AutoGate` (TemporalDetail.h) reads the source-ID consensus quality tier
(input.source_analysis identity, `SourceIdConsensus` Result.value.qualityTier, 0 = lowest .. 3, -1 unknown) and engages
after the tier has been <= 1 (bitrate quality below 0.5) for 1 s, releases after 3 s above it or unknown; idle = the input
bit-exact, no pass; `Status()` is the visible line ("auto: on (stream quality tier 1)", "auto: idle (...)"). The host
calls `Update(tier, contentHz)` per presented frame and passes `autoEngaged` (FrameIn / Gpu::Params); the store restarts
on each engagement. Gate thresholds that real B-frames will re-tune are tunables (`GateTuning`: reference-stronger margin,
significance band, base-match noise and grain allowance, motion-uncertainty limit) in `Model::Tuning::gates` and
`Gpu::Params::gates`, defaults = the constants above.

**Regionally adaptive gates** (owner 2026-10-31): the thresholds and strength take per-block factors on the active
picture's block grid (`FrameIn::blockGates` / `Gpu::Params::blockGates`, 4 per block: reference-stronger margin, base-match
allowance, motion-uncertainty limit, strength; absent = 1, the global `GateTuning`). The re-tune team's rule fills them
from per-block maps the module already has or the host owns: local texture deficit (q against its decayed peak, td.analysis /
the block state), vector / camera confidence (pair trust, composed uncertainty u, camera labels), grain level N
(grain.split blocks), brightness (block mean of the base band) and the frame's I/B phase (the rebound evidence). Applied
identically in the model and the GPU passes (`TemporalDetailGpuBlockGatesFollowTheModel`).

`TemporalDetail::ParseSettings` validates them (an invalid value keeps the previous one). They are not in
RendererProfileConfig, slots.json, the tier table or the Config editor yet.

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| (none) | | | |

## Products

Declared products and their sizes: see "Declared products" above. What the GPU module reads and holds (SA6):

| Item | Id / owner | Fill rule | Consumer fallback when absent |
|---|---|---|---|
| grain split | P10 `grain.split` (unit version `GrainSplit::kUnitVersion`), read through `gs_fetch` / `gs_blocks` | lane precompute or first-consumer live fill (the host's node) | the module's own Full split, visible ("grain split (temporal detail)"); its output textures are freed once the shared product has fitted for `kSplitReleaseSeconds` (1 s of content at the measured rate), and re-created, visibly, if it drops out again |
| scratch temporaries | arena scope `kArenaScope` (0x0401), owner `temporal_detail`, tier T1: 3 x rg32f at the active picture's size (two cleaner accumulators, the transferred delta; 1 without the cleaner) | requested by `Module::ArenaRequests(gpu, w, h, planeLines, templateClean, ownSplit)`; acquired per Apply after the own split's scope has closed | no arena (`Params::arena` null): private textures, byte-identical output; arena short: the frame fails visibly (see below) |
| own split intermediates | `grain_split` T0 requests of `Splitter::ArenaRequests` (merged into the list when `ownSplit`) | the Splitter's own scope | as above |
| previous-frame grain `prevg` | module texture (rg32f, persistent) | written by the recompose pass of every frame | the guard's lag-1 sums are skipped for a frame without it (`TD_LAG` off) |

Sizes at 4K (estimates, decimal MB): the arena temporaries are 3 x 66 = 199 MB plus the own split's bands 133 MB; they are the
arena's (`TextureBytes(..., tempsInArena = true)` is 332 MB smaller than the private form). The arena keeps one pool per texture
shape sized by its busiest scope, so a temporary only saves memory where another stage uses the same shape in another scope (the
own split's bands share the grain split producer's; the rg32f picture-size pool has no other user today). The freed own split
output (16 B per pixel, 133 MB at 4K) is a saving whenever the shared product fits.

`prevg` stays a module texture. It is not derivable from the shared product: its input half is `Y(N-1) - (base + s1 + s2)(N-1)`
and needs the previous frame's source luma, and its output half includes this module's own delta of that frame. Moving it to a
P10 history ring needs the P13 source ring (SA10) and a deliberate change of the guard's lag-1 law (new unit version), not a
move of the same arithmetic; it is left as an open item of SA6.

## Status and fallbacks

| Visible downgrade | Where it shows |
|---|---|
| own grain split instead of the shared product (Missing, Stale, WrongSize, Shed) | `Status()` "own grain split", catalog entry "grain split (temporal detail)", Ctrl+I line |
| scratch memory short (arena empty, T1 shed by the governor, nested scope, other thread) | `Apply` returns false for the frame: `LastError()` "temporal detail: scratch memory short (...)", `ScratchShort()`, the arena's catalog entry "scratch_arena" naming `temporal_detail` or `grain_split`, the failure counted in `Failures()`; the host renders the frame as it was; the next frame restarts the store (the skipped frame breaks the sequence) and recovers by itself once a larger plan is reserved |
| fewer reference slots (memory tier) | `Status()` "store N of 5 slots (memory tier)" |
| look-ahead / camera model not on the GPU path | `Status()` "not on GPU: ..." |

## Tests

Classes `TemporalDetailGpuTests` (GPU passes on WARP in the CPU tier unless `VP_TEST_GPU=hardware`) and `TemporalDetailModelTests`
(CPU model). SA6: `TemporalDetailGpuArenaIsByteIdentical` (arena output equals private output: own split, shared product, product
from frame 3), `TemporalDetailGpuArenaShortageIsVisibleAndRecovers` (empty plan and tier shed), `TemporalDetailGpuReleasesTheOwnSplitOnceTheSharedProductFits`, `TemporalDetailGpuTextureBytesCountsTheArenaTemporaries` (CPU). Command:
`vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:"Name~TemporalDetailGpu" /Logger:trx` (read the TRX counters).

## Determinism, generality, known limits

Fixed-order loops and double sums; shaders reduce in a fixed tree. No hard-coded rates,
cadences or resolutions; tests cover 1080-4320-line scales and 24-60 Hz.

Measured (model, synthetic I/B scene, four grain seeds; strict bounds on the two-seed grain isolate of the KeepsGrain scene):
default **6.3 dB** B-frame PSNR gain at the strict bounds (grain RMS 1.013, lag-1 +0.012, whole-field grain bands
1.045/1.000/1.009/1.005, no guard trips); the single-frame template holds them up to 3.7 dB at its edge budget 0.085 (3.0 dB at its former default 0.07).

| budget | multi-frame cleaner: gain, low band, lag-1 | single-frame template: gain, low band, lag-1 |
|---|---|---|
| 0.05 | 3.9 dB, 1.031, +0.008 | 2.1 dB, 1.032, +0.012 |
| 0.07 | 5.6 dB, 1.040, +0.011 | 3.0 dB, 1.042, +0.017 |
| **0.08** (default) | **6.3 dB, 1.045, +0.012** | 3.5 dB, 1.046, +0.020 |
| 0.085 | 6.5 dB, 1.048, +0.013 (edge) | 3.7 dB, 1.048, +0.021 |
| 0.09 | 6.7 dB, 1.050, +0.014 (fails at 3 decimals) | 4.0 dB, 1.051 (fails) |
| 0.10 | 6.9 dB, 1.053 (fails) | - |
| none | 6.9 dB, 1.057 (fails) | 5.7 dB, 1.125, +0.056 (fails) |

The low temporal band binds everywhere (RMS stays <= 1.014, lag-1 <= +0.015); grain RMS and the higher bands include the
fallback boost's <= 4% share (1.012 with no transfer at all). Kappa 2 .. 8 lies within 0.2 dB. The pre-fix draft's 10.5 dB was 9.8 dB structure plus a grain swap (own grain kept 31%, 73% of the
grain energy foreign/frozen); the first fix's 2.9 dB kept 80% own grain with 25% foreign.

Real streaming-like encodes (x265 IBBBP, 2160p 4-16 Mb/s, 1080p 1.5-6 Mb/s, 4 masters; `tools/eval/prototypes/
temporal_detail_real/`, CPU model with DIS stand-in vectors): the transfer is inert, B-frame PSNR change <= +0.001 dB at
every tier and every swept threshold (stronger margin 0.02-0.15, base allowance x1-x3, uncertainty 0.5-1 px); the
transfer ceiling (`Tuning::forceWeight` 1) is -0.02 dB, a per-block master-oracle strength only +0.04 dB, and a fitted
local rule (deficit, trust, grain, brightness, I/B phase) -0.008 dB on held-out clips. Real B-frame loss is not the
fine-band attenuation the coding simulation models. The fallback boost is the only active part (-0.004 dB, +2 % grain).

Limits: the cleaner removes most, not all, of the grain the split reads as texture (the rest is budgeted); the first slot
after a reset has no evidence (single-frame DCT Wiener); future sources are uncleaned (budgeted at rho2 = 1); the grain level needs quiet areas within 80 px (dense texture everywhere raises N, f
falls, transfer fails safe); block weights reach half a block past an edge of a soft region.

## Migration (retiring detail_pulse_stabilise / texture_restore)

`detail_pulse_stabilise`: its gain law survives only as the bounded fallback boost; history/look-ahead plumbing is
reused. `texture_restore`: its grain level + Wiener split become the shared `repair.grain_split` product, its focus gate
part of the guard; the grain boost is dropped.

Integration: rename the slot to `repair.temporal_detail`; map `detail_pulse_stabilise = on` to `temporal_detail =
steady` and `detail_pulse_cap` to `_boost_max = cap - 1` (clamped 0.3); drop `texture_restore*` with a logged
notice; Original pathway sets `temporal_detail = off`; reuse the host plumbing (crossfade, Forget triggers,
DetailLookAhead, status strings). Old keys are read for one release. `detail_pulse_stabilise = on` maps to `temporal_detail = steady` with `temporal_detail_boost = off` until the boost is re-validated. A repeat (same
sequence) returns the last output (repeat/foreign/phase pictures come at integration); `CameraModelMotionSource` stays uncompiled until feat/camera-model merges.

## GPU implementation (TemporalDetailGpu.h/.cpp, TemporalDetailShaders.h)

**Grain map: one producer, many readers (owner rule 2026-10-31).** `Gpu::Params::grain` (RegionEvidence::GrainView, the one
read API of the single `grain.split` node owned by input.source_analysis/regions; its luma GPU form), computed once, early, on the decoded source's active picture by the host; the module
then computes no split (bit-identical output, `TemporalDetailGpuReadsTheSharedGrainSplit`). Without it, or when it does not
fit (size / block / scale), the module computes its own and the status says "own grain split". The module's output is
`Y + delta` = the shared clean picture's repair plus the ORIGINAL grain; a later repair must read the same published
product, never a split of this output. Open point for the coordinator: the product is computed on the plane the readers
work on (today the re-based plane); if the re-base changes geometry, the published split must follow it (re-based once).

Every model step runs as libplacebo compute passes, nothing read back within a frame (the per-block controller lives in
GPU textures): grain split (repair.grain_split, GrainSplitGpu) -> MEAS -> COMPOSE -> EVAL (per slot x block) -> TRACK
-> ADMIT (one workgroup) -> CLEAN (4 parity classes of DCT patches; work only on admission frames) -> store slot ->
WEIGHT -> PROJ -> TRANSFER -> GUARD sums / EMA / pool -> counters -> APPLY. Deterministic (fixed reduction trees, no
atomics, no barrier in varying flow). Matches the model to 1/64 code (the output's quantisation) on WARP over I/B pans,
both template variants, resets / repeats / motion loss, the 2-slot tier and letterbox / pillarbox pictures. Not on the GPU
yet: the queued-mode future slot / referee and the camera homography chain (the model without them is what runs).

Memory (TextureBytes, exact for the allocation): 3840x2160 **945 MiB** at 5 slots (grain split 253 MiB, slot atlas 396,
cleaner accumulators 127, delta / previous grain / output 142, block grids ~24), **701 MiB** at 2 slots; 1920x1080 about a
quarter. The store is tier-able: `Params::slots` (1..5, model `Tuning::slots`), `SlotsForBudget(bytes, w, h)` picks the
largest that fits; below 5 the status says "store N of 5 slots (memory tier)". Further savings (not done): aliasing the
cleaner accumulators onto the split's band texture (-127 MiB), half-resolution base (README plan).

Time (estimates; TemporalDetailGpuTiming measures on hardware): ~1.3 G texel reads per 4K frame at 5 slots worst case
(EVAL, PROJ and TRANSFER each sample every live slot with 4x4 Catmull-Rom), plus the grain split (~0.7 G) and, on
admission frames, the cleaner (~4 GMAC). RTX 3090: ~5-8 ms at 4K (~1.5-2 ms at 1080p); RTX 3050 Ti laptop: ~20-35 ms at
4K (~5-9 ms at 1080p) - not real-time at 4K on the laptop with 5 slots. Next optimisation: shared-memory slot tiles per
block (the warp is near a translation per block, ~10x fewer reads).

Renderer call sites needed (after the Bill merge; none made): (1) add GrainSplitGpu.cpp and TemporalDetailGpu.cpp to
VideoProcessor-VPRenderer(.Beta).vcxproj; (2) in the prepass after the re-base (texture_restore's position), when
`temporal_detail = steady`: `Module::Apply(gpu, dispatch, rebasedFrame, params, out)` with seq / content rate / plane lines
/ Indicator stamps / cut-fade flags / the active picture rect / backward (and forward) vector textures scaled to the
re-based plane; on false render the frame as it was and show "Temporal detail: inactive (<reason>)"; (3) `Forget()` on the
existing Forget triggers; (4) `FieldMotionSource::Prune(presented)` (or the host's own vector ring) per presented frame;
(5) the memory tier from the card profile (`SlotsForBudget`); (6) settings keys into RendererProfileConfig / Config /
slots.json with the migration in "Migration" below.

## Tests and shaders

`TemporalDetailShaders.h` holds the GLSL; `tests/TemporalDetailGpuTests.cpp` (GpuWarp; timing GpuHardware) checks the
split and the module against the models. `tests/TemporalDetailModelTests.cpp` (CPU, model only): off identity, I/B pulse, sub-pixel pans, focus pulls,
rack focus, motion blur, occlusion, cuts and stamps, determinism, grain (owner bounds on a two-seed grain isolate; the
current grain untouched), the grain guard against forced damage, the gain decomposition (structure vs grain), the grain
split's exact add-back, flicker, future referee, irregular patterns, scale and rate, camera-adapter fallback, boost
fallback, settings parsing, the active picture (letterbox / pillarbox with a pan: bars bit-exact, output independent of the
bar level and identical to the picture processed alone), the bounded motion source (10,000 frames).

## Grain split read path (SA2)

The GPU module reads the shared grain split ONLY through the one accessor of repair/grain_split (GrainSplitGpu.h): every
shader fetch is `gs_fetch(p)` (s1, s2, f, base in codes) or `gs_blocks(b)` (N, e3); each grain-reading pass gets
`GrainSplit::Gpu::ViewGlsl(frame.storage, frame.codeUnit)` in its header and `BindView(frame, descs)` for its descriptors, so the
shared product may be Full or Compact and the module never touches the `split` / `gblocks` textures directly. Full generates
the same math as before (Off / Full output byte-identical).

Fallback rule: the shared product is used when `Fits` holds for this plane (size, block, scale, code unit, component, and the
second texture of a Compact frame); otherwise (absent, invalid view, wrong size or any misfit) the module computes its OWN
Full split and says so, visibly, every frame, and returns to the shared product the first frame it fits again (nothing is latched).
Status keeps "own grain split" / "own grain split (the shared product does not fit)". `LastGrainStatus()` is Ring (shared
product read), Missing (no product) or WrongSize (misfit); Shed is the node's. `GrainFallbackDecision()` is
GrainSplit::ReaderFallback("temporal detail", status): the catalog entry "grain split (temporal detail)", engaged while the own
split runs. Off / auto idle read no grain: Ring. Tests: TemporalDetailGpuGrainProduct*, ...FittingFullProduct*,
...CompactProduct*, ...GrainFallbackRecoversAutomatically (WARP).

## Private GPU textures (skeleton rule)

This module still creates its own GPU textures through the `Ensure` wrapper (its persistent state, `prevg`, the output, the block grids and, when no shared product fits, its own grain split; see the static test src/VP-V/tests/PrivateTextureScanTests.cpp). It migrates in SA6/SA7/SA8/SA12 (doc D:\codex\vp-handover\51-skeleton-adoption-audit.md section 6): kept state moves to skeleton product nodes and per-render temporaries to the shared scratch arena (src/VP-V/core/ScratchArena.h). SA6 (reduced) moved the per-render temporaries (cleaner accumulators, delta, own split intermediates) to the arena behind `Params::arena`; the reference store (`td.refstore`), `prevg` and the output remain, see "Products" above. Its grain reads already go through the one accessor (`gs_fetch` / `gs_blocks`); a shared product that is Missing, Stale (another frame or unit version, `Params::grainSeq`), WrongSize or Shed (`Params::grainShed`) makes it use its own split, visibly (FallbackCatalog "grain split (...)"), and it recovers on the first fitting frame.
