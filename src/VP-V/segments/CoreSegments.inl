// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P4-2 (doc 52 section 4.1 item d): the pipeline-slot registry update and the frame slot-hook ordering, moved VERBATIM out of
// LibplaceboVideoRenderer.cpp (only the signatures gained the LibplaceboVideoRenderer::Impl:: qualification; default arguments stay on
// the declarations in Impl). These are out-of-line member definitions of the renderer's Impl: this file is included ONCE by
// LibplaceboVideoRenderer.cpp right after the Impl struct. It is not a stand-alone header and no other translation unit may include it.
// Render thread (renderMutex held; Initialize and ApplyProfileSettingsLiveLocked), as before.
#pragma once

void LibplaceboVideoRenderer::Impl::UpdatePipelineSlots(const RendererSettings& settings)
{
	PipelineSlots::Registry next;
	if (settings.pipelineOriginalPathway)
		next.ApplyOriginalPathway();
	// Every refused value is visible (owner rule: no silent fallbacks), never log-only.
	std::string problems = settings.pipelineSettingsProblems;
	std::string token;
	auto flush = [&]()
	{
		if (token.empty())
			return;
		const PipelineSlots::Id id = PipelineSlots::FromName(token.c_str());
		if (id == PipelineSlots::Id::Count)
			problems += "pipeline_slots_off: unknown slot '" + token + "'; ";
		else if (!PipelineSlots::SwitchHonoured(id))
			problems += "pipeline_slots_off: " + token + " has no slot switch yet; ";
		else
			next.SetEnabled(id, false);
		token.clear();
	};
	for (char ch : settings.pipelineSlotsOff)
	{
		if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t')
			flush();
		else
			token += ch;
	}
	flush();
	if (!problems.empty())
	{
		problems.resize(problems.size() - 2);   // the last "; "
		EngageFallback("pipeline settings", "ignored: " + problems);
	}
	else
		ReleaseFallback("pipeline settings");
	std::string reason;
	if (!settings.pipelineOriginalPathway && !next.SetOrderByNames(settings.pipelineOrder, &reason))
		EngageFallback("pipeline order", "designed order kept: " + reason);   // an invalid order never runs
	else
		ReleaseFallback("pipeline order");
	if (settings.presentEveryVsyncWithheld)   // PresentationOwner (owner decision 2): visible, never log-only
		EngageFallback(PresentationOwner::FallbackId(), PresentationOwner::WithheldText());
	else
		ReleaseFallback(PresentationOwner::FallbackId());
	next.SetEntranceOnce(settings.pipelineEntranceOnce && !settings.pipelineOriginalPathway);   // B3: consumers after the entrance
	(void)next.ActiveEntrance();   // warm the kept list here, so the render thread only reads it
	pipelineSlots = next;
	// T-AR3-N2 (AUDIT-R 1fa78cdb9 N2): the ingress fix switches of THESE settings, published once here for the capture and
	// file threads (IngressHardwareArrival / OnTimedVideoFrame), instead of their reading activeSettings across threads.
	if (!Vpv::Hub::ExtensionHub::Instance().NoteFixSwitches(this, Vpv::Ingress::IngressState::FixSwitchBitsOf(Vpv::Ingress::IngressState::FixSwitchesOf(settings))))
		EngageFallback("ingress switches", "not published (host table full): capture-thread fix switches keep their built values and the Original-pathway gate was not recorded");   // counted: ExtensionHub::PathwayRefusals
	else
		ReleaseFallback("ingress switches");
	// PHASE 1 of the host seams plan (refs/107 s2.4 item 1; owner E3 2026-11-11): the ONE settings seam hands the VP-V
	// state over when the Original pathway is selected, so the gated module calls (RenderAhead::Presenter reporting, the
	// de-judder timeline plan: `// VPV-GATED`) can do nothing afterwards without a VP-V plan outliving the switch: the
	// render-ahead chain is broken and its gate reset once (Presenter::SetOriginalPathway), the per-vsync timeline is
	// released with its fallback line, the phase picture dropped. Idempotent; a VP-V apply only clears the fact.
	renderAheadPresenter.SetOriginalPathway(settings.pipelineOriginalPathway, RenderAheadNow());
	if (settings.pipelineOriginalPathway)
	{
		VpMotion::ReleaseVpTimeline(dejudderPolicy, [this](const char* what) { LogVpTimeline(what); });
		ReleaseFallback("timeline");
		vpPhasePlanesValid = false;
		// PHASE 2 (refs/113 F2): the re-base shader warm-up is idled here too; its per-frame call in the render loop is
		// gated in Original and would otherwise never clear a warm-up in flight (the "Preparing shaders..." status).
		const VpRebaseWarmup::Controller::Outcome warm = rebaseWarmup.EnterOriginalPathway();
		if (warm.fallbackChanged) SyncFallback(warm.fallback);
	}
	VpFrameAnalysis::Host::SetLimitedDomain(settings.sourceCodingFixes);   // DIV-AN-01 either/or (Original: off = Bill's codes)
	frameAnalysis.Owner(this).Configure(settings.frameAnalysisStore && !settings.pipelineOriginalPathway,   // WP-23
		VpFrameAnalysis::Host::FullLumaReaders(pipelineSlots, settings.opticalFlow, settings.motionAheadFrames,
			settings.sourceResolution == "auto"));   // running full-luma readers; record readers: none yet
	RendererExtension::NotifyLifecycle(RendererExtension::Lifecycle::SettingsApplied, this, nullptr, nullptr, settings.resyncEvent.c_str(), nullptr, settings.pipelineOriginalPathway ? 1 : 0, nullptr, static_cast<double>(settings.motionAheadFrames)); // VPV-HOOK
	entrance.Invalidate();
	std::string state = settings.pipelineOriginalPathway ? "Original pathway (every VP addition off)" :
		pipelineSlots.IsDefaultOrder() ? "designed order" : "custom order";
	std::string entrance;
	for (PipelineSlots::Id id : pipelineSlots.ActiveEntrance())
		entrance += (entrance.empty() ? "" : " > ") + std::string(PipelineSlots::Get(id).name);
	state += "; entrance: " + (entrance.empty() ? std::string("none") : entrance);
	const std::string slotSwitches = SlotSwitchSettings::Describe(SlotSwitchSettings::FromText(settings.pipelineSlotsOff),
		settings.pipelineOriginalPathway);
	if (!slotSwitches.empty())
		state += "; " + slotSwitches;
	state += settings.pipelineEntranceOnce && !settings.pipelineOriginalPathway ?
		"; entrance once (B3) on: motion pictures from the processed A and B" : "; entrance once (B3) off";
	// entrance_gpu_drain: visible while it is on (Ctrl+I "engaged" line, Config status), never silent.
	// Listed only when it can act: on, not the Original pathway, and scaling.rebase runs.
	EntranceGpuDrain::Mode drainMode = EntranceGpuDrain::Mode::Off;
	EntranceGpuDrain::Parse(settings.entranceGpuDrain, drainMode);
	if (drainMode != EntranceGpuDrain::Mode::Off && !settings.pipelineOriginalPathway &&
		pipelineSlots.Runs(PipelineSlots::Id::ScalingRebase))
	{
		state += std::string("; entrance GPU drain: ") + EntranceGpuDrain::Name(drainMode);
		EngageFallback("entrance GPU drain", EntranceGpuDrain::StatusReason(drainMode));   // a mode change updates the reason
	}
	else
		ReleaseFallback("entrance GPU drain");
	if (!settings.pipelineEntranceOnce || settings.pipelineOriginalPathway)
	{
		this->entrance.OnceSwitchedOff();   // its textures are freed on the render thread, never from here
	}
	if (state != pipelineStateLogged)
	{
		pipelineStateLogged = state;
		DebugLog::Log("Pipeline: %s", state.c_str());
	}
}

void LibplaceboVideoRenderer::Impl::OrderFrameSlotHooks()
{
	if (frameSlotHooks.size() < 2)
		return;
	std::vector<OutputHookOrder::Tagged<const pl_hook*>> tagged;
	tagged.reserve(frameHooks.size());
	for (const pl_hook* hook : frameHooks)
	{
		OutputHookOrder::Tagged<const pl_hook*> t;
		t.hook = hook;
		for (const auto& entry : frameSlotHooks)
			if (entry.first == hook)
			{
				t.kind = OutputHookOrder::Kind::Slot;
				t.slot = entry.second;
				break;
			}
		tagged.push_back(t);
	}
	frameHooks = OutputHookOrder::OrderSlotsInPlace(tagged,
		[this](PipelineSlots::Id id) { return pipelineSlots.Order(id); });
}

// M2 P4-4 (doc 52 section 4.1 item h): the reports after the render call - the merged output segments, the custom-hook failures,
// Dynamic Drive, extended range and the calibration LUT revert - moved VERBATIM out of RenderLocked (the block used no local of it:
// every name is an Impl member). Render thread, renderMutex held, immediately after the render call, as before.
void LibplaceboVideoRenderer::Impl::ReportRenderErrors()
{
    pl_render_errors hookErrors = pl_renderer_get_errors(renderer);
    // output_segments_merged (audit M4): a stage that failed inside a merged hook takes the
    // same path as a hook libplacebo disabled; a disabled merged hook falls back visibly.
    const std::vector<uint64_t> mergedFailed = outputMerger->Report(&hookErrors);
    if (outputMerger->FallbacksChanged())
        DebugLog::Log("%s", fallbacks.Heading().c_str());
    std::vector<uint64_t> allDisabled;
    if (!mergedFailed.empty())
    {
        allDisabled.assign(hookErrors.disabled_hooks, hookErrors.disabled_hooks + hookErrors.num_disabled_hooks);
        allDisabled.insert(allDisabled.end(), mergedFailed.begin(), mergedFailed.end());
        hookErrors.disabled_hooks = allDisabled.data();
        hookErrors.num_disabled_hooks = static_cast<int>(allDisabled.size());
    }
    if (customHooks.MarkRuntimeFailures(hookErrors.disabled_hooks,
        hookErrors.num_disabled_hooks))
        PublishCustomShaderStatus();
    dynamicDrive.OnHookErrors(hookErrors.disabled_hooks, hookErrors.num_disabled_hooks);
    extendedRange.NoteDisabledHooks(hookErrors.disabled_hooks, hookErrors.num_disabled_hooks);
    // output.calibration_lut (VP stage, R1): a disabled or failing hook reverts to libplacebo's
    // target LUT for the rest of the session, visibly (skeleton audit item 5).
    {
        const FallbackCatalog::Decision calibration = calibrationHost.AfterRender(hookErrors);
        if (calibration.engaged) EngageFallback(calibration.id.c_str(), calibration.reason);
    }
}
