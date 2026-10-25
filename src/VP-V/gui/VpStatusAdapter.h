#pragma once

// VP-V GUI adapter (WP-11): the ONE place the dialog and the stats overlay get VP's status
// lines and dispatch VP's shortcuts. VideoProcessorDlg.cpp (Bill's file) keeps one call per
// site; the wording and order live in src/VP-V/core/StatusText.h (CPU-tested), the renderer's VP
// lines arrive through the generic IVideoRenderer::GetModuleStatus entry (module "vp").
//
// Windows/MFC side (CString, IVideoRenderer); header-only so it needs no project registration
// beyond the includer. Every function reproduces the old dialog code's text byte for byte
// (src/VP-V/tests/VpStatusAdapterTests.cpp compares it with verbatim copies of that code).

#include <IRenderer.h>
#include <IntentInterpreterSupport.h>
#include <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>
#include <VP-V/core/StatusText.h>
#include <VP-V/gui/VpShortcuts.h>

#include <atlstr.h>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace VpvGui
{
	using WLine = VpStatusText::Line<wchar_t>;

	inline std::wstring Wide(const CString& text) { return std::wstring(text.GetString(), static_cast<size_t>(text.GetLength())); }
	// As the old call sites converted a status line: CStringA (the ANSI code page).
	inline std::string Narrow(const std::wstring& text) { return std::string(CStringA(text.c_str()).GetString()); }

	// One poll of the renderer's VP module: what the old GetDejudderStatus / GetFallbacksStatus /
	// GetLedDriveStatus / GetSourceResolutionStatus calls returned.
	struct Snapshot
	{
		bool rendererPresent = false;
		VpStatusText::RendererLines<wchar_t> vp;
	};

	inline Snapshot Poll(IVideoRenderer* renderer)
	{
		Snapshot snapshot;
		snapshot.rendererPresent = renderer != nullptr;
		CString text;
		if (renderer && renderer->GetModuleStatus(VpStatusText::kRendererModule, text))
			snapshot.vp = VpStatusText::ReadRendererModule(Wide(text));
		return snapshot;
	}

	// The "Fallbacks engaged" heading for the one-time notices and the Config status ("" = none).
	inline std::string FallbacksHeading(const Snapshot& snapshot)
	{
		return snapshot.vp.fallbacks.known ? Narrow(snapshot.vp.fallbacks.text) : std::string();
	}

	// The dialog's own VP lines (its state, not the renderer's).
	struct DialogLines
	{
		CString keyOverrides;   // KeyOverrideSummary()
		CString room;           // AmbientOverlayLine()
		CString content;        // ContentIntentLine()
		IntentStatus intent;    // m_intentStatus
	};

	inline VpStatusText::CtrlILines<wchar_t> IntentLines(const IntentStatus& intent)
	{
		VpStatusText::CtrlILines<wchar_t> lines;
		lines.motionIntent = Wide(CString(IntentInterpreter::MotionIntentLine(intent.motion).c_str()));
		lines.ambientIntent = Wide(CString(IntentInterpreter::AmbientIntentLine(intent.ambient).c_str()));
		lines.blackIntent = Wide(CString(IntentInterpreter::BlackIntentLine(intent.black).c_str()));
		lines.displayIntent = Wide(CString(IntentInterpreter::DisplayIntentLine(intent.display).c_str()));
		return lines;
	}

	// Ctrl+I: VP's rows for the overlay's two extension points. RowVec is the overlay's row vector
	// (value_type constructible from {CString label, CString value}). The de-judder line is the
	// VP Renderer's only, as before.
	template <class RowVec>
	void FillCtrlIRows(RowVec& afterShaderRule, RowVec& afterShaders, const Snapshot& snapshot,
		const DialogLines& dialog, bool isAlphaRenderer)
	{
		VpStatusText::CtrlILines<wchar_t> lines = IntentLines(dialog.intent);
		lines.keyOverrides = Wide(dialog.keyOverrides);
		lines.room = Wide(dialog.room);
		lines.content = Wide(dialog.content);
		if (isAlphaRenderer)
			lines.dejudder = snapshot.vp.dejudder.text;
		lines.fallbacks = snapshot.vp.fallbacks.text;
		const VpStatusText::CtrlIRows<wchar_t> rows = VpStatusText::BuildCtrlIRows(lines);
		afterShaderRule.clear();
		afterShaders.clear();
		for (const auto& row : rows.afterShaderRule)
			afterShaderRule.push_back({ CString(row.label.c_str()), CString(row.value.c_str()) });
		for (const auto& row : rows.afterShaders)
			afterShaders.push_back({ CString(row.label.c_str()), CString(row.value.c_str()) });
	}

	// Crash state: the renderer-owned keys. Recorder has SetValue(std::string, std::string).
	// presentation and render stall are the host's own getters (Bill's), read here as before.
	// `content` is ContentIntentLine(), `intent` m_intentStatus (unused without a renderer).
	template <class Recorder>
	void RecordCrashState(Recorder& recorder, IVideoRenderer* renderer, const Snapshot& snapshot,
		const CString& content, const IntentStatus& intent)
	{
		VpStatusText::CrashStateInputs<wchar_t> in;
		if (renderer)
		{
			in.dejudder = snapshot.vp.dejudder;
			in.fallbacks = snapshot.vp.fallbacks;
			in.ledDrive = snapshot.vp.ledDrive;
			in.sourceResolution = snapshot.vp.sourceResolution;
			CString text;
			const bool presentation = renderer->GetPresentationTimingStatus(text);
			in.presentation.Set(presentation, Wide(text));
			const bool stall = renderer->GetRenderStallStatus(text);
			in.renderStall.Set(stall, Wide(text));
			const VpStatusText::CtrlILines<wchar_t> intentLines = IntentLines(intent);
			in.content = Wide(content);
			in.motionIntent = intentLines.motionIntent;
			in.ambientIntent = intentLines.ambientIntent;
			in.blackIntent = intentLines.blackIntent;
			in.displayIntent = intentLines.displayIntent;
		}
		for (const auto& line : VpStatusText::CrashStateRendererLines(renderer != nullptr, in, Narrow))
			recorder.SetValue(line.first, line.second);
	}

	// Ctrl+Alt+D: the renderer's live lines at the key press, appended in the old order.
	template <class Settings>
	void AppendBundleSettings(Settings& settings, IVideoRenderer* renderer)
	{
		if (!renderer)
			return;
		const Snapshot snapshot = Poll(renderer);
		VpStatusText::BundleInputs<wchar_t> in;
		CString text;
		const bool presentation = renderer->GetPresentationTimingStatus(text);
		in.presentation.Set(presentation, Wide(text));
		in.dejudder = snapshot.vp.dejudder;
		in.fallbacks = snapshot.vp.fallbacks;
		const bool stall = renderer->GetRenderStallStatus(text);
		in.renderStall.Set(stall, Wide(text));
		const bool output = renderer->GetOutputModeInfo(text);
		in.outputMode.Set(output, Wide(text));
		for (const auto& item : VpStatusText::BundleRendererSettings(in, Narrow))
			settings.emplace_back(item.first, item.second);
	}

	// "sports (inferred)" / "film (from metadata)" / "classifying (...)"; empty while off.
	inline CString ContentIntentLine(const ContentIntentStatus& status)
	{
		VpStatusText::ContentFacts facts;
		facts.enabled = status.enabled;
		facts.classified = status.content != ContentClass::Unknown;
		facts.reason = status.reason;
		facts.content = ToString(status.content);
		facts.source = DisplayName(status.source);
		if (status.candidate != ContentClass::Unknown && status.candidate != status.content)
			facts.pendingCandidate = ToString(status.candidate);
		CString line;
		const std::string text = VpStatusText::ContentLine(facts);
		if (!text.empty())
			line.Format(TEXT("%hs"), text.c_str());   // the old line was built with %hs
		return line;
	}

	inline CString AmbientLine(const VpStatusText::AmbientFacts& facts)
	{
		return CString(VpStatusText::AmbientLine(facts, [](double lux)
		{
			CString reading;
			reading.Format(TEXT(" (sensor %.1f lux)"), lux);   // as the old line (see DefaultLuxReading)
			return std::string(CStringA(reading).GetString());
		}).c_str());
	}

	// Delta 6 "Key override" items, joined.
	inline CString KeyOverrideSummary(const std::vector<CString>& items)
	{
		std::vector<std::wstring> wide;
		for (const CString& item : items)
			wide.push_back(Wide(item));
		return CString(VpStatusText::KeyOverrideSummary(wide).c_str());
	}

	// ---- VP shortcuts -----------------------------------------------------------------------------
	// What the dialog lends the dispatcher. Handlers whose state still lives in the dialog
	// (room lights: the ambient glue; state report: the diagnostics glue) are callbacks until
	// their own work packages move them (WP-16, WP-17).
	struct CommandHost
	{
		std::function<void()> roomLights;
		std::function<void()> stateReport;
		std::function<void(const CString&)> showDetail;   // the status line (OnRendererDetailString)
		IVideoRenderer* renderer = nullptr;
	};

	// True when the command is VP's (handled or not).
	inline bool DispatchCommand(unsigned int command, const CommandHost& host)
	{
		if (!VpvShortcuts::IsVpCommand(command))
			return false;
		switch (command)
		{
		case ID_COMMAND_ROOM_LIGHTS:
			if (host.roomLights)
				host.roomLights();
			break;
		case ID_COMMAND_STATE_REPORT:
			if (host.stateReport)
				host.stateReport();
			break;
		case ID_COMMAND_RAW_CAPTURE_TOGGLE:
		{
			// Raw capture recording on / off (RawCaptureRecorder.h): the status line is logged
			// by the recorder and shown like the rendered-output capture status.
			const std::string status = RawCaptureRecorder::Instance().Toggle();
			if (host.showDetail)
				host.showDetail(CString(status.c_str()));
			break;
		}
		default:
			break;
		}
		return true;
	}
}
