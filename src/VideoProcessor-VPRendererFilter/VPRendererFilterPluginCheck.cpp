#include "VPRendererFilterPluginCheck.h"

#include <vprenderer/OptionalRendererLayout.h>

#include <cstdio>


namespace VPRendererFilter
{
	std::vector<std::wstring> PluginCandidatePaths(const std::wstring& moduleDirectory,
		bool beta)
	{
		std::vector<std::wstring> paths;
		if (moduleDirectory.empty())
			return paths;
		paths.push_back(OptionalRendererLayout::PluginPath(moduleDirectory, beta));
		paths.push_back(OptionalRendererLayout::Join(moduleDirectory,
			OptionalRendererLayout::PluginFileName(beta)));
		return paths;
	}


	PluginCheck CheckPluginExports(const PluginExportProbe& probe,
		uint32_t expectedApiVersion, bool wantBeta, std::string& reason)
	{
		char text[192];
		if (!probe.getApiVersion || !probe.hasCreate || !probe.hasDestroy)
		{
			snprintf(text, sizeof(text),
				"VP renderer plugin is missing required exports (version=%d create=%d destroy=%d)",
				probe.getApiVersion ? 1 : 0, probe.hasCreate ? 1 : 0, probe.hasDestroy ? 1 : 0);
			reason = text;
			return PluginCheck::MissingExports;
		}

		const uint32_t apiVersion = probe.getApiVersion();
		if (apiVersion != expectedApiVersion)
		{
			snprintf(text, sizeof(text),
				"VP renderer plugin API mismatch: filter expects %u, plugin reports %u "
				"(deploy VideoProcessorVPRenderer.dll and VPRendererFilter.ax from one build)",
				expectedApiVersion, apiVersion);
			reason = text;
			return PluginCheck::ApiMismatch;
		}

		if (!probe.isBeta)
		{
			reason = "VP renderer plugin has no flavor export";
			return PluginCheck::FlavorMissing;
		}
		const bool isBeta = probe.isBeta();
		if (isBeta != wantBeta)
		{
			snprintf(text, sizeof(text),
				"VP renderer plugin flavor mismatch: wanted %s, plugin is %s",
				wantBeta ? "beta" : "release", isBeta ? "beta" : "release");
			reason = text;
			return PluginCheck::FlavorMismatch;
		}

		snprintf(text, sizeof(text), "VP renderer plugin accepted: API %u, %s",
			apiVersion, isBeta ? "beta" : "release");
		reason = text;
		return PluginCheck::Ok;
	}


	const char* ToString(PluginCheck check)
	{
		switch (check)
		{
		case PluginCheck::Ok: return "ok";
		case PluginCheck::MissingExports: return "missing-exports";
		case PluginCheck::ApiMismatch: return "api-mismatch";
		case PluginCheck::FlavorMissing: return "flavor-missing";
		case PluginCheck::FlavorMismatch: return "flavor-mismatch";
		}
		return "unknown";
	}
}
