# VP-V nets: the VP-side net runner

The groundwork for running the TrainKit's exported nets inside VP (barrier B1, owner
2026-10-24). It is self-contained and NOT wired into the renderer yet: the skeleton lead wires it
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
| `GlslMini.h/.cpp` | A small interpreter for the exporter's GLSL subset (CPU reference only) |
| `CpuNetExecutor.h/.cpp` | `INetRunner` on the CPU in 32-bit float: the reference the GPU runner is checked against. Emulates rgba16f intermediates |
| `PlNetRunner.h` | The `pl_dispatch` runner: a STUB that refuses visibly; its header lists the exact shim to write |
| `RgbLumaDelta.h` | RGB capture: add (repaired luma - luma) to R, G and B. A CPU helper and the GPU pass text, tested to agree |
| `tests/NetRunnerTests.cpp` | The unit tests (compiled by `VideoProcessor-Test`, filter "VP-V") |
| `tests/make_reference.py` | Writes `tests/data/<set>/<case>` from the kit's PyTorch forward pass (read-only on the kit) |

## How it works

1. **Load.** `LoadExport(export_<net>.json)` -> one `NetPackage` per shader (C has `C_h`, `C_v`;
   B has `B_x2_h`, `B_x2_v`).
2. **Capabilities.** `CheckCapabilities(pkg, runner.Capabilities(device))` picks the
   intermediate format: rgba16f where the package allows it (trained rgba16f, or rgba32f with the
   export's `fp16_check.fp16_within_1_lsb`), else rgba32f. Compute passes need compute plus a
   storable format; `NAME_tex()` sampling needs a linear-filterable one.
3. **Fingerprint.** `CheckFingerprint(pkg, ctx)` compares, with `ctx.intermediate` = the format
   just picked:
   - the label schema (for nets with labels);
   - the pass order (`H>V`);
   - the intermediate format;
   - the stage contract;
   - the upstream key (`SlotRegistry::UpstreamKey(SlotOf(pkg.role))`);
   - the settings hashes (`stage_order`, `rebase_settings`, `grain_split`,
     `scaler_blend_settings`, `bounds`).

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
   recovery.

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

## How to wire it (for the skeleton lead)

1. **Implement `PlNetRunner`.** Follow the shim steps in its header (`sh_bind` per BIND, the
   NAME_* macros, PARAM variables, `pl_dispatch_finish`/`pl_dispatch_compute`, and a pl_timer
   into `RecordTiming`). Move it into a `.cpp` in the renderer project.
2. **Compile the parser in the renderer project.** Add `NetPackage.cpp`. The interpreter and the
   CPU executor stay test-only.
3. **Front end (A, C, B).** Register a stage in the entrance stage list:
   - A at `repair.encoding`, C inside `scaling.rebase` after each 1-D pass, and so on;
   - per source frame: `NetSlot::Run(device{gpu, dispatch}, {HOOKED: the plane}, out)`; on false,
     keep the input;
   - RGB capture: run A on the matcher's r16 luma, then run `kRgbLumaDeltaPass` with
     `LUMA_BEFORE` / `LUMA_AFTER`.
4. **D.** Inside AdaptiveDownscale's `POST_KERNEL` hook, build a `NetDeviceContext` from
   `pl_hook_params` (gpu, dispatch) and run D on `SCALER_SRC` / `SCALER_OUT`. Add its luma delta to
   R, G and B.
5. **Fingerprint context.**
   - `upstreamKey` = `registry.UpstreamKey(SlotOf(pkg.role))`;
   - `labelSchema` = what the identification supplies;
   - `stageContract` and the settings hashes from the pipeline: these are U3, still to be built
     to match the kit's hashing;
   - call `NetSlot::Load` again whenever any of these change.
6. **Show the entries.** Put `NetBypassSet` on the renderer's `FallbackStatus::Registry`, so it
   reaches Ctrl+I, the Config status and Ctrl+Alt+D.
7. **Cost.** `runner.SetTiming(&accumulator, SlotOf(pkg.role))`.

## Open items

- **The exporter writes no `upstream_key`.** It also has no typed `pass_order` /
  `intermediate_format`; the parser derives those two from prose. With the default policy, every
  round-1/2 export is therefore bypassed ("export has no upstream key"), which is correct until
  the kit adds the key.
- **The settings hashes** (`stage_order`, `rebase_settings`, ...) are computed by the kit. VP must
  compute identical hashes (U3).
- **Weights for the tests.** The kit's `.pt` stores no `delivered_bound`, which the A exports
  used. `make_reference.py` reads it from `<staging>/config.json`.
- **GPU filtering precision.** On the GPU, `NAME_tex()` filters with reduced fractional precision
  (NVIDIA 8-bit lerp weights). D's source features use it, so expect a small GPU-vs-CPU
  difference, or move those reads to texelFetch bilinear in the exporter.
- **D's light domain** (barrier R8) is not checked by the fingerprint beyond the stage contract
  text.

## Tests

`VideoProcessor-Test`, class `NetRunnerTests`. Run single-threaded:
`vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~NetRunnerTests /Logger:trx`.
Set `VP_NETS_EXPORT_DIR` to add export folders (`;`-separated).
