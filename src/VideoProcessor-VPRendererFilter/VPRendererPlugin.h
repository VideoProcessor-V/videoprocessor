#pragma once

// VP Renderer DirectShow filter: loads VideoProcessorVPRenderer.dll once per
// process (probe order in VPRendererFilterPluginCheck.h), checks it like the
// executable does (API version first, then flavor) and keeps it loaded.

#include <vprenderer/LibplaceboRendererPluginApi.h>

#include <string>


namespace VPRendererFilter
{
	struct LoadedPlugin
	{
		HMODULE module = nullptr;
		VideoProcessorLibplaceboCreateRendererFn createRenderer = nullptr;
		VideoProcessorLibplaceboDestroyRendererFn destroyRenderer = nullptr;
		std::wstring path;
		// Why the plugin is unusable (logged once); empty when loaded.
		std::string failure;

		bool IsLoaded() const { return module != nullptr; }
	};

	// TODO(VP Renderer): a filter setting may select the
	// beta flavor (vprenderer-beta\VideoProcessorVPRendererBeta.dll).
	constexpr bool kUseBetaPlugin = false;

	const LoadedPlugin& GetVPRendererPlugin();

	// Directory of the module containing the filter (VPRendererFilter.ax).
	std::wstring FilterModuleDirectory();
	std::wstring FilterModulePath();
	HMODULE FilterModuleHandle();
}
