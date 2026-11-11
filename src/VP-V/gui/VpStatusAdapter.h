#pragma once

// VP-V GUI adapter (WP-11): the ONE place the dialog and the stats overlay get VP's status
// lines and dispatch VP's shortcuts. VideoProcessorDlg.cpp (Bill's file) keeps one call per
// site; the wording and order live in src/VP-V/core/StatusText.h (CPU-tested), the renderer's VP
// lines arrive through the vpv.modules.v1 extension interface (Status, module "vp"; src/VP-V/abi).
//
// W1-C14: this file is a THIN CLIENT of the one status reader,
// src/VP-V/modules/control/diagnostics/StatusExtension.h. It only polls the renderer and the
// dialog's getters and converts CString <-> std::wstring; reading and composing the lines
// (fallbacks heading, Ctrl+I rows, crash-state keys, bundle lines) happens in the extension.
//
// Windows/MFC side (CString, IVideoRenderer); header-only so it needs no project registration
// beyond the includer. Every function reproduces the old dialog code's text byte for byte
// (src/VP-V/tests/VpStatusAdapterTests.cpp compares it with verbatim copies of that code).

#include <IRenderer.h>
#include <IntentInterpreterSupport.h>
#include <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>
#include <VP-V/modules/control/diagnostics/StatusExtension.h>
#include <VP-V/abi/VpvHostCalls.h>
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
	// GetLedDriveStatus / GetSourceResolutionStatus calls returned (StatusExtension::Snapshot).
	using Snapshot = StatusExtension::Snapshot;

	inline Snapshot Poll(IVideoRenderer* renderer)
	{
		CString text;
		const bool answered = renderer && VpvCalls::GetModuleStatus(renderer, VpStatusText::kRendererModule, text);
		return StatusExtension::Read(renderer != nullptr, answered, answered ? Wide(text) : std::wstring());
	}

	// The "Fallbacks engaged" heading for the one-time notices and the Config status ("" = none).
	inline std::string FallbacksHeading(const Snapshot& snapshot)
	{
		return StatusExtension::FallbacksHeading(snapshot, Narrow);
	}

	// The dialog's own VP lines (its state, not the renderer's).
	struct DialogLines
	{
		CString keyOverrides;   // KeyOverrideSummary()
		CString room;           // AmbientOverlayLine()
		CString content;        // ContentIntentLine()
		IntentStatus intent;    // m_intentStatus
	};

	// The dialog's wide texts for the extension (intent lines from the interpreter, as before).
	inline StatusExtension::DialogTexts DialogTextsOf(const DialogLines& dialog)
	{
		StatusExtension::DialogTexts texts;
		texts.motionIntent = Wide(CString(IntentInterpreter::MotionIntentLine(dialog.intent.motion).c_str()));
		texts.ambientIntent = Wide(CString(IntentInterpreter::AmbientIntentLine(dialog.intent.ambient).c_str()));
		texts.blackIntent = Wide(CString(IntentInterpreter::BlackIntentLine(dialog.intent.black).c_str()));
		texts.displayIntent = Wide(CString(IntentInterpreter::DisplayIntentLine(dialog.intent.display).c_str()));
		texts.keyOverrides = Wide(dialog.keyOverrides);
		texts.room = Wide(dialog.room);
		texts.content = Wide(dialog.content);
		return texts;
	}

	// Ctrl+I: VP's rows for the overlay's two extension points. RowVec is the overlay's row vector
	// (value_type constructible from {CString label, CString value}). The de-judder line is the
	// VP Renderer's only, as before.
	template <class RowVec>
	void FillCtrlIRows(RowVec& afterShaderRule, RowVec& afterShaders, const Snapshot& snapshot,
		const DialogLines& dialog, bool isAlphaRenderer)
	{
		const VpStatusText::CtrlIRows<wchar_t> rows =
			StatusExtension::CtrlIRows(snapshot, DialogTextsOf(dialog), isAlphaRenderer);
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
		StatusExtension::HostLines host;
		StatusExtension::DialogTexts texts;
		if (renderer)
		{
			CString text;
			const bool presentation = renderer->GetPresentationTimingStatus(text);
			host.presentation.Set(presentation, Wide(text));
			const bool stall = renderer->GetRenderStallStatus(text);
			host.renderStall.Set(stall, Wide(text));
			DialogLines dialog;
			dialog.intent = intent;
			texts = DialogTextsOf(dialog);
			texts.content = Wide(content);
		}
		for (const auto& line : StatusExtension::CrashState(snapshot, host, texts, Narrow))
			recorder.SetValue(line.first, line.second);
	}

	// Ctrl+Alt+D: the renderer's live lines at the key press, appended in the old order.
	template <class Settings>
	void AppendBundleSettings(Settings& settings, IVideoRenderer* renderer)
	{
		if (!renderer)
			return;
		const Snapshot snapshot = Poll(renderer);
		StatusExtension::HostLines host;
		CString text;
		const bool presentation = renderer->GetPresentationTimingStatus(text);
		host.presentation.Set(presentation, Wide(text));
		const bool stall = renderer->GetRenderStallStatus(text);
		host.renderStall.Set(stall, Wide(text));
		const bool output = renderer->GetOutputModeInfo(text);
		host.outputMode.Set(output, Wide(text));
		for (const auto& item : StatusExtension::Bundle(snapshot, host, Narrow))
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
