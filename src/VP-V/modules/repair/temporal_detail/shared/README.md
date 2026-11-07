# TdMapsView (shared unit U19): `repair/temporal_detail/shared`

The ONE read API of the `td.maps` product (doc 72): 12 named per-16 px-block fields in 3 RGBA16F planes, a CPU twin, GLSL accessors
`vpu_td_maps_<field>(ivec2 block)` and the unit-version check. A consumer reads only its fields (`FieldsOf(Reader)`).
Unit: repair/temporal_detail/shared (`TdMapsView::kUnitVersion = 1`).

## Settings

| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics

| Name | Purpose | Default | Scope |
|---|---|---|---|
| (none) | | | |

## Products

Reads `td.maps` (id 27, unit version 1); no producer in reset 1. Fields: F0 validity, F1/F2 trail confidence (bands 0/1), F3 residual, F4 real
detail, F5 stability, F6 own grain, F7 coding noise, F8/F9 lighting gain/offset, F10 I-to-I drift, F11 band mask + reason. Absent, stale or another
unit version: the reader takes its classical form (see `../products/README.md`).

## Status and fallbacks

Through `TdProducts::Read` (`../products/TdProductReads.h`): "<consumer>: classical form (temporal-detail products: td.maps absent|stale|unit version)".

## Tests

`TdProductsTests::TdMapsViewTwelveNamedFieldsInThreePlanesWithOneAccessor` (CPU): `vstest.console.exe ... /TestCaseFilter:"FullyQualifiedName~TdProductsTests"`. No GPU.
