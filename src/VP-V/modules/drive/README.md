# VP-V unit: drive

Dynamic Drive: LED measurement, LED compensation, black level, and the projector control.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 220 | [led_measure](led_measure/README.md) | built | output_hooks | Measure the picture's light (pre-compensation content, after Stage 13) for the LED drive; peaks picture-only, full coverage. |
| 230 | [led_compensation](led_compensation/README.md) | built | output_hooks | Compensate the picture for the LED drive change so content light stays constant; CNTR never above need. |
| 240 | [black_level](black_level/README.md) | placeholder | output_hooks | Compensate the projector black floor as LED/CNTR change (delta 11 toe / ANSI lift interplay), after compensation, before the calibration LUT. |
| 900 | [projector_control](projector_control/README.md) | built | host | RS-232 projector driver: LED (LPW1) + CNTR commands, script intent / internal script / compensate modes, latency timing. |
