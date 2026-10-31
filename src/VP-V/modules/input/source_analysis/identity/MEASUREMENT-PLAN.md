# Measurement plan: classical-only source identity on ABR reels

Goal: measure how reliable the classical voters are alone, and decide from data whether the ID net must come forward. Nothing in this plan assumes a codec, rate or resolution; every threshold below is to be measured and then written into `Settings`.

## Reel sources

1. Recorded streaming-box sessions through the capture card, with bandwidth throttled in known steps (a shaped network link or a bandwidth cap on the box).
2. Network shaping that forces rendition switches at known times (step down, step up, brief dips, sustained low).
3. Ground truth per reel: the player's stats overlay or its logged rendition switches, time-aligned to capture timestamps (a visible marker or the first frame after a known cut).
4. Control reels with no switches (long constant-quality titles) for false-positive rates, and reels with heavy dark scenes, fades, and cut-dense content for confound rates.

## Metrics

- Per field (axis H, axis V, kernel, codec class, quality tier): accuracy against ground truth, as a share of title time.
- Time to latch after a true switch (source time from the real boundary to the consensus change), median and 95th percentile.
- False-change rate per hour: consensus changes with no true switch.
- Flaps per hour: a change followed by the reverse change within the minimum hold.
- ABR detector precision and recall against the logged switches, and the timing error of the boundary.
- Notice false-positive rate per title-hour, and missed-notice rate for true sustained drops.
- Confound rate at cuts: share of detected steps that sat on a cut, and how many were rightly dropped.
- Dark and fade abstain share: time the detector had too few valid samples to judge.

## Per codec

Repeat for AVC, HEVC and AV1. VVC and LCEVC only when a real source exists; measure, do not assume (for example whether an LCEVC enhancement layer drops without a resolution step). Record which constants (windows, step size, stability bound) each codec needs; if they differ, that is a finding to report, not a branch to hard-code.

## Replay harness

Use the SRM history CSVs captured during the sessions. A deterministic CPU harness replays them: build `ClassicalReading` and `QualitySample` rows, feed `Consensus` and `AbrStepDetector` in source time, and write the result trace (`Result::Serialize`, `SerializeEvent`). The same CSV must always give the same trace, so threshold changes can be compared run against run.

## Decision rule for bringing the ID net forward

Bring the net forward when any of these holds on the measured reels:

- classical false notices exceed 1 per 10 title-hours;
- axis or kernel accuracy falls below the target the owner sets after seeing the first results;
- the median time to latch after a real switch exceeds a bound X (set from the first results, as a share of the typical segment length);
- flaps exceed the agreed budget per hour.

Otherwise keep classical-only and re-measure when the net's bitrate-tier head is re-validated.
