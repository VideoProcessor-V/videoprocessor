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
the only large artefact (cap 20 GB; the generator prints a measured estimate in --plan); the PLAYER CHAIN is never rendered to disk, it is an ffmpeg `-filter_complex` string
stored per reel in `reels.json` and executed by the harness pipe. No clip content or frame is committed to git; the media and the
manifest live in `D:\testclips\sourceid\` (see its README for sources and licences).

1. `python -I tools/eval/source_id/make_identity_reels.py --out D:/testclips/sourceid` (`--plan` first: reel and rung counts, size estimate, disk).
   Rungs: a master window re-encoded once at its coded size `N x ratio` (1/2, 2/3, 1/3, 5/12, 1/4; letterbox 2.39 and pillarbox 4:3 variants with the
   bars coded; native-4K stream encodes x265 8/10-bit and x264; studio 1080 to 2160 encodes) in `rungs\`, GOP = round(2 x master rate), scenecut off,
   frame types from ffprobe, per-frame QP from the x265 csv (`<rung>.frames.csv`; x264 and AV1 rungs have `qp = -1`). `--av1` adds one libaom 4K reel (slow).
2. `reels.json` (schema 1) holds `masters` (path, licence, url), `rungs` (coded size, active area, codec, rate, measured kbps) and `reels`.
3. `evaluation`: `run_identity_eval.ps1` (vstest on `SourceIdChainEvalTests`, then the scorer) or by hand:
   `set VP_SOURCE_ID_REELS=D:\testclips\sourceid\reels.json`, `set VP_SOURCE_ID_EVAL_OUT=<dir>`, `vstest.console.exe x64\Release\VideoProcessor-Test.dll /Tests:SourceIdChainEvalTests`
   (`VP_SOURCE_ID_REELS_ONLY=<id substrings>` selects reels). Output: one file set per reel under `<out>\reels\<reel>\` (written to `<reel>.tmp` and renamed when complete, so a restart skips finished reels; `VP_SOURCE_ID_EVAL_FORCE=1` redoes them) and `reels_done.csv`. Between reels the harness honours `<out>\STOP`, pauses while `D:\codex\build-slots\PRIORITY` exists and stops below 40 GB free. A STOP file is consumed at the end of the run (renamed `STOP.done.<timestamp>`), so a rerun of the same output folder runs. A reel whose directory cannot be placed is a failure (`rename_failed`), and a later success removes its stale `<reel>.failed` directory.
4. `python -I tools/eval/source_id/score_identity.py --reels <reels.json> --eval-dir <dir>` writes `summary.md` and `gates.json` (the GO table of doc 78
   section 9). `--selftest` runs the scorer on a tiny synthetic CSV.

### Reel families (reel counts and hours are printed by `--plan`; every non-R1 reel is >= 30 s, R2.clean >= 60 s, R1 is the FULL length of its master; no loops except the flagged sol levante / nocturne masters)

| Family | Content | Truth |
|---|---|---|
| R1 | native 4K at the FULL length of 13 masters (+ the Sidemen real AV1 4K stream as-is): as-is (8-bit), and stream encodes x264 veryfast 8-bit 2.5 Mbps (blocky) / 8 / 16 Mbps, x265 superfast 10-bit 6 / 14 Mbps; variants are cut (highest first, down to as-is + the blocky x264) when the measured estimate exceeds the 8 h budget; shot boundaries per master (ffmpeg scdet) | native-coded raster |
| R2 | clean: 10 masters x 5 ratios (1/2, 2/3, 1/3, 5/12, 1/4) at 60 s with a Latin-square kernel assignment (every kernel at every ratio; a quarter 8-bit with ordered dither); kernel sweep at 1/2; dirty (30 s): USM 0.6 / 1.0, 8-bit ordered or noise dither, crf 28 rung; USM sweep | n per axis, kernel, sharpening |
| R3 | 720 -> 1080 -> 2160 and 540 -> 1080 -> 2160 with two kernels | both stages (`double.n1`) |
| R4 | letterbox 2.39, pillarbox 4:3, 1:1 window and scaled window on a gray 4K desktop, mid-reel aspect/zoom toggles | active area + n |
| R5 | studio 1080 -> 2160 encoded at 2160 (crf and low rate) | native-coded; picture size "unsure" allowed (`picture_n_unsure_allowed`, `latent_picture`) |
| R6 | ABR splices at GOP boundaries: resolution down/up, quality-only down/up (crf 20 to 36), dip, alternation | `events` (geometry / quality, major / minor) |
| R7 | burned subtitles, PiP (small = masked, large = HOLD expected), synthetic 4K UI menu <-> stream with cut / 250 ms / 500 ms black / fade | regime, `hold_expected`, overlay windows |
| R8 | two stops darker, flat CG / anime, credits scroll, fades through black (`fade_spans` in the truth, `fade_in` / `fade_out` flags in the per-frame CSV), HDR-origin grain 60p | per R1 / R2 |
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

* `evaluations.csv`: one row per analysis (every 0.5 s of source time) per lane `off` / `on` (classical monitor, consensus off / on) and, once SID-C2 registers into the
  seam, `chain`. Columns: `reel,lane,out_frame,t,seg,bits,fx,fy,n_x,n_y,state,declared,conf,frame_quality,noise,classical_quality,tier,level,rec_ratio_h,rec_ratio_v,gen,reason,reason_h,ms,cuts,latched_axes,`
  `kernel,sharpen_amount,double_n1_x,double_n1_y,active_x,active_y,active_w,active_h,measured_n_x,measured_n_y,state_x,state_y,llr_x,llr_y,power_x,power_y,spatial_x,spatial_y,reason_bits_x,reason_bits_y,reason_x,reason_y`.
  Classical lanes: `n` = applied re-base factor x raster (the engaged AutoFactors decision), `state` LATCHED when engaged, CANDIDATE when a ratio is declared but not engaged, else UNKNOWN;
  `reason` = the AxisV reason, `reason_h` = AxisH, `latched_axes` = the record's latched bits, `cuts` = the monitor's cut counter. Chain lane: states UNKNOWN PROVISIONAL CANDIDATE LATCHED HOLD UNSTABLE
  ABSTAIN (doc 78 section 4), per axis state / measured n / applied n / kernel / sharpening / LLR / power / spatial agreement / reason bits. `n_x`, `n_y` are RASTER-referenced (raster x coded / active-picture
  ratio = the truth's `truth_n`), not window or active-area sizes.
* `events.csv`: `reel,lane,t,event_t,out_frame,seg,type,a,b,detail`: `engage` (factor change), `declared`, `abr` (`event_t` = the rung-change boundary the detector reports: matched against the truth boundary at
  +-1.0 s like replay.py), `notice`, and from the chain lane `latch`, `revert`, `hold`, `unstable`, `geometry` (matched within 2 s after the truth boundary), `note`.
* The evaluator seam is the header `identity/tests/ChainEvalSeam.h`: SID-C2's own test file registers with `static Tests::SidChain::Registrar reg(...)` (frame callback with 10-bit luma codes, `reelBegin` / `reelEnd`,
  `chainRow` filling a typed `ChainRow`, `chainEvents`); the harness never needs editing.

Thread budget (owner cap: 4 CPU threads in total): the generator and the runner run at BelowNormal priority pinned to 4 logical processors (`--affinity-mask 0xF`, runner `-AffinityMask 0xF`); decoder `-threads 2`
per input, filter graph 2 threads (1 for graphs with the noise filter), x265 `frame-threads=2:pools=4`. Only one generator or one vstest run at a time.

