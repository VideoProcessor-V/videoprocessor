// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Report-a-problem, diagnostic stats recording, VP command dispatch and DV compare capture
// toggle, moved out of Bill's VideoProcessorDlg.cpp into compiled VP-V GUI code.
#include "pch.h"

#include "VideoProcessorDlg.h"
#include "version.h"

#include <VP-V/abi/VpvHostCalls.h>
#include <VP-V/gui/VpStatusAdapter.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/control/diagnostics/DiagnosticsReportRun.h>
#include <VpBuildIdentity.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
// Report-a-problem (owner 2026-10-17, StateReport.h). The state_report
// shortcut (default Ctrl+Alt+D) and the system-menu item save a report of the
// last few seconds of VP's state to logs\reports for problems that do not
// crash VP. The UI thread takes the snapshots (rings, renderer rings, live
// settings, crash-state text) so they describe the moment of the key press;
// a worker hashes the binaries, reads the log and config, writes the zip and
// posts WM_MESSAGE_STATE_REPORT_DONE.
// VP-V (WP-11): VP's shortcuts (src/VP-V/gui/VpShortcuts.h) dispatch in one place.
void CVideoProcessorDlg::OnVpCommand(UINT commandId)
{
	VpvGui::CommandHost host;
	host.roomLights = [this] { OnCommandRoomLights(); };
	host.stateReport = [this] { SaveStateReport("hotkey"); };
	host.showDetail = [this](const CString& status)
		{ OnRendererDetailString(status, m_rendererGeneration.load(std::memory_order_acquire)); };
	host.renderer = m_videoRenderer.get();
	VpvGui::DispatchCommand(commandId, host);
}

void CVideoProcessorDlg::ShowStateReportToast(const std::string& value)
{
	DebugLog::Log("Problem report: %s", value.c_str());
	if (m_profileChangeDisplaySeconds == 0)
	{
		// The overlay is switched off: say it on the status line instead.
		OnRendererDetailString(CString(CStringA(("Problem report: " + value).c_str())),
			m_rendererGeneration.load(std::memory_order_acquire));
		return;
	}
	ProfileChangeOverlay::Item item;
	item.group = "report";
	item.label = "Problem report";
	item.value = value;
	ShowProfileOverlayItems({ item }, false, "state-report");
}

void CVideoProcessorDlg::MaybeAutoReport(const char* reason)
{
	if (m_reportRun.AllowAutomatic(m_stateReportSettings.autoReport, reason, GetTickCount64()))
		SaveStateReport((std::string("auto:") + reason).c_str());
}

void CVideoProcessorDlg::RecordDiagnosticStats(double sourceHz, double displayHz)
{
	const ULONGLONG now = GetTickCount64();
	if (m_diagnosticLastStatsTick != 0 && now - m_diagnosticLastStatsTick < 900)
		return;
	const double seconds = m_diagnosticLastStatsTick != 0 ?
		static_cast<double>(now - m_diagnosticLastStatsTick) / 1000.0 : 0.0;
	m_diagnosticLastStatsTick = now;
	DiagnosticHistory::StatsRecord record;
	record.values[0] = sourceHz;
	record.values[1] = displayHz;
	if (m_videoRenderer)
	{
		record.values[2] = static_cast<double>(m_videoRenderer->GetFrameQueueSize());
		record.values[3] = m_videoRenderer->EntryLatencyMs();
		record.values[4] = m_videoRenderer->ExitLatencyMs();
		record.values[5] = static_cast<double>(m_videoRenderer->DroppedFrameCount());
		record.values[6] = static_cast<double>(m_videoRenderer->SceneAwareCorrectionRepeatCount());
		RendererRenderLoad load;
		if (m_videoRenderer->GetRenderLoad(load) && load.gpuValid && load.framePeriodMs > 0.0)
			record.values[8] = load.gpu.average * 100.0 / load.framePeriodMs;
	}
	record.values[7] = m_processCpuUsage.CurrentPercent();
	const uint64_t captured = m_diagnosticCapturedFrames.load(std::memory_order_relaxed);
	if (seconds > 0.0)
		record.values[10] = static_cast<double>(captured - m_diagnosticLastCapturedFrames) / seconds;
	m_diagnosticLastCapturedFrames = captured;
	record.values[11] = static_cast<double>(m_diagnosticRejectedFrames.load(std::memory_order_relaxed));
	record.values[12] = static_cast<double>(m_rendererGeneration.load(std::memory_order_acquire));
	record.values[13] = m_captureDeviceVideoState && m_captureDeviceVideoState->valid ? 1.0 : 0.0;
	DiagnosticHistory::Global().RecordStats(record);
}

void CVideoProcessorDlg::SaveStateReport(const char* trigger)
{
	// The run, the shared inputs and the worker: VP diagnostics adapter (DiagnosticsReportRun.h).
	if (!m_reportRun.TryBegin(GetTickCount64()))
	{
		ShowStateReportToast("already saving");
		return;
	}
	auto inputs = m_reportRun.NewInputs(trigger, m_videoRenderer.get());
	inputs->buildId = VpBuildIdentity::Id();
	inputs->commit = VpBuildIdentity::Commit();
	inputs->version = std::string(CStringA(VERSION_DESCRIBE).GetString()) + " (" +
		std::string(CStringA(VERSION_BRANCH).GetString()) + (VERSION_DIRTY ? ", dirty)" : ")");
	inputs->pluginApi = VP_LIBPLACEBO_PLUGIN_API_VERSION;
	inputs->rendererName = !m_videoRenderer ? "none" :
		(m_activeRendererIsDirectShow ? "DirectShow" : "VP Renderer");

	// Live settings: the effective choices now (values are redacted on write).
	auto& settings = inputs->liveSettings;
	settings.emplace_back("renderer", inputs->rendererName);
	settings.emplace_back("renderer_generation", std::to_string(m_rendererGeneration.load(std::memory_order_acquire)));
	settings.emplace_back("renderer_state", std::to_string(static_cast<int>(m_rendererState)));
	settings.emplace_back("window", m_fullScreenVideoWindow && ::IsWindow(m_fullScreenVideoWindow->GetHWND()) ?
		"fullscreen" : "windowed");
	settings.emplace_back("video_only_ui", m_hideUI ? "true" : "false");
	settings.emplace_back("stats_overlay", m_statsOverlay ? "true" : "false");
	settings.emplace_back("profile_change_display_seconds", std::to_string(m_profileChangeDisplaySeconds));
	settings.emplace_back("shortcuts.foreground_only", m_shortcutsForegroundOnly ? "true" : "false");
	settings.emplace_back("shortcuts.global_hotkeys", m_shortcutsGlobalHotkeys ? "true" : "false");
	settings.emplace_back("logging.auto_report", m_stateReportSettings.autoReport ? "true" : "false");
	settings.emplace_back("logging.report_thumbnail", m_stateReportSettings.thumbnail ? "true" : "false");
	settings.emplace_back("logging.enhanced", DebugLog::IsEnhancedLoggingEnabled() ? "true" : "false");
	if (m_captureDeviceVideoState && m_captureDeviceVideoState->valid && m_captureDeviceVideoState->displayMode)
	{
		const auto& mode = *m_captureDeviceVideoState->displayMode;
		char source[160];
		snprintf(source, sizeof(source), "%ux%u%s %.3f Hz, %s, %s", mode.FrameWidth(), mode.FrameHeight(),
			mode.IsInterlaced() ? "i" : "p", mode.RefreshRateHz(),
			CStringA(ToString(m_captureDeviceVideoState->eotf)).GetString(),
			CStringA(ToString(m_captureDeviceVideoState->colorspace)).GetString());
		settings.emplace_back("source", source);
	}
	else
		settings.emplace_back("source", "no signal");
	if (const auto snapshot = m_profileRuntime.GetSnapshot())
		for (const auto& selection : RendererProfileConfig::FormatSelections(snapshot->effectiveSelections))
			settings.emplace_back("profile." + selection.first, selection.second);
	VpvGui::AppendBundleSettings(settings, m_videoRenderer.get());   // VP-V (WP-11)

	// Optional picture (report_thumbnail, off by default).
	m_reportRun.RequestThumbnail(*inputs, m_stateReportSettings.thumbnail, m_videoRenderer.get());

	ShowStateReportToast("saving...");
	if (!m_reportRun.Start(inputs, GetSafeHwnd(), WM_MESSAGE_STATE_REPORT_DONE))
		ShowStateReportToast("failed");
}

LRESULT CVideoProcessorDlg::OnStateReportDone(WPARAM, LPARAM lParam)
{
	const std::string toast = m_reportRun.Finish(lParam);
	if (!toast.empty())
		ShowStateReportToast(toast);
	return 0;
}

// feat/dv-l8 WIP: the dv_compare_capture shortcut - a DV comparison clip in the renderer
// (DvCompareCapture.h); the status line is shown like the raw capture's.
void CVideoProcessorDlg::OnCommandDvCompareCapture()
{
	CString status;
	if (!m_videoRenderer || !VpvCalls::ToggleDvCompareCapture(m_videoRenderer.get(), status))
		status = _T("DV comparison capture: needs the VP Renderer");
	OnRendererDetailString(status, m_rendererGeneration.load(std::memory_order_acquire));
}
