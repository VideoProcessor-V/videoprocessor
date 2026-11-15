# Tests for `scaling.shared`

`ScalerPassTests.cpp` (compiled into the VideoProcessor-Test project; headers are included as `<VP-V/modules/scaling/shared/...>`):

- `ScalerModelTests` (CPU only): `ScalerKernelTableMatchesKernelInverseTaps`, `ScalerKernelTableOrderAndExtras`,
  `ScalerGeometryDerivesRatiosPerAxis`, `ScalerGeometryIntegerWindow`, `ScalerReferenceWeightsSumToOneAndFlatStaysFlat`,
  `ScalerReferenceUnityRatioIsIdentity`, `ScalerReferenceBilinearStaysWithinInputRange`, `ScalerReferenceSitingPhaseShiftsTheSpan`,
  `ScalerReferenceAxisOrderAgrees`, `ScalerReferenceDeterministic`.
- `ScalerPassGpuTests` (libplacebo on WARP; `VP_TEST_GPU=hardware` selects the hardware adapter): `ScalerGpuParityAtTheBriefsRatios`
  (GPU vs CPU reference within 1 code at 10-bit; ratios 1.48 / 2 / 2.22 / 3 / 3.75, unequal pairs, single axes, downscales, other
  kernels), `ScalerGpu12BitRampWithinOneLsbOfTheF32Reference`, `ScalerGpuMigrationIdentityRebaseChromaPasses`,
  `ScalerGpuMigrationIdentityFaithfulOrtho` (bit-exact: ScalerPass == the quoted faithful ortho call sequence, `a == b`),
  `ScalerGpuPolarFlatStaysFlatAndIsDeterministic`,
  `ScalerGpuHalfFloatIsAnnouncedAndLinearLightRefused`, `ScalerGpuSingleAxisKeepsItsWindow`, and `ScalerGpuTiming` (category
  `GpuHardware`, skipped unless on a hardware GPU; 1920x1080 to 3840x2160 luma, median over 40 frames, logged not asserted).

Status when written: code-only, none of these have been run (see the module README, "Open / not verified").

`ScalerPassAdaptiveIdentityTests.cpp` (class `ScalerPassAdaptiveIdentityTests`, WARP unless `VP_TEST_GPU=hardware`): `ScalerGpuMigrationIdentityFaithfulOrtho` (an ADDITIONAL check beside the bit-exact one in `ScalerPassGpuTests`) drives the real
`AdaptiveDownscale::Module` (faithful_ortho, w_base 0) through `pl_render_image` and compares with ScalerPass within one 8-bit code. Filter for the
focused run: `ScalerModelTests`, `ScalerPassGpuTests`, `ScalerPassAdaptiveIdentityTests`.
