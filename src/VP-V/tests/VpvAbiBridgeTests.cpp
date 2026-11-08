// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// M2 P2 (doc 61 section 1.4): the renderer-side bridge (RendererAbiBridge.h) and the host-side calls
// (VpvHostCalls.h) over a fake renderer. CPU only: no DLL, no device.
//   - every iid gives its interface, an unknown or null iid gives null;
//   - every one of the 22 former virtuals reaches the host method with its arguments, and text crosses as UTF-8;
//   - a renderer without interfaces (DirectShow, a Bill DLL, no renderer) gives null and the old IVideoRenderer
//     defaults through VpvCalls;
//   - the host never caches: every call asks the renderer again.

#include <IRenderer.h>
#include <VP-V/abi/RendererAbiBridge.h>
#include <VP-V/abi/VpvHostCalls.h>

#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VpvAbiBridgeTestsNs
{
	namespace
	{
		const wchar_t* const kWide = L"on → 60 Hz, café — \U0001F3AC";   // arrows, accents and a non-BMP character
		const char* const kUtf8 = "on \xE2\x86\x92 60 Hz, caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8E\xAC";

		// The old method set the bridge calls (RendererAbiBridge.h lists it). Each records one call.
		struct FakeHost
		{
			mutable std::vector<std::string> calls;
			bool answer = true;
			CString text = CString(kWide);
			int lastBudget = -1;
			bool lastAutomatic = false;
			double lastLux = -1.0;
			uint32_t lastTimeout = 0;
			int64_t lastQpc = -1;

			bool Say(const char* name, CString& out) const { calls.push_back(name); out = text; return answer; }

			bool GetModuleStatus(const char* module, CString& status) const
			{ calls.push_back(std::string("GetModuleStatus:") + (module ? module : "(null)")); status = text; return answer; }
			bool InvokeModuleCommand(const char* module, const char* command, CString& reply)
			{ calls.push_back(std::string("InvokeModuleCommand:") + module + ":" + command); reply = text; return answer; }
			bool GetDejudderStatus(CString& s) const { return Say("GetDejudderStatus", s); }
			bool GetSourceResolutionStatus(CString& s) const { return Say("GetSourceResolutionStatus", s); }
			bool GetFallbacksStatus(CString& s) const { return Say("GetFallbacksStatus", s); }
			bool GetLedDriveStatus(CString& s) const { return Say("GetLedDriveStatus", s); }
			bool WriteDiagnosticHistory(VpDiagnosticHistorySink sink, void* context, int64_t referenceQpc) const
			{
				calls.push_back("WriteDiagnosticHistory");
				if (sink) sink(context, "row\n", 4);
				const_cast<FakeHost*>(this)->lastQpc = referenceQpc;
				return answer;
			}
			bool SuspendLedDriveForTest(CString& reason) { return Say("SuspendLedDriveForTest", reason); }
			void ResumeLedDriveAfterTest() { calls.push_back("ResumeLedDriveAfterTest"); }
			bool StopLedDriveForShutdown() { calls.push_back("StopLedDriveForShutdown"); return answer; }
			bool GetIntentStatus(IntentStatus& status) const { calls.push_back("GetIntentStatus"); status.motion.enabled = true; return answer; }
			bool GetContentIntentStatus(ContentIntentStatus& status) const { calls.push_back("GetContentIntentStatus"); status.enabled = true; return answer; }
			void SetAmbientLux(double lux, bool valid, double) { calls.push_back(valid ? "SetAmbientLux:valid" : "SetAmbientLux:invalid"); lastLux = lux; }
			bool GetCalibrationOutput(Calibration::PatternOutputInfo& info) const { calls.push_back("GetCalibrationOutput"); info.available = true; return answer; }
			bool SetCalibrationPattern(const Calibration::PatternRequest& request, CString& reason)
			{ calls.push_back(request.active ? "SetCalibrationPattern:active" : "SetCalibrationPattern:off"); reason = text; return answer; }
			bool SupportsTimedFileFrames() const { calls.push_back("SupportsTimedFileFrames"); return answer; }
			void OnTimedVideoFrame(VideoFrame&, const FileFrameTiming& timing) { calls.push_back("OnTimedVideoFrame"); lastQpc = timing.presentationQpc; }
			bool WaitForFileFrameSlot(uint32_t timeoutMs) { calls.push_back("WaitForFileFrameSlot"); lastTimeout = timeoutMs; return answer; }
			void CancelFileFrameWait() { calls.push_back("CancelFileFrameWait"); }
			void SetQueueDelayBudget(int totalMs) { calls.push_back("SetQueueDelayBudget"); lastBudget = totalMs; }
			void SetActivePictureLookaheadAutomatic(bool automatic) { calls.push_back("SetActivePictureLookaheadAutomatic"); lastAutomatic = automatic; }
			bool ToggleDvCompareCapture(CString& s) { return Say("ToggleDvCompareCapture", s); }
		};

		void __cdecl CollectRow(void* context, const char* data, size_t length)
		{
			static_cast<std::string*>(context)->append(data, length);
		}

		// A renderer with only Bill's pure virtuals; QueryExtension is the base default (null) unless `offer` is set.
		class PlainRenderer : public IVideoRenderer
		{
		public:
			size_t GetConvertedQueueSize() override { return 0; }
			bool OnVideoState(VideoStateComPtr&) override { return true; }
			void OnVideoFrame(VideoFrame&) override { ++captureFrames; }
			HRESULT OnWindowsEvent(LONG_PTR, LONG_PTR) override { return S_OK; }
			void Build() override {}
			void Start() override {}
			void Stop() override {}
			void Reset() override {}
			void OnSize() override {}
			void OnPaint() override {}
			void SetFrameQueueMaxSize(size_t) override {}
			void SetSceneAwareTimingCorrection(bool) override {}
			size_t GetFrameQueueSize() override { return 0; }
			double EntryLatencyMs() const override { return 0; }
			double ExitLatencyMs() const override { return 0; }
			uint64_t DroppedFrameCount() const override { return 0; }

			void* QueryExtension(const char* iid) override
			{
				++queries;
				return offer ? offer->Query(iid) : nullptr;
			}

			Vpv::RendererAbiBridge* offer = nullptr;
			int queries = 0;
			int captureFrames = 0;
		};

		const char* const kAllIids[] = {
			"vpv.modules.v1", "vpv.status.v1", "vpv.led.v1", "vpv.intent.v1", "vpv.calibration.v1",
			"vpv.timed_file_frames.v1", "vpv.presentation_ctl.v1" };
	}

	TEST_CLASS(VpvAbiBridgeTests)
	{
	public:
		TEST_METHOD(TheIidsAreTheDocumentedStrings)
		{
			Assert::AreEqual(std::string("vpv.modules.v1"), std::string(Vpv::IVpvModules::Iid()));
			Assert::AreEqual(std::string("vpv.status.v1"), std::string(Vpv::IVpvStatus::Iid()));
			Assert::AreEqual(std::string("vpv.led.v1"), std::string(Vpv::IVpvLed::Iid()));
			Assert::AreEqual(std::string("vpv.intent.v1"), std::string(Vpv::IVpvIntent::Iid()));
			Assert::AreEqual(std::string("vpv.calibration.v1"), std::string(Vpv::IVpvCalibration::Iid()));
			Assert::AreEqual(std::string("vpv.timed_file_frames.v1"), std::string(Vpv::IVpvTimedFileFrames::Iid()));
			Assert::AreEqual(std::string("vpv.presentation_ctl.v1"), std::string(Vpv::IVpvPresentationCtl::Iid()));
		}

		TEST_METHOD(EveryIidGivesItsOwnInterface)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			std::vector<void*> seen;
			for (const char* iid : kAllIids)
			{
				void* found = bridge.Query(iid);
				Assert::IsNotNull(found, (std::wstring(L"iid ") + std::wstring(iid, iid + std::strlen(iid))).c_str());
				for (void* earlier : seen)
					Assert::IsTrue(earlier != found, L"two iids gave one object");
				seen.push_back(found);
			}
			// The same iid gives the same object each time (stateless bridge, fixed members).
			Assert::IsTrue(bridge.Query(Vpv::IVpvLed::Iid()) == bridge.Query(Vpv::IVpvLed::Iid()));
		}

		TEST_METHOD(UnknownAndNullIidsGiveNull)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			Assert::IsNull(bridge.Query(nullptr));
			Assert::IsNull(bridge.Query(""));
			Assert::IsNull(bridge.Query("vpv.modules.v2"));
			Assert::IsNull(bridge.Query("vpv.modules"));
			Assert::IsNull(bridge.Query("VPV.MODULES.V1"));
			Assert::IsNull(bridge.Query("test.iid"));
		}

		// F2 of the P2 opus gate: only the seven Vpv iids cross the plugin boundary. The DLL's own modules
		// (vector engine, camera, resync) stay inside it, so QueryExtension never hands out their pointers.
		TEST_METHOD(ModuleIidsStayInsideTheDll)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			PlainRenderer renderer;
			renderer.offer = &bridge;
			for (const char* iid : { "vpv.vector_engine.v1", "vpv.camera.v1", "vpv.resync/1", "vpv.modules.v2" })
				Assert::IsNull(renderer.QueryExtension(iid), L"a module iid reached the host");
		}

		TEST_METHOD(NoRendererAndRenderersWithoutInterfacesGiveNull)
		{
			Assert::IsNull(Vpv::Query<Vpv::IVpvModules>(static_cast<IVideoRenderer*>(nullptr)));
			Assert::IsNull(Vpv::Query<Vpv::IVpvStatus>(std::shared_ptr<IVideoRenderer>()));
			PlainRenderer plain;   // the base default, as a DirectShow renderer has
			Assert::IsNull(Vpv::Query<Vpv::IVpvModules>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvStatus>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvLed>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvIntent>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvCalibration>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvTimedFileFrames>(&plain));
			Assert::IsNull(Vpv::Query<Vpv::IVpvPresentationCtl>(&plain));
			Assert::AreEqual(7, plain.queries);
		}

		TEST_METHOD(ARendererThatOffersTheBridgeGivesAllSeven)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			PlainRenderer renderer;
			renderer.offer = &bridge;
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvModules>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvStatus>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvLed>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvIntent>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvCalibration>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvTimedFileFrames>(&renderer));
			Assert::IsNotNull(Vpv::Query<Vpv::IVpvPresentationCtl>(&renderer));
		}

		// D-P2c: nothing is cached. Each call asks the renderer, and a renderer that stops offering (its plugin
		// went away, the bridge was destroyed) is seen at once.
		TEST_METHOD(EveryUseAsksTheRendererAgain)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			PlainRenderer renderer;
			renderer.offer = &bridge;
			CString status;
			Assert::IsTrue(VpvCalls::GetDejudderStatus(&renderer, status));
			Assert::IsTrue(VpvCalls::GetDejudderStatus(&renderer, status));
			Assert::AreEqual(2, renderer.queries);
			renderer.offer = nullptr;   // the bridge is gone
			Assert::IsFalse(VpvCalls::GetDejudderStatus(&renderer, status));
			Assert::AreEqual(3, renderer.queries);
			Assert::IsTrue(status.IsEmpty());
			Assert::AreEqual(size_t(2), host.calls.size(), L"the host method ran only for the first two calls");
		}

		TEST_METHOD(TextCrossesAsUtf8AndComesBackExactly)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			auto* status = static_cast<Vpv::IVpvStatus*>(bridge.Query(Vpv::IVpvStatus::Iid()));
			Vpv::StringSink sink;
			Assert::IsTrue(status->GetDejudderStatus(sink));
			Assert::AreEqual(std::string(kUtf8), sink.text);
			Assert::IsTrue(Vpv::FromUtf8(sink.text) == CString(kWide));
			// Through the host call as well.
			PlainRenderer renderer;
			renderer.offer = &bridge;
			CString viaHost;
			Assert::IsTrue(VpvCalls::GetFallbacksStatus(&renderer, viaHost));
			Assert::IsTrue(viaHost == CString(kWide));
		}

		TEST_METHOD(AnEmptyAnswerWritesNothingAndKeepsItsFlag)
		{
			FakeHost host;
			host.answer = false;
			host.text = CString();
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			auto* led = static_cast<Vpv::IVpvLed*>(bridge.Query(Vpv::IVpvLed::Iid()));
			Vpv::StringSink sink;
			Assert::IsFalse(led->Status(sink));
			Assert::IsTrue(sink.text.empty());
		}

		// All 22 former virtuals, each reaching the host method it replaced.
		TEST_METHOD(EveryFormerVirtualReachesItsHostMethod)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			PlainRenderer renderer;
			renderer.offer = &bridge;
			IVideoRenderer* r = &renderer;
			CString text;
			VideoFrame frame(nullptr, 1, 0, nullptr);
			FileFrameTiming timing;
			timing.presentationQpc = 4242;
			IntentStatus intent;
			ContentIntentStatus content;
			Calibration::PatternOutputInfo info;
			Calibration::PatternRequest request;
			request.active = true;
			std::string history;

			Assert::IsTrue(VpvCalls::GetModuleStatus(r, "vp", text));
			Assert::IsTrue(VpvCalls::InvokeModuleCommand(r, "vp", "toggle", text));
			Assert::IsTrue(VpvCalls::GetDejudderStatus(r, text));
			Assert::IsTrue(VpvCalls::GetSourceResolutionStatus(r, text));
			Assert::IsTrue(VpvCalls::GetFallbacksStatus(r, text));
			Assert::IsTrue(VpvCalls::WriteDiagnosticHistory(r, &CollectRow, &history, 77));
			Assert::IsTrue(VpvCalls::GetLedDriveStatus(r, text));
			Assert::IsTrue(VpvCalls::SuspendLedDriveForTest(r, text));
			VpvCalls::ResumeLedDriveAfterTest(r);
			Assert::IsTrue(VpvCalls::StopLedDriveForShutdown(r));
			Assert::IsTrue(VpvCalls::GetIntentStatus(r, intent));
			Assert::IsTrue(VpvCalls::GetContentIntentStatus(r, content));
			VpvCalls::SetAmbientLux(r, 12.5, true, 3.0);
			Assert::IsTrue(VpvCalls::GetCalibrationOutput(r, info));
			Assert::IsTrue(VpvCalls::SetCalibrationPattern(r, request, text));
			Assert::IsTrue(VpvCalls::SupportsTimedFileFrames(r));
			VpvCalls::OnTimedVideoFrame(r, frame, timing);
			Assert::IsTrue(VpvCalls::WaitForFileFrameSlot(r, 15));
			VpvCalls::CancelFileFrameWait(r);
			VpvCalls::SetQueueDelayBudget(r, 120);
			VpvCalls::SetActivePictureLookaheadAutomatic(r, true);
			Assert::IsTrue(VpvCalls::ToggleDvCompareCapture(r, text));

			const std::vector<std::string> expected = {
				"GetModuleStatus:vp", "InvokeModuleCommand:vp:toggle", "GetDejudderStatus", "GetSourceResolutionStatus",
				"GetFallbacksStatus", "WriteDiagnosticHistory", "GetLedDriveStatus", "SuspendLedDriveForTest",
				"ResumeLedDriveAfterTest", "StopLedDriveForShutdown", "GetIntentStatus", "GetContentIntentStatus",
				"SetAmbientLux:valid", "GetCalibrationOutput", "SetCalibrationPattern:active", "SupportsTimedFileFrames",
				"OnTimedVideoFrame", "WaitForFileFrameSlot", "CancelFileFrameWait", "SetQueueDelayBudget",
				"SetActivePictureLookaheadAutomatic", "ToggleDvCompareCapture" };
			Assert::AreEqual(expected.size(), host.calls.size());
			for (size_t i = 0; i < expected.size(); ++i)
				Assert::AreEqual(expected[i], host.calls[i]);
			Assert::AreEqual(std::string("row\n"), history);
			Assert::AreEqual(int64_t(4242), host.lastQpc, L"the timing struct reached the host method");
			Assert::AreEqual(uint32_t(15), host.lastTimeout);
			Assert::AreEqual(120, host.lastBudget);
			Assert::IsTrue(host.lastAutomatic);
			Assert::AreEqual(12.5, host.lastLux, 0.0);
			Assert::IsTrue(intent.motion.enabled && content.enabled && info.available);
			Assert::AreEqual(0, renderer.captureFrames, L"a timed frame must not fall back to the capture path");
		}

		// The old IVideoRenderer defaults, for a renderer with no interfaces.
		TEST_METHOD(WithoutInterfacesTheHostCallsGiveTheOldDefaults)
		{
			PlainRenderer renderer;
			IVideoRenderer* r = &renderer;
			CString text(L"stale");
			Assert::IsFalse(VpvCalls::GetModuleStatus(r, "vp", text));
			Assert::IsTrue(text.IsEmpty());
			text = L"stale";
			Assert::IsFalse(VpvCalls::InvokeModuleCommand(r, "vp", "x", text));
			Assert::IsTrue(text.IsEmpty());
			Assert::IsFalse(VpvCalls::GetDejudderStatus(r, text));
			Assert::IsFalse(VpvCalls::GetSourceResolutionStatus(r, text));
			Assert::IsFalse(VpvCalls::GetFallbacksStatus(r, text));
			Assert::IsFalse(VpvCalls::GetLedDriveStatus(r, text));
			text = L"stale";
			Assert::IsTrue(VpvCalls::SuspendLedDriveForTest(r, text), L"a renderer with no LED drive holds no port");
			Assert::IsTrue(text.IsEmpty());
			VpvCalls::ResumeLedDriveAfterTest(r);
			Assert::IsTrue(VpvCalls::StopLedDriveForShutdown(r));
			std::string history;
			Assert::IsFalse(VpvCalls::WriteDiagnosticHistory(r, &CollectRow, &history, 0));
			Assert::IsTrue(history.empty());
			IntentStatus intent;
			intent.motion.enabled = true;
			Assert::IsFalse(VpvCalls::GetIntentStatus(r, intent));
			Assert::IsFalse(intent.motion.enabled, L"the status is reset to its defaults");
			ContentIntentStatus content;
			content.enabled = true;
			Assert::IsFalse(VpvCalls::GetContentIntentStatus(r, content));
			Assert::IsFalse(content.enabled);
			VpvCalls::SetAmbientLux(r, 1.0, true, 1.0);
			Calibration::PatternOutputInfo info;
			info.available = true;
			Assert::IsFalse(VpvCalls::GetCalibrationOutput(r, info));
			Assert::IsFalse(info.available);
			Assert::AreEqual(std::string("this renderer cannot show calibration patches (use VP Renderer)"), std::string(info.reason));
			Calibration::PatternRequest request;
			request.active = true;
			Assert::IsFalse(VpvCalls::SetCalibrationPattern(r, request, text));
			Assert::IsTrue(text == CString(L"this renderer cannot show calibration patches (use VP Renderer)"));
			Assert::IsFalse(VpvCalls::SupportsTimedFileFrames(r));
			Assert::IsTrue(VpvCalls::WaitForFileFrameSlot(r, 5), L"a renderer that never queues file frames never makes the filter wait");
			VpvCalls::CancelFileFrameWait(r);
			VpvCalls::SetQueueDelayBudget(r, 50);
			VpvCalls::SetActivePictureLookaheadAutomatic(r, true);
			Assert::IsFalse(VpvCalls::ToggleDvCompareCapture(r, text));
			Assert::IsTrue(text.IsEmpty());
			// The old IVideoRenderer::OnTimedVideoFrame default forwarded to OnVideoFrame (capture semantics).
			VideoFrame frame(nullptr, 1, 0, nullptr);
			VpvCalls::OnTimedVideoFrame(r, frame, FileFrameTiming());
			Assert::AreEqual(1, renderer.captureFrames);
			// And with no renderer at all nothing throws.
			VpvCalls::OnTimedVideoFrame(nullptr, frame, FileFrameTiming());
			Assert::IsFalse(VpvCalls::GetModuleStatus(nullptr, "vp", text));
			Assert::IsTrue(VpvCalls::StopLedDriveForShutdown(nullptr));
		}

		TEST_METHOD(ANullHistorySinkWritesNothing)
		{
			FakeHost host;
			Vpv::RendererAbiBridgeT<FakeHost> bridge(host);
			PlainRenderer renderer;
			renderer.offer = &bridge;
			Assert::IsFalse(VpvCalls::WriteDiagnosticHistory(&renderer, nullptr, nullptr, 0));
			Assert::IsTrue(host.calls.empty(), L"the proxy's old null-sink guard kept the DLL out of it");
		}
	};
}
