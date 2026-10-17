# BTB/WTW preservation: investigation and proposal

2026-10-11. Implementer → planner. The owner's requirement: preserve BTB and
WTW in all cases. Clipping source detail before processing is not
acceptable; black textures such as suit jackets carry real detail below
code 64 (10-bit).

The investigation ran in a separate clone during the 6b-2a audit. Tools are
kept in `C:\dev\vp-tools\btb-wtw` (`RangeProbe.inc`,
`ExtendedRangeProbe.inc`, `QualityProbe.inc`, `apply_probe.py`, build and
test scripts), together with the raw result files.

## 1. Where libplacebo 7.360.1 clips (source read and measured)

Measured on the GPU harness: 10-bit limited YCbCr neutral greys at codes 4,
16, 40 (BTB), 64 … 940, 980 and 1019 (WTW), rendered into R10G10B10A2
limited output (colour depth 8, bit shift 2).

| Step | BTB | WTW | Triggered by |
|---|---|---|---|
| `pl_shader_linearize` / `delinearize`: `max(color, 0)`. The source comment admits it ignores BT.2100's sub-blacks "for sanity". | cut | kept | any transfer or primaries conversion, **every SDR downscale** (linear-light scaling) |
| `pl_shader_sigmoidize`: `clamp(color, 0, 1)` | cut | **cut** | **every SDR upscale** (sigmoid on by default; VP `sigmoid: auto`) |
| Gamut mapping: intensity limited to the target's range | cut | cut | BT.2020 → P3 with the default (perceptual) and every other function except `clip` and `saturation` |
| Full-range output | cut | cut | no room, by definition |

Measured, codes in → out:

```
same, 1:1                 16 40 64 100 502 900 940 980 1019   (kept)
same, up 1.5              64 64 64 100 502 900 940 940  940   (sigmoid)
same, up 1.5 nosig        16 40 64 100 502 900 940 979 1019
same, down 0.5            64 64 64 100 502 900 940 980 1019   (linear light)
2.2->2.4 / 1886->2.2      64 64 64 ...                        (BTB lost)
709->P3                   64 64 64 100 502 900 940 980 1019
2020->P3 default          64 64 64 100 502 900 940 940  940
```

The owner's chain (BT.709 → P3, 1080p upscaled to 2560×1600) therefore
loses **both** BTB and WTW today.

## 2. What VP's own code does

- **Kept:** sigma-delta (codes 0–255), `OutputEncodingRepair` (no clamp),
  de-judder (source codes).
- **LED compensation shader:** `max(color, 0)` whenever `gamma_boost` or
  the gain ≠ 1. That clips BTB; it needs a sign-preserving fix (one line).
- **CfL chroma, FSRCNNX/ArtCNN luma:** they run on the planes in texture
  space (codes 0–1023), so they do not clip BTB or WTW.
- **NLS (MAIN stage):** passes values and alpha through. **SSim
  downscaler/super-res (POSTKERNEL):** keep alpha, but clamp RGB
  internally. The proposal below keeps them in range.
- **3D calibration LUT:** clips outside its domain unless it is built with
  extended range. It is off today; noted for later.

## 3. Proposal: no fork. VP-owned SDR conversion plus a "carry" for scaling

Two native SIG_COLOR hooks, both prototyped and measured.

1. **`PL_HOOK_RGB`: carry.**
   - For a pixel outside 0–1, compute the offset
     `o = min(r, g, b)` if negative, else `max(r, g, b) − 1` if above 1,
     else 0.
   - Shift the pixel into range (`rgb −= o`) and store `o` in alpha (the
     source has none). libplacebo never linearises, sigmoidises or clamps
     alpha.
   - Normal pixels have `o = 0`, so they go through libplacebo's scaling
     **exactly as today**.
2. **`PL_HOOK_SCALED`: restore and convert.**
   - If libplacebo scaled in linear light, undo that with its own
     `pl_shader_delinearize`, so its SDR black scaling is matched exactly.
   - Add `o` back and drop alpha.
   - For SDR, apply VP's conversion with sign-preserving transfers and
     libplacebo's own primaries matrix:
     `sign(v)|v|^g_src`, then `M`, then `sign(l)|l|^(1/g_dst)`.
   - libplacebo is told the source is already in the target colour space,
     so its own (clamping) conversion is a no-op.

Measured results (neutral greys):

```
IDEAL                       4 16 40 64 100 502 900 940 980 1019
VP 709->P3 1:1              4 16 40 64 100 502 900 940 980 1019
VP 2020->P3 1:1             4 16 40 64 100 502 900 940 980 1019
VP 709->P3 up1.5 carry      4 16 40 64 100 502 900 940 980 1019
VP 709->P3 down0.5 carry    4 16 40 64 100 502 900 940 980 1019
IDEAL 2.2->2.4             -11 3 32 64 111 528 903 940 977 1012
VP 2.2->2.4                 0 3 32 64 111 528 903 940 977 1012   (-11 is below code 0)
```

Scaling quality on hard in-range edges (1-, 2- and 7-pixel bars), against
libplacebo's own default scaling:

```
carry     up 1.5:   max 1 code,  mean 0.021   (rounding)
carry     down 0.5: max 0 codes, mean 0.000
REFERENCE gamma-light scaling (sigmoid/linear off) vs default: mean 20-78 codes
```

So the carry keeps today's scaling exactly. A simpler "compand" (squeeze
[−a, 1+b] into [0, 1]) was also tried; it changed the scaling look by 9–19
codes on average and was rejected. libplacebo's log stayed clean throughout.

## 4. Open design points

1. **Out-of-gamut colours (BT.2020 → P3).** A pure matrix leaves saturated
   colours outside P3, and the output encoding then clips them per channel.
   Measured, R,G,B codes:

   ```
   strong red    libplacebo 921,87,121   VP matrix 1017,0,128
   bright cyan   libplacebo 455,922,940  VP matrix 325,1023,1023
   ```

   VP needs its own gamut compression. It should reduce chroma toward the
   same luminance for colours outside P3, and leave the luminance excursion
   (BTB/WTW) alone. This is the one piece of real colour design left.
   Proposal: a soft chroma compression in a perceptual space, with
   neutrals and in-gamut colours untouched, tested against libplacebo's
   perceptual result for in-range colours.
2. **HDR.** libplacebo's tone mapping linearises (PQ) and so clips BTB.
   This proposal covers SDR. HDR needs its own study, perhaps the same
   carry around the tone mapper. The owner's cinema profile uses a static
   LLDV tone map.
   *Resolved 2026-10-17:* after HDR->SDR tone mapping BTB/WTW have no
   meaning, by design (the tone mapper maps the HDR signal range into the
   SDR range; no footroom is left). HDR passthrough is carried; an SDR
   source on an HDR output is not covered. See docs/VP-btb-wtw-answers.md,
   "BTB/WTW and HDR".
3. **Output floor.** Codes below the output's representable range still
   clip. 8-bit limited allows 1–254; 10-bit allows 4–1019. That is
   correct.
4. **LED compensation shader:** make it sign-preserving (the module guide
   §6 changes).
5. **Hook order:** the carry must run before NLS at the RGB stage, and the
   restore before LED measurement and the PRE_OUTPUT texture hooks.
   `OutputEncodingRepair` is unaffected; it works at OUTPUT.
6. **Settings:** one switch, e.g. `preserve_extended_range: on` (default
   on?), with off giving today's exact path. That is the owner's call.

## 5. The fork alternative (not recommended now)

The project already ships a locally patched libplacebo (`billslack2/libplacebo`,
source and build manifest in `3rdparty/libplacebo/source`). Patching
`linearize` and `sigmoidize` to be sign-preserving and extended would be
possible. But this PC has no MSYS2/MinGW/meson toolchain, and the hooks
above reach the same result without a rebuild. Keep the fork as a fallback
if a later libplacebo change breaks the hooks.

## 6. Proposed build order (after Stage 6)

1. Hooks (carry + SDR conversion), the switch, and GPU tests from the
   probes, all checked against the measured tables above.
2. Gamut compression for BT.2020 → P3, with tests.
3. LED compensation sign fix.
4. HDR study.
