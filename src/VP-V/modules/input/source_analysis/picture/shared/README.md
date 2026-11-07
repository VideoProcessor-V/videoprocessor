# PictureRect (shared unit U8, `VpUnit::PictureRect`)

The one active-picture rect rule (doc 62, section 1.2 U8): `PictureRect.h`, `kUnitVersion = 1`. No new detector: Bill's rect is the
authority when given, else the near-black rows/columns on the 1/8 luma, else the whole frame.

- `Compute` is the CPU reference of the matcher's PICTURE pass; `KernelHeader()` / `KernelBody()` are that pass's GLSL, moved
  byte-for-byte from `DejudderWarp.cpp` (the matcher dispatches them; `SharedUnitsW1Tests` compares them with a frozen copy).
- Readers: `Effective`, `InPicture`, `Coverage` (CPU) and `vpu_in_picture` / `vpu_picture_coverage` (`Glsl()`); `Halved` is the
  inner matcher's half-size rect.
- Migrated: the matcher (`DejudderWarp.cpp`). Registered bug fix, deferred: TextureRestore and the debander ignore the rect and gain
  `InPicture` / `Coverage` reads in their own packages (shader change, GPU run). TD / CfL rect handling moves with SA5.
- GPU proof: the matcher's existing GPU tests (`DejudderHookTests`) run the unit's kernel text.
