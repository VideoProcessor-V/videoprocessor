# VP modules

One folder per pipeline slot (`<unit>/<slot>/`): README.md (module doc), MANIFEST.json
(components required/optional, config keys, upstream modules, fallback, LIVE FILE LIST),
tests/. The tree mirrors docs/design/slots.json; the master component list is
docs/design/components-master.json, generated from the manifests.

- Drift check: `python tools/check_module_manifests.py` (exit 1 on mismatch);
  regenerate the master list with `--write`.
- No duplicate source: code still lives at its legacy path (see each `live_files`) and moves
  here only when its slot is touched (ADR-0017 migration directive).
- These folders are not part of any .vcxproj yet; nothing here is compiled until code migrates.

| Order | Slot | Side | Status | Folder |
|---|---|---|---|---|
| 10 | Capture (`input.capture`) | input | built | [input/capture](input/capture/README.md) |
| 20 | Signal (`input.signal`) | input | built | [input/signal](input/signal/README.md) |
| 30 | Queue + timing (`input.queue_timing`) | input | built | [input/queue_timing](input/queue_timing/README.md) |
| 40 | Source analysis (`input.source_analysis`) | input | partial | [input/source_analysis](input/source_analysis/README.md) |
| 50 | Motion estimation + vector store (`motion.estimation`) | input | built | [motion/estimation](motion/estimation/README.md) |
| 60 | Re-base (entrance stage 2a) (`scaling.rebase`) | input | built | [scaling/rebase](scaling/rebase/README.md) |
| 70 | Encoding repair (entrance stage 1) (`repair.encoding`) | input | placeholder | [repair/encoding](repair/encoding/README.md) |
| 80 | Deband (`repair.deband`) | input | built | [repair/deband](repair/deband/README.md) |
| 90 | Texture restore + breathing add-on (`repair.texture_restore`) | input | built | [repair/texture_restore](repair/texture_restore/README.md) |
| 100 | 4K route (entrance stage 2b) (`scaling.route_4k`) | input | placeholder | [scaling/route_4k](scaling/route_4k/README.md) |
| 110 | Refine net (`scaling.refine`) | input | placeholder | [scaling/refine](scaling/refine/README.md) |
| 120 | Chroma upscale (placeholder) (`scaling.chroma_upscale`) | input | placeholder | [scaling/chroma_upscale](scaling/chroma_upscale/README.md) |
| 130 | Vector delivery (`motion.vector_delivery`) | processing | partial | [motion/vector_delivery](motion/vector_delivery/README.md) |
| 140 | Motion consumers (`motion.consumers`) | processing | built | [motion/consumers](motion/consumers/README.md) |
| 150 | Geometry (`scaling.geometry`) | processing | built | [scaling/geometry](scaling/geometry/README.md) |
| 160 | Output scaling (hybrid SSIM + D) (`scaling.output`) | processing | partial | [scaling/output](scaling/output/README.md) |
| 170 | Tone + gamut map (`colour.tone_gamut`) | processing | built | [colour/tone_gamut](colour/tone_gamut/README.md) |
| 175 | BTB/WTW preservation (`colour.extended_range`) | processing | built | [colour/extended_range](colour/extended_range/README.md) |
| 200 | SDR tone shape + DV trims (`colour.sdr_tone_shape`) | output | built | [colour/sdr_tone_shape](colour/sdr_tone_shape/README.md) |
| 210 | Stage 13 appearance (CAM16) (`colour.appearance`) | output | placeholder | [colour/appearance](colour/appearance/README.md) |
| 220 | LED measurement (`drive.led_measure`) | output | built | [drive/led_measure](drive/led_measure/README.md) |
| 230 | LED compensation (adaptive gamma) (`drive.led_compensation`) | output | built | [drive/led_compensation](drive/led_compensation/README.md) |
| 240 | Black level correction (`drive.black_level`) | output | placeholder | [drive/black_level](drive/black_level/README.md) |
| 250 | Projector calibration LUT (`output.calibration_lut`) | output | partial | [output/calibration_lut](output/calibration_lut/README.md) |
| 260 | Dither / sigma-delta (`output.dither`) | output | built | [output/dither](output/dither/README.md) |
| 270 | Presentation (`output.presentation`) | output | built | [output/presentation](output/presentation/README.md) |
| 900 | Projector control (`drive.projector_control`) | control | built | [drive/projector_control](drive/projector_control/README.md) |
| 910 | Net runtime + fingerprints (`control.net_runtime`) | control | placeholder | [control/net_runtime](control/net_runtime/README.md) |
| 920 | Status, fallbacks + diagnostics (`control.diagnostics`) | control | partial | [control/diagnostics](control/diagnostics/README.md) |
| 930 | Config + tiers (`control.config`) | control | partial | [control/config](control/config/README.md) |
