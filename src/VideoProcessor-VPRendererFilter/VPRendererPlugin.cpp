#include <pch.h>

#include "VPRendererPlugin.h"
#include "VPRendererFilterPluginCheck.h"

#include <DebugLog.h>
#include <VpBuildIdentity.h>
#include <vprenderer/OptionalRendererLayout.h>


namespace VPRendererFilter
{
	HMODULE FilterModuleHandle()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&FilterModuleHandle), &module);
		return module;
	}

	std::wstring FilterModulePath()
	{
		wchar_t path[MAX_PATH * 2] = {};
		const DWORD length = GetModuleFileNameW(FilterModuleHandle(), path,
			static_cast<DWORD>(_countof(path)));
		if (length == 0 || length >= _countof(path))
			return std::wstring();
		return std::wstring(path, length);
	}

	std::wstring FilterModuleDirectory()
	{
		std::wstring path = FilterModulePath();
		const size_t separator = path.find_last_of(L"\\/");
		if (separator == std::wstring::npos)
			return std::wstring();
		path.resize(separator);
		return path;
	}


	namespace
	{
		LoadedPlugin LoadPlugin()
		{
			LoadedPlugin result;
			const std::wstring directory = FilterModuleDirectory();
			const std::vector<std::wstring> candidates =
				PluginCandidatePaths(directory, kUseBetaPlugin);
			if (candidates.empty())
			{
				result.failure = "VP Renderer filter: cannot resolve its own module directory";
				DebugLog::Log("%s", result.failure.c_str());
				return result;
			}

			for (size_t i = 0; i < candidates.size(); ++i)
			{
				const std::wstring& path = candidates[i];
				DebugLog::Log("VP Renderer filter plugin probe: path=%ls", path.c_str());
				if (!OptionalRendererLayout::IsFile(path))
					continue;
				// The install layout carries its private libplacebo runtime next
				// to the plugin; report a missing one instead of a loader error.
				std::wstring missing;
				if (i == 0 && OptionalRendererLayout::FindMissingRuntimeFile(
					directory, missing, kUseBetaPlugin))
				{
					DebugLog::Log("VP Renderer filter plugin incomplete: missing %ls", missing.c_str());
					result.failure = "VP renderer plugin runtime is incomplete";
					continue;
				}

				HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
				if (!module)
				{
					DebugLog::Log("VP Renderer filter plugin load failed: path=%ls error=%lu",
						path.c_str(), GetLastError());
					result.failure = "VP renderer plugin could not be loaded";
					continue;
				}

				const auto getApiVersion = reinterpret_cast<VideoProcessorLibplaceboGetApiVersionFn>(
					GetProcAddress(module, VP_LIBPLACEBO_VERSION_EXPORT));
				const auto getBuildId = reinterpret_cast<VideoProcessorLibplaceboGetBuildIdFn>(
					GetProcAddress(module, VP_LIBPLACEBO_BUILD_ID_EXPORT));
				const auto isBeta = reinterpret_cast<bool(__cdecl*)()>(
					GetProcAddress(module, VP_LIBPLACEBO_FLAVOR_EXPORT));
				const auto create = reinterpret_cast<VideoProcessorLibplaceboCreateRendererFn>(
					GetProcAddress(module, VP_LIBPLACEBO_CREATE_EXPORT));
				const auto destroy = reinterpret_cast<VideoProcessorLibplaceboDestroyRendererFn>(
					GetProcAddress(module, VP_LIBPLACEBO_DESTROY_EXPORT));

				PluginExportProbe probe;
				probe.hasCreate = create != nullptr;
				probe.hasDestroy = destroy != nullptr;
				if (getApiVersion)
					probe.getApiVersion = [getApiVersion]() { return getApiVersion(); };
				if (getBuildId)
					probe.getBuildId = [getBuildId]() { return getBuildId(); };
				if (isBeta)
					probe.isBeta = [isBeta]() { return isBeta(); };

				std::string reason;
				const PluginCheck check = CheckPluginExports(probe,
					VP_LIBPLACEBO_PLUGIN_API_VERSION, VpBuildIdentity::Id(), kUseBetaPlugin, reason);
				DebugLog::Log("VP Renderer filter plugin check: %s path=%ls (%s)",
					ToString(check), path.c_str(), reason.c_str());
				if (check != PluginCheck::Ok)
				{
					FreeLibrary(module);
					result.failure = reason;
					continue;
				}

				// The plugin keeps our log sink and callbacks; never let COM unload
				// the filter module while the plugin is loaded.
				HMODULE pinned = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_PIN,
					reinterpret_cast<LPCWSTR>(&FilterModuleHandle), &pinned);

				result.module = module;
				result.createRenderer = create;
				result.destroyRenderer = destroy;
				result.path = path;
				result.failure.clear();
				return result;
			}

			if (result.failure.empty())
				result.failure = "VideoProcessorVPRenderer.dll not found next to VPRendererFilter.ax "
					"or in its vprenderer folder";
			DebugLog::Log("VP Renderer filter plugin unavailable: %s", result.failure.c_str());
			return result;
		}
	}


	const LoadedPlugin& GetVPRendererPlugin()
	{
		// Loaded once and kept for the process lifetime, as in the executable:
		// renderer objects and callbacks can outlive any one filter instance.
		static const LoadedPlugin plugin = LoadPlugin();
		return plugin;
	}
}
