# Tests for `scaling.chroma_upscale`

`ChromaCflTests.cpp` (compiled into the VideoProcessor-Test project; the module headers are included as `<VP-V/modules/scaling/chroma_upscale/...>`):

- `ChromaCflModelTests` (CPU only): `ChromaCflFlatChromaStaysExact` (incl. BTB 40 / WTW 1000),
  `ChromaCflSitingMatchesChromaSamplesAtSites`, `ChromaCflReconstructsLumaCorrelatedEdges`, `ChromaCfl422Works`,
  `ChromaCfl444IsNothing`, `ChromaCflGrainIsPassedNotSmoothed`, `ChromaCflDeterministic`, `ChromaCflActivePictureKeepsBars`,
  `ChromaCflSettingsParse`.
- `ChromaCflGpuTests` (libplacebo on WARP; `VP_TEST_GPU=hardware` selects the hardware adapter): GPU vs model within 0.02 codes
  (`ChromaCflGpu420MatchesModel`, `ChromaCflGpu422MatchesModel`, `ChromaCflGpuLetterboxMatchesModelAndKeepsBars`),
  `ChromaCflGpuOffReturnsTheSameFrame`, `ChromaCflGpuOutputFrameIsFullSizeWithZeroShifts`,
  `ChromaCflGpuThreePlaneSourceMatchesTwoPlane`, `ChromaCflGpuDeterministic`, `ChromaCflGpu444IsNothing`, and
  `ChromaCflGpuTiming` (category `GpuHardware`, skipped unless on a hardware GPU; 3840x2160 and 1920x1080 4:2:0, median over 40 frames).
