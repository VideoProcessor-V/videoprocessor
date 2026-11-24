#pragma once

// Hardware preconditions for tests that need more than a CPU and a build: a desktop
// large enough for the requested window bounds, a performance-class CPU, or a
// hardware D3D11 adapter the libplacebo renderer can start on.
//
// Each check measures the condition itself (never "am I on CI"). When it is missing,
// RequireConditionOrSkip logs the fixed marker
//   SKIP-NO-HARDWARE <test>: <reason>
// (searchable in the TRX; the CI summary counts it separately) and the test returns.
// With VP_TEST_REQUIRE_HARDWARE=1 (the owner's local gates) the test fails instead,
// so a machine that should have the hardware never passes by skipping.

#include "CppUnitTest.h"

#include <string>

namespace HardwarePrerequisites
{
	enum class Kind { Hardware, Gpu, Meter, Projector, Footage, Corpus, Bench, ParkedNets };

	// The sustained 4K60 converter budgets are set for the machines VP runs on (the laptop
	// has 16 logical processors, the cinema PC more); below this count the CPU class cannot
	// be expected to hold them while the OS and test host share the cores.
	constexpr unsigned kPerformanceLogicalProcessors = 8;

	bool EnvIsOne(const char* name);
	bool RequireAll();
	bool RequireHardware();
	bool Required(Kind kind);
	const char* KindMarker(Kind kind);
	const char* KindEnvName(Kind kind);
	bool RequireOrSkip(Kind kind, const char* test, bool met, const std::string& reason);
	bool BenchOptedIn();
	bool RequireConditionOrSkip(const char* test, bool met, const std::string& reason);
	bool DesktopFitsWindow(int width, int height, std::string& reason);
	bool PerformanceCpu(std::string& reason);
	bool HardwareD3D11Renderer(std::string& reason);
}

// A native test that needs the libplacebo fork's patch 0004 (pl_mpv_user_shader_set_texture, defined only by the Beta's
// custom.h as PL_VP_MPV_USER_SHADER_SET_TEXTURE) cannot run on the native lanes. Its first body statement is
// VP_FORK_ONLY_PL0004_OR_RETURN(L"<exact native FQN>"): with the patch it is a no-op; without it the test logs
//   SKIP-FORK-ONLY-PL0004: ... Beta-host evidence: BETA-RAN <FQN>
// and returns. The Beta host runs the same case and prints BETA-RAN <FQN>; the TRX coverage tool credits the native test
// only when that id equals the FQN exactly. The FQN is passed explicitly (the tool needs the exact text).
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
#define VP_FORK_ONLY_PL0004_OR_RETURN(fqn) ((void)0)
#else
#define VP_FORK_ONLY_PL0004_OR_RETURN(fqn) do { ::Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(L"SKIP-FORK-ONLY-PL0004: needs libplacebo fork patch 0004 (pl_mpv_user_shader_set_texture); Beta-host evidence: BETA-RAN " fqn L"\n"); return; } while (0)
#endif
