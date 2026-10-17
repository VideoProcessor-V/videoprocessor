#pragma once

// VP Renderer DirectShow filter log. VP's DebugLog writes beside the host
// executable (MPC's folder) by default, so the filter routes DebugLog (its own
// and, through the plugin's logSink, the renderer DLL's) into one file:
//   VP_RENDERER_FILTER_LOG (absolute path), when set, else
//   %LOCALAPPDATA%\VideoProcessor\logs\vprfilter.log
// TODO(VP Renderer integration, agent D): take the path from the filter
// settings once they exist; the environment variable stays as an override.

#include <string>


namespace VPRendererFilterLog
{
	// Idempotent; called on the first filter creation and by registration.
	void Initialize();
	// The logSink handed to VideoProcessorLibplaceboCreateRenderer.
	void __cdecl Sink(const char* message);
	std::wstring ActivePath();
}
