# Dynamic Drive module — delta 4: gain is defined on the final encoded value

2026-10-11. One rule for the compensation shader; resolves the
`compensation_eotf` 2.2-vs-2.41 question.

## Rule

The LED gain compensates the *light the projector makes*, and the
projector makes light from the **final** code with its real exponent
`E ≈ 2.41` (the light-meter fit). Therefore, in the compensation shader:

1. apply the gamma boost first: `v₁ = v^(2.2/2.4)` (today's boost);
2. then the gain in the projector's light domain on `v₁`:
   `v₂ = (v₁^E · gain)^(1/E)` with `E = compensation_eotf = 2.41`,
   knee as now;
3. emit `v₂`.

`compensation_eotf` is then unambiguously the measured display exponent,
2.41, used once, at the end. Set it so in the test config.

## Why this matters

Today's order (boost first, gain in a 2.2 domain) gives an effective light
exponent of 2.2 × 2.4/2.2 = 2.4 — correct for a 2.41 display only by
coincidence of the boost ratio. Any later change to the boost (the owner
intends to revisit it) would silently break the compensation. Gain *after*
the boost but still in a 2.2 domain would be ~10% off at LED 100. Rule
above makes the maths exact by construction and independent of the boost.

## Also

- The raw-frame 1D transfer and the highlight-linear calculation use the
  same `E` on the same final value, so all three agree.
- `script` mode's emulated CNTR gain (`gain_domain = encoded`) is
  unaffected: it multiplies the final encoded value directly.
- Test: code 126 grey, LED 255 → 100 with compensation on, at E = 2.41:
  predicted light constant to <1%; repeat with the boost set to 1.0 and
  confirm still constant (independence from the boost).
