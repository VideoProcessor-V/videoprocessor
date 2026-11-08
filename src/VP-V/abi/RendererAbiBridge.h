// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Renderer side of the VP-V extension ABI (M2 P2-2): implements the seven interfaces of VpvInterfaces.h by
// calling the renderer's own public methods (LibplaceboVideoRenderer keeps their bodies; P11 moves them).
// LibplaceboVideoRenderer::QueryExtension returns Query(iid), which gives the address of the matching nested
// interface object. The bridge is created in the renderer's constructor and destroyed before its Impl; it holds
// nothing but a back-pointer and keeps no state (each method runs on the calling thread under the contract
// documented in VpvInterfaces.h).
//
// The class template is header-only so a CPU test can bind it to a fake host; the renderer's instance is made by
// MakeRendererAbiBridge (RendererAbiBridge.cpp, renderer projects only). Host must provide the old method set:
//   bool GetModuleStatus(const char*, CString&) const;  bool InvokeModuleCommand(const char*, const char*, CString&);
//   bool GetDejudderStatus/GetSourceResolutionStatus/GetFallbacksStatus/GetLedDriveStatus(CString&) const;
//   bool WriteDiagnosticHistory(VpDiagnosticHistorySink, void*, int64_t) const;
//   bool SuspendLedDriveForTest(CString&);  void ResumeLedDriveAfterTest();  bool StopLedDriveForShutdown();
//   bool GetIntentStatus(IntentStatus&) const;  bool GetContentIntentStatus(ContentIntentStatus&) const;
//   void SetAmbientLux(double, bool, double);
//   bool GetCalibrationOutput(Calibration::PatternOutputInfo&) const;
//   bool SetCalibrationPattern(const Calibration::PatternRequest&, CString&);
//   bool SupportsTimedFileFrames() const;  void OnTimedVideoFrame(VideoFrame&, const FileFrameTiming&);
//   bool WaitForFileFrameSlot(uint32_t);  void CancelFileFrameWait();
//   void SetQueueDelayBudget(int);  void SetActivePictureLookaheadAutomatic(bool);  bool ToggleDvCompareCapture(CString&);
// C++14.

#include <VP-V/abi/VpvInterfaces.h>
#include <VP-V/abi/VpvText.h>

#include <atlstr.h>
#include <cstring>
#include <memory>

class LibplaceboVideoRenderer;

namespace Vpv
{
	// The non-template base, so the renderer header needs only a forward declaration of it.
	class RendererAbiBridge
	{
	public:
		virtual ~RendererAbiBridge() = default;
		// The interface object for `iid` (the exact pointer type Interface::Iid() names), or nullptr for an unknown
		// iid or a null iid. Never throws; takes no lock.
		virtual void* Query(const char* iid) = 0;
	};

	template <class Host>
	class RendererAbiBridgeT final : public RendererAbiBridge
	{
	public:
		explicit RendererAbiBridgeT(Host& host)
			: modules_(host), status_(host), led_(host), intent_(host), calibration_(host), timed_(host), presentation_(host)
		{
		}

		void* Query(const char* iid) override
		{
			if (!iid)
				return nullptr;
			if (std::strcmp(iid, IVpvModules::Iid()) == 0) return static_cast<IVpvModules*>(&modules_);
			if (std::strcmp(iid, IVpvStatus::Iid()) == 0) return static_cast<IVpvStatus*>(&status_);
			if (std::strcmp(iid, IVpvLed::Iid()) == 0) return static_cast<IVpvLed*>(&led_);
			if (std::strcmp(iid, IVpvIntent::Iid()) == 0) return static_cast<IVpvIntent*>(&intent_);
			if (std::strcmp(iid, IVpvCalibration::Iid()) == 0) return static_cast<IVpvCalibration*>(&calibration_);
			if (std::strcmp(iid, IVpvTimedFileFrames::Iid()) == 0) return static_cast<IVpvTimedFileFrames*>(&timed_);
			if (std::strcmp(iid, IVpvPresentationCtl::Iid()) == 0) return static_cast<IVpvPresentationCtl*>(&presentation_);
			return nullptr;
		}

	private:
		struct ModulesImpl final : IVpvModules
		{
			explicit ModulesImpl(Host& h) : host(h) {}
			bool Status(const char* module, IVpvTextSink& sink) const override
			{
				CString text;
				const bool answered = host.GetModuleStatus(module, text);
				WriteText(sink, text);
				return answered;
			}
			bool Command(const char* module, const char* command, IVpvTextSink& reply) override
			{
				CString text;
				const bool handled = host.InvokeModuleCommand(module, command, text);
				WriteText(reply, text);
				return handled;
			}
			Host& host;
		};

		struct StatusImpl final : IVpvStatus
		{
			explicit StatusImpl(Host& h) : host(h) {}
			bool GetDejudderStatus(IVpvTextSink& sink) const override { return Line(sink, &Host::GetDejudderStatus); }
			bool GetSourceResolutionStatus(IVpvTextSink& sink) const override { return Line(sink, &Host::GetSourceResolutionStatus); }
			bool GetFallbacksStatus(IVpvTextSink& sink) const override { return Line(sink, &Host::GetFallbacksStatus); }
			bool WriteDiagnosticHistory(VpDiagnosticHistorySink sink, void* context, int64_t referenceQpc) const override
			{
				return host.WriteDiagnosticHistory(sink, context, referenceQpc);
			}
			bool Line(IVpvTextSink& sink, bool (Host::*getter)(CString&) const) const
			{
				CString text;
				const bool known = (host.*getter)(text);
				WriteText(sink, text);
				return known;
			}
			Host& host;
		};

		struct LedImpl final : IVpvLed
		{
			explicit LedImpl(Host& h) : host(h) {}
			bool Status(IVpvTextSink& sink) const override
			{
				CString text;
				const bool known = host.GetLedDriveStatus(text);
				WriteText(sink, text);
				return known;
			}
			bool SuspendForTest(IVpvTextSink& reason) override
			{
				CString text;
				const bool suspended = host.SuspendLedDriveForTest(text);
				WriteText(reason, text);
				return suspended;
			}
			void ResumeAfterTest() override { host.ResumeLedDriveAfterTest(); }
			bool StopForShutdown() override { return host.StopLedDriveForShutdown(); }
			Host& host;
		};

		struct IntentImpl final : IVpvIntent
		{
			explicit IntentImpl(Host& h) : host(h) {}
			bool GetIntentStatus(IntentStatus& status) const override { return host.GetIntentStatus(status); }
			bool GetContentIntentStatus(ContentIntentStatus& status) const override { return host.GetContentIntentStatus(status); }
			void SetAmbientLux(double lux, bool valid, double nowSeconds) override { host.SetAmbientLux(lux, valid, nowSeconds); }
			Host& host;
		};

		struct CalibrationImpl final : IVpvCalibration
		{
			explicit CalibrationImpl(Host& h) : host(h) {}
			bool GetOutput(::Calibration::PatternOutputInfo& info) const override { return host.GetCalibrationOutput(info); }
			bool SetPattern(const ::Calibration::PatternRequest& request, IVpvTextSink& reason) override
			{
				CString text;
				const bool shown = host.SetCalibrationPattern(request, text);
				WriteText(reason, text);
				return shown;
			}
			Host& host;
		};

		struct TimedImpl final : IVpvTimedFileFrames
		{
			explicit TimedImpl(Host& h) : host(h) {}
			bool Supports() const override { return host.SupportsTimedFileFrames(); }
			void OnTimedVideoFrame(VideoFrame& videoFrame, const FileFrameTiming& timing) override { host.OnTimedVideoFrame(videoFrame, timing); }
			bool WaitSlot(uint32_t timeoutMs) override { return host.WaitForFileFrameSlot(timeoutMs); }
			void Cancel() override { host.CancelFileFrameWait(); }
			Host& host;
		};

		struct PresentationImpl final : IVpvPresentationCtl
		{
			explicit PresentationImpl(Host& h) : host(h) {}
			void SetQueueDelayBudget(int totalMs) override { host.SetQueueDelayBudget(totalMs); }
			void SetActivePictureLookaheadAutomatic(bool automatic) override { host.SetActivePictureLookaheadAutomatic(automatic); }
			bool ToggleDvCompareCapture(IVpvTextSink& status) override
			{
				CString text;
				const bool handled = host.ToggleDvCompareCapture(text);
				WriteText(status, text);
				return handled;
			}
			Host& host;
		};

		ModulesImpl modules_;
		StatusImpl status_;
		LedImpl led_;
		IntentImpl intent_;
		CalibrationImpl calibration_;
		TimedImpl timed_;
		PresentationImpl presentation_;
	};

	// The renderer's bridge (RendererAbiBridge.cpp).
	std::unique_ptr<RendererAbiBridge> MakeRendererAbiBridge(LibplaceboVideoRenderer& renderer);
}
