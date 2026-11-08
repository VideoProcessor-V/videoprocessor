// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The VP-V extension interfaces (M2 P2, D-P2a): pure abstract, no STL type and no CString across the plugin
// boundary (text goes through IVpvTextSink). They are reached with IVideoRenderer::QueryExtension(Iid()) and
// the host never caches the pointer (D-P2c, VpvRendererAccess.h). A changed interface gets a NEW iid,
// never an edit; the iids are the ABI.
//
// Thread contracts are those of the virtuals these methods replaced (IRenderer.h at the 45-number line) and are
// unchanged; each is repeated at its method. The implementation (RendererAbiBridge.h) is stateless beyond a
// back-pointer and calls the renderer's own public methods on the calling thread.
//
// C++14: the Lib, renderer and Test projects compile this header.

#include <VP-V/abi/VpvTextSink.h>

#include <IntentDescriptor.h>
#include <FileFrameTiming.h>
#include <VideoFrame.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/output/calibration_lut/calibration/CalibrationPattern.h>

#include <cstddef>
#include <cstdint>

namespace Vpv
{
	// Generic module status and command entries (was IVideoRenderer::GetModuleStatus / InvokeModuleCommand,
	// plugin API 44). `module` "" asks for the index of module ids. Status text is "key=value" lines
	// (ModuleStatusText.h). Caller: VP-V/gui/VpStatusAdapter.h.
	class IVpvModules
	{
	public:
		static constexpr const char* Iid() { return "vpv.modules.v1"; }
		// Any thread (the dialog's UI and stats timers). False: the renderer has no such module.
		virtual bool Status(const char* module, IVpvTextSink& sink) const = 0;
		// A shortcut or host action a module handles. `reply` is key/value text; its "status" key is the line to
		// show. False: not handled.
		virtual bool Command(const char* module, const char* command, IVpvTextSink& reply) = 0;
	protected:
		~IVpvModules() = default;
	};

	// The renderer's status lines (was GetDejudderStatus, GetSourceResolutionStatus, GetFallbacksStatus,
	// WriteDiagnosticHistory). Callers: the dialog's stats (through the module entry), Ctrl+Alt+D, the crash state.
	class IVpvStatus
	{
	public:
		static constexpr const char* Iid() { return "vpv.status.v1"; }
		// Any thread. One line about the de-judder and occlusion mask; false (empty) when both are off.
		virtual bool GetDejudderStatus(IVpvTextSink& status) const = 0;
		// Any thread. The source's effective resolution, bitrate quality and bit depth and what source_resolution
		// auto chose. False when the renderer does not read them.
		virtual bool GetSourceResolutionStatus(IVpvTextSink& status) const = 0;
		// Any thread. "Fallbacks engaged: <id> (<reason>); ..." or "Fallbacks engaged: none".
		virtual bool GetFallbacksStatus(IVpvTextSink& status) const = 0;
		// Report-a-problem: the renderer DLL writes its own DiagnosticHistory rings into `sink` as JSONL rows
		// tagged src "renderer", times relative to referenceQpc. Never throws; false when there is no history.
		// Any thread (the report worker). The sink is a C function pointer, not a C++ type.
		virtual bool WriteDiagnosticHistory(VpDiagnosticHistorySink sink, void* context, int64_t referenceQpc) const = 0;
	protected:
		~IVpvStatus() = default;
	};

	// The LED drive (was GetLedDriveStatus, Suspend/ResumeLedDriveForTest, StopLedDriveForShutdown).
	class IVpvLed
	{
	public:
		static constexpr const char* Iid() { return "vpv.led.v1"; }
		// Any thread. One line about the drive: its source, whether it drives the projector, the last levels sent.
		virtual bool Status(IVpvTextSink& status) const = 0;
		// RS-232 connection test (dialog thread): frees the projector's COM port for the test. False (with the
		// reason written to the sink) when the drive will not, e.g. while video is playing.
		virtual bool SuspendForTest(IVpvTextSink& reason) = 0;
		// After the test (dialog thread): the drive starts again with the saved settings.
		virtual void ResumeAfterTest() = 0;
		// The forced shutdown stops the drive first so it restores full light. MUST NOT wait on the renderer's
		// state lock: the caller bounds the call on its own thread (LedShutdownRestore.h). True when stopped or
		// there is none.
		virtual bool StopForShutdown() = 0;
	protected:
		~IVpvLed() = default;
	};

	// Intent interpreter and ambient adaptation (was GetIntentStatus, GetContentIntentStatus, SetAmbientLux).
	class IVpvIntent
	{
	public:
		static constexpr const char* Iid() { return "vpv.intent.v1"; }
		// Dialog thread (about 1 Hz). Motion intent, ambient adaptation, black protection, display mapping
		// readout. False (status default: every function off) when the renderer has none.
		virtual bool GetIntentStatus(IntentStatus& status) const = 0;
		// Dialog thread. The content class the rules' `content` variable carries, and its source. False (and
		// status.enabled false) when the renderer has none or intent_content is off.
		virtual bool GetContentIntentStatus(ContentIntentStatus& status) const = 0;
		// Dialog thread (about 1 Hz). The newest room reading in lux; `valid` false when the detector is lost
		// or ambient switching is off. `nowSeconds` is GetTickCount64() / 1000.
		virtual void SetAmbientLux(double lux, bool valid, double nowSeconds) = 0;
	protected:
		~IVpvIntent() = default;
	};

	// Automatic calibration patches (was GetCalibrationOutput / SetCalibrationPattern, plugin API 39).
	// Caller: CalibrationController (VP-V), under its own mutex.
	class IVpvCalibration
	{
	public:
		static constexpr const char* Iid() { return "vpv.calibration.v1"; }
		// The output a patch is shown in and which patch generation was presented. False (info.available false,
		// info.reason set) when this renderer cannot show patches.
		virtual bool GetOutput(Calibration::PatternOutputInfo& info) const = 0;
		// Shows a patch (exact codes written to the backbuffer; sigma-delta, dither, LUT and all shading
		// bypassed for the patch only) or, with request.active false, returns to video. While a patch is active
		// the renderer drops incoming frames and holds the LED drive at full light. The failure reason is written
		// to the sink.
		virtual bool SetPattern(const Calibration::PatternRequest& request, IVpvTextSink& reason) = 0;
	protected:
		~IVpvCalibration() = default;
	};

	// Frames from a file source (was SupportsTimedFileFrames, OnTimedVideoFrame, WaitForFileFrameSlot,
	// CancelFileFrameWait, plugin API 37). The frame carries its own presentation time and exact source rate, so
	// the renderer skips capture timing and fills its film clock directly. Caller: VPRendererFilter (the
	// filter's streaming thread; Cancel also from the filter's control thread).
	class IVpvTimedFileFrames
	{
	public:
		static constexpr const char* Iid() { return "vpv.timed_file_frames.v1"; }
		virtual bool Supports() const = 0;
		virtual void OnTimedVideoFrame(VideoFrame& videoFrame, const FileFrameTiming& timing) = 0;
		// A file source never drops: Receive waits here until the queue has room. True when a frame can be queued
		// without a drop; false on timeout or after Cancel (flush, stop). Must not hold locks the render thread
		// needs.
		virtual bool WaitSlot(uint32_t timeoutMs) = 0;
		// Wakes every WaitSlot with false until the next timed frame.
		virtual void Cancel() = 0;
	protected:
		~IVpvTimedFileFrames() = default;
	};

	// Presentation controls (was SetQueueDelayBudget, SetActivePictureLookaheadAutomatic,
	// ToggleDvCompareCapture). Caller: the dialog thread.
	class IVpvPresentationCtl
	{
	public:
		static constexpr const char* Iid() { return "vpv.presentation_ctl.v1"; }
		// VP-V delay budget (ADR-0020): the ONE total delay the user set (target_latency_ms), the same at every
		// frame rate. 0 = no budget (frame-count queue).
		virtual void SetQueueDelayBudget(int totalMs) = 0;
		// The active-picture look-ahead given by SetActivePictureLookaheadFrames is automatic (it follows the
		// queue) rather than an explicit value. An automatic look-ahead sets no floor on the locked-film queue
		// target. (VP-V's own: bug hunt 3 F2, not a registered Bill fix; see README.md.)
		virtual void SetActivePictureLookaheadAutomatic(bool automatic) = 0;
		// The dv_compare_capture shortcut: start a DV comparison clip, or stop one early. `status` = the line to
		// show on screen. False: not supported.
		virtual bool ToggleDvCompareCapture(IVpvTextSink& status) = 0;
	protected:
		~IVpvPresentationCtl() = default;
	};
}
