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

## Identity reels R1-R9 (SID-D, doc 78 section 9)

Shared by the classical detector, the chain estimator (SID-C) and the TD-2 frame-pattern scorer. Machine time, CPU only. Rung files are
the only large artefact (about 3-6 GB, cap 15 GB); the PLAYER CHAIN is never rendered to disk, it is an ffmpeg `-filter_complex` string
stored per reel in `reels.json` and executed by the harness pipe. No clip content or frame is committed to git; the media and the
manifest live in `D:\testclips\sourceid\` (see its README for sources and licences).

1. `python -I tools/eval/source_id/make_identity_reels.py --out D:/testclips/sourceid` (`--plan` first: reel and rung counts, size estimate, disk).
   Rungs: a master window re-encoded once at its coded size `N x ratio` (1/2, 2/3, 1/3, 5/12, 1/4; letterbox 2.39 and pillarbox 4:3 variants with the
   bars coded; native-4K stream encodes x265 8/10-bit and x264; studio 1080 to 2160 encodes) in `rungs\`, GOP = round(2 x master rate), scenecut off,
   frame types from ffprobe, per-frame QP from the x265 csv (`<rung>.frames.csv`; x264 and AV1 rungs have `qp = -1`). `--av1` adds one libaom 4K reel (slow).
2. `reels.json` (schema 1) holds `masters` (path, licence, url), `rungs` (coded size, active area, codec, rate, measured kbps) and `reels`.
3. `evaluation`: `run_identity_eval.ps1` (vstest on `SourceIdChainEvalTests`, then the scorer) or by hand:
   `set VP_SOURCE_ID_REELS=D:\testclips\sourceid\reels.json`, `set VP_SOURCE_ID_EVAL_OUT=<dir>`, `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:SourceIdChainEvalTests`
   (`VP_SOURCE_ID_REELS_ONLY=<id substrings>` selects reels). Output: `evaluations.csv`, `events.csv`, `reels_done.csv`.
4. `python -I tools/eval/source_id/score_identity.py --reels <reels.json> --eval-dir <dir>` writes `summary.md` and `gates.json` (the GO table of doc 78
   section 9). `--selftest` runs the scorer on a tiny synthetic CSV.

### Reel families (about 175 reels, each >= 30 s at the full raster, flagged when a short master had to loop)

| Family | Content | Truth |
|---|---|---|
| R1 | native 4K masters as-is (8-bit), native 4K stream encodes x265 3 / 8 / 20 Mbps (8 / 10-bit), x264 8 Mbps | native-coded raster |
| R2 | rungs 1/2, 2/3, 1/3, 5/12, 1/4 on 7 masters x a rotating kernel (clean 10-bit); kernel sweep at 1/2; dirty (USM 0.6 / 1.0, 8-bit ordered or noise dither, crf 28 rung); USM sweep | n per axis, kernel, sharpening |
| R3 | 720 -> 1080 -> 2160 and 540 -> 1080 -> 2160 with two kernels | both stages (`double.n1`) |
| R4 | letterbox 2.39, pillarbox 4:3, 1:1 window and scaled window on a gray 4K desktop, mid-reel aspect/zoom toggles | active area + n |
| R5 | studio 1080 -> 2160 encoded at 2160 (crf and low rate) | native-coded; picture size "unsure" allowed (`picture_n_unsure_allowed`, `latent_picture`) |
| R6 | ABR splices at GOP boundaries: resolution down/up, quality-only down/up (crf 20 to 36), dip, alternation | `events` (geometry / quality, major / minor) |
| R7 | burned subtitles, PiP (small = masked, large = HOLD expected), synthetic 4K UI menu <-> stream with cut / 250 ms / 500 ms black / fade | regime, `hold_expected` |
| R8 | two stops darker, flat CG / anime, credits scroll, fades through black, HDR-origin grain 60p | per R1 / R2 |
| R9 | 24p / 23.976p / 25p / 30p to 60 / 59.94 / 50 with 3:2 or 2:2 repeats, and blended FRC | per R2, `repeat` / `blended` flags |

R10 (loop) and R11 (Apple TV) need the GPU rig / the owner and have no reels here. Fast pans come from the 60p and 30p sport masters; the static long take is the Funchal window master.

### Truth in `reels.json` (per reel: `segments[]`, `events[]`, `truth_csv`)

Segment fields: `out_start_frame`, `out_frames`, `regime` (stream / menu / black), `truth_n {x,y}` (the original size per axis a re-base should find: raster x
coded/active-area ratio; the raster itself for native), `coded_raster`, `n_active_coded`, `active_area_out {x,y,w,h}`, `native_coded`, `kernel {name,family,params}`,
`sharpening {amount,radius,msize}`, `double {n1,kernel1}`, `bits` (8 or 10: the plane depth the chain delivers), `dither`, `chroma_chain` (for SID-F),
`hold_expected`, `picture_n_unsure_allowed`, `latent_picture`, `rung {codec,rate,kbps}`, `frc`. `events[]`: `frame`, `kind` (geometry / quality / regime / content),
`cls` (major when the geometry changes or the rate steps >= 1.6x), `direction`.

### Per-frame truth CSV for TD-2 (`truth\<reel>.frames.csv`)

`reel,out_frame,t_s,seg,regime,src_id,src_frame,pict_type,qp,repeat,blended,gop_idx,idr,flags`: output frame to the source frame it shows (index within the rung or
master window), `pict_type` I / P / B from ffprobe on the encode (as-is masters: the container codec's own types, indicative only), `qp` the x265 per-frame QP (-1 =
none: menu / black frames, x264, AV1, as-is masters), `repeat` 1 when the output frame repeats the previous source frame (FRC), `blended` 1 for blended FRC outputs,
`gop_idx` / `idr` from the fixed GOP, `flags` fade_in / fade_out. The 3:2 mapping follows ffmpeg's `fps` filter (checked against real output: 3,2,3,2 from frame 0).

### Output CSVs of the harness

* `evaluations.csv`: one row per analysis (every 0.5 s of source time) per lane `off` / `on` (classical monitor, consensus off / on) and, once SID-C2 plugs into the
  seam, `chain`. Columns: `reel,lane,out_frame,t,seg,bits,fx,fy,n_x,n_y,state,declared,conf,frame_quality,noise,classical_quality,tier,level,rec_ratio_h,rec_ratio_v,gen,reason,ms,`
  `kernel,sharpen_amount,double_n1_x,double_n1_y,active_x,active_y,active_w,active_h,llr_x,llr_y`. Classical lanes: `n` = applied re-base factor x raster (the engaged
  AutoFactors decision), `state` LATCHED when engaged, CANDIDATE when a ratio is declared but not engaged, else UNKNOWN. Chain lane states: UNKNOWN PROVISIONAL CANDIDATE
  LATCHED HOLD UNSTABLE ABSTAIN (doc 78 section 4); `reason` is the UNSURE reason.
* `events.csv`: `reel,lane,t,out_frame,seg,type,a,b,detail` with type `engage` (factor change), `declared`, `abr`, `notice`, and `geometry` from the chain lane.
* The evaluator seam (`SidChain::Seam()` in the test): `frame(luma10, w, h, stride, seq, t)` per decoded frame (10-bit codes, 8-bit segments scaled x4 like SourceFrameTap),
  `reelBegin` / `reelEnd`, and `chainRow(t, row)` to emit chain-lane rows.

Thread budget: one ffmpeg per reel (`VP_SOURCE_ID_REELS_FFMPEG_THREADS`, default 3) plus the test thread; the runner pins the vstest tree to 6 logical processors.
