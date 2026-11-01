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

		// Report-a-problem (API 36): the proxy forwards the renderer DLL's history
		// (without an override IVideoRenderer's default returns false: no rows).
		TEST_METHOD(WriteDiagnosticHistoryHasExplicitProxyOverride)
		{
			using ExpectedMethod = bool (LibplaceboPluginVideoRenderer::*)(
				VpDiagnosticHistorySink, void*, int64_t) const;
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::WriteDiagnosticHistory),
				ExpectedMethod>));
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

		TEST_METHOD(TimedFileFramesHaveExplicitProxyOverrides)
		{
			// ABI 37 (claimed 35 on the filter branch): every file-frame virtual is forwarded to the plugin (a
			// missing override would silently fall back to capture semantics).
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::SupportsTimedFileFrames),
				bool (LibplaceboPluginVideoRenderer::*)() const>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::OnTimedVideoFrame),
				void (LibplaceboPluginVideoRenderer::*)(VideoFrame&, const FileFrameTiming&)>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::WaitForFileFrameSlot),
				bool (LibplaceboPluginVideoRenderer::*)(uint32_t)>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::CancelFileFrameWait),
				void (LibplaceboPluginVideoRenderer::*)()>));
			Assert::AreEqual(static_cast<uint32_t>(45), VP_LIBPLACEBO_PLUGIN_API_VERSION);
		}

		TEST_METHOD(IntentAndCalibrationHaveExplicitProxyOverrides)
		{
			// ABI 38 (intent: GetIntentStatus / SetAmbientLux) and ABI 39 (automatic
			// calibration, claimed 36 on its branch, appended after the intent methods):
			// every call is forwarded to the plugin.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::GetIntentStatus),
				bool (LibplaceboPluginVideoRenderer::*)(IntentStatus&) const>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::SetAmbientLux),
				void (LibplaceboPluginVideoRenderer::*)(double, bool, double)>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::GetCalibrationOutput),
				bool (LibplaceboPluginVideoRenderer::*)(Calibration::PatternOutputInfo&) const>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::SetCalibrationPattern),
				bool (LibplaceboPluginVideoRenderer::*)(const Calibration::PatternRequest&, CString&)>));
			// ABI 40 (bug hunt 3 F2, integration round 5): appended after calibration.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::SetActivePictureLookaheadAutomatic),
				void (LibplaceboPluginVideoRenderer::*)(bool)>));
			// ABI 41 (r6 fix 7, "Fallbacks engaged"): appended after the look-ahead switch.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::GetFallbacksStatus),
				bool (LibplaceboPluginVideoRenderer::*)(CString&) const>));
			// ABI 42 (VP-V delay budget, ADR-0020): appended after GetFallbacksStatus.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::SetQueueDelayBudget),
				void (LibplaceboPluginVideoRenderer::*)(int)>));
			// ABI 43 (feat/dv-l8, assigned at the integ/batch1 merge): appended after SetQueueDelayBudget.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::ToggleDvCompareCapture),
				bool (LibplaceboPluginVideoRenderer::*)(CString&)>));
			// ABI 44 (WP-11, integration group C): the generic module status and command entries,
			// appended after ToggleDvCompareCapture.
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::GetModuleStatus),
				bool (LibplaceboPluginVideoRenderer::*)(const char*, CString&) const>));
			Assert::IsTrue((std::is_same_v<
				decltype(&LibplaceboPluginVideoRenderer::InvokeModuleCommand),
				bool (LibplaceboPluginVideoRenderer::*)(const char*, const char*, CString&)>));
			Assert::AreEqual(static_cast<uint32_t>(45), VP_LIBPLACEBO_PLUGIN_API_VERSION);
		}
	};
}
