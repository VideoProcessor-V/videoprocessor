#include <pch.h>

#include <ConfigFile.h>
#include "version.h"
#include <DebugLog.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include <vprenderer/LibplaceboVideoRenderer.h>


extern "C" __declspec(dllexport) uint32_t __cdecl
VideoProcessorLibplaceboGetApiVersion()
{
	return VP_LIBPLACEBO_PLUGIN_API_VERSION;
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
		DebugLog::Log("VP build identity: module=renderer commit=%ls branch=%ls dirty=%d build=%ls",
			VERSION_URL, VERSION_BRANCH, VERSION_DIRTY ? 1 : 0, VERSION_DESCRIBE);
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
