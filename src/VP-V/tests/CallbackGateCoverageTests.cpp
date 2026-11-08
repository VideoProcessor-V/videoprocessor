// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// M2 P10-8 (fix C F2, audit slice 4 F1): every renderer entry point that a capture or file thread calls takes a
// CallbackGate Use before it touches anything else, and every m_impl reset / replacement is preceded by CloseAndDrain.
// CPU only: scans the source text (the renderer cannot be built in-process here).

#include "SettingsTestSupport.h"

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		const char kUse[] = "const Vpv::Ingress::CallbackGate::Use ingressUse(m_ingressGate); if (!ingressUse) return";

		// True when `signature` opens a body whose first few lines take the Use.
		bool TakesUseFirst(const std::string& text, const std::string& signature, size_t window)
		{
			const size_t at = text.find(signature);
			if (at == std::string::npos)
				return false;
			const size_t body = text.find('{', at);
			if (body == std::string::npos)
				return false;
			const size_t use = text.find(kUse, body);
			return use != std::string::npos && use - body < window;
		}
	}

	TEST_CLASS(CallbackGateCoverageTests)
	{
	public:
		TEST_METHOD(CaptureAndFileEntryPointsTakeTheGateFirst)
		{
			const std::string root = SettingsTestSupport::RepoRoot();
			const std::string lvr = SettingsTestSupport::ReadFile(root + "src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			const std::string file = SettingsTestSupport::ReadFile(root + "src\\VP-V\\modules\\input\\queue_timing\\TimedFileFrames.inl");
			Assert::IsFalse(lvr.empty() || file.empty(), L"sources found");
			// OnVideoFrame: after Bill's callback time (one line); OnVideoState: first; OnTimedVideoFrame: after the
			// slot gate re-arm and the callback time.
			Assert::IsTrue(TakesUseFirst(lvr, "void LibplaceboVideoRenderer::OnVideoFrame(VideoFrame& videoFrame)", 120), L"OnVideoFrame");
			Assert::IsTrue(TakesUseFirst(lvr, "bool LibplaceboVideoRenderer::OnVideoState(VideoStateComPtr& videoState)", 40), L"OnVideoState");
			Assert::IsTrue(TakesUseFirst(file, "void LibplaceboVideoRenderer::OnTimedVideoFrame(", 260), L"OnTimedVideoFrame");
		}

		TEST_METHOD(EveryImplResetIsDrainedFirst)
		{
			const std::string lvr = SettingsTestSupport::ReadFile(SettingsTestSupport::RepoRoot() +
				"src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			size_t resets = 0;
			for (size_t at = lvr.find("m_impl.reset("); at != std::string::npos; at = lvr.find("m_impl.reset(", at + 1))
			{
				++resets;
				const size_t drain = lvr.rfind("m_ingressGate.CloseAndDrain();", at);
				Assert::IsTrue(drain != std::string::npos && at - drain < 200, L"an m_impl reset without a drain just before it");
			}
			const size_t swap = lvr.find("m_impl.swap(");
			Assert::IsTrue(swap != std::string::npos && swap - lvr.rfind("m_ingressGate.CloseAndDrain();", swap) < 200, L"Build's swap");
			Assert::IsTrue(resets >= 3, L"plausibility: destructor, Retire, FinalizeRetirementForShutdown");
		}
	};
}
