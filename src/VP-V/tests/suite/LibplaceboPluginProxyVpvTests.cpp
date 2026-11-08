// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's LibplaceboPluginProxyTests.cpp by tools/m2/move_tests.py.
#include "pch.h"
#include "CppUnitTest.h"

#include "../VideoProcessor-Lib/vprenderer/LibplaceboPluginVideoRenderer.h"
#include "../VideoProcessor-Lib/AspectRatioOsd.h"
#include "../VideoProcessor-Lib/vprenderer/OptionalRendererLayout.h"

#include <type_traits>
#include <ConfigFile.h>
#include <UnifiedProfileRuntime.h>
#include "../VideoProcessor-Lib/vprenderer/LibplaceboRendererPluginApi.h"
#include <VP-V/abi/VpvAbi.h>
#include <VP-V/abi/VpvInterfaces.h>
#include "../VideoProcessor-Lib/VpBuildIdentity.h"
#include "../VideoProcessor-Lib/vprenderer/RendererDiagnosticCodes.h"
#include <DiagnosticHistory.h>
#include <memory>
#include <sstream>
#include <string>
#include <VP-V/tests/support/HardwarePrerequisites.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>

namespace
{
    std::mutex snapshotTestLogMutex;
    std::ofstream snapshotTestLog;
    std::atomic<unsigned> snapshotTestReads{0};
    std::atomic<unsigned> nativeP210Initializations{0};
    void __cdecl SnapshotTestLog(const char* message)
    {
        if (strstr(message, "native P210 (no CPU repack) upload")) ++nativeP210Initializations;
        if (strstr(message, "Configuration disk read:")) ++snapshotTestReads;
        std::lock_guard<std::mutex> guard(snapshotTestLogMutex);
        if (snapshotTestLog) snapshotTestLog << message << std::endl;
    }
}



using namespace Microsoft::VisualStudio::CppUnitTestFramework;


namespace VideoProcessorTest
{
	TEST_CLASS(LibplaceboPluginProxyTestsVpv)
	{
	public:

		// Hunt 2 (2026-10-17, MEDIUM-1): the pairing rule the loader applies.
		TEST_METHOD(BuildIdentityPairingRefusesAnyMismatch)
		{
			const char* a = "0123456789abcdef0123456789abcdef";
			const char* b = "0123456789abcdef0123456789abcdee";
			std::string message = "stale";
			Assert::IsTrue(VpBuildIdentity::CheckPair(a, a, message));
			Assert::IsTrue(message.empty());
			Assert::IsFalse(VpBuildIdentity::CheckPair(a, b, message));
			// The refusal names both IDs and the fix.
			Assert::IsTrue(message.find(a) != std::string::npos);
			Assert::IsTrue(message.find(b) != std::string::npos);
			Assert::IsTrue(message.find("same build") != std::string::npos);
			Assert::IsTrue(message.find("VideoProcessorVPRenderer.dll") != std::string::npos);
			// An older DLL without the export, and malformed IDs, never pair; two equal
			// malformed IDs (e.g. a placeholder) do not either.
			Assert::IsFalse(VpBuildIdentity::CheckPair(a, nullptr, message));
			Assert::IsTrue(message.find("missing") != std::string::npos);
			Assert::IsFalse(VpBuildIdentity::CheckPair(nullptr, a, message));
			Assert::IsFalse(VpBuildIdentity::CheckPair("", "", message));
			Assert::IsFalse(VpBuildIdentity::CheckPair("unknown", "unknown", message));
			const char* upper = "0123456789ABCDEF0123456789ABCDEF";
			Assert::IsFalse(VpBuildIdentity::CheckPair(upper, upper, message));
			const char* longer = "0123456789abcdef0123456789abcdef0";
			Assert::IsFalse(VpBuildIdentity::CheckPair(longer, longer, message));
			const char* shorter = "0123456789abcdef0123456789abcde";
			Assert::IsFalse(VpBuildIdentity::CheckPair(shorter, shorter, message));
		}

		TEST_METHOD(HostBuildIdIsWellFormedAndEmbeddedAsOneMarker)
		{
			Assert::IsTrue(VpBuildIdentity::IsWellFormed(VpBuildIdentity::Id()));
			// The packaging scan relies on "VP-BUILD-ID:" immediately preceding the ID.
			const char* id = VpBuildIdentity::Id();
			const size_t prefix = strlen(VpBuildIdentity::kMarkerPrefix);
			Assert::AreEqual(0, memcmp(id - prefix, VpBuildIdentity::kMarkerPrefix, prefix));
			Assert::IsNotNull(VpBuildIdentity::Commit());
		}

		// The built renderer DLL exports the same build ID as this binary (both link
		// the VideoProcessor-Lib of one build). Loading the DLL needs no GPU.
		TEST_METHOD(RendererDllExportsTheHostBuildId)
		{
			HMODULE testModule = nullptr;
			Assert::IsTrue(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&SnapshotTestLog), &testModule) != FALSE);
			wchar_t modulePath[MAX_PATH] = {};
			GetModuleFileNameW(testModule, modulePath, MAX_PATH);
			const std::wstring fullPath(modulePath);
			const std::wstring root = fullPath.substr(0, fullPath.find_last_of(L"\\/"));
			for (const bool beta : { false, true })
			{
				const std::wstring pluginPath = OptionalRendererLayout::PluginPath(root, beta);
				HMODULE plugin = LoadLibraryExW(pluginPath.c_str(), nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				Assert::IsNotNull(plugin, L"Build the Release renderer DLLs before this test");
				const auto getBuildId = reinterpret_cast<VideoProcessorLibplaceboGetBuildIdFn>(
					GetProcAddress(plugin, VP_LIBPLACEBO_BUILD_ID_EXPORT));
				const auto version = reinterpret_cast<VideoProcessorLibplaceboGetApiVersionFn>(
					GetProcAddress(plugin, VP_LIBPLACEBO_VERSION_EXPORT));
				const std::string pluginId = getBuildId && getBuildId() ? getBuildId() : "";
				const uint32_t apiVersion = version ? version() : 0;
				FreeLibrary(plugin);
				Assert::IsTrue(getBuildId != nullptr, L"renderer DLL lacks the build-ID export");
				Assert::AreEqual(VP_LIBPLACEBO_PLUGIN_API_VERSION, apiVersion);
				std::string message;
				Assert::IsTrue(VpBuildIdentity::CheckPair(VpBuildIdentity::Id(), pluginId.c_str(), message),
					std::wstring(message.begin(), message.end()).c_str());
			}
		}

		// M2 P2: the proxy no longer forwards the 22 VP-V methods one by one (they are no longer IVideoRenderer virtuals);
		// it forwards QueryExtension, so the VP-V interfaces of the plugin renderer reach the host (the old report-a-problem
		// override check, now: the history writer is an interface method and the proxy has the one forwarder).
		TEST_METHOD(ProxyForwardsQueryExtensionAndNothingElseOfVpv)
		{
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::QueryExtension),
				void* (LibplaceboPluginVideoRenderer::*)(const char*)>));
			Assert::IsTrue((std::is_same_v<
				decltype(&Vpv::IVpvStatus::WriteDiagnosticHistory),
				bool (Vpv::IVpvStatus::*)(VpDiagnosticHistorySink, void*, int64_t) const>));
		}

		// What LibplaceboVideoRenderer::WriteDiagnosticHistory sends across the plugin
		// boundary: JSONL rows tagged "renderer", in whole lines, through a C sink.
		TEST_METHOD(RendererDiagnosticHistoryReachesCSinkAsTaggedRows)
		{
			struct Collected
			{
				std::string text;
				size_t chunks = 0;
				bool wholeLines = true;
			};
			struct Sink
			{
				static void __cdecl Append(void* context, const char* data, size_t length)
				{
					auto* collected = static_cast<Collected*>(context);
					++collected->chunks;
					if (length == 0 || data[length - 1] != '\n')
						collected->wholeLines = false;
					collected->text.append(data, length);
				}
			};
			auto recorder = std::make_unique<DiagnosticHistory::Recorder>();
			const int64_t reference = DiagnosticHistory::NowQpc();
			// A full frame ring (more than one 64 KB chunk), a hold and a decision.
			for (uint64_t i = 0; i < DiagnosticHistory::kFrameCapacity; ++i)
			{
				DiagnosticHistory::FrameRecord frame;
				frame.stage = DiagnosticHistory::StagePresented;
				frame.counter = i;
				frame.renderMs = 1.5f;
				recorder->RecordFrame(frame);
			}
			DiagnosticHistory::FrameRecord hold;
			hold.stage = DiagnosticHistory::StageRepeated;
			hold.holdReason = RendererDiagnostics::HoldRenderAheadGate;
			recorder->RecordFrame(hold);
			recorder->RecordDecision("ra-gate", "late frame", 0, 3, 4.0, 5.0);
			recorder->RecordLed("LPW1", "sent", 412, 1, 412.0);

			Collected collected;
			Assert::IsFalse(DiagnosticHistory::WriteJsonlToSink(*recorder, "renderer", reference,
				nullptr, &collected));
			Assert::IsTrue(DiagnosticHistory::WriteJsonlToSink(*recorder, "renderer", reference,
				&Sink::Append, &collected));
			Assert::IsTrue(collected.chunks > 1, L"a full frame ring spans several chunks");
			Assert::IsTrue(collected.wholeLines, L"every chunk ends on a line boundary");
			std::istringstream lines(collected.text);
			std::string line;
			size_t rows = 0, frames = 0;
			bool decision = false, led = false, holdRow = false;
			while (std::getline(lines, line))
			{
				++rows;
				Assert::AreEqual(size_t(0), line.find("{\"src\":\"renderer\",\"ring\":"));
				Assert::IsTrue(line.back() == '}');
				if (line.find("\"ring\":\"frame\"") != std::string::npos) ++frames;
				if (line.find("\"ring\":\"decision\"") != std::string::npos &&
					line.find("\"name\":\"ra-gate\"") != std::string::npos) decision = true;
				if (line.find("\"ring\":\"led\"") != std::string::npos &&
					line.find("\"a\":412") != std::string::npos) led = true;
				if (line.find("\"stage\":\"repeated\"") != std::string::npos &&
					line.find("\"hold\":31") != std::string::npos) holdRow = true;
			}
			Assert::AreEqual(size_t(DiagnosticHistory::kFrameCapacity), frames);
			Assert::IsTrue(decision && led && holdRow);
			Assert::AreEqual(frames + 2 + 5, rows);   // + decision, LED, 5 ring_meta rows
		}

		// The hold codes written into frame rows (RendererDiagnosticCodes.h).
		TEST_METHOD(DejudderHoldTextsMapToStableCodes)
		{
			using namespace RendererDiagnostics;
			Assert::AreEqual(uint32_t(HoldNoMotionEstimate), ClassifyDejudderHold("no motion estimate for this frame"));
			Assert::AreEqual(uint32_t(HoldVectorsInvalid), ClassifyDejudderHold("frame gap"));
			Assert::AreEqual(uint32_t(HoldCadenceRepeat), ClassifyDejudderHold("cadence repeat"));
			Assert::AreEqual(uint32_t(HoldComposedWithoutPicture), ClassifyDejudderHold("composed source without the cached picture"));
			Assert::AreEqual(uint32_t(HoldComposedSubtitle), ClassifyDejudderHold("composed source (subtitle moving)"));
			Assert::AreEqual(uint32_t(HoldTimingUnknown), ClassifyDejudderHold("timing not known: display rate"));
			Assert::AreEqual(uint32_t(HoldNoSwapchain), ClassifyDejudderHold("no present id"));
			Assert::AreEqual(uint32_t(HoldRenderAheadGate), ClassifyDejudderHold("render-ahead gate: hold (extras dropped)"));
			Assert::AreEqual(uint32_t(HoldRenderAheadDropped), ClassifyDejudderHold("render-ahead: extras dropped (gate or late)"));
			Assert::AreEqual(uint32_t(HoldRenderAheadPhase), ClassifyDejudderHold("render-ahead: every refresh after the frame's own is a hold (phase)"));
			Assert::AreEqual(uint32_t(HoldExtraFailed), ClassifyDejudderHold("no swapchain frame"));
			Assert::AreEqual(uint32_t(HoldExtraFailed), ClassifyDejudderHold("rendered warp failed"));
			Assert::AreEqual(uint32_t(HoldPlannerStop), ClassifyDejudderHold("past the next frame"));
			Assert::AreEqual(uint32_t(HoldNextFrameLate), ClassifyDejudderHold("the frame after extras showed a refresh late"));
			Assert::AreEqual(uint32_t(HoldOther), ClassifyDejudderHold("something new"));
			Assert::AreEqual(uint32_t(HoldOther), ClassifyDejudderHold(nullptr));
		}

		TEST_METHOD(PostStallTelemetrySuppressionHasExplicitProxyOverride)
		{
			// Hunt 2 LOW-1: the proxy dropped this call (IVideoRenderer's no-op ran).
			using ExpectedMethod = void (LibplaceboPluginVideoRenderer::*)(uint64_t);
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::
					SetPostStallResetTelemetrySuppressedUntil),
				ExpectedMethod>));
		}

		// M2 P2: every former virtual is an interface method of the plugin's bridge with the same signature apart from
		// text (IVpvTextSink instead of CString&) and the iid map (src/VP-V/abi/README.md). The host reaches them only
		// through Vpv::Query (VpvHostCalls.h); a missing forwarder can no longer fall back to a default silently,
		// because there is nothing to forward but QueryExtension.
		TEST_METHOD(ExtensionInterfacesCarryTheFormerVirtualsSignatures)
		{
			// vpv.timed_file_frames.v1 (was ABI 37).
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvTimedFileFrames::Supports), bool (Vpv::IVpvTimedFileFrames::*)() const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvTimedFileFrames::OnTimedVideoFrame),
				void (Vpv::IVpvTimedFileFrames::*)(VideoFrame&, const FileFrameTiming&)>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvTimedFileFrames::WaitSlot), bool (Vpv::IVpvTimedFileFrames::*)(uint32_t)>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvTimedFileFrames::Cancel), void (Vpv::IVpvTimedFileFrames::*)()>));
			// vpv.intent.v1 (was ABI 38) and vpv.calibration.v1 (was ABI 39).
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvIntent::GetIntentStatus), bool (Vpv::IVpvIntent::*)(IntentStatus&) const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvIntent::GetContentIntentStatus), bool (Vpv::IVpvIntent::*)(ContentIntentStatus&) const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvIntent::SetAmbientLux), void (Vpv::IVpvIntent::*)(double, bool, double)>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvCalibration::GetOutput),
				bool (Vpv::IVpvCalibration::*)(Calibration::PatternOutputInfo&) const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvCalibration::SetPattern),
				bool (Vpv::IVpvCalibration::*)(const Calibration::PatternRequest&, Vpv::IVpvTextSink&)>));
			// vpv.presentation_ctl.v1 (was ABI 40, 42, 43).
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvPresentationCtl::SetActivePictureLookaheadAutomatic),
				void (Vpv::IVpvPresentationCtl::*)(bool)>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvPresentationCtl::SetQueueDelayBudget), void (Vpv::IVpvPresentationCtl::*)(int)>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvPresentationCtl::ToggleDvCompareCapture),
				bool (Vpv::IVpvPresentationCtl::*)(Vpv::IVpvTextSink&)>));
			// vpv.status.v1 (was ABI 25, 31, 41, 36) and vpv.modules.v1 (was ABI 44).
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvStatus::GetFallbacksStatus), bool (Vpv::IVpvStatus::*)(Vpv::IVpvTextSink&) const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvModules::Status), bool (Vpv::IVpvModules::*)(const char*, Vpv::IVpvTextSink&) const>));
			Assert::IsTrue((std::is_same_v<decltype(&Vpv::IVpvModules::Command),
				bool (Vpv::IVpvModules::*)(const char*, const char*, Vpv::IVpvTextSink&)>));
			// The ABI numbers: Bill's, and VP-V's own.
			Assert::AreEqual(static_cast<uint32_t>(34), VP_LIBPLACEBO_PLUGIN_API_VERSION);
			Assert::AreEqual(static_cast<uint32_t>(2), Vpv::kVpvExtensionAbi);
		}
	};
}
