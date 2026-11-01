<!-- Copyright(C) 2026 VP-V contributors -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->
# vp-stamp/1: frame stamps for end-to-end checks

A small machine-readable stamp painted into a frame corner so a recording of VP's
output (StreamRecorder `--stamps`) can be matched to what was sent: which source
frame, which cadence phase, in which order, with nothing dropped or repeated.

The format's single source of truth is
`src/VP-V/modules/control/diagnostics/FrameStamp.h`; `framestamp.py` mirrors it
bit-exactly (golden vectors in `FrameStampTests.cpp` and `test_framestamp.py`).

## Format

- Payload, 64 bits, MSB first: magic 8 bits (0xA5), kind 2 (0 source, 1 output), aux 6
  (source: cadence phase; output: low 6 bits of the source sequence), index 32,
  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, xorout 0) over the first
  48 bits as 6 bytes.
- Geometry proportional to the frame height: unit u = H / 270. The block is 18 x 7 units
  including a 1-unit dark border; the reference row (16 cells alternating 1,0,1,0,...) is
  followed by 4 data rows of 16 cells; every cell is 1u x 1u. Source stamp top-left at
  (2u, 2u), output stamp top-right at (W - 2u - 18u, 2u). A pixel belongs to a cell when
  its centre lies inside the cell, so a stamp scaled with the picture keeps its geometry.
- Luma only: 1 = white, 0 = black at the frame's depth and range (limited 64/940 at
  10 bit, 16/235 at 8 bit; full 0/max). Chroma under the block should be neutral.
- Decoder: inner 50% of each cell, threshold from the reference row, contrast of at least
  25% of the code range, alternating reference row, magic and CRC; small shifts are found
  by a search (default radius 1 unit, step 0.25 unit). Any bit depth.

## Files

- `framestamp.py`: `word`, `write`, `decode` on numpy arrays (any depth), `cpu_reference`.
- `make_stamped_clip.py`: writes a Y4M and `sent.csv` (`index,aux,pts_num,pts_den`).
  `--fps` (NUM/DEN) and `--size` (WxH) are required, there are no defaults.
  `--cadence 2:3` makes aux cycle 0..4 (index mod the pattern length; without
  `--cadence` aux is 0). `--420` writes C420p10 instead of C422p10.
  `--pattern levels --set levels10` draws the base picture with `tools/levels/patterns.py`
  (used when that module exists, otherwise the flat grey is used with a notice).
- `test_framestamp.py`: `python -I -B tools/framestamp/test_framestamp.py` (any cwd).

## Making a test file (documented only; not run by the tests)

```text
python -I tools/framestamp/make_stamped_clip.py --out D:\testclips\stamp --frames 240 --fps 24000/1001 --size 3840x2160 --cadence 2:3
D:\tools\ffmpeg\bin\ffmpeg.exe -i D:\testclips\stamp\stamped.y4m -c:v libx265 -pix_fmt yuv420p10le -crf 10 -tag:v hvc1 D:\testclips\stamp\stamped_hevc10.mp4
D:\tools\ffmpeg\bin\ffmpeg.exe -i D:\testclips\stamp\stamped.y4m -c:v prores_ks -profile:v 3 -pix_fmt yuv422p10le D:\testclips\stamp\stamped_prores.mov
```

Lossy encoders blur the stamp edges slightly; the decoder samples only the inner half of
each cell, so a high-quality encode decodes. Play the file on the source device, record
VP's output with `VideoProcessorStreamRecorder.exe --stamps`, then compare `stamps.csv`
with `sent.csv`.

## Output stamp (debug only)

`FrameStampShader.h` builds a libplacebo OUTPUT-stage hook that draws the output stamp
after the quantiser (`OutputHookOrder::Kind::DebugStamp`, rank 500). It exists only when
the environment variable `VP_DEBUG_OUTPUT_STAMP` equals `1` (default off, no config key);
off means no hook text at all, so normal viewing is byte-identical.
