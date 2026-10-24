# ADR-0002: Below-black and above-white are preserved in all cases

- Status: Accepted
- Date: 2026-10-13 (owner decision; recorded 2026-10-24)
- Slots: colour.extended_range, colour.appearance, drive.led_compensation, output.calibration_lut

## Context

The owner requires BTB and WTW kept through every conversion; libplacebo clips them on some conversions.

## Decision

VP keeps BTB/WTW end to end (ExtendedRange: VP relabels the source and its hooks do the conversion). Prefer a VP-owned conversion hook over forking libplacebo. Every output slot must leave the BTB/WTW probe codes identical on and off; true BTB/WTW codes pass through Stage 13 and LED compensation unchanged.

## Consequences / open

Known limitation: libplacebo's frame mixer (smooth motion) clips BTB on mixed refreshes; VP's own motion engine keeps both.
