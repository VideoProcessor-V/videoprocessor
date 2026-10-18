#include <pch.h>

#include <ConfigFile.h>
// Hunt 2 LOW-2: generated per project into $(IntDir); both renderer projects used
// to rewrite one shared src/VideoProcessor-VPRenderer/version.h concurrently.
#include "vp_renderer_version.h"
#include <DebugLog.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include <vprenderer/LibplaceboVideoRenderer.h>
#include <VpBuildIdentity.h>
#include <led/LedDriveHoldover.h>

#include <type_traits>

// INFO-1 (hunt 2): VideoProcessorLibplaceboDestroyRenderer cannot guard a throwing
// destructor (destructors are noexcept; a throw terminates inside the DLL whatever
// the export does). Keep the renderer and its members non-throwing on destruction.
static_assert(std::is_nothrow_destructible_v<LibplaceboVideoRenderer>,
	"LibplaceboVideoRenderer must not throw from its destructor");


extern "C" __declspec(dllexport) uint32_t __cdecl
VideoProcessorLibplaceboGetApiVersion()
{
	return VP_LIBPLACEBO_PLUGIN_API_VERSION;
}


// Hunt 2 (2026-10-17): this DLL's build ID, compiled by VideoProcessor-Lib from the
// same generated header as the executable's. The loader refuses any mismatch.
extern "C" __declspec(dllexport) const char* __cdecl
VideoProcessorLibplaceboGetBuildId()
{
	return VpBuildIdentity::Id();
}


// Tests only (planner Q4): the effective-settings fingerprint of a configuration file;
// returns its length (0 = not loaded), copying at most size - 1 characters.
extern "C" __declspec(dllexport) int __cdecl
VideoProcessorLibplaceboSettingsFingerprintForTest(const char* configPath, char* out, int size)
{
	// Bug hunt 2026-10-17: no C++ exception may cross the DLL's C ABI.
	try
	{
		const std::string fingerprint = LibplaceboRendererSettingsFingerprintForTest(configPath);
		if (out && size > 0)
		{
			const size_t n = (std::min)(fingerprint.size(), static_cast<size_t>(size - 1));
			memcpy(out, fingerprint.data(), n);
			out[n] = 0;
		}
		return static_cast<int>(fingerprint.size());
	}
	catch (...)
	{
		if (out && size > 0)
			out[0] = 0;
		return 0;
	}
}


extern "C" __declspec(dllexport) IVideoRenderer* __cdecl
VideoProcessorLibplaceboCreateRenderer(
	IRendererCallback* callback,
	uint32_t rendererGeneration,
	HWND videoHwnd,
	ITimingClock* timingClock,
	bool useFrameQueue,
	size_t frameQueueMaxSize,
	VideoConversionOverride videoConversionOverride,
	const char* rendererConfigPath,
	VideoProcessorLibplaceboLogSink logSink)
{
	// Bug hunt 2026-10-17: the set-up below (logging, configuration path) is
	// inside the try as well, so no C++ exception can cross the C ABI.
	try
	{
		DebugLog::SetExternalSink(logSink);
		DebugLog::Log("VP build identity: module=renderer commit=%ls branch=%ls dirty=%d build=%ls build_id=%s",
			VERSION_URL, VERSION_BRANCH, VERSION_DIRTY ? 1 : 0, VERSION_DESCRIBE, VpBuildIdentity::Id());
		ConfigFile::SetRendererConfigurationPath(
			rendererConfigPath ? rendererConfigPath : "");
		if (!callback)
		{
			DebugLog::Log("libplacebo plugin factory rejected a null renderer callback");
			return nullptr;
		}

		return new LibplaceboVideoRenderer(
			*callback,
			rendererGeneration,
			videoHwnd,
			timingClock,
			useFrameQueue,
			frameQueueMaxSize,
			videoConversionOverride);
	}
	catch (const std::exception& e)
	{
		DebugLog::Log("libplacebo plugin factory failed: %s", e.what());
	}
	catch (...)
	{
		DebugLog::Log("libplacebo plugin factory failed: unknown exception");
	}
	return nullptr;
}


extern "C" __declspec(dllexport) void __cdecl
VideoProcessorLibplaceboDestroyRenderer(IVideoRenderer* renderer)
{
	delete renderer;
}

// VP Renderer filter (2026-10-17): see VP_LIBPLACEBO_END_HELD_LED_DRIVE_EXPORT.
extern "C" __declspec(dllexport) int __cdecl
VideoProcessorLibplaceboEndHeldLedDrive()
{
	try
	{
		return LedDrive::EndHeldLedDrive("the VP Renderer filter is closing") ? 1 : 0;
	}
	catch (...)
	{
		return 0;
	}
}

extern "C" __declspec(dllexport) bool __cdecl VideoProcessorLibplaceboIsBeta()
{
#if defined(VP_LIBPLACEBO_BETA)
    return true;
#else
    return false;
#endif
}

extern "C" __declspec(dllexport) void __cdecl
VideoProcessorLibplaceboConfigureAnalysisDiagnosticSink(void* context,
    AnalysisDiagnosticTrace::RecordSink records, AnalysisDiagnosticTrace::FrameSink frames,
    AnalysisDiagnosticTrace::FrameCaptureActiveCallback active)
{
    // Bug hunt 2026-10-17: no C++ exception may cross the DLL's C ABI.
    try
    {
        AnalysisDiagnosticTrace::SetExternalSinks(context,records,frames,active);
    }
    catch (...)
    {
        DebugLog::Log("libplacebo plugin analysis diagnostic sink configuration failed");
    }
}
