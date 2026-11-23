# Tests for `input.capture`

- `EarlyUploadSecondReaderTests.cpp` (refs/105 5a): `EarlyUpload::SlotReusable` truth table; on the GPU test device a
  one-slot pool is not reused until the payload, the render side and the second reader release it, the reader reads
  the uploaded bytes, a dead reader holds its slot only until `DetachReader`, re-attach works, Close with a reader.
- `SourcePlanesStageTests.cpp` (CB slice 18 S1): P13 stage pictures served by stage + size + ordinal as 0-byte frame-scoped
  views with their facts; every decline says why; no crop axis; dropped at the next Begin, released at the next seal; render
  thread only; the table bound and its counted refusal. CPU only.

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).
