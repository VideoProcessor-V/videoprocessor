# M2 step 0 forwarders

Commit A of M2 step 0 (generator: a one-off include sweep; re-runnable idea in WP-18 plan). Our includers were switched to the module path; a forwarder was deleted only when no file present at `origin/v1.4.02-beta` includes it. Commit B (Bill-file include patch) deletes the rest.

| Forwarder | New path | Bill includer files | Bill include lines | Our includes switched | Deleted |
|---|---|---|---|---|---|
| `src/VideoProcessor-Lib/DiagnosticHistory.h` | `src/VP-V/modules/control/diagnostics/DiagnosticHistory.h` | 3 | 3 | 0 | no |
| `src/VideoProcessor-Lib/DolbyVisionTunnelGate.h` | `src/VP-V/modules/input/signal/DolbyVisionTunnelGate.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/DolbyVisionTunnelHandoff.h` | `src/VP-V/modules/input/signal/DolbyVisionTunnelHandoff.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/HdrInference.h` | `src/VP-V/modules/input/signal/HdrInference.h` | 3 | 3 | 1 | no |
| `src/VideoProcessor-Lib/HdrInferenceSignalBuilder.h` | `src/VP-V/modules/input/signal/HdrInferenceSignalBuilder.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/HdrVetoCaptureGate.h` | `src/VP-V/modules/input/signal/HdrVetoCaptureGate.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/KernelInverse.h` | `src/VP-V/modules/scaling/rebase/KernelInverse.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/MotionAhead.h` | `src/VP-V/modules/motion/estimation/MotionAhead.h` | 4 | 4 | 0 | no |
| `src/VideoProcessor-Lib/MotionValidity.h` | `src/VP-V/modules/motion/estimation/MotionValidity.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/QueueLatency.h` | `src/VP-V/modules/input/queue_timing/QueueLatency.h` | 3 | 3 | 0 | no |
| `src/VideoProcessor-Lib/QueueLookahead.h` | `src/VP-V/modules/input/queue_timing/QueueLookahead.h` | 3 | 3 | 1 | no |
| `src/VideoProcessor-Lib/RawCaptureCorpus.h` | `src/VP-V/modules/control/diagnostics/RawCaptureCorpus.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/RawCaptureRecorder.h` | `src/VP-V/modules/control/diagnostics/RawCaptureRecorder.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/StoreZipWriter.h` | `src/VP-V/modules/control/diagnostics/StoreZipWriter.h` | 0 | 0 | 0 | yes |
| `src/VideoProcessor-Lib/calibration/CalibrationConfig.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationConfig.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/calibration/CalibrationController.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationController.h` | 0 | 0 | 1 | yes |
| `src/VideoProcessor-Lib/calibration/CalibrationPattern.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationPattern.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/calibration/CalibrationSimulation.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationSimulation.h` | 0 | 0 | 1 | yes |
| `src/VideoProcessor-Lib/calibration/PatternCodes.h` | `src/VP-V/modules/output/calibration_lut/calibration/PatternCodes.h` | 0 | 0 | 1 | yes |
| `src/VideoProcessor-Lib/led/ConnectionTest.h` | `src/VP-V/modules/drive/projector_control/ConnectionTest.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/led/LedDriveArbiter.h` | `src/VP-V/modules/drive/projector_control/LedDriveArbiter.h` | 0 | 0 | 0 | yes |
| `src/VideoProcessor-Lib/led/LedDriveConfig.h` | `src/VP-V/modules/drive/led_compensation/LedDriveConfig.h` | 4 | 5 | 2 | no |
| `src/VideoProcessor-Lib/led/LedDriveHoldover.h` | `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.h` | 0 | 0 | 0 | yes |
| `src/VideoProcessor-Lib/led/LedDriveModule.h` | `src/VP-V/modules/drive/led_compensation/LedDriveModule.h` | 0 | 0 | 0 | yes |
| `src/VideoProcessor-Lib/led/LedMeasurementHook.h` | `src/VP-V/modules/drive/led_measure/LedMeasurementHook.h` | 0 | 0 | 4 | yes |
| `src/VideoProcessor-Lib/led/LedRawTransfer.h` | `src/VP-V/modules/drive/led_compensation/LedRawTransfer.h` | 0 | 0 | 0 | yes |
| `src/VideoProcessor-Lib/led/SerialPort.h` | `src/VP-V/modules/drive/projector_control/SerialPort.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/AdaptiveDownscale.h` | `src/VP-V/modules/scaling/output/AdaptiveDownscale.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/CustomHookChain.h` | `src/VP-V/modules/output/dither/CustomHookChain.h` | 2 | 2 | 5 | no |
| `src/VideoProcessor-Lib/vprenderer/DejudderAhead.h` | `src/VP-V/modules/motion/consumers/DejudderAhead.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DejudderPolicy.h` | `src/VP-V/modules/motion/vector_delivery/DejudderPolicy.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DejudderWarp.h` | `src/VP-V/modules/motion/vector_delivery/DejudderWarp.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DetailPulse.h` | `src/VP-V/modules/repair/texture_restore/DetailPulse.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DolbyVisionTrims.h` | `src/VP-V/modules/colour/sdr_tone_shape/DolbyVisionTrims.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DolbyVisionTunnelUnpackReference.h` | `src/VP-V/modules/input/signal/DolbyVisionTunnelUnpackReference.h` | 4 | 4 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/DownscaleLight.h` | `src/VP-V/modules/scaling/output/DownscaleLight.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/EarlyUpload.h` | `src/VP-V/modules/input/capture/EarlyUpload.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/ExtendedRange.h` | `src/VP-V/modules/colour/extended_range/ExtendedRange.h` | 3 | 3 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/FrameInterpolator.h` | `src/VP-V/modules/motion/consumers/FrameInterpolator.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/GpuCaptureUnpack.h` | `src/VP-V/modules/input/capture/GpuCaptureUnpack.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/GpuDolbyVisionTunnelUnpack.h` | `src/VP-V/modules/input/signal/GpuDolbyVisionTunnelUnpack.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/GpuRgbUnpack.h` | `src/VP-V/modules/input/capture/GpuRgbUnpack.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/InterpolationPolicy.h` | `src/VP-V/modules/motion/consumers/InterpolationPolicy.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/InterpolationTimeline.h` | `src/VP-V/modules/motion/consumers/InterpolationTimeline.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/MotionAheadProducer.h` | `src/VP-V/modules/motion/estimation/MotionAheadProducer.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/MotionEngineIdle.h` | `src/VP-V/modules/motion/estimation/MotionEngineIdle.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/OcclusionMaskHook.h` | `src/VP-V/modules/motion/consumers/OcclusionMaskHook.h` | 2 | 2 | 1 | no |
| `src/VideoProcessor-Lib/vprenderer/OpenLoopDisplayRate.h` | `src/VP-V/modules/output/presentation/OpenLoopDisplayRate.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/OutputEncodingRepair.h` | `src/VP-V/modules/output/dither/OutputEncodingRepair.h` | 2 | 2 | 4 | no |
| `src/VideoProcessor-Lib/vprenderer/PulldownDetector.h` | `src/VP-V/modules/input/queue_timing/PulldownDetector.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/RenderAheadPolicy.h` | `src/VP-V/modules/output/presentation/RenderAheadPolicy.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/RenderedWarp.h` | `src/VP-V/modules/motion/consumers/RenderedWarp.h` | 2 | 2 | 2 | no |
| `src/VideoProcessor-Lib/vprenderer/RendererDiagnosticCodes.h` | `src/VP-V/modules/control/diagnostics/RendererDiagnosticCodes.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/SdrToneShape.h` | `src/VP-V/modules/colour/sdr_tone_shape/SdrToneShape.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/SigmaDeltaDomain.h` | `src/VP-V/modules/output/dither/SigmaDeltaDomain.h` | 2 | 2 | 2 | no |
| `src/VideoProcessor-Lib/vprenderer/SourceRebase.h` | `src/VP-V/modules/scaling/rebase/SourceRebase.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/SourceResolutionMonitor.h` | `src/VP-V/modules/input/source_analysis/SourceResolutionMonitor.h` | 2 | 2 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/TextureRestore.h` | `src/VP-V/modules/repair/texture_restore/TextureRestore.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/UploadReuse.h` | `src/VP-V/modules/input/capture/UploadReuse.h` | 1 | 1 | 0 | no |
| `src/VideoProcessor-Lib/vprenderer/VpDebander.h` | `src/VP-V/modules/repair/deband/VpDebander.h` | 2 | 2 | 0 | no |

Forwarders: 60; deleted in commit A: 9; kept for commit B: 51 (Bill include lines: 92).

## Removed forwarders

Deleted in commit A (no Bill file included them); old paths in dated records resolve to:

| Old path | Target |
|---|---|
| `src/VideoProcessor-Lib/StoreZipWriter.h` | `src/VP-V/modules/control/diagnostics/StoreZipWriter.h` |
| `src/VideoProcessor-Lib/calibration/CalibrationController.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationController.h` |
| `src/VideoProcessor-Lib/calibration/CalibrationSimulation.h` | `src/VP-V/modules/output/calibration_lut/calibration/CalibrationSimulation.h` |
| `src/VideoProcessor-Lib/calibration/PatternCodes.h` | `src/VP-V/modules/output/calibration_lut/calibration/PatternCodes.h` |
| `src/VideoProcessor-Lib/led/LedDriveArbiter.h` | `src/VP-V/modules/drive/projector_control/LedDriveArbiter.h` |
| `src/VideoProcessor-Lib/led/LedDriveHoldover.h` | `src/VP-V/modules/drive/led_compensation/LedDriveHoldover.h` |
| `src/VideoProcessor-Lib/led/LedDriveModule.h` | `src/VP-V/modules/drive/led_compensation/LedDriveModule.h` |
| `src/VideoProcessor-Lib/led/LedMeasurementHook.h` | `src/VP-V/modules/drive/led_measure/LedMeasurementHook.h` |
| `src/VideoProcessor-Lib/led/LedRawTransfer.h` | `src/VP-V/modules/drive/led_compensation/LedRawTransfer.h` |
