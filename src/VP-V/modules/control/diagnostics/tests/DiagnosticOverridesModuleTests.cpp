// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Plan 58 T1 (B5): the diagnostic override line reaches the Fallbacks status through the hub.
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/control/diagnostics/DiagnosticOverridesModule.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DiagnosticOverridesModuleTest
{
	TEST_CLASS(DiagnosticOverridesModuleTests)
	{
	public:
		TEST_METHOD(FallbacksStatusCarriesTheOverrideRowOnly)
		{
			namespace RE = RendererExtension;
			Vpv::Diagnostics::DiagnosticOverridesModule shown([] { return std::vector<std::string>{ "VP_DISABLE_AVX2", "VP_TEST_ADAPTER" }; });
			Vpv::Diagnostics::DiagnosticOverridesModule none([] { return std::vector<std::string>(); });
			RE::Context ctx;
			std::vector<std::string> lines;
			shown.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::AreEqual<size_t>(1, lines.size());
			Assert::AreEqual(std::string("diagnostic override active: VP_DISABLE_AVX2, VP_TEST_ADAPTER"), lines[0]);
			lines.clear();
			shown.OnStatus(ctx, RE::StatusKind::Lines, lines, nullptr, nullptr);
			shown.OnStatus(ctx, RE::StatusKind::WorkerFault, lines, "x", "y");
			none.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::IsTrue(lines.empty(), L"no override, or another status kind: no row");
			// Inside this test host the default provider shows nothing (harness).
			Vpv::Diagnostics::DiagnosticOverridesModule real;
			real.OnStatus(ctx, RE::StatusKind::Fallbacks, lines, nullptr, nullptr);
			Assert::IsTrue(lines.empty());
		}
	};
}
