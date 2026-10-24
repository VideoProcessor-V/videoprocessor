# VP-V modules

One folder per pipeline slot (`<unit>/<slot>/`). Each folder holds:

- `README.md`: the module doc;
- `MANIFEST.json`: required/optional components, config keys, upstream modules, the fallback,
  and the LIVE FILE LIST;
- `tests/`;
- the slot's code, once it has migrated.

The tree mirrors `docs/design/slots.json`. The master component list is
`docs/design/components-master.json`, generated from the manifests.

- **Drift check:** `python tools/check_module_manifests.py` exits 1 on any mismatch.
  Regenerate the master list with `--write`.
- **No duplicate source.** Code still lives at its legacy path (see each `live_files`). It moves
  here only when its slot is touched (ADR-0017), and a moved file leaves no copy behind (the
  drift check enforces this).
- **Migrated so far:** `output/calibration_lut/CalibrationLutStage.*`, the VP calibration-LUT
  hook compiled by both renderer projects.
- **Early migrations** (2026-10-24, `docs/design/migration-early-inventory.md`; code no active
  branch touches, one module per commit):
  - `control/diagnostics`: crash state ring (`CrashState.*`) and state report (`StateReport*`).
  - `drive/projector_control`: LED controller, Barco ASCII + projector drivers, RS-232 serial port, connection test, clock seam, shutdown restore (forwarders: `led/SerialPort.h`, `led/ConnectionTest.h`).
  - `drive/led_measure`: LED picture measurement and look-ahead (`LedMeasurement.*`, `LedLookahead.*`); `LedMeasurementHook.*` stays (the test project compiles it).
  - `drive/led_compensation`: Dynamic Drive core (`LedDriveModule.*`), COM-port arbiter, hold-over (forwarders for all three headers: the renderer includes them); `LedRawTransfer.*` stays (the test project compiles it).
  - `scaling/rebase`: kernel inverse helper (`KernelInverse.*`; forwarder `KernelInverse.h` for the renderer and the entrance/srcres code, which keep the old include); `SourceRebase.*` stays (entrance code).
- **Order and moves:** see `src/VP-V/core/README.md`. A module moves only within its zone
  (ADR-0021).

| Order | Slot | Side | Zone | Status | Folder |
|---|---|---|---|---|---|
| 10 | Capture (`input.capture`) | input | host | built | [input/capture](input/capture/README.md) |
| 20 | Signal (`input.signal`) | input | host | built | [input/signal](input/signal/README.md) |
| 30 | Queue + timing (`input.queue_timing`) | input | host | built | [input/queue_timing](input/queue_timing/README.md) |
| 40 | Source analysis (`input.source_analysis`) | input | host | partial | [input/source_analysis](input/source_analysis/README.md) |
| 50 | Motion estimation + vector store (`motion.estimation`) | input | front_end | built | [motion/estimation](motion/estimation/README.md) |
| 60 | Re-base (entrance stage 2a) (`scaling.rebase`) | input | front_end | built | [scaling/rebase](scaling/rebase/README.md) |
| 70 | Encoding repair (entrance stage 1) (`repair.encoding`) | input | front_end | placeholder | [repair/encoding](repair/encoding/README.md) |
| 80 | Deband (`repair.deband`) | input | front_end | built | [repair/deband](repair/deband/README.md) |
| 90 | Texture restore + breathing add-on (`repair.texture_restore`) | input | front_end | built | [repair/texture_restore](repair/texture_restore/README.md) |
| 100 | 4K route (entrance stage 2b) (`scaling.route_4k`) | input | front_end | placeholder | [scaling/route_4k](scaling/route_4k/README.md) |
| 110 | Refine net (`scaling.refine`) | input | front_end | placeholder | [scaling/refine](scaling/refine/README.md) |
| 120 | Chroma upscale (placeholder) (`scaling.chroma_upscale`) | input | front_end | placeholder | [scaling/chroma_upscale](scaling/chroma_upscale/README.md) |
| 130 | Vector delivery (`motion.vector_delivery`) | processing | host | partial | [motion/vector_delivery](motion/vector_delivery/README.md) |
| 140 | Motion consumers (`motion.consumers`) | processing | front_end | built | [motion/consumers](motion/consumers/README.md) |
| 150 | Geometry (`scaling.geometry`) | processing | libplacebo | built | [scaling/geometry](scaling/geometry/README.md) |
| 160 | Output scaling (hybrid SSIM + D) (`scaling.output`) | processing | libplacebo | partial | [scaling/output](scaling/output/README.md) |
| 170 | Tone + gamut map (`colour.tone_gamut`) | processing | libplacebo | built | [colour/tone_gamut](colour/tone_gamut/README.md) |
| 175 | BTB/WTW preservation (`colour.extended_range`) | processing | libplacebo | built | [colour/extended_range](colour/extended_range/README.md) |
| 200 | SDR tone shape + DV trims (`colour.sdr_tone_shape`) | output | output_hooks | built | [colour/sdr_tone_shape](colour/sdr_tone_shape/README.md) |
| 210 | Stage 13 appearance (CAM16) (`colour.appearance`) | output | output_hooks | placeholder | [colour/appearance](colour/appearance/README.md) |
| 220 | LED measurement (`drive.led_measure`) | output | output_hooks | built | [drive/led_measure](drive/led_measure/README.md) |
| 230 | LED compensation (adaptive gamma) (`drive.led_compensation`) | output | output_hooks | built | [drive/led_compensation](drive/led_compensation/README.md) |
| 240 | Black level correction (`drive.black_level`) | output | output_hooks | placeholder | [drive/black_level](drive/black_level/README.md) |
| 250 | Projector calibration LUT (`output.calibration_lut`) | output | output_hooks | partial | [output/calibration_lut](output/calibration_lut/README.md) |
| 260 | Dither / sigma-delta (`output.dither`) | output | quantise | built | [output/dither](output/dither/README.md) |
| 270 | Presentation (`output.presentation`) | output | present | built | [output/presentation](output/presentation/README.md) |
| 900 | Projector control (`drive.projector_control`) | control | host | built | [drive/projector_control](drive/projector_control/README.md) |
| 910 | Net runtime + fingerprints (`control.net_runtime`) | control | host | placeholder | [control/net_runtime](control/net_runtime/README.md) |
| 920 | Status, fallbacks + diagnostics (`control.diagnostics`) | control | host | partial | [control/diagnostics](control/diagnostics/README.md) |
| 930 | Config + tiers (`control.config`) | control | host | partial | [control/config](control/config/README.md) |
