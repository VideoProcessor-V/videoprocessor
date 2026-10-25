# Net export fingerprint: what the TrainKit exporter must emit (from round 3)

Status: spec for the kit, written 2026-10-24. Round 2 is not changed: it is verified and about to
run. Until round 3, VP derives the missing fields with `tools/net_stamp.py`, from what the kit
recorded, into a sidecar (`<export stem>.vpstamp.json`); `NetPackage` reads the sidecar.

Consumers:

- `src/VP-V/nets/NetPackage.*`: parses the export;
- `src/VP-V/nets/NetFingerprint.h`: runs the check;
- `src/VP-V/nets/NetSettingsHash.h`: computes VP's side of the settings hashes.

## Why

VP bypasses a net, visibly, unless the pipeline in front of it is the one it was trained on
(vp-neural-bounded). The round-1/2 exports record the settings hashes and a prose
`texture_formats` / `stage_contract`. They do NOT record these fields:

- the upstream module order, in VP's terms;
- a typed pass order;
- a typed intermediate format;
- the label schema of a label-less net;
- A's `delivered_bound` (it is missing from the `.pt` as well).

VP cannot check fields it has to guess, so the exporter must write them.

## Fields (in `export_<net>.json` "fingerprint" AND in each shader header)

| Field | Type / values | Meaning |
|---|---|---|
| `fingerprint` | 16 hex | `h(all fields below except fingerprint and stage_order_text)`, `h(o) = sha256(json.dumps(o, sort_keys=True))[:16]`, as today |
| `slot` | VP slot id | The slot the net serves: `repair.encoding` (A), `scaling.rebase` (C), `scaling.route_4k` (B), `scaling.refine` (4K refine), `scaling.output` (D) |
| `upstream_key` | `>`-joined VP slot ids, may be `""` | Picture-changing VP slots in front of the net during training, in order (VP's `SlotRegistry::UpstreamKey`, without non-modifying slots such as `motion.estimation`). Use `""` for A on the original frame |
| `upstream_nets` | `slot=identity\|...`, may be `""` | The NETS the training data went through, in order: `slot=<net file stem>@<sha256 of its exported shader, 16 hex>`, or `slot=bypassed` when the net slot ran without its net. The fingerprint id alone cannot name a net (A_single and A_motion share one). Record what was actually used, including the kit's fallbacks: a downstream net trained "with the fallback" must say so |
| `hybrid_motion_mode` | `"no_vectors"` / `"vectors"` / `"both"` | D only: the hybrid scaler's motion mode in its training renders (D_motion: `"vectors"`; round 2 wrote prose `"NO VECTORS: ..."`, read as `no_vectors`). VP bypasses D while the hybrid runs in a mode D was not validated for |
| `render_settings_sha256` | sha256 | Of `render_settings.json` (see below), so the file can be tied to the export |
| `render_tool_sha256`, `render_dll_sha256` | sha256 (full) | The VpScaleRender executable and renderer DLL that made the training renders (the motion-mode build for D_motion). The legacy `renderer_dll_sha256` (16 hex) stays as is |
| `front_end_states` | object | The VP front-end stages in the training renders, each `"on"`/`"off"`: `deband`, `deband_vp`, `texture_restore`, `detail_pulse_stabilise`, `source_resolution`, and the hybrid scaler's motion mode (for D) |
| `pass_order` | `"H>V"` / `"n/a"` | The separable pass order the net assumes (B, C); `"n/a"` for A and D. The shared `texture_formats` wording is NOT read as a pass order |
| `intermediate_format` | `"rgba32f"` / `"rgba16f"` | The net's intermediates in training/validation |
| `handoff_format` | `"r16"` / `"n/a"` | The re-base / x2 hand-off between the axes |
| `fp16_within_1_lsb` | bool | Copied from `fp16_check`, so the shader header carries it too |
| `stage_contract` | text | As today |
| `label_schema` | `"vpnn-labels-v1"` / `"none"` | Always present; `"none"` when the net has no PARAM labels |
| `delivered_bound` | `{c0, c1, cmax}` / `"n/a"` | A's output bound, used in its shader |
| `bounds`, `grain_split`, `rebase_settings`, `stage_order`, `scaler_blend_settings` | 16 hex | As today. VP recomputes them (`NetSettingsHash.h`); keep the objects exactly as `trainkit/main.py fingerprint()` builds them, or bump `fingerprint_schema` |
| `bilinear_mode` | `"hardware"` / `"texelfetch"` | How `NAME_tex()` sampling is emitted (see below) |
| `fingerprint_schema` | `"vpnn-fp/2"` | Bump whenever a field or a hashed object changes |
| `kit_version` | text | The `kit_manifest.json` sha256 of the kit that exported, plus the trainkit commit or version |

## The `.pt` file

Store the full config subset the shaders used: `split_eps_codes`, `split_r2160`, `bound`,
`scale_bound` and **`delivered_bound`**. The round-1/2 `.pt` lacks `delivered_bound`, so
rebuilding A from the `.pt` uses the wrong bound; VP's reference generator reads
`<staging>/config.json` instead.

Store the fingerprint too, as today.

## Render settings

Write the `scaling_manifest.json` settings object the hash was made from next to the export as
`render_settings.json` (`{"settings": {...}}`, or the settings object itself). A hash alone
cannot say which front-end stages were off. The stamp tool reads it automatically from next to
the export, or takes `--scaling-manifest`, and verifies it against `scaler_blend_settings`.

For a net whose inputs ARE those renders (D), the verified settings also prove two things:
- **No upstream nets.** When the settings load only classical shader profiles (no VPNN shader),
  the stamp writes `upstream_nets: ""`. VP then runs D only while no upstream net is active (A off
  or bypassed) and bypasses it, visibly, when A runs.
- **The motion mode.** `dejudder_strength` 0, `interpolation` off and no motion keys mean VP's
  motion estimate never started: `hybrid_motion_mode: no_vectors`.

A recorded field always wins. These settings never prove anything for C or B, whose inputs are
the kit's Python ports.

## `bilinear_mode` (exporter option)

`NAME_tex(pos)` uses hardware LINEAR filtering. On NVIDIA GPUs the lerp weights have 8-bit
fractional precision, so GPU results differ slightly from PyTorch's `grid_sample` and from VP's CPU
reference. The passes affected are D's source features at output positions and the repair nets'
`VPNNQ_tex`.

- Option `--bilinear texelfetch`: emit the 4-tap bilinear with `texelFetch` and float weights
  (`mi()`-clamped coordinates). It is exact on every GPU, at about 4 fetches per sample.
- Record the choice in `bilinear_mode`. VP's capability check then does not require
  linear-filterable formats for `texelfetch` exports.

## Sidecar (until round 3)

`tools/net_stamp.py --export <export json> --config <staging config.json>
[--scaling-manifest <kit>/data/scaling/scaling_manifest.json] [--kit <kit>] --out <dir>`.

It writes the fields above that it can derive, plus the shaders' sha256 and the export id. It
refuses, with the reason, when any derivation lacks a record:

- an edited fingerprint;
- a config whose `bounds` / `grain_split` hashes differ;
- an unknown stage token;
- B or D without hash-verified render settings;
- A without `delivered_bound`, or with bound constants that differ from the shader's.

A sidecar may only fill missing fields. A field the export has must agree with it, or the load
fails. So once the exporter emits a field natively, a stale sidecar is caught, not trusted.
