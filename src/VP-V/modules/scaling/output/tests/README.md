# Tests for `scaling.output`

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).

## X5 cut A/B (FIX-X5-CUT-P4)

The owner requires a GPU before/after of the grain-term cut before x5dup lands. Since 2026-11-15 (owner-intent fix) the A/B counts FALSE cuts as well as missed cuts: a false cut is equally unacceptable.

- Capture: `X5AbCaptureTests.inl` (included by `AdaptiveScalingOneAxisMotionTests.cpp`). It does nothing unless `VP_X5AB_LIST` is set, so a pass without that variable is not evidence of anything and is never cited. Both sides run the same `.inl` text; the before branch must carry the commit that added the `cut_state` column (the scorer refuses a CSV without it).
- Clips: `tools/x5ab-extract.ps1` (default `-Set cuts`) makes the cut set: `cut` (a splice at frame 60), `flash` (+64 codes on frames 60-61), `pan` (one still frame panned, no cut) and `fade` (one still frame faded to black and back, no cut). It writes `x5ab-list.txt` and, only when absent, the truth template `x5ab-truth.txt`.
- Truth: hand-marked. `pan` and `fade` are `synthetic` (no cut by construction). `cut` and `flash` are live footage and start `UNMARKED`: a person watches them, confirms the splice, adds any real cut inside either clip and sets `marked:<who>:<date>`. The scorer refuses UNMARKED lines. The truth file is data and is never overwritten by the script. No clip pixels are committed.
- Run each side under GPU-QUIET on the same adapter (`VP_TEST_GPU=hardware`, `VP_X5AB_LIST`, `VP_X5AB_OUT`, `VP_X5AB_SIDE=before` or `after`; clear the CSV first, the harness appends), then `tools/x5ab-score.ps1 -Out <dir>`.
- Report, per clip and per side: missed cuts, false cuts (total and per minute), recall and precision against the truth list, and the grain-term snaps with their frames (every CSV row is one advance, so `cut=1` is one snap; the definitional first-frame snap, `cut_state=first`, is counted as a snap but never scored as a cut).
- Pass rule (after side): zero false cuts on `pan`, `flash` and `fade`; no missed truth cut on `cut`; no Unavailable or unasked frame (otherwise "no false cut" could be vacuous).
- A false cut on the after side is a defect of the SHARED U7 law (`CutFadeJudge::ClassifyEvents`): report it to U7-ACCURACY. Never fix it with an x5dup-local threshold or veto (one copy; no private detector). If U7-ACCURACY has landed a fixed law by the run, the after side runs on it, with x5dup's call site unchanged.
- These 5-second clips are a per-clip A/B, not the U7-ACCURACY recall/precision >= 99.5 % gate.
- The `__has_include` / `X5AB_TIP_CUT` toggle in `AdaptiveScalingOneAxisMotionTests.cpp` (gate S5) stays until the A/B has run; its removal is a separate WAIVER commit.
