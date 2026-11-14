# Tests for `input.source_analysis`

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).

Here (VideoProcessor-Test, CPU only):
- `SourceFrameTapTests.cpp` (`SourceFrameTapTests`): the source frame tap; `SourceViewOfIsTheTapsViewForEveryFormat` pins `VpFrameAnalysis::SourceViewOf` (C9 commit A).
- `HalfLumaEngineViewTests.cpp` (`HalfLumaEngineViewTests`, C9 commit A, doc 105 phase 4a): the engine-half law exhaustive over S = 0..4092, the engine quarter (the producer's own law, not the P1 quarter), `P1Reader` (lane published under the slot seqlock / live fill / lane off) against a frozen copy of the producer's `HalfLuma8` per format at 384x216, 376x214 and 382x214 (odd half width; not R12) (byte-identical for P010, P210, DecodedDv422Full, V210, Uyvy, SdrTunnel, PackedDvR210; RGB structure luma and raw full-range 4:2:x are the pinned named exception commit B handles, with P210 and UYVY at full range pinned too; `P1EngineViewDiffersFromProducer` is true exactly for them), and the seqlock (a recycled slot is never returned; threaded writer/reader, no torn copy). Filter: `/TestCaseFilter:"FullyQualifiedName~HalfLumaEngineViewTests"`.

- `P1AcquireSourceTests.cpp` (`P1AcquireSourceTests`, L-B4): `P1AcquireSource` (an `IP1Source` over an injected Acquire function). The identical formats read through a memo with a P1 owner fill equal the producer's `HalfLuma8` with one fill per frame across threads; the RGB / full-range formats read false (counted `OwnDecode`, no fill) until FIX-B4-RGB.
- `P1GpuFillTests.cpp` (`P1GpuFillTests`, L-B4): the float emulation of the P1 R16 unorm GPU kernel equals the lane CPU law bit for bit at 8/10/12/16 bits (odd sizes); the Float32 variant is the accessor mean.
