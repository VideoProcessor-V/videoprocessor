# VP-V unit: colour

Tone and gamut mapping (libplacebo), BTB/WTW preservation, the SDR tone shape and Stage 13.

One folder per slot below. Each slot's README.md is its module doc, and its MANIFEST.json holds
its components and its live file list. The order and zones come from docs/design/slots.json;
see src/VP-V/core/README.md for how the skeleton runs them.

| Order | Slot | Status | Zone | Purpose |
|---|---|---|---|---|
| 170 | [tone_gamut](tone_gamut/README.md) | built | libplacebo | libplacebo tone/gamut mapping (keeps all of its colour science) OR, for low-end cards, the combined static HDR->SDR 3D LUT in the same slot. |
| 175 | [extended_range](extended_range/README.md) | built | libplacebo | Keep below-black and above-white codes through every conversion (owner rule: both kept in all cases). |
| 200 | [sdr_tone_shape](sdr_tone_shape/README.md) | built | output_hooks | Delta 13's SDR tone shape and headroom, and the (non-certified) DV trims; first at PRE_OUTPUT. |
| 210 | [appearance](appearance/README.md) | placeholder | output_hooks | CAM16 Stevens/Hunt appearance matching from reference to dim-screen conditions (static per viewing setup), replacing the manual gamma boost + saturation. |
