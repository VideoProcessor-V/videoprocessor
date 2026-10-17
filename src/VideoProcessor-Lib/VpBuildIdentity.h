#pragma once

#include <string>


// Hunt 2 (2026-10-17, MEDIUM-1): build identity of the EXE/renderer-DLL pair.
//
// VideoProcessor-Lib compiles VP_BUILD_ID (tools/generate_build_identity.ps1, a
// content hash of the sources and build inputs) into VpBuildIdentity.cpp. The
// same object is linked into VideoProcessor.exe and into each renderer DLL; the
// DLL exports its copy (VP_LIBPLACEBO_BUILD_ID_EXPORT) and the EXE refuses a DLL
// whose ID differs. Owner rule: a mismatched pair never loads. There is no
// override: rebuild the solution, or redeploy both files from one build.
namespace VpBuildIdentity
{
	// Literal prefix of the marker embedded in every binary that links Lib.
	// tools/runtime_packaging.ps1 scans built binaries for "VP-BUILD-ID:" plus
	// 32 lower-case hex digits; keep this prefix out of format strings.
	constexpr const char* kMarkerPrefix = "VP-BUILD-ID:";
	constexpr size_t kIdLength = 32;

	// This binary's build ID (32 lower-case hex digits, static storage).
	const char* Id();
	// Commit the ID was generated at, informational only ("unknown" without Git).
	const char* Commit();

	// True for exactly 32 lower-case hex digits.
	bool IsWellFormed(const char* id);

	// Pure pairing rule. Accepts only two well-formed, equal IDs. On refusal,
	// message names both IDs and the fix; on acceptance it is cleared.
	bool CheckPair(const char* hostId, const char* pluginId, std::string& message);
}
