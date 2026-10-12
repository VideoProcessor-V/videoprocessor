# Draft: libplacebo upstream report - black pixels from debanding in the HDR path

Status: draft, not sent (sending it publishes it; the owner decides).

## Summary

With `pl_render_high_quality_params` (debanding on, default
`pl_deband_default_params`), rendering a PQ BT.2020 source (1000-nit
metadata) tone-mapped to an SDR target, about 1 in 10^4 output pixels per
frame comes out black (code 64 of 10-bit limited) at random positions, on
bright pixels. Repeated renders of the same frame place them differently.
With debanding off, repeated renders are bit-identical and no pixel is black.
SDR sources with debanding on show only the expected grain (+-3 codes
between renders), no black pixels.

## Versions

- libplacebo 7.360.1 (Windows, D3D11) and a 7.x build at API 377.
- Devices: D3D11 WARP and an NVIDIA GeForce RTX 3050 Ti Laptop GPU (driver
  32.0.15.9608). Same rate on both: 1-2 of 15360 pixels per frame.

## Reproduction (outline)

1. A 640x96 Y'CbCr 4:2:2 10-bit (16-bit storage, shift 6) source, limited
   range, BT.2020 NC, PQ, `hdr.max_luma = 1000`, `min_luma = 0.005`;
   content: a textured luma field 100..900 (PQ codes) plus a grey ramp.
2. Target: 320x48 R10G10B10A2, limited range, P3 primaries, gamma 2.2,
   `max_luma` 100 nits.
3. `pl_render_high_quality_params` with `peak_detect_params = NULL`,
   `dither_params = NULL`, Mitchell downscaler.
4. Render the same frame three times; compare outputs. Black pixels appear
   in some renders where the others show ~927-940.

## Expected

Debanding grain may change pixels slightly between renders but should not
produce black pixels.

## Workaround in VideoProcessor

Debanding off for HDR rendering profiles.
