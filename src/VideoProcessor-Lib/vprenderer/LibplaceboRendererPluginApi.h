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
// 36: GetCalibrationOutput / SetCalibrationPattern (automatic calibration,
// 2026-10-17). Calibration phase 2 (feat/calibration-phase2) extends
// PatternOutputInfo (VP's tone curve and the HDR metadata sent) without
// raising the number on its branch: renumber at merge.
//
// The counter is maintained by hand and can collide across branches (the MPC
// branch also plans 35) or miss a layout change. The build ID is the
// mechanical check: the loader refuses any DLL whose
// VP_LIBPLACEBO_BUILD_ID_EXPORT differs from the executable's own build ID
// (VpBuildIdentity.h), so only a DLL from the same build loads.
static constexpr uint32_t VP_LIBPLACEBO_PLUGIN_API_VERSION = 36;
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

static constexpr const char* VP_LIBPLACEBO_DIAGNOSTIC_EXPORT = "VideoProcessorLibplaceboConfigureAnalysisDiagnosticSink";
using VideoProcessorLibplaceboConfigureDiagnosticFn = void (__cdecl *)(void*,
    AnalysisDiagnosticTrace::RecordSink, AnalysisDiagnosticTrace::FrameSink,
    AnalysisDiagnosticTrace::FrameCaptureActiveCallback);
