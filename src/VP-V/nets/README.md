# VP-V nets: the VP-side net runner

The groundwork for running the TrainKit's exported nets inside VP (barrier B1, owner
2026-10-24). It is self-contained and NOT wired into the renderer yet (only the call sites remain): the skeleton lead wires it
once the baseline is verified (see "How to wire it" below).

## Why VP runs the passes itself

The exporter (`trainkit/export_glsl.py`) writes mpv-format user shaders. libplacebo can only run
those as hooks inside `pl_render`, which is the wrong place for VP's nets:

- a `LUMA` hook runs AFTER deband and texture restore; the agreed order is A, then deband,
  then texture restore;
- libplacebo never fires a `LUMA` hook for RGB frames, so A would silently never run on RGB
  capture;
- `SCALER_SRC` / `SCALER_OUT` (D) are not libplacebo stages;
- intermediates are one global switch inside libplacebo; each net needs its own format.

So VP parses the package and runs each pass itself on VP-owned textures, through `pl_dispatch`:

- in the front end (entrance, once per source frame): A, C, B, the 4K refine;
- inside the hybrid scaler's `POST_KERNEL` hook: D.

## Files

| File | One job |
|---|---|
| `NetPackage.h/.cpp` (+ `.inl`) | Parses one exported shader into a typed `NetPackage`: passes (DESC/HOOK/BIND/SAVE/WIDTH/HEIGHT/COMPONENTS/COMPUTE), PARAM labels, saved textures, the binding contract, the role/slot, the fingerprint. `LoadExport` reads `export_<net>.json` and its shaders and merges the json fingerprint (it must agree with the header). `EvalSizeExpr` evaluates the RPN sizes as libplacebo does |
| `NetFingerprint.h` | `CheckFingerprint(package, runtime context)` -> MATCH / MISMATCH(reasons), with a per-field `Requirement`. `NetBypassSet` publishes "Fallbacks engaged: nets: bypassed (<net>: <reason>, ...)" |
| `NetRunner.h` | The `INetRunner` interface (`Prepare`/`Run`/`Release`, capabilities, the timing hook), `CheckCapabilities` (picks the intermediate format), `ResolveInputs` (the contract rules, the 8K rule, A_motion's neighbour fallback), `ChooseRepairVariant` (low latency) |
| `NetSlot.h` | One net in one slot: load (capabilities -> format -> fingerprint -> prepare) and per-frame run; every refusal bypasses that net only, visibly |
| `NetLabels.h` | Label schema v1 -> the PARAM values (`vpnn_ry`, `vpnn_rx`, `vpnn_kernel`, `vpnn_codec`, `vpnn_fgs`, `vpnn_bpp_tier`, `vpnn_bits10`) |
| `NetJson.h` | A minimal JSON reader for the export files and test cases |
| `NetSettingsHash.h` | VP's computation of the kit's settings hashes (`rebase_settings`, `grain_split`, `stage_order`, `scaler_blend_settings`, `bounds`) and of the fingerprint id: byte-identical Python `json.dumps(sort_keys=True)` + sha256[:16] (U3) |
| `NetSha256.h` | SHA-256 for the hashes and the shader ties |
| `../../../tools/net_stamp.py` (+ `test_net_stamp.py`) | The stamp tool: derives the fields round-1/2 exports lack (upstream key, typed pass order/formats, label schema, `delivered_bound`) ONLY from the kit's records, into `<export stem>.vpstamp.json`; refuses with a reason otherwise |
| `GlslMini.h/.cpp` | A small interpreter for the exporter's GLSL subset (CPU reference only) |
| `CpuNetExecutor.h/.cpp` | `INetRunner` on the CPU in 32-bit float: the reference the GPU runner is checked against. Emulates rgba16f intermediates |
| `PlNetRunner.h/.cpp` | The libplacebo runner: each pass through `pl_shader_custom` + `pl_dispatch_finish` / `pl_dispatch_compute`, with the mpv macros, PARAM variables, pooled VP-owned intermediates (fp16/fp32 per fingerprint), the net output kept at r32f (or r16 on request), and one `pl_timer` per pass into SlotTiming. Has a front-end entry and a POST_KERNEL entry (`RunInPostKernelHook`) |
| `tests/PlNetRunnerGpuTests.cpp`, `tests/NetTestSupport.h` | GPU tests: headless D3D11, WARP by default, `VP_TEST_GPU=hardware` + `VP_TEST_ADAPTER=NVIDIA` for the GPU. Shared test helpers |
| `RgbLumaDelta.h` | RGB capture: add (repaired luma - luma) to R, G and B. A CPU helper and the GPU pass text, tested to agree |
| `tests/NetRunnerTests.cpp` | The unit tests (compiled by `VideoProcessor-Test`, filter "VP-V") |
| `tests/make_reference.py` | Writes `tests/data/<set>/<case>` from the kit's PyTorch forward pass (read-only on the kit) |

## How it works

1. **Load.** `LoadExport(export_<net>.json, shaderDir, out, &err, sidecarDirs)` -> one
   `NetPackage` per shader (C has `C_h`, `C_v`; B has `B_x2_h`, `B_x2_v`).
   - The id is recomputed from the recorded fields (`fingerprint.idVerified`).
   - A stamp sidecar (`tools/net_stamp.py`) found in `sidecarDirs` or next to the json fills
     only the fields the export lacks (`fingerprint.stamped` lists them). It must name the
     export id and the shader's sha256, and it may not contradict the export.
   - Spec of the native fields from round 3: `docs/design/net-export-fingerprint.md`.
2. **Capabilities.** `CheckCapabilities(pkg, runner.Capabilities(device))` picks the
   intermediate format: rgba16f where the package allows it (trained rgba16f, or rgba32f with the
   export's `fp16_check.fp16_within_1_lsb`), else rgba32f. Compute passes need compute plus a
   storable format; `NAME_tex()` sampling needs a linear-filterable one.
3. **Fingerprint.** `CheckFingerprint(pkg, ctx)` compares, with `ctx.intermediate` = the format
   just picked:
   - the label schema (for nets with labels);
   - the pass order (`H>V`), for the separable nets (B, C) only;
   - the intermediate format;
   - the stage contract;
   - the id itself: it must recompute from the recorded fields (`idVerified`);
   - the upstream NETS actually running (`UpstreamNetsKey`: each net slot's
     `NetPackage::Identity()` = `stem@sha16`, or `bypassed`). It is also checked per frame
     (`NetRunInputs::upstreamNets`; `NetSlot::State()` gives a net's entry for the nets
     downstream). Round-1/2 exports record no upstream nets, so only A can be stamped; C, B and D
     stay bypassed;
   - D: the hybrid's motion mode (`hybrid_motion_mode`; round 2 records "no vectors");
   - the upstream key (`SlotRegistry::UpstreamKey(SlotOf(pkg.role))`), compared with
     `NormaliseUpstreamKey` on both sides: only picture-changing slots count
     (`motion.estimation` does not); an empty declared key (A) is valid;
   - the settings hashes (`stage_order`, `rebase_settings`, `grain_split`,
     `scaler_blend_settings`, `bounds`): VP computes them with `SettingsHashes(...)`
     (`NetSettingsHash.h`); the defaults reproduce the kit's recorded round-1 values.

   The VP commit and the renderer hash are recorded but do not gate by default (a new commit must
   not bypass every net); a bench that reproduces training sets `Requirement::Must`.
4. **Prepare.** `runner.Prepare(device, pkg, {intermediate, output})`.
5. **Per frame.** `ResolveInputs` checks the contract inputs and their sizes, then:
   - the 8K rule: a source above 3840x2160 on either axis is a designed bypass, still shown;
   - A_motion with invalid vectors (or no neighbours) runs its trained single-frame fallback:
     prev = next = the frame, mask = 0. This is not a bypass.

   Then `runner.Run(device, inputs, output)`.
6. **Visibility.** `NetSlot` does steps 2-5 and routes every refusal to `NetBypassSet`. It shows
   "Fallbacks engaged: nets: bypassed (A_single_small: label schema ...)" and clears the entry on
   recovery. A net that runs but not as trained shows
   "nets: degraded (...)": missing labels (the export's defaults) or A_motion's single-frame
   fallback.
   - No silent downgrades in the GPU runner either: a pass that samples bilinearly from a
     texture without linear filtering is refused; the capability check probes r / rg / rgba.
   - The runner's real intermediate format (`EffectiveIntermediate`) is what the fingerprint is
     checked against. Fused fp16 storage is refused unless the fingerprint allows fp16.
   - Exported `tanh(x)` is clamped to `tanh(clamp(x, -15, 15))` on the GPU: HLSL tanh can return
     NaN for large |x|; the value is unchanged in float.

### The binding contract (derived from the package)

| Net | Run site | Inputs | Labels | Output |
|---|---|---|---|---|
| A (delivered) | front end | `HOOKED` = luma of the original delivered frame (active picture, delivered size); A_motion also `VPNN_PREV`, `VPNN_NEXT` (aligned neighbour luma, same size), `VPNN_MASK` (.x prev, .y next validity) | schema v1 (7 PARAMs) | the same plane, repaired |
| C (re-base refine) | front end, after EACH 1-D re-base pass | `HOOKED` = the r16 pass output | none | the same size; `C_v` = transposed weights |
| B (x2) | front end | `HOOKED` = the re-based plane | none | doubled on the pass axis; the H output is quantised to r16 in-shader |
| D (scaler refine) | `POST_KERNEL` | `SCALER_OUT` (output size), `SCALER_SRC` (source size) | none | `SCALER_OUT` + bounded correction (luma; add it to R, G and B alike) |

The mpv semantics the runner implements (from libplacebo's `custom_mpv.c`):

- `NAME_tex` = bilinear filtering with clamp addressing;
- `NAME_pos` = this pixel's normalised position;
- `NAME_size` = `textureSize`;
- out size = `roundf(RPN)`;
- a pass without SAVE replaces the hooked stage;
- 1- and 2-component textures read back as (r, g, 0, 1).

## Test results (2026-10-24, laptop)

- **CPU vs PyTorch.** On the laptop's smoke export (`D:\nn-corpus\VP-NN-TrainKit\smoke`):
  - rgba32f is within 0.0001 code (10-bit) of PyTorch for C_h, C_v, D, A_single and A_motion
    (gate open and closed);
  - rgba16f is within 0.002 code.
- **The round-1 trained export** (`E:\VP-NN-TrainKit\work\staging`, made while E: was attached)
  gave:
  - rgba32f: C 0.0002 / 0.0007, D 0.0004, A 0.0000 codes;
  - rgba16f: C 0.22 / 0.19, D 0.45 codes. This is why their fingerprints keep rgba32f; their
    fp16 checks failed.
- **The round-1 A gate** stays closed on every test input. Those cases prove the exact-copy path.
  The `_open` cases raise the gate bias by +24 on both sides, so the whole trunk, the
  conditioning and the bound are exercised.
- **Skips.** A missing export logs SKIPPED for its cases. The weights are not committed. Each
  case records the sha256 of its shader.

## Expected GPU-vs-CPU tolerance (for the GPU oracle test once wired)

Estimates, to be confirmed on the first GPU run (`VP_TEST_ADAPTER=NVIDIA`, mains only); CPU =
`CpuNetExecutor` on the same inputs, codes are 10-bit:

- **Nets without `NAME_tex()` sampling** (A delivered, C, B) at rgba32f: at most 0.005 code.
  Only FMA contraction and summation order differ.
- **Nets with hardware bilinear** (D's source features; the old repair nets' `VPNNQ_tex`): at
  most 0.05 code. NVIDIA lerp weights are 8-bit fixed point, an error of up to 1/512 of the
  neighbour difference per sample, scaled by the bound (up to 32 codes) through tanh. The
  exporter's planned `bilinear_mode: texelfetch` brings these back to the first line.
- **rgba16f** (GPU vs the CPU's half emulation): at most 0.01 code. A GPU may round fp16
  stores differently at exact ties.
- **Gate decisions** must agree exactly. Any tile whose decision flips counts as a failure: a
  flipped tile is a different code path, not a tolerance.

## Fused compute path (optional, `PlNetRunner::SetFusion`)

The exporter writes one fragment pass per group of 4 output channels, and each pass re-reads every
input group at 9 taps. One D layer (24 -> 24 channels) is therefore 6 passes x 6 inputs x 9 taps =
324 texture fetches per pixel. `NetFusion.h` regroups those passes into layers, keeping the
package's own weights, and runs each layer as ONE compute pass:

- **Shared tile.** The workgroup loads its input tile plus a 1-pixel halo into shared memory
  once, mirrored at the picture edges exactly like the exporter's `mi()`.
- **Direct.** Each thread computes all output groups of its pixel. Each tap is read once per
  input group (54 shared reads per pixel for D, instead of 324 texture fetches).
- **Winograd F(2x2,3x3).** Each thread computes a 2x2 block from a 4x4 patch,
  `Y = A^T [(G g G^T) . (B^T d B)] A`. The weight transform `U = G g G^T` is done when the shader
  is generated, so 4 outputs take 16 multiplies instead of 36 (2.25x fewer for the 3x3 convs).
- **Precision.** Accumulation is always fp32. `fp16Storage` stores the layer outputs and the
  shared tile in fp16; it is a side check only, kept only if it saves meaningfully within
  tolerance (owner 2026-10-24).
- **Kept as exported:** the bias, ReLU, identity skip and tile gate; and the feature, pooling,
  regroup and bounded-output passes, plus A's conditioned first layer.
- **Stays exported:** a layer whose tile exceeds the shared-memory limit (32 KB on D3D11), whose
  output count exceeds D3D11's 8 compute UAV slots, or whose input sizes differ.

Results: see the efficiency report (laptop timings; projected 3090 gain).

### Tensor cores (investigation only)

- **VP's backend.** VP creates its libplacebo device with `pl_d3d11_create` (D3D11, FL 11.x).
  D3D11 has no cooperative-matrix / wave-matrix access: HLSL WaveMatrix was a D3D12 SM 6.8
  preview and was dropped; the D3D12 "cooperative vector" preview is D3D12-only. So VP's nets
  cannot reach the 3090's tensor cores through the current backend.
- **The route that exists.** libplacebo's Vulkan backend (`pl_vulkan_create`) accepts
  `opt_extensions` and an extra `features` chain:
  - enable `VK_KHR_cooperative_matrix`, which NVIDIA Ampere drivers expose with fp16 inputs and
    fp16/fp32 accumulators;
  - its feature struct;
  - and `#extension GL_KHR_cooperative_matrix` in a `pl_custom_shader` prelude.

  That would mean running VP (or at least the nets) on a Vulkan pl_gpu: a renderer-wide backend
  change, or Vulkan-D3D11 interop for the net textures. It is also fp16-input maths, which the
  owner keeps out unless it is proven within tolerance.

## How to wire it (for the skeleton lead): only the call sites remain

`PlNetRunner` is implemented: libplacebo dispatch on caller textures, pooled intermediates,
pl_timer -> SlotTiming, capability checks with visible bypass, and both entry modes. It is
tested on a headless D3D11 device against the CPU reference.

1. **Build.** Add `NetPackage.cpp` and `PlNetRunner.cpp` (they need only `<libplacebo/...>`) to
   the renderer project. The interpreter and the CPU executor stay test-only.
2. **Load (settings change, not per frame).**
   - `LoadExport(json, "", pkgs, &err, {stampDir})`;
   - per package: `PlNetRunner runner; NetSlot slot(runner, bypassSet);`
     `slot.Load({gpu, dispatch}, pkg, ctx)`.

   `ctx` is filled as in step 5. `bypassSet` sits on the renderer's `FallbackStatus::Registry`,
   which already feeds Ctrl+I, the Config status and Ctrl+Alt+D.
3. **Front-end call site (A, B).** In the entrance stage list, per source frame:
   - `NetRunInputs in; in.textures["HOOKED"] = {plane, w, h, 1, fmt, mul};`
     `in.params = ParamsFromLabelsV1(labels); in.state = {vectorsValid, lowLatency};`
   - then `slot.Run({gpu, vpDispatch}, in, out)`. On false, keep the input;
   - `out.handle` is the runner's `pl_tex` (r32f; valid until the next Run);
   - r16 planes holding 10-bit MSB-aligned codes: `mul = 65535.0f / 65472.0f`;
   - RGB capture: the matcher's luma of native RGB ingest is FULL range, but A was trained on
     limited 10-bit luma:
     - convert first (`FullToLimited`, or `kLumaFromRgbPass` with `vp_range_scale` 876/1023 and
       `vp_range_offset` 64/1023);
     - run A;
     - add the delta with `kRgbLumaDeltaPass`, `vp_delta_scale` = 1023/876;
     - no clamp anywhere (BTB/WTW kept);
   - choose A_motion or A_single with `ChooseRepairVariant`.
   - C is dropped from the design; leave it bypassed.
4. **POST_KERNEL call site (D).** In AdaptiveDownscale's blend hook:
   - call `runner.RunInPostKernelHook(params, scalerSrc, out, &err, options)`.
     - SCALER_OUT is `params->tex` over `[0, dst_rect size)`; libplacebo may hand a larger
       pooled texture, so the picture is copied to an exact texture first.
     - `scalerSrc` is the hybrid's source with its `srcRect` in `NetTexture::rect`. It must be
       texel-aligned; a fractional crop is refused visibly.
   - The hook texture is RGB. The runner makes luma (`kLumaFromRgbPass`, kr/kb and the trained
     range), runs D on luma and adds the delta to R, G and B (`kRgbLumaDeltaPass`). `out` is the
     hook's RGB with D applied: return it as the hook texture.
   - The hook's transfer domain (linear light with `downscale_light: linear`) is what D sees.
     D's training domain is barrier R8, which only the stage contract checks today.
   - Use `NetSlot::Load` with `PlNetRunner::DeviceFromHook(params)` at the first hook call.
5. **Fingerprint context.**
   - `upstreamKey` = `registry.UpstreamKey(SlotOf(pkg.role))`;
   - `labelSchema` = what the identification supplies;
   - `settingsHashes` = `SettingsHashes(...)` from VP's live settings (`NetSettingsHash.h`);
   - `stageContract`;
   - call `Load` again whenever any of these change.
6. **Cost.** `runner.SetTiming(&slotTiming, SlotOf(pkg.role))` before `Load`.

## Open items

- **The exporter writes no `upstream_key`.** Until round 3 emits it
  (`docs/design/net-export-fingerprint.md`), stamp each export with `tools/net_stamp.py`.
  Without a stamp the default policy bypasses the net ("export has no upstream key").
- **Stamping B and D** needs the kit's render settings (`data/scaling/scaling_manifest.json`).
  The round-1 TRAINED D was made with other render settings (hash `74b9e456fef63121`), whose
  manifest is on the kit drive; stamp it when E: is attached.
- **Weights for the tests.** The kit's `.pt` stores no `delivered_bound`, which the A exports
  used. `make_reference.py` reads it from `<staging>/config.json`.
- **GPU filtering precision.** On the GPU, `NAME_tex()` filters with reduced fractional precision
  (NVIDIA 8-bit lerp weights). D's source features use it, so expect a small GPU-vs-CPU
  difference, or move those reads to texelFetch bilinear in the exporter.
- **D's light domain** (barrier R8) is not checked by the fingerprint beyond the stage contract
  text.

## Private exports (never committed)

Shaders and weights stay out of the public repo. To run the reference cases, point the tests at
a local private copy of an export (read only):

- `set VP_NETS_EXPORT_DIR=D:\vp-private\nets\round1\staging` (`;`-separated for several);
- the folder layout is the kit's staging: `export_<net>.json`, `shaders/`, `config.json`;
- the built-in fallbacks are `E:\VP-NN-TrainKit\work\staging` (the round-1 kit drive) and
  `D:\nn-corpus\VP-NN-TrainKit\smoke\staging` (the laptop's smoke export).

Each reference case runs only against a shader with its recorded sha256; anything else logs
SKIPPED. Stamp sidecars (no weights; sha256 + derived fields) may be committed:
`tests/data/stamps/<set>/`.

## Tests

`VideoProcessor-Test`, class `NetRunnerTests`. Run single-threaded:
`vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~NetRunnerTests /Logger:trx`.
Set `VP_NETS_EXPORT_DIR` to add export folders (`;`-separated). The stamp tool's tests:
`python -B tools/test_net_stamp.py`.
