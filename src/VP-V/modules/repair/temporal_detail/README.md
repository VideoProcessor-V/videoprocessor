# TemporalDetail (`repair.temporal_detail`, DRAFT)

Unit **repair** | replaces `repair.texture_restore` (order 90) | status **draft**, no call sites | hook: prepass

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
   **Template grain budget:** the template keeps `sum c_k^2` of one member's grain (grain-free S still holds the grain the
   split reads as texture); `beta = min(1, kGrainBudget / sum om w max(tau) sqrt(sum c^2))` keeps that foreign grain
   <= 0.07 x the current grain RMS (Model::Tuning::grainBudget = 0 runs the designed strength, for measurement).
   **Amplitude limit** on the grain-free prediction: `|P + delta| <= 1.1 max(|P|, |T|)`, `P = lambda T` (the current
   pixel's own value is never clamped). **Fallback boost** where no valid reference exists but the guard (referee included)
   holds: `+ b S`, `b <= boost_max` (0.15), with `b f <= kBoostGrainShare` (0.04) so the current grain scales by at most 4%.
9. **Recompose: add the current grain back exactly.** `Y_out = TD(clean) + G = Y + delta`. A pixel with nothing to add is
   copied unchanged (bit-exact). Diagnostics: `structureOut = S + delta`, `grainOut = G` (plus any guard add-back).
10. **Output-vs-input grain guard** (safety net on top of 8-9). On the same grain map, per block (pooled 3 x 3, EMA over
   `Frames(0.25 s)`), over grain-dominated pixels (`f < 0.2`): `G_out = G_in + (1 - f) delta`, the level ratio, the
   projection `<(1 - f) delta, G_in> / <G_in, G_in>` (current grain removed) and the lag-1 correlation of `G_out` / `G_in`
   against the previous frame's, one hop back along the backward vectors (measurement only). (a) A measured loss of current
   grain above 1% is added back at once (`delta += k G`). (b) A violation (loss > 0.04, rise > 0.05, lag-1 increase > 0.05)
   trips the block (hysteresis: released below half), its weight factor falls to `guard / v` (rate `1 / Frames(0.1 s)`)
   and recovers over `Frames(0.5 s)`. Counted (`guardTrips`, `addBackBlocks`) and visible in the status
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

## Declared products (TemporalDetailProducts.h)

Descriptors mirror doc 25 §B1 (map 1:1 onto `core/Products.h`).

| Product | Inputs | Backend | History | Size at 4K |
|---|---|---|---|---|
| `td.analysis` (S/G split, block maps, rebounds, softening run) | re-based luma, P4 cut/fade, bitrate quality (optional) | GpuCompute | 1 | ~1 MB |
| `td.refstore` (K past + 1 future slots, per-block state) | analysis, P3/P2 motion, camera model (optional), queue frames (+d) | GpuCompute | K | ~16 MB R16F S + 4 MB base per slot |
| `td.out` (luma) | analysis, refstore | RenderOnly | 0 | plane |

## Settings (declared, not wired)

| Key | Values | Default |
|---|---|---|
| `temporal_detail` | off, steady | off (draft) |
| `temporal_detail_strength` | 0.0 .. 1.0 | 1.0 |
| `temporal_detail_max_age` | 0.25 .. 4.0 s | 2.0 |
| `temporal_detail_boost_max` | 0.0 .. 0.3 | 0.15 |

`TemporalDetail::ParseSettings` validates them (an invalid value keeps the previous one). They are not in
RendererProfileConfig, slots.json, the tier table or the Config editor yet.

## Determinism, generality, known limits

Fixed-order loops and double sums; shaders reduce in a fixed tree. No hard-coded rates,
cadences or resolutions; tests cover 1080-4320-line scales and 24-60 Hz.

Measured (model, synthetic I/B scene, grain isolate of two seeds; README of the tests): default **3.0 dB** B-frame PSNR gain,
all of it structure (own-grain retention 1.003, foreign grain 0.6% of the grain energy); grain RMS 1.013, lag-1 +0.017,
whole-field grain bands 1.042/0.998/1.006/1.005. At the designed strength (no budget) the gain is 7.5 dB (6.0 dB with the
guard) but the bands reach 1.13 and lag-1 +0.06-0.08: the frozen residual of template grain in dense texture is what the
budget trades against. The pre-fix draft's 10.5 dB was 9.8 dB structure plus a grain swap (own grain kept 31%, 73% of the
grain energy foreign/frozen); the first fix's 2.9 dB kept 80% own grain with 25% foreign.

Limits: grain the split reads as texture stays in the template (budgeted, not removed); more slots lower it (K = 8: +0.2 dB
at the same budget, more memory); the grain level needs quiet areas within 80 px (dense texture everywhere raises N, f
falls, transfer fails safe); block weights reach half a block past an edge of a soft region.

## Migration (retiring detail_pulse_stabilise / texture_restore)

`detail_pulse_stabilise`: its gain law survives only as the bounded fallback boost; history/look-ahead plumbing is
reused. `texture_restore`: its grain level + Wiener split become the shared `repair.grain_split` product, its focus gate
part of the guard; the grain boost is dropped.

Integration: rename the slot to `repair.temporal_detail`; map `detail_pulse_stabilise = on` to `temporal_detail =
steady` and `detail_pulse_cap` to `_boost_max = cap - 1` (clamped 0.3); drop `texture_restore*` with a logged
notice; Original pathway sets `temporal_detail = off`; reuse the host plumbing (crossfade, Forget triggers,
DetailLookAhead, status strings). Old keys are read for one release. A repeat (same
sequence) returns the last output (repeat/foreign/phase pictures come at integration); `CameraModelMotionSource` stays uncompiled until feat/camera-model merges.

## Tests and shaders

`TemporalDetailShaders.h` holds the draft GLSL (not dispatched). `tests/TemporalDetailModelTests.cpp` (CPU, model only): off identity, I/B pulse, sub-pixel pans, focus pulls,
rack focus, motion blur, occlusion, cuts and stamps, determinism, grain (owner bounds on a two-seed grain isolate; the
current grain untouched), the grain guard against forced damage, the gain decomposition (structure vs grain), the grain
split's exact add-back, flicker, future referee, irregular patterns, scale and rate, camera-adapter fallback, boost
fallback, settings parsing.
