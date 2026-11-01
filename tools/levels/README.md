# Level and colour verification tools

Python 3.14 + numpy. Run the self-tests from any directory:

    python -I -B tools/levels/test_levels.py

## Capture assumptions

The loop capture is a DeckLink Mini Recorder 4K: 10-bit YUV 4:2:2 (v210), standard
modes only, at most 2160p30. Codes are verified directly in 10-bit; there are no
"8-bit limited" rows. No rate or resolution is hard-coded: patch geometry is
fractional and the frame size is always an argument.

## Workflow

1. Generate a pattern and its layout (size is required):

       python -I tools/levels/patterns.py --set levels10 --size 1920x1080 --frames 4 --out scratch [--v210]

   Sets: `levels10`, `levels8`, `ramp10` (64 constant steps, 4..1019, monotonic check),
   `bars709`, `bars2020` (100% and 75% amplitude R'G'B' bars, 75% includes the white bar),
   `primaries2020` (R G B C M Y at 100% and 50%), `near_black` (64..80 step 2, 9 patches),
   `near_white` (924..1012 step 8 plus 1019, 13 patches). Output is Y4M `C422p10`
   (frame rate unspecified, `--fps` optional) or raw packed v210 (`--v210`), plus
   `<set>.layout.json` (patch id, fractional and pixel rect, source Y'CbCr codes,
   matrix, range, transfer).

2. Play it through the chain under test and capture it (Y4M, or StreamRecorder
   `raw_*.bin` v210 frames).

3. Check:

       python -I tools/levels/check.py capture.y4m --layout scratch/levels10.layout.json --frames 8 --out report
       python -I tools/levels/check.py stream-DIR --layout L.json --src bt1886:bt709:limited:10 --dst pq:bt2020nc:limited:10

   Inputs: a Y4M file, `raw_*.bin` files (`--width --height --row-bytes`; sizes are
   read from the sibling `events.txt` when omitted; row bytes default to the v210
   128-byte stride), or a StreamRecorder directory (raw frames listed in
   `events.txt`, format must be `10BitYUV`). Each patch's inner 50% is averaged
   over up to `--frames` frames (the mean absorbs sigma-delta dither).
   Writes `report.json` and `report.md`.

4. Print the expected-code table of a chain:

       python -I tools/levels/expected.py --set levels10 --size 1920x1080 --format md --src ... --dst ...

## Verdicts

Each row is PASS, FAIL or NO DATA (missing frames, patch outside the frame, ramp
segments or BTB/WTW patches without data). Overall: FAIL if any row fails, else
INCOMPLETE if any row is NO DATA, else PASS. NO DATA never counts as PASS.
Exit codes: 0 PASS, 1 FAIL, 3 INCOMPLETE, 2 usage or input error.

- Passthrough chain (same transfer/matrix/range/depth): max code error (Y, Cb, Cr) <= 1.
- Processed chain: CIEDE2000 <= 1 for a bt1886 destination (XYZ relative to the display
  white, Y = 100), delta E ITP (BT.2124) <= 1 for PQ/HLG destinations.
- `ramp_monotonic`: no segment mean falls more than the tolerance and last > first.
- `btb_reached` / `wtw_reached`: where the chain keeps BTB/WTW (default) every patch
  expected below nominal black (10-bit 64) or above nominal white (940) must measure
  beyond it. The owner rule is that both are preserved. Sets without such patches
  omit the row and say so in the report notes (omission is not a pass).
- Thresholds: `--thresholds file.json` with any of `max_code_error`, `max_de2000`,
  `max_de_itp`, `ramp_tolerance`, `btb_wtw_margin`; unknown keys are rejected.

## Reserved codes

10-bit codes 0-3 and 1020-1023 are timing reference codes in SDI/HDMI-style
signals and may be replaced or blanked by receivers. Patterns never generate them,
so `levels10` stops at 4 and 1019, and `levels8` skips 8-bit 0 and 255 (= 0 and
1020 as 10-bit). A capture that shows such codes in a patch is not trusted.

## Expected-code model

Codes -> normalised Y'CbCr -> R'G'B' -> linear nits (BT.1886 with Lw/Lb, default
100/0; PQ absolute; HLG OOTF with system gamma for a given display peak) ->
primaries conversion (3x3 matrices computed from chromaticities, D65) -> target
inverse EOTF -> codes, round half to even. All transfer functions are extended
oddly so BTB/WTW survive unclipped when `keeps_btb_wtw` is set (`--no-keep-btb-wtw`
clips R'G'B' to 0..1). An SDR source in a PQ/HLG target sits at reference white
203 nits (BT.2408, `--ref-white`). There is no tone or gamut mapping.
