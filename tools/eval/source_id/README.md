# Source identity: streaming-suite evaluation (doc 61 section 2.5)

Machine-time, CPU only (ffmpeg decode + the real source-resolution monitor; no GPU, no VP playback leg).

1. `python -I tools/eval/source_id/make_abr_reels.py --out <dir>`: reads `tools/eval/suites/streaming_{hevc,hevc_uhd,av1}.json` and writes
   `reels.tsv` (a reel = segments of the suite's decoded rung files spliced at IDR points of the top-rung encode, or the suite's own
   `switch_congestion` case), `truth.tsv` (splice frame, direction, MAJOR/MINOR class) and `shots.tsv` (per-shot rate and complexity).
   Nothing is copied or re-encoded.
2. `VP_SOURCE_ID_EVAL_DIR=<dir> vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:SourceIdStreamingEvalTests`: the test
   decodes every reel and feeds tile copies (every 0.5 s of the frame clock) to two monitors, consensus OFF (today) and ON, with
   `AutoFactors` on every frame as the render loop does. It writes `frames.csv`, `events.csv`, `engages.csv`, `notices.csv`.
   `VP_SOURCE_ID_EVAL_ONLY=<substring>` filters reels; `VP_SOURCE_ID_EVAL_FFMPEG` overrides the ffmpeg path.
3. `python -I tools/eval/source_id/replay.py --dir <dir>`: writes `results.md` / `results.json`.

Scoring (NR metrics are weak, so nothing uses an absolute quality threshold):

* ABR events against the splice points (+-1.0 s): precision, recall (MAJOR drops and rises, MINOR steps reported separately), detection
  latency (median / p95), boundary error.
* No-switch controls: false events, consensus changes after the first 10 s (flaps) and one-time notices per stream-hour.
* False engages on clean high-bitrate sources: the top rung of each ladder (native raster, highest rate) with consensus off and on.
* Consensus latch time after a major drop; notice firing and latency.
* Relative quality: rung ordering (Spearman per stream) and starved shots (shot rate / complexity against the shot's quality relative
  to the stream's best shot).

`RESULTS.md` is the committed result of the last run (see its header for the commit it was produced from).
