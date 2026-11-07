# Tests for `repair/temporal_detail/products`

`TdProductsTests.cpp` (VideoProcessor-Test, CPU only): dense ids after P14 and a valid registry; the contract table equals the registry tokens
and fill rules; the base buffers' chroma planes; the VRAM budget per tier (hand-summed at 1080p / 4K / 8K); the read rule (not requested,
requested without products, stale / foreign version / recovery); dither need, the CfL result and the adaptive stage unchanged while the engine
is not requested. The catalog agreement (consumer rows = `NeededBy`, unit version = catalog expectation, live-fill owners) is in
`src/VP-V/tests/ProductCatalogTests.cpp`.
