# PictureRect (shared unit U8, `VpUnit::PictureRect`)

The one active-picture rect rule (doc 62, section 1.2 U8): `PictureRect.h`, `kUnitVersion = 1`. No new detector: Bill's rect is the authority when given, else the near-black rows/columns on the 1/8 luma, else the whole frame.

- `Compute` is the CPU reference of the matcher's PICTURE pass; `KernelHeader()` / `KernelBody()` are that pass's GLSL, moved byte-for-byte from `DejudderWarp.cpp` (the matcher dispatches them, and takes black / bar margin from `ForRange`).
- Readers: `Effective`, `InPicture`, `Coverage` (CPU) and `vpu_in_picture` / `vpu_picture_coverage` (`Glsl()`); `Halved` is the inner matcher's half-size rect.
- Registered bug fix, deferred: TextureRestore and the debander ignore the rect and gain `InPicture` / `Coverage` reads in their own packages (shader change, GPU run). TD / CfL rect handling moves with SA5.

Unit: input/source_analysis (picture/shared)

## Settings
| Key | Section | Type | Range / values | Default | Config placement | Original (Bill) | Fallbacks / status | Tiers |
|---|---|---|---|---|---|---|---|---|
| (none) | | | | | | | | |

## Environment and diagnostics
| Name | Purpose | Default | Scope (test/debug/runtime) |
|---|---|---|---|
| (none) | | | |

## Products
P4 active-picture rect (carried from Bill's rect, evidence rect as fallback). Unit version 1 (`kUnitVersion`); consumers: the matcher (live), P4 / TD / CfL readers later. Absent or version mismatch: the consumer uses the whole frame (visible per its own fallback id).

## Status and fallbacks
No downgrade of its own: with no rect given and no bars found the picture is the whole frame (not a fallback). GPU proof: the matcher's existing GPU tests (`DejudderHookTests`) run the unit's kernel text.

## Tests
`VideoProcessor-Test.dll`, class `SharedUnitsW1Tests` (CPU only; frozen-copy identity proofs and law cases): `vstest.console.exe x64\Release\VideoProcessor-Test.dll /TestCaseFilter:FullyQualifiedName~SharedUnitsW1Tests /Logger:trx`. Checker: `python -I tools/check_shared_units.py --strict`.
