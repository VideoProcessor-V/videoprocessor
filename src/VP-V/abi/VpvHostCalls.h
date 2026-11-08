// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Host-side one-line calls for the 22 methods that used to be IVideoRenderer virtuals (M2 P2-4). Each
// function queries the renderer for the interface on EVERY call (VpvRendererAccess.h, D-P2c; nothing is
// cached), converts the text, and, when the renderer has no such interface (DirectShow renderers, a renderer
// without VP-V, no renderer), returns exactly what the old IVideoRenderer default returned.
// Windows/MFC side (CString); header-only; C++14.

#include <VP-V/abi/VpvInterfaces.h>
#include <VP-V/abi/VpvRendererAccess.h>
#include <VP-V/abi/VpvText.h>

#include <atlstr.h>
#include <string.h>
#include <string>

namespace VpvCalls
{
	namespace detail
	{
		// The text an interface wrote, as a CString.
		inline CString Text(const Vpv::StringSink& sink) { return Vpv::FromUtf8(sink.text); }
	}

	// ---- vpv.modules.v1 ------------------------------------------------------------------------------
	// Default (no interface): false, text empty.
	inline bool GetModuleStatus(IVideoRenderer* renderer, const char* module, CString& status)
	{
		status.Empty();
		Vpv::IVpvModules* modules = Vpv::Query<Vpv::IVpvModules>(renderer);
		if (!modules)
			return false;
		Vpv::StringSink sink;
		const bool answered = modules->Status(module, sink);
		status = detail::Text(sink);
		return answered;
	}

	inline bool InvokeModuleCommand(IVideoRenderer* renderer, const char* module, const char* command, CString& reply)
	{
		reply.Empty();
		Vpv::IVpvModules* modules = Vpv::Query<Vpv::IVpvModules>(renderer);
		if (!modules)
			return false;
		Vpv::StringSink sink;
		const bool handled = modules->Command(module, command, sink);
		reply = detail::Text(sink);
		return handled;
	}

	// ---- vpv.status.v1 -------------------------------------------------------------------------------
	// Default: false, text empty.
	inline bool GetDejudderStatus(IVideoRenderer* renderer, CString& status)
	{
		status.Empty();
		Vpv::IVpvStatus* i = Vpv::Query<Vpv::IVpvStatus>(renderer);
		if (!i)
			return false;
		Vpv::StringSink sink;
		const bool known = i->GetDejudderStatus(sink);
		status = detail::Text(sink);
		return known;
	}

	inline bool GetSourceResolutionStatus(IVideoRenderer* renderer, CString& status)
	{
		status.Empty();
		Vpv::IVpvStatus* i = Vpv::Query<Vpv::IVpvStatus>(renderer);
		if (!i)
			return false;
		Vpv::StringSink sink;
		const bool known = i->GetSourceResolutionStatus(sink);
		status = detail::Text(sink);
		return known;
	}

	inline bool GetFallbacksStatus(IVideoRenderer* renderer, CString& status)
	{
		status.Empty();
		Vpv::IVpvStatus* i = Vpv::Query<Vpv::IVpvStatus>(renderer);
		if (!i)
			return false;
		Vpv::StringSink sink;
		const bool known = i->GetFallbacksStatus(sink);
		status = detail::Text(sink);
		return known;
	}

	// Default: false (no rows). A null sink writes nothing (the plugin proxy's old guard).
	inline bool WriteDiagnosticHistory(IVideoRenderer* renderer, VpDiagnosticHistorySink sink, void* context,
		int64_t referenceQpc)
	{
		if (!sink)
			return false;
		Vpv::IVpvStatus* i = Vpv::Query<Vpv::IVpvStatus>(renderer);
		return i && i->WriteDiagnosticHistory(sink, context, referenceQpc);
	}

	// ---- vpv.led.v1 ----------------------------------------------------------------------------------
	inline bool GetLedDriveStatus(IVideoRenderer* renderer, CString& status)
	{
		status.Empty();
		Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer);
		if (!i)
			return false;
		Vpv::StringSink sink;
		const bool known = i->Status(sink);
		status = detail::Text(sink);
		return known;
	}

	// Default: true (no drive holds a port), reason empty.
	inline bool SuspendLedDriveForTest(IVideoRenderer* renderer, CString& reason)
	{
		reason.Empty();
		Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer);
		if (!i)
			return true;
		Vpv::StringSink sink;
		const bool suspended = i->SuspendForTest(sink);
		reason = detail::Text(sink);
		return suspended;
	}

	inline void ResumeLedDriveAfterTest(IVideoRenderer* renderer)
	{
		if (Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer))
			i->ResumeAfterTest();
	}

	// Default: true (stopped or there is none).
	inline bool StopLedDriveForShutdown(IVideoRenderer* renderer)
	{
		Vpv::IVpvLed* i = Vpv::Query<Vpv::IVpvLed>(renderer);
		return i ? i->StopForShutdown() : true;
	}

	// ---- vpv.intent.v1 -------------------------------------------------------------------------------
	// The status is default-constructed first (as the plugin proxy did), so a renderer that answers false leaves
	// the defaults. Default (no interface): the defaults, false.
	inline bool GetIntentStatus(IVideoRenderer* renderer, IntentStatus& status)
	{
		status = {};
		Vpv::IVpvIntent* i = Vpv::Query<Vpv::IVpvIntent>(renderer);
		return i && i->GetIntentStatus(status);
	}

	inline bool GetContentIntentStatus(IVideoRenderer* renderer, ContentIntentStatus& status)
	{
		status = {};
		Vpv::IVpvIntent* i = Vpv::Query<Vpv::IVpvIntent>(renderer);
		return i && i->GetContentIntentStatus(status);
	}

	inline void SetAmbientLux(IVideoRenderer* renderer, double lux, bool valid, double nowSeconds)
	{
		if (Vpv::IVpvIntent* i = Vpv::Query<Vpv::IVpvIntent>(renderer))
			i->SetAmbientLux(lux, valid, nowSeconds);
	}

	// ---- vpv.calibration.v1 --------------------------------------------------------------------------
	static constexpr const char* kNoCalibrationText = "this renderer cannot show calibration patches (use VP Renderer)";

	// Default: info.available false, info.reason = kNoCalibrationText, false.
	inline bool GetCalibrationOutput(IVideoRenderer* renderer, Calibration::PatternOutputInfo& info)
	{
		Vpv::IVpvCalibration* i = Vpv::Query<Vpv::IVpvCalibration>(renderer);
		if (!i)
		{
			info = Calibration::PatternOutputInfo();
			strncpy_s(info.reason, sizeof(info.reason), kNoCalibrationText, _TRUNCATE);
			return false;
		}
		return i->GetOutput(info);
	}

	// Default: reason = kNoCalibrationText, false.
	inline bool SetCalibrationPattern(IVideoRenderer* renderer, const Calibration::PatternRequest& request, CString& reason)
	{
		Vpv::IVpvCalibration* i = Vpv::Query<Vpv::IVpvCalibration>(renderer);
		if (!i)
		{
			reason = CString(kNoCalibrationText);
			return false;
		}
		Vpv::StringSink sink;
		const bool shown = i->SetPattern(request, sink);
		reason = detail::Text(sink);
		return shown;
	}

	// ---- vpv.timed_file_frames.v1 --------------------------------------------------------------------
	// Default: false.
	inline bool SupportsTimedFileFrames(IVideoRenderer* renderer)
	{
		Vpv::IVpvTimedFileFrames* i = Vpv::Query<Vpv::IVpvTimedFileFrames>(renderer);
		return i && i->Supports();
	}

	// Default: the frame is a capture frame (the old IVideoRenderer default forwarded to OnVideoFrame).
	inline void OnTimedVideoFrame(IVideoRenderer* renderer, VideoFrame& videoFrame, const FileFrameTiming& timing)
	{
		if (Vpv::IVpvTimedFileFrames* i = Vpv::Query<Vpv::IVpvTimedFileFrames>(renderer))
			i->OnTimedVideoFrame(videoFrame, timing);
		else if (renderer)
			renderer->OnVideoFrame(videoFrame);
	}

	// Default: true (a renderer that never queues file frames never makes the filter wait).
	inline bool WaitForFileFrameSlot(IVideoRenderer* renderer, uint32_t timeoutMs)
	{
		Vpv::IVpvTimedFileFrames* i = Vpv::Query<Vpv::IVpvTimedFileFrames>(renderer);
		return i ? i->WaitSlot(timeoutMs) : true;
	}

	inline void CancelFileFrameWait(IVideoRenderer* renderer)
	{
		if (Vpv::IVpvTimedFileFrames* i = Vpv::Query<Vpv::IVpvTimedFileFrames>(renderer))
			i->Cancel();
	}

	// ---- vpv.presentation_ctl.v1 ---------------------------------------------------------------------
	inline void SetQueueDelayBudget(IVideoRenderer* renderer, int totalMs)
	{
		if (Vpv::IVpvPresentationCtl* i = Vpv::Query<Vpv::IVpvPresentationCtl>(renderer))
			i->SetQueueDelayBudget(totalMs);
	}

	inline void SetActivePictureLookaheadAutomatic(IVideoRenderer* renderer, bool automatic)
	{
		if (Vpv::IVpvPresentationCtl* i = Vpv::Query<Vpv::IVpvPresentationCtl>(renderer))
			i->SetActivePictureLookaheadAutomatic(automatic);
	}

	// Default: false, status empty.
	inline bool ToggleDvCompareCapture(IVideoRenderer* renderer, CString& status)
	{
		status.Empty();
		Vpv::IVpvPresentationCtl* i = Vpv::Query<Vpv::IVpvPresentationCtl>(renderer);
		if (!i)
			return false;
		Vpv::StringSink sink;
		const bool handled = i->ToggleDvCompareCapture(sink);
		status = detail::Text(sink);
		return handled;
	}
}
