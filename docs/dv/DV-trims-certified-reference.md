# dv_trims against a certified reference (hardware step, planned)

Goal: calibrate VP's non-certified `dv_trims` (L2 trims applied after libplacebo's tone
mapping) against Dolby's certified engine in the Apple TV. Same authored file, two routes:

1. **Reference:** Apple TV plays the file in **LLDV** mode (player-led: the Apple TV's
   certified Dolby engine applies the mapping and the trims for the EDID's target) ->
   capture card -> VP raw recording (`VP_RAW_CAPTURE_PATH`, Ctrl+Alt+W).
2. **VP:** the same file in **TV-led tunnel** mode -> VP with `dv_trims: off`, then `on`
   (Ctrl+Alt+S captures, or the raw recording rendered with
   `DvAutotestGpuRendersTunnelCorpusForLldvCompare`).

Compare patch by patch with the manifest (below) and `tools/dv_compare/dv_lldv_vs_tunnel.py`.

## The test file

`tools/dv_synth/export_hevc.py` (software only: libx265 through ffmpeg, dovi_tool 2.3.4
`generate` + `inject-rpu`; no hardware encoders):

```
python -I tools\dv_synth\export_hevc.py D:\codex\dv-synth-hevc --ffmpeg D:\tools\ffmpeg-full\bin\ffmpeg.exe --dovi-tool D:\tools\dovi_tool\dovi_tool.exe
```

- `dv-synth-p81.hevc`: Dolby Vision **profile 8.1** (HDR10-compatible BT.2020 PQ 4:2:0
  10-bit base layer + one RPU per frame), 1920x1080 23.976, 3 scenes x 48 frames: the
  corpus's policy pictures (wedge, colour patches, ramps, extremes) mastered at 4000 /
  1000 / 200 nits, L1 per scene, L6 4000/0.005, three CM v2.9 L2 trims (100, 600, 1000
  nits; the 100-nit trim lifts mids and desaturates a little). L8 is not authored yet.
- `generator.json`: the dovi_tool config (the authored metadata, readable).
- `manifest.json`: per scene the frames, L1, every patch rectangle and its source light
  (BT.2020 nits), and the trims (codes and decoded values).

Verified 2026-10-24: `dovi_tool info` on the extracted RPU reads profile 8, L1 3079 for the
mid scene, L2 targets 2081 / 2851 / ... as authored.

**Container (to do before playing):** ffmpeg 7.x copies the RPUs but writes no DV
configuration box, so Infuse / tvOS may not engage DV. Mux with one of (verified tools,
official releases):

- GPAC MP4Box: `MP4Box -add dv-synth-p81.hevc:dvp=8.1:xps_inband -fps 23.976 -new dv-synth-p81.mp4`
- MKVToolNix: `mkvmerge -o dv-synth-p81.mkv --default-duration 0:24000/1001p dv-synth-p81.hevc`

Check with `ffprobe` (a "DOVI configuration record" side data line) or MediaInfo
("Dolby Vision, Version 1.0, dvhe.08...").

## Steps (owner present, about 30 min)

1. Copy the muxed file to a share Infuse can read; Apple TV: Match Dynamic Range on.
2. Integral 2 EDID = **LLDV** (low-latency DV block), target = the value VP's profile uses
   (`sdr_target_nits`, 100 in the cinema profile) so VP adds no second tone map; record
   10-20 s per scene (raw recording).
3. Integral 2 EDID = **TV-led DV**; tunnel switch on; record the same; repeat with
   `dv_trims: on`.
4. Claude: align by scene, measure the manifest patches in each recording, report dE ITP
   and luminance ratios: VP (trims off / on) vs the Apple TV's LLDV picture.

## Limits

- LLDV is 12-bit (or 10-bit) YCbCr 4:2:2 PQ to the EDID target; the decode is not the
  question, the mapping is. The Apple TV's DM version (CM v2.9 vs v4.0 handling) applies.
- Authoring L8 (CM v4.0 trims, target display index via L10 / predefined targets) is the
  next extension of `generator.json`.
