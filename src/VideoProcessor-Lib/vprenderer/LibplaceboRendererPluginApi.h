#pragma once

#include <IRenderer.h>
#include <AnalysisDiagnosticTrace.h>
#include <ITimingClock.h>
#include <VideoConversionOverride.h>

#include <cstddef>
#include <cstdint>


// Increment whenever the factory signature or any shared interface vtable
// or shared snapshot layout changes. The executable and optional renderer DLL are deployed separately,
// so this is the fail-closed ABI boundary for a mismatched pair.
// 25: GetDejudderStatus (stage 6b-2b, FL35) and, separately, Bill's rc4 Beta
// flavor export; 26: both together (rc4 merge); 27: Suspend/ResumeLedDrive
// for the RS-232 connection test (FL35); 28 (Bill, v1.4.02): the analysis
// diagnostic sink, Dolby luminance, chroma/motion profiles; 29: 27 and 28
// together (v1.4.02 merge, 2026-10-15); 30: GetLedDriveStatus (crash state);
// 31: GetSourceResolutionStatus (Stage 12, crash state); 32: the DV L1
// scene minimum in DolbyVisionLuminanceStatus (2026-10-17);
// 33: StopLedDriveForShutdown (2026-10-17 integration);
// 34: GetContentIntentStatus and the DV L5/L6/L11 fields in
// DolbyVisionTunnelFrameMetadata (content intent, 2026-10-17; the tunnel
// metadata itself stays inside the DLL, GetContentIntentStatus needed it);
// 35: the required build-ID export below (hunt 2, 2026-10-17). Raised so an
// older executable, which does not check build IDs, refuses this DLL too.
// 36: IVideoRenderer::WriteDiagnosticHistory (report-a-problem, 2026-10-17).
// 37: VP Renderer DirectShow filter (MPC-HC/BE; claimed 35 on its branch,
// renumbered at the 2026-10-17 integ/mpc-state merge) - NV12/P010/P016
// encodings, FileFrameTiming and the file-frame IVideoRenderer methods
// (SupportsTimedFileFrames, OnTimedVideoFrame, WaitForFileFrameSlot,
// CancelFileFrameWait), after WriteDiagnosticHistory in the vtable.
// 38: GetIntentStatus / SetAmbientLux (intent interpreter items 2-5,
// 2026-10-17; claimed 36 on its branch), after the file-frame methods in the
// vtable. The check is equality, so a gap in the numbering is harmless.
// 39: GetCalibrationOutput / SetCalibrationPattern (automatic calibration,
// 2026-10-17; claimed 36 on its branch), after GetIntentStatus /
// SetAmbientLux in the vtable. Calibration phase 2 grew PatternOutputInfo
// (VP's tone curve and the HDR metadata sent) within 39, before any 39 build
// shipped (integration round 4).
// 40: SetActivePictureLookaheadAutomatic (bug hunt 3 F2, integration round 5),
// after GetCalibrationOutput / SetCalibrationPattern in the vtable.
// 41: GetFallbacksStatus ("Fallbacks engaged", r6 fix 7, 2026-10-24), after
// SetActivePictureLookaheadAutomatic in the vtable.
// 42: SetQueueDelayBudget (VP-V delay budget, ADR-0020 / barrier B2, 2026-10-24), after
// GetFallbacksStatus in the vtable.
//
// The counter is maintained by hand and can collide across branches or miss
// a layout change. The build ID is the
// mechanical check: the loader refuses any DLL whose
// VP_LIBPLACEBO_BUILD_ID_EXPORT differs from the executable's own build ID
// (VpBuildIdentity.h), so only a DLL from the same build loads.
static constexpr uint32_t VP_LIBPLACEBO_PLUGIN_API_VERSION = 42;
static constexpr const char* VP_LIBPLACEBO_BUILD_ID_EXPORT =
	"VideoProcessorLibplaceboGetBuildId";
// Returns the DLL's 32-hex-digit build ID (static storage); never throws.
using VideoProcessorLibplaceboGetBuildIdFn = const char* (__cdecl *)();
static constexpr const char* VP_LIBPLACEBO_FLAVOR_EXPORT = "VideoProcessorLibplaceboIsBeta";
static constexpr const char* VP_LIBPLACEBO_VERSION_EXPORT =
	"VideoProcessorLibplaceboGetApiVersion";
static constexpr const char* VP_LIBPLACEBO_CREATE_EXPORT =
	"VideoProcessorLibplaceboCreateRenderer";
static constexpr const char* VP_LIBPLACEBO_DESTROY_EXPORT =
	"VideoProcessorLibplaceboDestroyRenderer";

using VideoProcessorLibplaceboLogSink = void (__cdecl *)(const char* message);
using VideoProcessorLibplaceboGetApiVersionFn = uint32_t (__cdecl *)();
using VideoProcessorLibplaceboCreateRendererFn = IVideoRenderer* (__cdecl *)(
	IRendererCallback* callback,
	uint32_t rendererGeneration,
	HWND videoHwnd,
	ITimingClock* timingClock,
	bool useFrameQueue,
	size_t frameQueueMaxSize,
	VideoConversionOverride videoConversionOverride,
	const char* rendererConfigPath,
	VideoProcessorLibplaceboLogSink logSink);
using VideoProcessorLibplaceboDestroyRendererFn = void (__cdecl *)(
	IVideoRenderer* renderer);

// Optional export (VP Renderer filter, 2026-10-17), outside the versioned
// contract: resolve it with GetProcAddress and tolerate its absence. Ends an
// LED drive the filter process is holding between graphs (file_led_holdover_ms,
// led/LedDriveHoldover.h) through its normal stop, which restores full light.
// Returns 1 when one was held. The filter calls it when it is going away for
// good (never from DllMain).
static constexpr const char* VP_LIBPLACEBO_END_HELD_LED_DRIVE_EXPORT =
	"VideoProcessorLibplaceboEndHeldLedDrive";
using VideoProcessorLibplaceboEndHeldLedDriveFn = int (__cdecl *)();

static constexpr const char* VP_LIBPLACEBO_DIAGNOSTIC_EXPORT = "VideoProcessorLibplaceboConfigureAnalysisDiagnosticSink";
using VideoProcessorLibplaceboConfigureDiagnosticFn = void (__cdecl *)(void*,
    AnalysisDiagnosticTrace::RecordSink, AnalysisDiagnosticTrace::FrameSink,
    AnalysisDiagnosticTrace::FrameCaptureActiveCallback);
