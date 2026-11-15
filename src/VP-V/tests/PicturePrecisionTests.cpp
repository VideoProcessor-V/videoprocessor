// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// S-12BIT (refs/119 #29 / #30 / #48 / #49 / #51): the intermediate-picture precision follows the setting alone and is
// visible; libplacebo's latched error flags are visible.

#include <VP-V/core/FallbackPlainText.h>
#include <VP-V/modules/control/diagnostics/LibplaceboErrors.h>
#include <VP-V/modules/scaling/output/PicturePrecision.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace PicturePrecisionTest
{
	TEST_CLASS(PicturePrecisionTests)
	{
		static std::string ReadText(const std::string& path)
		{
			std::ifstream in(path, std::ios::binary);
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}
		static std::string RepositoryRoot()
		{
			std::string dir = __FILE__;   // 4 levels up: tests, VP-V, src, root
			for (int i = 0; i < 4; ++i)
			{
				const size_t cut = dir.find_last_of("\\/");
				if (cut == std::string::npos) break;
				dir.resize(cut);
			}
			return dir;
		}

	public:
		TEST_METHOD(FloatIntermediatesFollowTheSettingAlone)
		{
			Assert::IsTrue(PicturePrecision::FboFloat32(-1), L"auto is float32");
			Assert::IsTrue(PicturePrecision::FboFloat32(1));
			Assert::IsFalse(PicturePrecision::FboFloat32(0));
		}

		TEST_METHOD(StableLaneIsLabelledBelowTwelveBit)
		{
			const FallbackCatalog::Decision d = PicturePrecision::Precision(false, false, -1);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("picture precision"), d.id);
			Assert::IsTrue(d.reason.find("Stable 7.360") != std::string::npos);
			Assert::IsTrue(d.reason.find("below 12-bit") != std::string::npos);
		}

		TEST_METHOD(BetaLaneIsQuietUnlessSetOff)
		{
			Assert::IsFalse(PicturePrecision::Precision(false, true, -1).engaged);
			Assert::IsFalse(PicturePrecision::Precision(false, true, 1).engaged);
			const FallbackCatalog::Decision off = PicturePrecision::Precision(false, true, 0);
			Assert::IsTrue(off.engaged);
			Assert::IsTrue(off.reason.find("fbo_float32 is off") != std::string::npos);
		}

		TEST_METHOD(OriginalPathwayIsNeverLabelled)
		{
			Assert::IsFalse(PicturePrecision::Precision(true, false, 0).engaged);
			Assert::IsFalse(PicturePrecision::Precision(true, true, 0).engaged);
		}

		TEST_METHOD(NoticeSentencesExist)
		{
			Assert::IsTrue(FallbackNoticeText("picture precision", std::string()).find("16-bit") != std::string::npos);
			Assert::IsTrue(FallbackNoticeText("libplacebo errors", std::string()).find("internal error") != std::string::npos);
		}

		TEST_METHOD(LatchedErrorsAreVisibleAndNamed)
		{
			Assert::IsFalse(LibplaceboErrors::Latched(0).engaged);
			const FallbackCatalog::Decision d = LibplaceboErrors::Latched(LibplaceboErrors::kFbo | LibplaceboErrors::kDebanding);
			Assert::IsTrue(d.engaged);
			Assert::AreEqual(std::string("libplacebo errors"), d.id);
			Assert::IsTrue(d.reason.find("FBO") != std::string::npos);
			Assert::IsTrue(d.reason.find("debanding") != std::string::npos);
			Assert::IsTrue(d.reason.find("sampling") == std::string::npos);
		}

		TEST_METHOD(OriginalPathwayReleasesTheEntries)
		{
			const int flags = LibplaceboErrors::kFbo | LibplaceboErrors::kSampling;
			Assert::IsTrue(LibplaceboErrors::Latched(flags, false).engaged);
			const FallbackCatalog::Decision off = LibplaceboErrors::Latched(flags, true);
			Assert::IsFalse(off.engaged, L"a stale entry must release in Original");
			Assert::AreEqual(std::string("libplacebo errors"), off.id);
			FallbackStatus::Registry registry;
			FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(flags, false));
			FallbackCatalog::Sync(registry, PicturePrecision::Precision(false, false, -1));
			Assert::IsTrue(registry.Engaged("libplacebo errors") && registry.Engaged("picture precision"));
			FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(flags, true));
			FallbackCatalog::Sync(registry, PicturePrecision::Precision(true, false, -1));
			Assert::IsFalse(registry.Engaged("libplacebo errors"));
			Assert::IsFalse(registry.Engaged("picture precision"));
		}

		// Source pin (audit SHOULD 2): the wiring in CoreSegments.inl. fbo_float32 comes from the setting alone (no scaling.output
		// slot), and both entries are synced in CollectFrameHooks (every pathway), never inside the pure-move ReportRenderErrors.
		TEST_METHOD(TheCollectFrameHooksWiringIsPinned)
		{
			const std::string body = ReadText(RepositoryRoot() + "\\src\\VP-V\\segments\\CoreSegments.inl");
			Assert::IsFalse(body.empty(), L"CoreSegments.inl");
			const size_t collect = body.find("LibplaceboVideoRenderer::Impl::CollectFrameHooks(");
			Assert::IsTrue(collect != std::string::npos);
			const std::string tail = body.substr(collect);
			const size_t assign = tail.find("frameParams.fbo_float32 = PicturePrecision::FboFloat32(activeSettings.fboFloat32);");
			Assert::IsTrue(assign != std::string::npos, L"fbo_float32 from the setting alone");
			const size_t slot = tail.find("ScalingOutput", assign);
			Assert::IsTrue(slot == std::string::npos || slot - assign > 200, L"no scaling.output slot in the assignment");
			const size_t errors = tail.find("SyncFallback(LibplaceboErrors::Latched(static_cast<int>(renderErrors.errors), pipelineSlots.OriginalPathway()));");
			const size_t precision = tail.find("SyncFallback(PicturePrecision::Precision(pipelineSlots.OriginalPathway(), laneHasFloat32Fbo, activeSettings.fboFloat32));");
			Assert::IsTrue(errors != std::string::npos, L"libplacebo errors synced");
			Assert::IsTrue(precision != std::string::npos, L"picture precision synced");
			Assert::IsTrue(errors < assign && precision < assign, L"both syncs precede the render parameters");
			const size_t read = tail.find("renderErrors = pl_renderer_get_errors(renderer);");
			Assert::IsTrue(read != std::string::npos && read < errors, L"renderErrors is read before it is used");
			// ReportRenderErrors stays a pure move: no pathway check, no precision sync.
			const size_t report = body.find("LibplaceboVideoRenderer::Impl::ReportRenderErrors()");
			Assert::IsTrue(report != std::string::npos && report < collect);
			const std::string reportBody = body.substr(report, collect - report);
			Assert::IsTrue(reportBody.find("PicturePrecision") == std::string::npos && reportBody.find("LibplaceboErrors") == std::string::npos);
		}

		// Audit SHOULD 5: the entry follows the CURRENT flag word every frame, so a transient flag cannot stick: once the renderer
		// resets the latch (flags 0) the next sync releases it, with exactly one heading change each way.
		TEST_METHOD(ALatchedFlagClearsOnResetAndCannotStick)
		{
			FallbackStatus::Registry registry;
			Assert::IsFalse(FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(0, false)), L"clean: nothing changes");
			Assert::IsTrue(FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(LibplaceboErrors::kDebanding, false)));
			Assert::IsFalse(FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(LibplaceboErrors::kDebanding, false)), L"latched: no repeat");
			Assert::IsTrue(registry.Engaged("libplacebo errors"));
			Assert::IsTrue(FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(0, false)), L"reset: released");
			Assert::IsFalse(registry.Engaged("libplacebo errors"));
			Assert::IsFalse(FallbackCatalog::Sync(registry, LibplaceboErrors::Latched(0, false)), L"and stays released");
			Assert::IsTrue(LibplaceboErrors::Latched(LibplaceboErrors::kBlur, false).reason.find("clears on renderer reset") != std::string::npos);
		}

		TEST_METHOD(DisabledHooksAreNotDoubleReported)
		{
			Assert::IsFalse(LibplaceboErrors::Latched(LibplaceboErrors::kHooks).engaged, L"hooks have their own entry");
			Assert::IsTrue(LibplaceboErrors::Latched(LibplaceboErrors::kHooks | LibplaceboErrors::kBlur).engaged);
		}

		TEST_METHOD(EveryOtherBitHasAName)
		{
			for (int bit = 1; bit <= (1 << 12); bit <<= 1)
			{
				if (bit == LibplaceboErrors::kHooks) continue;
				Assert::IsFalse(LibplaceboErrors::Names(bit).empty(), L"a latched flag with no name");
			}
		}
	};
}
