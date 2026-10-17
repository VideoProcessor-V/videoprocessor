#pragma once

// VP Renderer DirectShow filter: where VideoProcessorVPRenderer.dll is looked
// for and whether a loaded one may be used. Pure decisions (the loader in
// VPRendererPlugin.cpp supplies the exports) so the refusal logic is tested.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>


namespace VPRendererFilter
{
	// Candidate plugin paths for the directory holding VPRendererFilter.ax, in
	// probe order: the VP install layout (<dir>\vprenderer\<plugin>, as the
	// executable uses via OptionalRendererLayout) and then <dir>\<plugin>.
	std::vector<std::wstring> PluginCandidatePaths(const std::wstring& moduleDirectory,
		bool beta);

	enum class PluginCheck
	{
		Ok,
		MissingExports,   // version, create or destroy export absent
		ApiMismatch,      // VideoProcessorLibplaceboGetApiVersion() != expected
		FlavorMissing,    // VideoProcessorLibplaceboIsBeta export absent
		FlavorMismatch,   // beta plugin where the release one is wanted, or v.v.
	};

	struct PluginExportProbe
	{
		bool hasCreate = false;
		bool hasDestroy = false;
		// Null when the export is absent. Called at most once.
		std::function<uint32_t()> getApiVersion;
		// Null when the export is absent. Called only after the API version
		// matched: a plugin of another API version is not trusted with any
		// other call (the executable's order, bug hunt 2026-10-17).
		std::function<bool()> isBeta;
	};

	// The connection is refused unless this returns Ok; reason is the logged
	// text.
	PluginCheck CheckPluginExports(const PluginExportProbe& probe,
		uint32_t expectedApiVersion, bool wantBeta, std::string& reason);

	const char* ToString(PluginCheck check);
}
