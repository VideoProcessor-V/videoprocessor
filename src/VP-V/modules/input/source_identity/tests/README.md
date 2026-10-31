# Tests for `input.source_identity`

CPU-only unit tests, registered in `src/VideoProcessor-Test/VideoProcessor-Test.vcxproj.vcxproj`:

- `SourceIdConsensusTests.cpp`: dwell, hold on disagreement, flapping, stale votes, safety veto, per-axis Indicator port calls, the net's trust profile, minimum hold, ABR dwell, discontinuity, determinism and submission-order independence.
- `AbrStepDetectorTests.cpp`: ABR step detection on synthetic series (fixed-seed LCG, 4 samples per second of source time) and the one-time notice policy (`AbrNoticePolicyTests`).

No GPU, files or clocks. Run with `/Tests:SourceIdConsensusTests,AbrStepDetectorTests,AbrNoticePolicyTests`.
