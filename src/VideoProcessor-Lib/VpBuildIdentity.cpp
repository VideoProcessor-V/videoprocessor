#include <pch.h>
#include "VpBuildIdentity.h"

// Generated per build into Lib's intermediate directory (see Directory.Build.targets).
#include <VpBuildIdentity.generated.h>

#include <cstring>


namespace VpBuildIdentity
{
	namespace
	{
		// One contiguous literal so the packaging scan finds "VP-BUILD-ID:<id>".
		const char kMarker[] = "VP-BUILD-ID:" VP_BUILD_ID;
		static_assert(sizeof(kMarker) == 12 + kIdLength + 1,
			"VP_BUILD_ID must be 32 hex digits");
	}

	const char* Id()
	{
		return kMarker + std::strlen(kMarkerPrefix);
	}

	const char* Commit()
	{
		return VP_BUILD_ID_COMMIT;
	}

	bool IsWellFormed(const char* id)
	{
		if (!id)
			return false;
		size_t length = 0;
		for (; id[length] && length <= kIdLength; ++length)
		{
			const char c = id[length];
			if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
				return false;
		}
		return length == kIdLength;
	}

	bool CheckPair(const char* hostId, const char* pluginId, std::string& message)
	{
		const bool hostOk = IsWellFormed(hostId);
		const bool pluginOk = IsWellFormed(pluginId);
		if (hostOk && pluginOk && std::strcmp(hostId, pluginId) == 0)
		{
			message.clear();
			return true;
		}
		const std::string host = hostOk ? hostId : "invalid";
		const std::string plugin = !pluginId ? "missing (older build without a build ID)" :
			pluginOk ? pluginId : "invalid";
		message = "VP Renderer DLL refused: it is from a different build than VideoProcessor.exe "
			"(EXE build ID " + host + ", DLL build ID " + plugin + "). "
			"Redeploy VideoProcessor.exe and vprenderer\VideoProcessorVPRenderer.dll "
			"(and vprenderer-beta\VideoProcessorVPRendererBeta.dll) together from the same build.";
		return false;
	}
}
