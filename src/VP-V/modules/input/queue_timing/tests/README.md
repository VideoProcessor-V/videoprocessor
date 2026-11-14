# Tests for `input.queue_timing`

Standalone module tests land here as the module migrates. Until then the tests listed in ../README.md live in the legacy test projects (VideoProcessor-Test, VideoProcessor-BetaTests, VideoProcessor-ConfigTests).

- CaptureRepeatBitTests.cpp (C8-PROD, R14-59): the always-on capture signature / captureRepeat bit (kept on unlocked queued frames, pulldown on/off parity), ResolveRepeatFacts consumer rows, the Ctrl+Alt+D line and the 4K hash-cost measurement.
