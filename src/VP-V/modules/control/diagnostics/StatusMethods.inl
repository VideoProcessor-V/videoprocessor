// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// refs/110 s2.3 (P11a-e): the VP-V public status / control methods of LibplaceboVideoRenderer, moved VERBATIM out of
// LibplaceboVideoRenderer.cpp into renderer member definitions (the pattern of IngressPolicy.inl): DynamicDriveContext,
// GetCalibrationOutput, SetCalibrationPattern, the LED suspend / stop / resume, GetDejudderStatus, GetFallbacksStatus,
// GetModuleStatus, ToggleDvCompareCapture, GetLedDriveStatus, GetSourceResolutionStatus, GetContentIntentStatus,
// GetIntentStatus and SetAmbientLux. They are reached through the VP-V ABI bridge (src/VP-V/abi/RendererAbiBridge.h,
// the S6 seam QueryExtension / InvokeModuleCommand, which stay in the renderer) and the C14 reader
// (StatusExtension). Bill's own methods of the same family (RequestCalibrationLutReload, the scene status methods,
// GetOutputContractStatus, GetPresentationTimingStatus, GetDolbyVisionLuminanceStatus) exist in Bill's file and stay.
// LibplaceboVideoRenderer.cpp includes this file ONCE, after the queue-target inline file; no other translation unit may.
// No text inside a moved body changed.
#pragma once

// Dynamic Drive (WP-15): what the host adapter needs from this renderer, built per call.
LedDrive::HostContext LibplaceboVideoRenderer::DynamicDriveContext() const
{
	LedDrive::HostContext context;
	if (m_impl)
	{
		context.frame = &m_impl->dynamicDrive;
		// VP-V: slot drive.projector_control (off in the Original pathway, ADR-0019, or at slot level).
		context.projectorAllowed = m_impl->pipelineSlots.Runs(PipelineSlots::Id::DriveProjectorControl);
		context.lastRenderStartedTick = m_impl->libplaceboRenderStartedTick.load(std::memory_order_acquire);
	}
	return context;
}

// RS-232 connection test: stop the LED drive (it restores full light and
// closes the port), unless video is playing; ResumeLedDriveAfterTest starts it
// again from the saved settings.
bool LibplaceboVideoRenderer::GetCalibrationOutput(Calibration::PatternOutputInfo& info) const
{
	info = Calibration::PatternOutputInfo();
	auto setReason = [&info](const char* text) { strncpy_s(info.reason, sizeof(info.reason), text, _TRUNCATE); };
	if (!m_impl || !m_implInitialized.load(std::memory_order_acquire))
	{
		setReason("the VP Renderer is not running (start VP with any source)");
		return false;
	}
	if (m_state.load(std::memory_order_acquire) != RendererState::RENDERSTATE_RENDERING)
	{
		setReason("the VP Renderer is not rendering (start VP with any source)");
		return false;
	}
	return m_impl->calibrationHost.CollectOutputInfo(m_impl->renderMutex, m_calibrationPatternActive.load(std::memory_order_acquire),
		[this]() -> CalibrationLutHost::OutputFacts {   // runs with renderMutex held
			double hz = m_impl->renderLoadDisplayRefreshHz.load(std::memory_order_acquire);
			if (!(hz > 0.0)) hz = MonitorRefreshHz(m_videoHwnd);
			return { m_impl->negotiatedSwapchainFormat, m_impl->activeSettings.displayBitDepth,
				Impl::EncodingLevels(m_impl->actualOutput.encoding) == PL_COLOR_LEVELS_LIMITED, m_impl->TargetIsBt2020(), hz,
				m_impl->swapchain != nullptr && !m_impl->outputResourcesRetired };
		}, info);
}

bool LibplaceboVideoRenderer::SetCalibrationPattern(const Calibration::PatternRequest& request, CString& reason)
{
	reason.Empty();
	if (!m_impl)
	{
		reason = TEXT("the VP Renderer is not running");
		return !request.active;
	}
	if (request.active)
	{
		if (m_state.load(std::memory_order_acquire) != RendererState::RENDERSTATE_RENDERING)
		{
			reason = TEXT("the VP Renderer is not rendering (start VP with any source)");
			return false;
		}
	}
	return m_impl->calibrationHost.SetPatterns(request, m_calibrationPatternActive,
		{ [this]() { return m_dynamicDrive.SuspendForCalibration(DynamicDriveContext()); },
		  [this](const char* why) { BeginQueueGeneration(why); },
		  [this]() { m_queueChanged.notify_all(); },
		  [this]() { m_dynamicDrive.ResumeAfterCalibration(DynamicDriveContext()); },
		  [this]() { m_impl->ReleaseFallback("calibration patch"); } });
}

bool LibplaceboVideoRenderer::SuspendLedDriveForTest(CString& reason)
{
	reason.Empty();
	std::string why;
	const bool suspended = m_dynamicDrive.SuspendForTest(DynamicDriveContext(), why);
	if (!suspended)
		reason = CString(why.c_str());
	return suspended;
}

// Integration 2026-10-17: the forced shutdown's LED stop. The render state lock may be
// held by the blocked retirement, so the module comes from the render loop's atomic copy
// (or the host adapter's when the lock is free at once). Stop() restores full light.
bool LibplaceboVideoRenderer::StopLedDriveForShutdown()
{
	// Only the render side's module copy and a lock that is free at once (never pipelineSlots:
	// the render thread may be rebuilding it while a blocked retirement holds the state lock).
	LedDrive::HostContext context;
	if (m_impl)
		context.frame = &m_impl->dynamicDrive;
	return m_dynamicDrive.StopForShutdown(context);
}

void LibplaceboVideoRenderer::ResumeLedDriveAfterTest()
{
	m_dynamicDrive.ResumeAfterTest(DynamicDriveContext());
}

bool LibplaceboVideoRenderer::GetDejudderStatus(CString& status) const
{
	status.Empty();
	if (!m_impl)
		return false;
	std::lock_guard<std::mutex> guard(m_impl->dejudderStatusMutex);
	if (m_impl->dejudderStatus.empty())
		return false;
	status = CString(m_impl->dejudderStatus.c_str());
	return true;
}

bool LibplaceboVideoRenderer::GetFallbacksStatus(CString& status) const
{
	status.Empty();
	if (!m_impl)
		return false;
	status = CString(RendererExtension::WithStatusLines(RendererExtension::StatusKind::Fallbacks, m_impl.get(), m_impl->fallbacks.Heading()).c_str()); // VPV-HOOK
	return true;
}

// VP-V (WP-11): the renderer's VP lines through the generic module status entry; the keys and
// format live in src/VP-V/core/StatusText.h, the lines come from the getters above.
bool LibplaceboVideoRenderer::GetModuleStatus(const char* module, CString& status) const
{
	std::wstring text;
	const bool answered = VpStatusText::RendererModuleStatus<wchar_t>(module,
		[this](VpStatusText::RendererLines<wchar_t>& lines)
		{
			CString line;   // read after each getter (argument order is unspecified)
			const auto take = [&line](VpStatusText::Line<wchar_t>& to, bool known) { to.Set(known, line.GetString()); };
			take(lines.dejudder, GetDejudderStatus(line));
			take(lines.fallbacks, GetFallbacksStatus(line));
			take(lines.ledDrive, GetLedDriveStatus(line));
			take(lines.sourceResolution, GetSourceResolutionStatus(line));
			std::vector<std::string> extensionLines; RendererExtension::CollectStatusLines(RendererExtension::StatusKind::Lines, m_impl.get(), extensionLines); // VPV-HOOK
			VpStatusText::SetExtensionLines(lines, extensionLines, [](const std::string& text) { return std::wstring(CString(text.c_str()).GetString()); }); // VPV-HOOK
		}, text);
	status = text.c_str();
	return answered;
}

// feat/dv-l8 WIP (plugin API 43): the dv_compare_capture shortcut (DvCompareCapture.h).
bool LibplaceboVideoRenderer::ToggleDvCompareCapture(CString& status)
{
	status.Empty();
	if (!m_impl)
		return false;
	std::string reply;
	if (!m_impl->captures.Invoke(DvCompareCapture::kCommand, reply))
		return false;
	status = CString(reply.c_str());
	return true;
}

bool LibplaceboVideoRenderer::GetLedDriveStatus(CString& status) const
{
	status = CString(m_dynamicDrive.StatusText().c_str());
	return true;
}

bool LibplaceboVideoRenderer::GetSourceResolutionStatus(CString& status) const
{
	status.Empty();
	if (!m_impl)
		return false;
	const std::string selection = m_impl->sourceMonitor.Selection();
	std::string line = SourceResolutionMonitor::Describe(m_impl->sourceMonitor.Read(), m_impl->sourceMonitor.AutoRatio(), selection == "auto");
	line += "; source_resolution " + selection;
	std::string p0Line;   // R4a (resync_event on): the per-axis re-base and its source-time fades
	if (m_impl->rebaseP0Status.Text(p0Line))
		line += "; " + p0Line;
	status = CString(line.c_str());
	return true;
}

bool LibplaceboVideoRenderer::GetContentIntentStatus(ContentIntentStatus& status) const
{
	status = {};
	if (!m_impl)
		return false;
	status = m_impl->contentIntent->Status();
	return status.enabled;
}

// Intent interpreter items 2-5 (2026-10-17): what the GUI shows (stats overlay,
// crash state, log). False when every one of the four functions is off.
bool LibplaceboVideoRenderer::GetIntentStatus(IntentStatus& status) const
{
	status = {};
	if (!m_impl)
		return false;
	status.motion = m_impl->vpv.motion.intent.intentInterpreter->MotionStatus();
	std::lock_guard<std::mutex> guard(m_impl->vpv.led.intent.intentMutex);
	const double ownerBlack = m_impl->sdrBlackNits;
	AmbientAdapt::FillStatus(m_impl->vpv.led.intent.ambientSettings, m_impl->vpv.led.intent.ambientFilter, ownerBlack,
		m_impl->vpv.led.intent.ambientAppliedBlackNits > 0.0 ? m_impl->vpv.led.intent.ambientAppliedBlackNits : ownerBlack, status.ambient);
	status.ambient.enabled = m_impl->vpv.led.intent.ambientSettings.enabled;
	if (status.ambient.enabled && !status.ambient.sensorValid)
		CopyIntentText(status.ambient.reason, "no room reading (detector off or lost)");
	else if (status.ambient.enabled && !(m_impl->vpv.led.intent.ambientSettings.strength > 0.0))
		CopyIntentText(status.ambient.reason, "strength 0");
	if (m_impl->vpv.led.intent.intentBlackPublished != BlackIntentMode::Off)
	{
		status.black = m_impl->vpv.led.intent.blackIntentStatus;
		status.black.mode = m_impl->vpv.led.intent.intentBlackPublished;
	}
	if (m_impl->vpv.led.intent.intentDisplayPublished)
	{
		status.display = m_impl->vpv.led.intent.displayIntentStatus;
		status.display.enabled = true;
	}
	return status.motion.enabled || status.ambient.enabled || status.black.mode != BlackIntentMode::Off ||
		status.display.enabled;
}

// The GUI's [ambient] detector reading, about once a second (IRenderer contract).
void LibplaceboVideoRenderer::SetAmbientLux(double lux, bool valid, double nowSeconds)
{
	if (!m_impl)
		return;
	std::lock_guard<std::mutex> guard(m_impl->vpv.led.intent.intentMutex);
	if (!m_impl->vpv.led.intent.ambientSettings.enabled)
		return;
	if (valid && std::isfinite(lux))
		m_impl->vpv.led.intent.ambientFilter.AddReading(lux < 0.0 ? 0.0 : lux, nowSeconds);
	else
		m_impl->vpv.led.intent.ambientFilter.Tick(nowSeconds);
}
