Spotread transcripts for CalibrationMeterTests (automatic calibration).

Scripts for fake_spotread.exe (src/VideoProcessor-FakeSpotread). They are
modelled on ArgyllCMS 3.x spotread output for each case VP must handle: a
colorimeter session, a spectro's sensor-calibration prompt, a failed reading,
no instrument, an initialisation failure, a hung instrument (timeout), an
unexpected exit and a pseudo-console redraw. The gated hardware test
(VP_TEST_METER=1) saves the real meter's transcript to
%TEMP%\vp-spotread-transcript.txt; commit such recordings here as
recorded_<meter>.txt and add them to the parser tests.
