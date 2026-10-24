# ADR-0013: Output-end order and the two LUTs

- Status: Accepted
- Date: 2026-10-20 (owner decision; recorded 2026-10-24)
- Slots: colour.sdr_tone_shape, colour.appearance, drive.led_measure, drive.led_compensation, drive.black_level, output.calibration_lut, output.dither

## Context

The hybrid scaler runs inside libplacebo before its PRE_OUTPUT hooks; LED compensation must measure pre-compensation content and the calibration LUT must be the last colour op. libplacebo applies its normalized target LUT during colour mapping, BEFORE PRE_OUTPUT hooks.

## Decision

libplacebo: scaling -> tone/gamut map (or the combined LUT). VP hooks, in order: SDR tone shape -> Stage 13 CAM16 -> LED measurement -> LED compensation -> black level -> projector calibration LUT (applied by VP, last colour op) -> dither/sigma-delta. The calibration LUT moves from libplacebo's target LUT to a VP hook.

## Consequences / open

Catch: a combined HDR->SDR LUT outputs projector codes but LED compensation needs linear light; decide when building the LUT option. Verify the PRE_OUTPUT output-encoding repair interplay.
