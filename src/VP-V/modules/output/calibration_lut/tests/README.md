# Tests for `output.calibration_lut`

The automatic-calibration tests live here (M2 step 3, WP-12). VideoProcessor-Test compiles them (filter "VP-V"); run them single-threaded and read the TRX (see ../README.md, "Test it standalone").

- `CalibrationCoreTests.cpp`, `CalibrationRemainderTests.cpp`, `CalibrationPhase2Tests.cpp`: CPU, no hardware.
- `CalibrationMeterTests.cpp`: the real ConPTY with `x64\Release\test-tools\fake_spotread.exe` (project `src/VideoProcessor-FakeSpotread`); hardware cases skip unless `VP_TEST_METER=1`.
- `CalibrationTestSupport.h`: the scripted terminal and paths; `data/spotread/`: the spotread transcripts it plays.

The render-time LUT tests (`LibplaceboLutParserTests`, `LibplaceboOutputPolicyTests`, `ExtendedRangeCarryTests`) stay in VideoProcessor-Test: they test Bill's `LibplaceboDisplayLut` and the shared output policy.
