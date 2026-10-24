# Net runner wiring (draft for the skeleton lead; not applied)

`net-runner-wiring.patch` drafts the renderer side of the VP-side net runner (feat/net-runner),
against feat/skeleton fd60b650. Everything below the call sites is already built and tested in
`src/VP-V/nets`.

## What the patch adds

1. **Config keys** (`[vpv]`, global like the pipeline keys; also validated in
   `RendererProfileConfig.h`'s `vpvKeys` list, which the patch does not show yet):
   - `nets_enabled`: off|on (default off);
   - `nets_export_dir`: a LOCAL PRIVATE export folder in the kit's staging layout. Shaders and
     weights are never in the repo or a deployment;
   - `nets_stamp_dir`: the `tools/net_stamp.py` sidecars (default: the export folder);
   - `nets_repair`: auto|single|motion|off. `auto` = `ChooseRepairVariant`: A_single in low
     latency, else A_motion;
   - `nets_d`: off|on (D inside the hybrid's POST_KERNEL).

   A refused value goes to `pipelineSettingsProblems` and so appears under "Fallbacks engaged".
2. **Members:** one `NetSlot` + `PlNetRunner` per net slot (A, D), a `NetBypassSet` on the
   renderer's existing `fallbacks` registry, and a `SlotTiming::Accumulator`.
3. **`UpdateNets`**, called from `UpdatePipelineSlots` after the registry is built:
   - it loads the exports and stamps;
   - it sets `repair.encoding`'s component, so the registry key reflects A;
   - it loads A with `NetContextFor`. D loads at its first POST_KERNEL call, on the hook's
     dispatch.
4. **`NetContextFor`**, the runtime context from real state:
   - `UpstreamKey(SlotOf(role))`;
   - the upstream NETS running (`repairSlot.State`, with bypassed slots left out);
   - the hybrid's motion mode (`vectors` when its grain map uses vectors);
   - the settings hashes from `activeSettings` (`NetSettingsHash.h`).
5. **Front end (A):** a `case RepairEncoding` in `RunEntrance` (once per source frame under B3).
   - Plane 0 (r16, MSB-aligned 10-bit) goes in with `mul = 65535/65472`, the crop rect as
     `NetTexture::rect`, and labels v1.
   - The output goes back into r16 with `outputScale = 65472/65535`.
   - RGB capture: the full-range matcher luma goes through the range helpers, then the delta is
     added to RGB.
6. **POST_KERNEL (D):** AdaptiveDownscale gets a `postBlend` callback.
   - `Measuring` (PRE_KERNEL) copies the scaler's source.
   - `Blending` hands the blend output to `RunInPostKernelHook`, which makes luma, runs D and
     adds the delta to RGB.
   - D reloads when the upstream nets change (A starts or stops).
7. **Status:**
   - a Ctrl+I line "Nets: A_motion_small (fp32, x ms), D_hybrid_refine (y ms)" or "none running";
   - bypasses and degradations appear under "Fallbacks engaged" automatically (Ctrl+I, Config
     status, Ctrl+Alt+D).

## What the lead still decides or fills in (TODOs in the patch)

- **`ScalerBlendJson()`:** one table mapping `activeSettings` to the kit's
  `common/down/up/adapter` keys, so `scaler_blend_settings` hashes what VP runs.
- **`latchFixes` / grain-split constants:** read them from the real fields.
- **VP's own stage-contract text** per slot. Today the context copies the package's (U3).
- **`CurrentLabelsV1()`:** map the Stage 12 identification onto labels v1 (codec class, AV1 FGS,
  bpp tier, bit depth, encoded/delivered ratio, player kernel).
- **A_motion's neighbours:** the motion-aligned prev/next luma and mask, from the B3 ring and
  the vector field. Absent = the trained single-frame fallback, shown as "nets: degraded".
- **`PlaneWithRepairedCrop`:** A outputs the crop rect; write it back into a plane-sized pooled
  r16 at the crop offset.
- **D's light domain (R8):** the hook's RGB is linear light when `downscale_light` is linear.
  Confirm D's training domain before enabling D by default.
- **`NetSlot::SetOutputScale`:** a one-line forwarder to `NetPrepareParams::outputScale`, to add
  with the wiring.

## Behaviour to expect (from the fingerprint rules)

- **A (round 1/2):** runs only in the A-first order (`pipeline_order` with `repair.encoding`
  before `scaling.rebase`). In the default order it is bypassed, visibly ("upstream key
  'scaling.rebase' != trained ''").
- **D (round 1/2, stamped):** runs only while A is off or bypassed, with deband / texture
  restore off and the hybrid without vectors. Otherwise it is bypassed, visibly, until a D
  trained on A's output (or with vectors: D_motion) exists.
- **C and B** stay bypassed: C is dropped; B has no upstream-net record.

## Tests to add with the wiring

- **Renderer harness (CPU-only parts):** `ReadPipelineSettings` reads the nets keys, and an
  invalid value lands in "Fallbacks engaged".
- **Settings hashes from real state:**
  - `NetSettingsHashesNow` on the kit's locked settings (`scaling_render.json`) must give the
    recorded round-1 values;
  - one changed setting must change exactly its hash.
- **GPU (`VP_TEST_GPU=hardware`, one at a time):**
  - the renderer capture harness with `nets_enabled` on a private export: A runs (Ctrl+I line,
    SlotTiming samples), and the output differs from nets off only inside the gate's open tiles;
  - D through the real `AdaptiveDownscale` hook matches `RunInPostKernelHook` on the same
    textures;
  - with A on, D is bypassed with the "upstream nets" reason.
- **Zero cost:** with nets off, the picture is byte-identical to fd60b650 (the existing
  byte-identity test).
