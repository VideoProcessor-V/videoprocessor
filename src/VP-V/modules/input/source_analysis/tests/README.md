# Tests for `input.source_analysis`

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).

Here (VideoProcessor-Test, CPU only):
- `SourceFrameTapTests.cpp` (`SourceFrameTapTests`): the source frame tap; `SourceViewOfIsTheTapsViewForEveryFormat` pins `VpFrameAnalysis::SourceViewOf` (C9 commit A).
- `HalfLumaEngineViewTests.cpp` (`HalfLumaEngineViewTests`, C9 commit A, doc 105 phase 4a): the engine-half law exhaustive over S = 0..4092, the engine quarter (the producer's own law, not the P1 quarter), `P1Reader` (lane published under the slot seqlock / live fill / lane off) against a frozen copy of the producer's `HalfLuma8` per format at 384x216 and 376x214 (byte-identical for P010, P210, DecodedDv422Full, V210, Uyvy, SdrTunnel, PackedDvR210; RGB structure luma and raw full-range 4:2:x are the pinned named exception commit B handles), and the seqlock (a recycled slot is never returned; threaded writer/reader, no torn copy). Filter: `/TestCaseFilter:"FullyQualifiedName~HalfLumaEngineViewTests"`.
