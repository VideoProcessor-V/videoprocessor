# Streaming-realistic suite: findings (2026-11-01)

Suites: `suites/streaming_hevc.json`, `streaming_hevc_uhd.json`, `streaming_av1.json` (tos, sintel, meridian, crowd, grain35;
HEVC/AVC/AV1/AV1-FGS, per-shot VBR with dark-weighted allocation, uniform-bitrate controls, congestion rung switches).
Encoders run single-threaded (multi-threaded x265/x264/SVT-AV1 were measured non-deterministic run to run; single-threaded
repeats were bit-identical, 3 runs each); speed comes from `--jobs` parallel processes.

NR-vs-FR validation (`nr_validate.py`, ~870 sampled frames, `streaming_validation_corr.md`):
* noise: within-clip Spearman -0.60 against FR noise loss (expected sign) - the one metric that tracks rate reliably.
* block: 0.46 pooled / 0.29 within-clip; ring: 0.44 pooled / 0.03 within-clip (content-driven pooled); detail: 0.30 within-clip.
* band: no signal (-0.06 / 0.03): these masters/rates produce little visible banding; needs smooth CG gradients at low rate.
* Lowest-vs-top rung AUC (same raster) is 0.49-0.58 for all: absolute "poor" thresholds are NOT reliable. Use relative drops
  within one stream (noise/detail vs the stream's own baseline) and the per-second series.
HEVC deblocking/SAO leaves little classic blockiness or ringing at 3-6 Mb/s; the dominant loss is fine detail and grain.

Recordings: D:\testclips\pulldown holds six timing-only recordings (frames.csv + labels.csv, no pixels, no thumbs.y8, no raw
frames), so no quality-over-time exists for them; `scan_recordings.py` reports cadence/timing and will report NR quality as soon
as a recording has raw frames or nr.csv. No other StreamRecorder folders with pixels were found on C: or D:.
