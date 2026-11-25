// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P4-2 (doc 52 section 4.1 item d): the pipeline-slot registry update and the frame slot-hook ordering, moved VERBATIM out of
// LibplaceboVideoRenderer.cpp (only the signatures gained the LibplaceboVideoRenderer::Impl:: qualification; default arguments stay on
// the declarations in Impl). These are out-of-line member definitions of the renderer's Impl: this file is included ONCE by
// LibplaceboVideoRenderer.cpp right after the Impl struct. It is not a stand-alone header and no other translation unit may include it.
// Render thread (renderMutex held; Initialize and ApplyProfileSettingsLiveLocked), as before.
#pragma once
#include <VP-V/modules/scaling/chroma_upscale/CflHookOffsets.h>
#include <VP-V/segments/CoreSegmentCaches.h>
static bool ReadHookListTraceEnv() { char f[4] = {}; return GetEnvironmentVariableA("VP_HOOK_LIST_TRACE", f, sizeof(f)) != 0 && f[0] == '1'; }

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
	{   // CF-1 CAM-4: appearance owns the room-light correction; ambient_adapt was forced off (VpvRendererSettings PostLoad), said here
		const FallbackCatalog::Decision replaced = FallbackCatalog::AmbientAdaptReplaced(settings.ambientAdaptReplaced);
		if (replaced.engaged)
			EngageFallback(replaced.id.c_str(), replaced.reason);   // VPV-FALLBACK ambient_adapt: replaced by appearance
		else
			ReleaseFallback(replaced.id.c_str());
		// FIX1 MUST 6: the saturation step is replaced by appearance (forced off in PostLoad), said here
		const FallbackCatalog::Decision saturation = FallbackCatalog::SaturationReplaced(settings.saturationReplaced, settings.saturationConfigured);
		if (saturation.engaged)
			EngageFallback(saturation.id.c_str(), saturation.reason);   // VPV-FALLBACK saturation: replaced by appearance
		else
			ReleaseFallback(saturation.id.c_str());
	}
	// CF1-G7a: the calibration LUT stage's configured value (auto or not) from these settings, before the next frame's TakeLut.
	calibrationHost.NoteConfiguredStage(settings.calibrationLutStageConfigured == "auto");
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
		ReleaseFallback("calibration LUT BTB/WTW");
		vpPhasePlanesValid = false;
		// PHASE 2 (refs/113 F2): the re-base shader warm-up is idled here too; its per-frame call in the render loop is
		// gated in Original and would otherwise never clear a warm-up in flight (the "Preparing shaders..." status).
		const VpRebaseWarmup::Controller::Outcome warm = rebaseWarmup.EnterOriginalPathway();
		if (warm.fallbackChanged) SyncFallback(warm.fallback);
		ResetEntrancePlan();   // P4 e-i: the planner stops before the VP-V stages in Original; its last outputs go back to native
	}
	VpFrameAnalysis::Host::SetLimitedDomain(settings.sourceCodingFixes);   // DIV-AN-01 either/or (Original: off = Bill's codes)
	frameAnalysis.Owner(this).Configure(settings.frameAnalysisStore && !settings.pipelineOriginalPathway,   // WP-23
		VpFrameAnalysis::Host::FullLumaReaders(pipelineSlots, settings.opticalFlow, settings.motionAheadFrames,
			settings.sourceResolution == "auto"));   // running full-luma readers; record readers: none yet
	// SID r2 (audit MED-1, rule 32; refs/107 s1 S6 CONFIG/STATUS, this settings seam): source_geometry_detect reaches the
	// source monitor on a settings apply, parsed only when it changed, with its unknown-entry line; with auto off nothing is
	// held, so the held re-base notice is released here (OrderFrameSlotHooks syncs it per frame while auto runs).
	if (vpv.sourceAnalysis.sourceMonitor.SetGeometryDetect(settings.sourceGeometryDetect))
		SyncFallback(vpv.sourceAnalysis.sourceMonitor.GeometryDetectFallback());
	if (settings.sourceResolution != "auto")
	{
		SyncFallback(FallbackCatalog::SourceResolutionHeld(std::string()));
		vpv.sourceAnalysis.sourceMonitor.ForgetConventionHold();
	}
	RendererExtension::NotifyLifecycle(RendererExtension::Lifecycle::SettingsApplied, this, nullptr, nullptr, settings.resyncEvent.c_str(), nullptr, settings.pipelineOriginalPathway ? 1 : 0, nullptr, static_cast<double>(settings.motionAheadFrames)); // VPV-HOOK
	Vpv::Segments::CoreSegmentCaches::Instance().RefreshTraceFlag(this, &ReadHookListTraceEnv);
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
	// SID r2 (audit MED-1, rule 32; refs/107 s1 S3 RENDER-PASS, the frame's slot-hook assembly): the held re-base's notice
	// ("source resolution held"), synced only when the monitor's held state changed (audit LOW-1), while auto runs.
	{
		FallbackCatalog::Decision held;
		if (sourceAutoActive.load(std::memory_order_acquire) && vpv.sourceAnalysis.sourceMonitor.ConventionHoldChanged(held))
			SyncFallback(held);
	}
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
    outputFinish.EndFrameDraws();   // GEM-025(b): the frame's draws are over; drops the callback that references the frame's guard
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
    // GEM-025(a): the occlusion mask hook disabled by libplacebo is latched (OutputFinishSegment) and reported visibly.
    if (outputFinish.NoteDisabledHooks(hookErrors.disabled_hooks, hookErrors.num_disabled_hooks))
        SyncFallback(FallbackCatalog::OcclusionMaskDisabled(true));
    // output.calibration_lut (VP stage, R1): a disabled or failing hook reverts to libplacebo's
    // target LUT for the rest of the session, visibly (skeleton audit item 5).
    {
        const FallbackCatalog::Decision calibration = calibrationHost.AfterRender(hookErrors);
        if (calibration.engaged) EngageFallback(calibration.id.c_str(), calibration.reason);
        SyncFallback(calibrationHost.SlopeFallbackDecision());
    }
}

// P4 f test recorder (VP_HOOK_LIST_TRACE=1, cached per host/session in CoreSegmentCaches: zero steady-state env reads).
static void TraceFrameHookList(const void* renderer, const pl_render_params& frameParams,
    const std::vector<std::pair<const pl_hook*, PipelineSlots::Id>>& frameSlotHooks, const CustomHookChain::Chain& customHooks,
    const pl_hook* submittedNls)
{
    if (!Vpv::Segments::CoreSegmentCaches::Instance().TraceHookListEnabled(renderer, &ReadHookListTraceEnv)) return;
    std::string list;
    for (int i = 0; i < frameParams.num_hooks; ++i)
    {
        const pl_hook* hook = frameParams.hooks[i];
        std::string tag = "other";
        for (const auto& entry : frameSlotHooks)
            if (entry.first == hook) { tag = PipelineSlots::Get(entry.second).name; break; }
        if (tag == "other")
        {
            if (hook != nullptr && hook == submittedNls) tag = "nls";
            else if (customHooks.IsQuantizerHook(hook)) tag = "quantiser";
            else if (customHooks.IsPrescalerHook(hook)) tag = "prescaler";
        }
        list += (i ? ", " : "") + tag;
    }
    static const void* lastRenderer = nullptr;   // render thread only (renderMutex held)
    static std::string lastList;
    if (renderer == lastRenderer && list == lastList)
        return;
    lastRenderer = renderer;
    lastList = list;
    DebugLog::Log("VP hook list: n=%d [%s]", frameParams.num_hooks, list.c_str());
}

// M2 P4 f (refs/110 s1.3): the frame's hook collection - output colour and Dynamic Drive, the adaptive downscale, the
// sigma-delta and its vectors, the deband and dither plans and the temporal-history decision - moved VERBATIM out of RenderLocked
// (dedent only). The four results the render call needs (frameParams, renderErrors, extendedRangeRelabelled, temporalEntrance) are
// out-parameters: their declarations became assignments. nextVectorGuard stays declared in RenderLocked (its restore runs after the
// render) and comes in by reference. Render thread, renderMutex held, immediately before the render call, as before.
void LibplaceboVideoRenderer::Impl::CollectFrameHooks(pl_frame& baseTarget, pl_frame& renderImage, pl_frame& target, const int width, const int height, bool cadenceRepeat, uint64_t frameGeneration, uint64_t sourceSequence, double captureRateHz, const double nominalSourceRateHz, const VideoState& state, const VideoFrame& videoFrame, const std::shared_ptr<const QueuedFrame::ConvertedPayload>& convertedPayload, HdrPeakAnalysisCrop::TrustedPicture& hdrTrustedPicture, SubtitleCutPasteGeometry& relocatedSubtitle, SubtitleBoxPadding& safeSubtitlePadding, bool subtitleCompositionSucceeded, const pl_hook* submittedNls, const bool lowLatencyQueue, CustomHookChain::TemplateValues& hookTemplates, SigmaDeltaVectors::NextVectorGuard& nextVectorGuard, pl_render_params& frameParams, pl_render_errors& renderErrors, bool& extendedRangeRelabelled, TemporalHistoryPolicy::Decision& temporalEntrance)
{
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
    if (pipelineSlots.OriginalPathway()) renderParams.output_fbo_float32 = false;   // CC W4 MUST-6: Original = Bill's output pass (libplacebo's default FBO), not rgba32f
#endif
    outputFinish.BeginFrameDraws(pipelineSlots.OriginalPathway() ? std::function<void()>() : std::function<void()>([&nextVectorGuard] { nextVectorGuard.Release(); }));   // GEM-025(b)
    frameHooks.clear();
    frameSlotHooks.clear();
    // S2 (output colour + drive): the blocks below decide; outputColour.Collect adds the hooks.
    OutputSegments::ColourFrame colourFrame;
    int sdrToneShapeState = 0;
    // Delta 13: the SDR tone shape and headroom, first at PRE_OUTPUT
    // (before the LED measurement and compensation). SDR source and
    // output only; off installs nothing.
    {
        const bool sdr = !pl_color_space_is_hdr(&renderImage.color) && !pl_color_space_is_hdr(&target.color);
        const bool wanted = SdrToneShape::Active(activeSettings.sdrTone) &&
            pipelineSlots.Runs(PipelineSlots::Id::ColourSdrToneShape);
        // audit 2026-10-16: not after a calibration 3D LUT (PRE_OUTPUT runs after the LUT; the shape
        // would bend the calibrated response with the LUT's input gamma).
        int state = !wanted ? 0 : !sdr ? 2 : target.lut ? 4 : 1;
        if (state == 1)
        {
            if (!sdrToneShapeConfigured)
            {
                ConfiguredShaderRule rule;
                rule.name = "sdr_tone_shape";
                rule.filename = "SDR tone shape.glsl";
                sdrToneShape.Configure({ rule });
                sdrToneShapeConfigured = true;
            }
            sdrToneShape.Ensure(hookTemplates);
            const pl_color_map_params* colorMap = renderParams.color_map_params ?
                renderParams.color_map_params : &pl_color_map_default_params;
            colourFrame.toneShape = true;
            colourFrame.tone.headroom = activeSettings.sdrTone.headroom;
            colourFrame.tone.kneeStart = activeSettings.sdrTone.kneeStart;
            colourFrame.tone.strength = SdrToneShape::EffectiveStrength(activeSettings.sdrTone);
            colourFrame.tone.eotf = LedDisplayGamma(target.color.transfer);
            colourFrame.tone.recovery = SdrToneShape::EffectiveRecovery(activeSettings.sdrTone, colorMap->contrast_recovery);
        }
        sdrToneShapeState = state;
    }
    auto logSdrToneShape = [&](int state)
    {
        SyncFallback(SdrToneShape::StateFallback(state));   // GEM-004: state 4 (calibration LUT) is listed
        if (state != sdrToneShapeLogged)
        {
            sdrToneShapeLogged = state;
            static const char* const names[] = { "off", "active", "off for HDR", "NOT applied (shaders\\SDR tone shape.glsl did not load)", "off with a calibration LUT (it would act after the LUT)" };
            DebugLog::Log("SDR tone shape: %s (shape %s, headroom %.2f, knee start %.2f, strength %.2f, recovery %s)",
                names[state], activeSettings.sdrTone.knee ? "knee" : "off", activeSettings.sdrTone.headroom,
                activeSettings.sdrTone.kneeStart, activeSettings.sdrTone.kneeStrength,
                activeSettings.sdrTone.recovery < 0.0 ? "auto" : std::to_string(activeSettings.sdrTone.recovery).c_str());
        }
    };
    // WP-14 (DvHostStage), phase 1: the Dolby-style curve hook (dv_tone_mapping), the L8 / L2 trims
    // decision and their parameters for the S2 segment (it collects, tags and parameterises the
    // trims hook - the one emitter), and the DV comparison capture.
    // CF-1 P3 (either/or): dv_display_mapping=original is Bill's libplacebo DV mapping, byte-identical to
    // dv_tone_mapping=libplacebo with dv_trims off; the stored dv_* values are kept, only the effective ones change.
    // CF-1 P4: the contrast-recovery need for Dolby-style DV pictures is the P14 view (never a private estimate).
    const bool dvNeedsView = activeSettings.dvDisplayMapping != "original" && activeSettings.dvToneMapping == "dolby_style" &&
        pipelineSlots.Runs(PipelineSlots::Id::ColourDmCurve);
    SourceIdentity::QualityView dvQualityView;
    if (dvNeedsView) dvQualityView = vpv.sourceAnalysis.sourceMonitor.QualityView();
    static const std::string dvOriginalTone = "libplacebo";
    // CF-1 FIX1 MUST 3: an R12B / R12L tunnel picture cannot carry VP's mapping: libplacebo's runs, listed (DmContainer.h).
    const char* const dvContainer = state.videoFrameEncoding == VideoFrameEncoding::R12B ? "R12B" :
        state.videoFrameEncoding == VideoFrameEncoding::R12L ? "R12L" : "";
    const bool dvVpMapping = dvStage.SyncContainer(state.dolbyVisionTunnel, dvContainer, activeSettings.dvDisplayMapping);
    colourFrame.dv.swap(dvStage.TrimsParameterBuffer());   // GEM-016: last frame's list capacity (PrepareToneAndTrims clears it)
    dvStage.PrepareToneAndTrims({ dvVpMapping ? activeSettings.dvToneMapping : dvOriginalTone, dvVpMapping && activeSettings.dvTrims, activeSettings.toneMapping,
        activeSettings.dvCompareCaptureFolder, pipelineSlots.Runs(PipelineSlots::Id::ColourDmCurve), renderImage, target,
        convertedPayload ? &convertedPayload->dolby : nullptr, LedDisplayGamma(target.color.transfer), state,
        convertedPayload ? Vpv::Capture::RawFrameOf(*convertedPayload) : nullptr, videoFrame.GetCounter(),
        pipelineSlots.Runs(PipelineSlots::Id::ColourDmTrims),
        dvNeedsView ? &dvQualityView : nullptr },   // VPV-HOOK
        colorMapParams, hookTemplates, colourFrame.dvTrims, colourFrame.dv);
    // CF-1 CAM-3 (colour.appearance, refs/125): the CAM16 room and screen adaptation. Off by default; off on the Original pathway.
    // The decision, hook and status live in DvHostStage (VP-V); this is the one call site. The room is the room_light product
    // (the LED intent's one sensor filter); the picture's L11 reference-mode flag and the trim that ran come from the DV metadata.
    {
        AppearanceStage::Inputs appearance;
        appearance.on = activeSettings.appearance && pipelineSlots.Runs(PipelineSlots::Id::ColourAppearance) && !pipelineSlots.OriginalPathway();
        if (appearance.on)
        {
            appearance.strength = activeSettings.appearanceStrength;
            appearance.room.room = activeSettings.appearanceRoom == "living_low" ? RoomLight::Room::LivingLow :
                activeSettings.appearanceRoom == "living_lit" ? RoomLight::Room::LivingLit :
                activeSettings.appearanceRoom == "auto" ? RoomLight::Room::Auto : RoomLight::Room::Cinema;
            appearance.room.reflectNitsPerLux = activeSettings.ambientScreenReflect;
            appearance.room.maxBlackNits = activeSettings.ambientMaxBlackNits;
            appearance.room.anchors.luxDark = activeSettings.ambientLuxDark;   // FIX1 SHOULD 3: [ambient] lux_dark / lux_lit
            appearance.room.anchors.luxLit = activeSettings.ambientLuxLit;
            appearance.recordAvailable = vpv.led.intent.roomLight.ReadLatest(appearance.record);
            appearance.now = static_cast<double>(GetTickCount64()) / 1000.0;
            appearance.screenWhiteNits = activeSettings.appearanceScreenWhiteNits;
            appearance.screenWhiteMeasured = activeSettings.appearanceScreenWhiteMeasured;
            appearance.trimApplied = dvStage.TrimsWillRun();
            appearance.trimTargetNits = dvStage.TrimsTargetNits();
            appearance.trimSaturationGain = dvStage.TrimsSaturationGain();   // FIX1 SHOULD 2: the trim x Hunt cap
            appearance.toeKnee = dynamicDrive.PublishedParams().boostKnee;   // FIX1 SHOULD 3: gamma_boost_knee, not a fixed 0
            const ExtendedRange::OutputTransfer appTrc = ExtendedRange::DescribeOutputTransfer(target.color);
            appearance.transferSupported = appTrc.supported; appearance.signalPq = appTrc.pq;
            appearance.transferKind = appTrc.kind; appearance.transferParam = appTrc.param;
            appearance.outputGamma = LedDisplayGamma(target.color.transfer);
            appearance.honourL11 = activeSettings.appearanceHonourL11;
            appearance.calibrationLutBefore = target.lut != nullptr;   // FIX1 MUST 2 (null when VP's stage took the LUT)
            const DolbyVisionTunnelFrameMetadata* dolbyMetadata = convertedPayload ? &convertedPayload->dolby : nullptr;
            appearance.l11ReferenceMode = dolbyMetadata && dolbyMetadata->valid && dolbyMetadata->hasL11 && dolbyMetadata->l11ReferenceMode;
            pl_color_space display = target.color;
            pl_color_space_infer(&display);
            // FIX1 SHOULD 1 (refs/125 s2): keyed by the SOURCE content: SDR content refers to 100 nits; HDR10, DV without a
            // trim and LLDV refer to the configured sdr_target_nits (the trim target comes first when a trim ran).
            appearance.sourceSdr = !pl_color_space_is_hdr(&renderImage.color);
            appearance.sdrTargetNits = activeSettings.sdrTargetNits;
            if (const pl_raw_primaries* prim = pl_raw_primaries_get(display.primaries))
            {
                const double red[2] = { prim->red.x, prim->red.y }, green[2] = { prim->green.x, prim->green.y };
                const double blue[2] = { prim->blue.x, prim->blue.y }, white[2] = { prim->white.x, prim->white.y };
                DvMath::RgbToXyz(red, green, blue, white, appearance.rgbToXyz);   // false: the BT.709 default stays
            }
        }
        const AppearanceStage::Decision& decision = dvStage.PrepareAppearance(appearance, hookTemplates,
            vpv.motion.intent.intentInterpreter ? vpv.motion.intent.intentInterpreter->DisplayedCuts() : 0);
        colourFrame.appearance = decision.active;
        colourFrame.appearanceParams = decision.parameters;
        colourFrame.appearanceChain = &dvStage.AppearanceHook();
    }
    // Rendered-frame captures (DV comparison capture; the WP-17 diagnostics adapter is the one
    // caller). The context is built only while one is on (it consumes the trim selection).
    captures.Configure(DvCompareCapture::kCommand, activeSettings.dvCompareCaptureFolder);
    if (captures.AnyEnabled())
    {
        const DvTrimSelection::Selection* trimsSelection = dvStage.TakeTrimsSelection();
        captures.OnFrameRendered({ &state, convertedPayload ? Vpv::Capture::RawFrameOf(*convertedPayload) : nullptr, videoFrame.GetCounter(),   // VPV-HOOK
            convertedPayload && state.dolbyVisionTunnel ? &convertedPayload->dolby : nullptr, activeSettings.dvTrims,
            trimsSelection, dvStage.TrimsTargetNits() });
    }
    // Automatic calibration phase 2 (HDR verify): VP's own tone curve (PQ targets).
    calibrationHost.NoteToneCurve(renderImage.color, target.color, renderParams.color_map_params);
    // S2 Dynamic Drive (WP-15 host adapter): gain in and this frame's LED decisions for the S2
    // segment, the only emitter of the LED hooks (DynamicDriveFrame::DecideFrame; merge audit C1).
    LedDrive::DynamicDriveFrameInputs led;
    {
        led.image = &renderImage;
        led.target = &target;
        led.screen = &baseTarget;
        led.params = &renderParams;
        led.hookTemplates = &hookTemplates;
        led.sourceRange = &activeSettings.sourceRange;
        led.displayGamma = LedDisplayGamma(target.color.transfer);
        led.appearanceActive = dvStage.AppearanceDecision().active;   // FIX1 MUST 6: appearance replaces the gamma boost
        led.captureRateHz = captureRateHz;
        led.nominalSourceRateHz = nominalSourceRateHz;
        led.renderAheadActive = RenderAheadActive() && renderAheadSchedule.Active();
        led.renderAheadNextVsyncSeconds = led.renderAheadActive ? renderAheadSchedule.NextVsync() : 0.0;
        led.sourceSequence = sourceSequence;
        led.trustedPicture = &hdrTrustedPicture;
        led.subtitle = &subtitleBoxResult;
        led.sourceWidth = width;
        led.sourceHeight = height;
        led.subtitlePadding = safeSubtitlePadding;
        led.subtitleComposed = subtitleCompositionSucceeded;
        led.relocatedSubtitle = &relocatedSubtitle;
        led.nlsSubmitted = submittedNls != nullptr;
        led.nlsMapping = [this] { return KnownNlsMapping(); };
        led.grainMap = adaptiveDownscale.Active() && adaptiveDownscale.LastMapState() ==
            AdaptiveDownscale::MapState::Used ? adaptiveDownscale.GrainMap() : nullptr;
        led.grainFloor = static_cast<float>(activeSettings.adaptive.grainFloor);
        led.measureSlotRuns = pipelineSlots.Runs(PipelineSlots::Id::DriveLedMeasure);
        led.compensationSlotRuns = pipelineSlots.Runs(PipelineSlots::Id::DriveLedCompensation);
        {   // R3 (resync_event on): FULL = look-ahead restart, gain held, CNTR to baseline; timing = period smoothing
            const VpIndicator::CounterMask ledResync = VpResync::TakeIfDriving(resync, resyncDrive, VpResync::Reader::LedDrive);
            dynamicDrive.OnResync((ledResync & VpIndicator::Bit(VpIndicator::kFull)) != 0,
                (ledResync & VpIndicator::Bit(VpIndicator::kTiming)) != 0, sourceSequence, captureRateHz);
        }
        dynamicDrive.DecideFrame(led, colourFrame);
    }
    // The user's shaders (the S2 edge) and VP's calibration LUT stage; then S2 adds this
    // frame's hooks in the segment's order, exactly as the separate blocks did.
    userFrameHooks.clear();
    customHooks.Collect(lowLatencyQueue, userFrameHooks);
    colourFrame.userPreOutput = &userFrameHooks;
    colourFrame.calibrationLut = calibrationHost.HookForFrame() != nullptr;   // S2 pushes it (WP-12: the adapter never does)
    dvStage.SyncCalibrationLut(activeSettings.calibrationLutStageConfigured, activeSettings.calibrationLutStage, colourFrame.calibrationLut);   // FIX1 MUST 3
    // CF1-FIN SHOULD A: auto keeps the VP stage only while a step that must follow the LUT wants to act (appearance on with a
    // strength, or a DV picture for VP's display mapping); the next frame's TakeLut reads it (AutoKeepsVpStage).
    calibrationHost.SetAutoNeed(activeSettings.calibrationLutStageConfigured == "auto",
        (activeSettings.appearance && activeSettings.appearanceStrength > 0.0 && pipelineSlots.Runs(PipelineSlots::Id::ColourAppearance) &&
            !pipelineSlots.OriginalPathway()) || dvStage.DisplayMappingWantsLutAfter());
    const OutputSegments::ColourCounts colourCounts = outputColour.Collect(colourFrame, pipelineSlots, frameHooks,
        [this](size_t start, PipelineSlots::Id slot) { TagSlotHooks(start, slot); });
    logSdrToneShape(sdrToneShapeState == 1 && colourCounts.toneShape == 0 ? 3 : sdrToneShapeState);
    dvStage.FinishTrims(colourCounts.dvTrims);   // WP-14 phase 2: state, status, fallback, logs
    colourFrame.dv.swap(dvStage.TrimsParameterBuffer());   // GEM-016: the list back to its owner for the next frame
    dvStage.FinishAppearance(colourCounts.appearance);   // CF-1 CAM-3: shader-missing fallback and the status line
    SyncFallback(colourCounts.unhandled ? FallbackCatalog::Engaged("S2 placeholder stage",
        "a placeholder slot has a component but no output-segment stage yet (not run)") :
        FallbackCatalog::Released("S2 placeholder stage"));
    dynamicDrive.AfterCollect(led, colourCounts.ledCompensation, colourCounts.ledCompensationLoaded);
    // A cadence repeat shows the previous picture again: the sigma-delta skips its motion search (zero motion; owner 2026-10-13 efficiency).
    for (const pl_hook* hook : frameHooks)
        CustomHookChain::SetHookParameter(hook, "static_refresh", cadenceRepeat ? 1.0 : 0.0);
    ChromaUpscale::ApplyCflHookOffsets(renderImage, frameHooks, requestedShaderSelection, pipelineSlots.OriginalPathway());
    // Stage 8-2 / 8b (scaling.output): the adaptive blend's measure and blend hooks, its bitrate weighting,
    // residual motion and grain field (VP-V/modules/scaling/output/AdaptiveDownscaleStage.h; WP-13).
    {
        AdaptiveDownscaleStage::Frame adaptiveFrame;
        adaptiveFrame.adaptiveDownscale = activeSettings.adaptiveDownscale;
        adaptiveFrame.adaptiveUpscale = activeSettings.adaptiveUpscale;
        adaptiveFrame.upscaleWBase = activeSettings.adaptiveUpscaleWBase;
        adaptiveFrame.upscaleSuperresScale = activeSettings.adaptiveUpscaleSuperresScale;
        adaptiveFrame.adaptive = &activeSettings.adaptive;
        adaptiveFrame.lowBitrateOff = activeSettings.lowBitrateHandling == "off";
        adaptiveFrame.lowBitrateOn = activeSettings.lowBitrateHandling == "on";
        adaptiveFrame.ssimBitrateAware = activeSettings.ssimBitrateAware;
        adaptiveFrame.ssimBitrateFloor = activeSettings.ssimBitrateFloor;
        adaptiveFrame.fboFloat32 = activeSettings.fboFloat32;
        adaptiveFrame.sourceSequence = sourceSequence;
        adaptiveFrame.nominalSourceRateHz = nominalSourceRateHz;
        adaptiveFrame.sourceCrop = renderImage.crop;
        adaptiveFrame.targetCrop = target.crop;
        adaptiveFrame.rebaseFactor = rebaseFactor;
        adaptiveFrame.rebaseFactorX = rebaseFactorX;
        adaptiveFrame.nlsSubmitted = submittedNls != nullptr;
        adaptiveFrame.nlsFile = nlsRule.filename.c_str();
        adaptiveFrame.slotRuns = pipelineSlots.Runs(PipelineSlots::Id::ScalingOutput);
        adaptiveFrame.vectorsRun = MotionVectorsRun();   // motion.estimation / vector_delivery off: no grain-map estimate
        AdaptiveDownscaleStage::Host adaptiveHost;
        adaptiveHost.fallbackFramePeriod = [this]() { return AdaptiveFallbackFramePeriod(); };
        adaptiveHost.bitrate = [this](double& quality) {
            const SourceResolutionMonitor::Snapshot snap = vpv.sourceAnalysis.sourceMonitor.Read();
            quality = vpv.sourceAnalysis.sourceMonitor.SharedQuality(snap, SourceIdentity::QualityConsumer::AdaptiveScaling);   // U12: the shared view (consensus off: the classical value)
            return snap.reading;
        };
        adaptiveHost.nlsMapping = [this]() { return KnownNlsMapping(); };
        adaptiveHost.ssimInChain = [&]() { return customHooks.SubmitsFile("SSimDownscaler.glsl", lowLatencyQueue); };
        adaptiveHost.motion = [&]() {
            AdaptiveDownscaleStage::MotionSource m;
            const VpMotion::P3FieldSet live = vectorEstimate.LiveFor(sourceSequence, activeSettings.motionHistoryFixes);   // P5c: R1   // VPV-HOOK
            m.live = live.valid && !VpPhaseDrawn(renderImage);   // VPV-HOOK
            if (m.live) { m.field = live.field; m.frameTrust = live.frameTrust; m.grainError = live.grainError; m.minTrustedShare = vectorEstimate.MinTrustedShare(); }   // VPV-HOOK
            return m;
        };
        adaptiveHost.publishStatus = [this]() { PublishCustomShaderStatus(); };
        adaptiveHost.isPrescaler = [this](const pl_hook* hook) { return customHooks.IsPrescalerHook(hook); };
        adaptiveStage.Register(adaptiveFrame, adaptiveHost,
            { bitrateWeightScale, adaptiveGrainMapWanted, adaptiveNoSsimLogged }, frameHooks);
        // ADR-0001 (findings 2026-10-25 #1): SSimDownscaler missing or a GPU failure is visible.
        const FallbackCatalog::Decision scalingFallback = adaptiveStage.Fallback();
        if (scalingFallback.engaged) EngageFallback(scalingFallback.id.c_str(), scalingFallback.reason);
        else ReleaseFallback(scalingFallback.id.c_str());
    }
    // output.dither switched off (pipeline_slots_off): no quantiser hook, libplacebo's dither.
    if (!pipelineSlots.Runs(PipelineSlots::Id::OutputDither))
        DitherSlot::RemoveQuantisers(frameHooks, [&](const pl_hook* h) { return customHooks.IsQuantizerHook(h); });
    // Delta 5 (D5-1): the sigma-delta's code domain comes from this
    // frame's target, never from the shader rule: range, swapchain sample
    // depth and the colour depth it quantises to. A float or 16-bit
    // target is not an 8/10-bit code domain: the sigma-delta is left out
    // of that frame (audit delta5-beta.md).
    // Audit 2026-10-16 (bit depth): rgba32f output FBO support cached per (host, gpu, gpuEpoch) in CoreSegmentCaches.
    bool outputFloat32 = false;
#if defined(PL_RENDER_PARAMS_OUTPUT_FBO_FLOAT32)
    outputFloat32 = renderParams.output_fbo_float32 && d3d11 && Vpv::Segments::CoreSegmentCaches::Instance().GpuSupportsFloat32Fbo(this, d3d11->gpu, [](const void* g) { const pl_fmt f = pl_find_fmt(static_cast<pl_gpu>(g), PL_FMT_FLOAT, 4, 32, 0, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE)); return f != nullptr && f->component_depth[0] >= 32; });
#endif
    SigmaDeltaDomain::Domain ditherDomain; OutputSegments::DitherPrep ditherPrep; bool ditherAny = false;
    {
        ditherDomain = SigmaDeltaDomain::FromTarget(target.repr);
        outputFinish.BeginFrame();   // S3 (output finish), step 1: the dither's code domain
        ditherPrep = outputFinish.PrepareDither(frameHooks, ditherDomain, outputFloat32, pipelineSlots);
        ditherAny = ditherPrep.found && !ditherPrep.slotOff;
        sigmaDeltaRanLast = ditherAny && ditherDomain.supported && !ditherPrep.domainDropped;   // vector engine: the output consumer's want
        const std::string domainLog = OutputSegments::OutputFinishSegment::DescribeDitherDomainChange(ditherDomain, ditherAny, sigmaDeltaDomainLogged);
        if (!domainLog.empty())
            DebugLog::Log("%s", domainLog.c_str());
        // Silent-fallbacks batch: the sigma-delta asked for but not running is visible (ADR-0001).
        SyncFallback(OutputSegments::OutputFinishSegment::DitherFallback(ditherPrep, ditherDomain, customHooks.QuantizerProblem()));
    }
    // The dither is the last processing of every picture: if a configured
    // shader would run after the sigma-delta at the OUTPUT stage, the
    // sigma-delta is moved to the end (logged once). The occlusion mask
    // and the encoding repair are placed relative to it below.
    {
        // S3 step 2.
        if (outputFinish.KeepDitherLast(frameHooks, pl_color_levels_guess(&target.repr) == PL_COLOR_LEVELS_FULL))
        {
            if (!sigmaDeltaReorderLogged)
                DebugLog::Log("Sigma-delta: another OUTPUT-stage shader was configured after it; the sigma-delta now runs last");
            sigmaDeltaReorderLogged = true;
        }
    }
    if (submittedNls)
        frameHooks.push_back(submittedNls);
    // Stage 6c: the occlusion mask for this source frame's first refresh,
    // into alpha just before the sigma-delta (which must take it: its
    // `occlusion_mask` parameter). Not on cadence repeats (the same
    // picture again) and not with NLS (a non-linear output mapping).
    // S1: the grain map (Stage 8, the previous source frame's: this
    // render's map pass writes the other buffer) for the sigma-delta's
    // tolerance, on every present while the map is in use.
    pl_tex grainMap = activeSettings.ditherGrainToleranceScale > 1.0 && adaptiveDownscale.Active() &&
        adaptiveDownscale.LastMapState() == AdaptiveDownscale::MapState::Used ? adaptiveDownscale.GrainMap() : nullptr;
    if (activeSettings.occlusionMask || dejudderMaskParamSet || grainMap || grainToleranceParamSet)
    {
        int maskSlot = -1;
        for (int slot = 0; slot < 2; ++slot)
            if (dejudderExtras.dejudderMaskSequence[slot] == sourceSequence && sourceSequence != 0 && motionValidity.Current(dejudderExtras.dejudderMaskEpoch[slot]))
                maskSlot = slot;
        // Stage 12: both map through the native source crop by relative
        // position, so they hold at any re-base size (the vectors are the
        // native frame's; a factor change does not touch them).
        // (A picture between frames is not A: A's mask would sit t * v off.)
        bool install = activeSettings.occlusionMask && maskSlot >= 0 && dejudder && !cadenceRepeat && !VpPhaseDrawn(renderImage) &&
            vectorEstimate.OcclusionOnVp(pipelineSlots.OriginalPathway(), maskSlot);   // VPV-HOOK (VPV-GATED)
        bool grainInstall = grainMap != nullptr;
        // 6c-2: with NLS the mask follows the NLS shader's own map. Only
        // the bundled NLS / NLS+ maps are known; any other NLS shader, or
        // a rule missing a parameter, keeps the mask off (logged once).
        NlsMaskMapping::Mapping nlsMapping;
        if ((install || grainInstall) && submittedNls)
        {
            nlsMapping = KnownNlsMapping();
            if (!nlsMapping.valid)
            {
                if (install && !dejudderMaskNlsLogged)
                    DebugLog::Log("De-judder occlusion mask: not used with NLS shader '%s' (its map is not known)",
                        nlsRule.filename.c_str());
                if (install)
                    dejudderMaskNlsLogged = true;
                install = false;
                grainInstall = false;
            }
        }
        if (outputFinish.OcclusionMaskDisabled()) { install = false; grainInstall = false; }   // GEM-025(a): libplacebo disabled the mask hook
        // S3 step 3: the mask's parameters and its place just before the dither.
        const size_t sigmaIndex = outputFinish.MarkMask(frameHooks, install, grainInstall,
            activeSettings.ditherGrainToleranceScale);
        if (sigmaIndex == frameHooks.size())
            grainInstall = false;
        grainToleranceParamSet = grainInstall;
        if (install && sigmaIndex == frameHooks.size())
        {
            install = false;
            if (!dejudderMaskNoSigmaLogged)
                DebugLog::Log("De-judder occlusion mask: on, but no sigma-delta in the shader chain takes it");
            dejudderMaskNoSigmaLogged = true;
            dejudderMaskState = "mask off (no sigma-delta)";
        }
        else if (activeSettings.occlusionMask && maskSlot >= 0 && submittedNls && !nlsMapping.valid)
            dejudderMaskState = "mask off (NLS shader not known)";
        else if (!submittedNls && dejudderMaskState == "mask off (NLS shader not known)")
            dejudderMaskState.clear();   // NLS is off again (audit 6c-2)
        // Set only when the parameter was really written as 1 (audit 6b-2b).
        dejudderMaskParamSet = install;
        if (install || grainInstall)
        {
            occlusionMaskHook.SetFrame(install ? vectorEstimate.OcclusionOnVp(pipelineSlots.OriginalPathway(), maskSlot) : nullptr, renderImage.crop, target.crop);   // VPV-HOOK (VPV-GATED)
            occlusionMaskHook.SetGrain(grainInstall ? grainMap : nullptr,
                static_cast<float>(activeSettings.adaptive.grainFloor));
            // NLS's coordinates span the source crop (measured: the
            // MAIN-stage texture libplacebo hooks is the cropped picture).
            if (submittedNls)
                occlusionMaskHook.SetNls(nlsMapping, renderImage.crop);
            outputFinish.InsertMask(frameHooks, sigmaIndex);
        }
        if (install)
        {
            dejudderExtras.dejudderMaskSequence[maskSlot] = 0;   // used: not again for a repeat
            dejudderMaskUsedTick = GetTickCount64();
            dejudderMaskState.clear();
            static GeneralRate::LogInterval maskLog = GeneralRate::LogInterval::ForFrames(2400);   // fixed-constant batch B7 2026-10-18: seconds, not frames
            if (maskLog.DueOr(++dejudderMaskFrames == 1))
                DebugLog::Log("De-judder occlusion mask: used on %llu source frames (hook applied %llu, failed %llu)",
                    dejudderMaskFrames, occlusionMaskHook.Applied(), occlusionMaskHook.Failed());
        }
    }
    // Stage 10: BTB/WTW through libplacebo for SDR. Decided on this
    // frame's source and target; libplacebo is then told the source is in
    // the target's colour space and VP's hooks do the conversion.
    // Hunt 2 (08 #1): true when Prepare relabelled renderImage.color; the
    // temporal renderer's retained frames then get the same label, so no
    // frame is converted twice (TemporalRenderer::Render historyColor).
    extendedRangeRelabelled = false;
    {
        ExtendedRangeStage::Frame extendedFrame;
        extendedFrame.preserve = activeSettings.preserveExtendedRange;
        extendedFrame.moreCases = activeSettings.extendedRangeMoreCases;
        extendedFrame.hdrDynamicToneMapping = renderParams.peak_detect_params != nullptr;   // audit 2026-10-16
        extendedFrame.temporalActive = temporalRenderer.Active();
        extendedFrame.temporalMixing = temporalRenderer.Mixing();
        extendedFrame.singlePlaneRgb = renderImage.num_planes == 1;
        extendedFrame.targetLut = target.lut;
        extendedFrame.targetLutType = target.lut ? target.lut_type : PL_LUT_UNKNOWN;
        extendedFrame.slotRuns = pipelineSlots.Runs(PipelineSlots::Id::ColourExtendedRange);
        extendedRangeRelabelled = extendedRange.Prepare(extendedFrame, renderImage.color, target.color, frameHooks);
        // Efficiency audit 2026-10-24 #7 / #8b (WP-5), only with efficiency_fixes (off: the original two passes,
        // textures kept): one-pass carry; ER's textures freed after it has been idle. Through the stage's hooks
        // (CarryHooks); group D integration: to fold into ExtendedRangeStage::Prepare (m2-output doc).
        extendedRangeHooks.SetOnePassAllowed(activeSettings.efficiencyFixes);
        if (activeSettings.efficiencyFixes && extendedRangeIdle.Note(extendedRange.LastDecision().Active(),
            extendedRangeHooks.HoldsTextures(), std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count()))
            extendedRangeHooks.Release();
        // #7: one pass wanted but two passes run (same output, one extra pass): listed with its reason.
        if (extendedRangeHooks.CarryFallbackEngaged())
            EngageFallback("extended range one-pass carry", ExtendedRange::CarryPassReasonText(extendedRangeHooks.CarryPasses()));
        else
            ReleaseFallback("extended range one-pass carry");
        // Silent-fallbacks batch (WP-4): BTB/WTW clipping is visible; the stage's Fallback is the same mapping.
        SyncFallback(extendedRange.Fallback(activeSettings.preserveExtendedRange));
    }
    // Stage 6d (motion.consumers): the de-judder rendered-picture cache and the extras' geometry, at today's position
    // (DejudderCacheStage.h; WP-13). In place until P4 moves the hook block (module README open item).
    RegisterDejudderCacheFrame(target, renderImage, relocatedSubtitle, subtitleCompositionSucceeded, submittedNls != nullptr,   // VPV-HOOK M2 P5b-6
        cadenceRepeat, sourceSequence);
    // WP-24 repeat reuse, call 1 of 3: decide this refresh; arm the cache for a first present the
    // queue holds a repeat of (before the encoding-repair check, which looks at PRE_OUTPUT hooks).
    repeatReuse.SetOptions({ activeSettings.repeatRenderReuse, pipelineSlots.OriginalPathway(),
        pipelineSlots.Runs(PipelineSlots::Id::OutputPresentation) });
    if (repeatReuse.Active())
    {
        repeatReuse.SetContext(RepeatReuseContext(renderImage, target));
        repeatReuse.PrepareFirstPresent(RepeatRenderReuse::KeyFrom(sourceSequence, cadenceRepeat, repeatNextQueued,
            frameGeneration, activeSettingsGeneration, renderImage, target, renderParams, DisplayLutSignature()), frameHooks);
    }
    // libplacebo 7.360.1 drops the output encoding (limited range) after
    // a texture-input PRE_OUTPUT hook; the repair runs first at OUTPUT,
    // before any OUTPUT-stage shader such as the sigma-delta.
    renderErrors = pl_renderer_get_errors(renderer);
    // S3 step 4.
    if (outputFinish.Repair(frameHooks, &renderErrors, target.lut ? target.lut_type : PL_LUT_UNKNOWN, target.repr))
    {
        if (!outputEncodingRepairLogged)
            DebugLog::Log("libplacebo output encoding repair active (PRE_OUTPUT hook in the chain; target levels=%d)",
                static_cast<int>(target.repr.levels));
        outputEncodingRepairLogged = true;
    }
    frameParams = renderParams;
    // CF1-FIN SHOULD B: saturation is replaced only while appearance is ACTIVE this frame. PostLoad forced the step off for a
    // configured appearance; a frame where the pass is not active (a calibration LUT before it) gets the user's saturation
    // back, and the Fallbacks row follows the frame. Never on the Original pathway (saturationReplaced is false there).
    const bool saturationGivenBack = activeSettings.saturationReplaced && !dvStage.AppearanceDecision().active;
    SyncFallback(FallbackCatalog::SaturationReplaced(activeSettings.saturationReplaced && !saturationGivenBack,
        activeSettings.saturationConfigured));   // VPV-FALLBACK saturation: replaced by appearance
    // CF1-G7b: ambient_adapt likewise, per frame: replaced only while appearance acts (the intent interpreter's
    // ConfigureIntentAmbient gives it back on the other frames, AppearanceStage::AmbientAdaptOn).
    SyncFallback(FallbackCatalog::AmbientAdaptReplaced(activeSettings.ambientAdaptReplaced &&
        !AppearanceStage::AmbientAdaptGivenBack(activeSettings.ambientAdaptReplaced, dvStage.AppearanceDecision().active)));   // VPV-FALLBACK ambient_adapt: replaced by appearance
    // Gate G1 MUST-5: ambient_adapt given back, but the LED intent evaluation follows the saved setting: listed (Ctrl+I,
    // Config status, problem report), never silent.
    SyncFallback(FallbackCatalog::AmbientAdaptLedShadowCapSaved(AppearanceStage::LedShadowCapUsesSavedAmbient(
        AppearanceStage::AmbientAdaptGivenBack(activeSettings.ambientAdaptReplaced, dvStage.AppearanceDecision().active),
        activeSettings.ambientAdapt, activeSettings.intentBlack != BlackIntentMode::Off, activeSettings.intentDisplay,
        vpv.led.intent.ledIntentLimitsSent.shadowMinimumShare, vpv.led.intent.ledIntentLimitsSent.ambientBlackNits)));   // VPV-FALLBACK room-light: LED shadow cap
    if (saturationGivenBack || vpv.led.sdrLift.ambientSdrLift)
    {
        frameColorAdjustment = renderParams.color_adjustment ? *renderParams.color_adjustment : pl_color_adjustment_neutral;
        if (saturationGivenBack)
            frameColorAdjustment.saturation = 1.0f + static_cast<float>(activeSettings.saturationConfigured) / 100.0f;
        frameParams.color_adjustment = &frameColorAdjustment;
    }
    // Hunt 3 F2: ambient adaptation's SDR black lift, on top of the configured adjustment
    // (saturation): libplacebo's colour decode, before anything else sees the picture.
    if (vpv.led.sdrLift.ambientSdrLift)
    {
        LibplaceboRenderParameters::ApplySdrAmbientLift(vpv.led.sdrLift.ambientSdrContrast, vpv.led.sdrLift.ambientSdrBrightness, frameColorAdjustment);
        frameParams.color_adjustment = &frameColorAdjustment;
    }
    // Hunt 2 picture sweep: deband_vp_exclusive yields libplacebo's automatic deband to VP's
    // debander, which is idle while smooth motion or deinterlacing runs; those frames keep
    // libplacebo's deband as configured (otherwise nothing would deband them).
    // Silent-fallbacks batch: a FAILED VP debander is replaced by libplacebo's deband, visibly. The
    // slot switched off is a choice (no substitute); DV tunnel pictures are never substituted.
    const FallbackCatalog::DebandPlan debandPlan = FallbackCatalog::PlanDeband(debandYieldedToVp,
        pipelineSlots.Runs(PipelineSlots::Id::RepairDeband), debander.Failures() > 0,
        renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION, debander.LastFailureError());
    SyncFallback(debandPlan.fallback);
    SyncFallback(FallbackCatalog::VpDeband(activeSettings.debandVp != "off", debandYieldedToVp,
        pipelineSlots.Runs(PipelineSlots::Id::RepairDeband), debander.Failures() > 0,
        renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION, debander.LastFailureError()));
    if (debandYieldedToVp && yieldedDebandOn && (temporalRenderer.Active() || !debandPlan.libplaceboYields))
    {
        frameParams.deband_params = &yieldedDebandParams;
        if (!yieldedDebandLogged)
            DebugLog::Log("libplacebo deband: on for temporal frames (VP's debander does not run with smooth motion or deinterlacing)");
        yieldedDebandLogged = true;
    }
    else
        yieldedDebandLogged = false;
    // No libplacebo plane deband on the raw DV tunnel planes with dv_tunnel_fixes (DvHostStage, WP-14).
    if (dvStage.SkipPlaneDeband(activeSettings.dvTunnelFixes, renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION,
        frameParams.deband_params != nullptr))
        frameParams.deband_params = nullptr;
    {   // CF-DV P5 (colour.dm_pre): post-decode I-only DV debander (shaders/vp_dm_pre_deband.glsl at MAIN; zero hooks when off).
        DmPreDeband::Inputs dmIn;
        dmIn.dolbyVisionPicture = renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION;
        dmIn.slotRuns = pipelineSlots.Runs(PipelineSlots::Id::ColourDmPre);
        dmIn.originalPathway = pipelineSlots.OriginalPathway();
        dmIn.mode = activeSettings.dvPostDecodeDeband;
        if (dmIn.dolbyVisionPicture && dmIn.slotRuns && !dmIn.originalPathway && DmPreDeband::ParseMode(dmIn.mode) == DmPreDeband::Mode::Auto)
        {
            const SourceResolutionMonitor::Snapshot dmSnap = vpv.sourceAnalysis.sourceMonitor.Read();
            dmIn.tapAvailable = renderImage.num_planes > 0 && renderImage.planes[0].texture != nullptr && renderImage.repr.bits.color_depth >= 12;
            dmIn.qualityAvailable = dmSnap.reading && !dmSnap.codingUnavailable;
            dmIn.effectiveBits = dmSnap.reading ? dmSnap.effectiveBits : 12.0;
            dmIn.bitrateQuality = dmSnap.reading ? vpv.sourceAnalysis.sourceMonitor.SharedQuality(dmSnap, SourceIdentity::QualityConsumer::Deband) : 1.0;   // U12
            dmIn.snapLevels = activeSettings.sourceCodingFixes;
            dmIn.grainGuard = activeSettings.debandVpGrainGuard;
        }
        const DmPreDeband::Decision& dmDec = dvStage.PrepareDmPreDeband(dmIn, hookTemplates);
        size_t dmCollected = 0;
        if (dmDec.active)
        {
            const size_t start = frameHooks.size();
            dvStage.DmPreDebandHook().Collect(false, frameHooks);
            dmCollected = frameHooks.size() - start;
            if (dmCollected > 0) {
                TagSlotHooks(start, PipelineSlots::Id::ColourDmPre);
                const auto dmParams = DmPreDeband::ShaderParameters(dmDec, height);
                for (size_t i = start; i < frameHooks.size(); ++i)
                    for (const auto& kv : dmParams) CustomHookChain::SetHookParameter(frameHooks[i], kv.first, kv.second);
            }
        }
        dvStage.FinishDmPreDeband(dmCollected);
    }
    // Stage 8-1: a downscaling frame in gamma light when chosen (the de-
    // judder extras reuse these parameters).
    // 8b: both branches in one light domain (AdaptiveDownscaleStage::ApplyLight; WP-13).
    adaptiveStage.ApplyLight(frameParams, { activeSettings.downscaleLight == "gamma",
        activeSettings.upscaleLight == "gamma", renderImage.crop, target.crop,
        pipelineSlots.Runs(PipelineSlots::Id::ScalingOutput), rebaseFactor, rebaseFactorX, !pipelineSlots.OriginalPathway() && activeSettings.downscaler == "auto" && activeSettings.quality != "fast" });
    OrderFrameSlotHooks();   // the registry's order for slot hooks (unchanged for the designed order)
    frameParams.hooks = frameHooks.empty() ? nullptr : frameHooks.data();
    frameParams.num_hooks = static_cast<int>(frameHooks.size());
    // Efficiency (owner 2026-10-14): B's own first present takes the
    // sigma-delta's vectors made from A's field at the end of A's extras
    // (same sequence and geometry only); restored after this render
    // (SigmaDeltaVectors::NextVectorGuard, output.dither; WP-13).
    if (dejudderExtras.dejudderNextVectorsSequence != 0 && dejudderExtras.dejudderNextVectorsSequence == sourceSequence && !cadenceRepeat &&
        dejudderExtras.dejudderNextVectors && dejudderExtras.dejudderGeometryValid && SameGeometry(dejudderExtras.dejudderGeometry, dejudderExtras.dejudderNextVectorsGeometry) &&
        dejudderExtras.ditherNextGate.CheckAt(motionValidity, sourceSequence) &&
        VpResync::CarryValid(resync, dejudderExtras.dejudderNextVectorsFrom, sourceSequence))   // R3: no carry across a FULL, slip or re-base change
    {
        if (nextVectorGuard.Apply(frameParams.hooks, frameParams.num_hooks, dejudderExtras.dejudderNextVectors))
        {
            ++dejudderExtras.ditherVectorsNextUsed;
            if (!dejudderExtras.ditherVectorsNextLogged)
            {
                dejudderExtras.ditherVectorsNextLogged = true;
                DebugLog::Log("Sigma-delta vectors on the new frame's present: the de-judder's (own search skipped)");
            }
        }
    }
    dejudderExtras.dejudderNextVectorsSequence = 0;   // used (or not) once
    dejudderExtras.dejudderNextVectorsFrom = 0;
    VpResync::TakeIfDriving(resync, resyncDrive, VpResync::Reader::SigmaDelta);   // R3: checked by stamps above
    dejudderExtras.ditherNextGate.Clear();
    // W23-4p (G-T1): live dither_need GPU block pass + HookGuard bind (default off; never Original; NLS guarded).
    {
        OutputSegments::DitherNeedGpuRequest dnReq;
        SourceIdentity::QualityView dnQuality;
        if (activeSettings.ditherNeed == "auto" && !pipelineSlots.OriginalPathway()) {
            dnQuality = vpv.sourceAnalysis.sourceMonitor.QualityView();
            OutputSegments::OutputFinishSegment::PopulateDitherNeedRequest(d3d11 ? d3d11->gpu : nullptr, this, sourceSequence,
                ditherAny, ditherPrep.slotOff, ditherDomain.supported, ditherDomain.color, submittedNls != nullptr, &dnQuality,
                activeSettings.sdrTargetNits, activeSettings.sdrBlackNits, activeSettings.hdrToneMapTargetGamma.c_str(),
                activeSettings.toneMapping.c_str(), activeSettings.quality.c_str(), activeSettings.toneTuning, activeSettings.sdrTone,
                activeSettings.sdrInputTransfer.c_str(), activeSettings.outputGamma.c_str(), activeSettings.hasContrastRecovery,
                static_cast<double>(activeSettings.contrastRecovery), renderImage, target, captureRateHz, nominalSourceRateHz, customHooks, dnReq);
        } else dnReq.originalPathway = pipelineSlots.OriginalPathway();
        const DitherNeedSeam::Outcome need = outputFinish.RunDitherNeedGpu(dnReq, frameParams.hooks, frameParams.num_hooks,
            [&nextVectorGuard](std::function<void()> fn) { nextVectorGuard.AttachRelease(std::move(fn)); });
        SyncFallback(need.fallback);
        if (outputFinish.DitherNeedStatusChanged(need.status) && !need.status.empty())
            DebugLog::Log("%s", outputFinish.FormatDitherNeedLog(need.status).c_str());
    }
    // S-12BIT (refs/119 #51, #30): libplacebo's latched error flags (FBO, sampling, debanding, ...) and the float16 picture precision
    // are visible, and released in the Original pathway (a stale entry is a silent state in reverse). Here, not in ReportRenderErrors:
    // that body is a pure move that never runs in Original. The flags are renderErrors (read above from the renderer: the previous frame's latch).
    static_assert(LibplaceboErrors::kFbo == PL_RENDER_ERR_FBO && LibplaceboErrors::kHooks == PL_RENDER_ERR_HOOKS &&
        LibplaceboErrors::kBlur == PL_RENDER_ERR_BLUR && LibplaceboErrors::kContrastRecovery == PL_RENDER_ERR_CONTRAST_RECOVERY,
        "LibplaceboErrors bit values must mirror enum pl_render_error");
    SyncFallback(LibplaceboErrors::Latched(static_cast<int>(renderErrors.errors), pipelineSlots.OriginalPathway()));
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
    constexpr bool laneHasFloat32Fbo = true;   // Beta lane (API 388)
#else
    constexpr bool laneHasFloat32Fbo = false;  // Stable 7.360: no float32 intermediate switch
#endif
    SyncFallback(PicturePrecision::Precision(pipelineSlots.OriginalPathway(), laneHasFloat32Fbo, activeSettings.fboFloat32));
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
    // S2: every FBO in rgba32f (exact highlights; +bandwidth). Auto: on everywhere, the
    // low-latency path too (owner 2026-10-16: higher precision is the default; half-float
    // there was never agreed). Off by hand only.
    frameParams.fbo_float32 = PicturePrecision::FboFloat32(activeSettings.fboFloat32);   // S-12BIT: the setting alone, not the scaling.output slot
#endif
    // Only a quantiser submitted with this frame replaces libplacebo's dither (audit 2026-10-16:
    // a sigma-delta left out of the frame for its domain still cleared the dither).
    const bool quantiserInFrame = std::any_of(frameHooks.begin(), frameHooks.end(),
        [&](const pl_hook* h) { return customHooks.IsQuantizerHook(h); });
    if (DitherSlot::ClearsLibplaceboDither(quantiserInFrame, activeSettings.temporalDither == "off",
        pipelineSlots.Runs(PipelineSlots::Id::OutputDither)))
    {
        frameParams.dither_params = nullptr;
        frameParams.error_diffusion = nullptr;
    }
    // Bug hunt 2026-10-17: the retry withdraws the failed NLS hook from
    // frameHooks itself (a member), so frameParams - read again by the
    // de-judder extras - never points at a list local to the recovery.
    // output_segments_merged: the segments' checks rerun only when the module order or switches
    // change (a cheap signature, no string building per frame).
    if (!outputMerger)
        outputMerger = std::make_unique<OutputSegments::Merger>(d3d11->gpu, outputColour, outputFinish);
    outputMerger->Configure(pipelineSlots, activeSettings.outputSegmentsMerged, 30, fallbacks);
    if (outputMerger->FallbacksChanged())
        DebugLog::Log("%s", fallbacks.Heading().c_str());
    temporalEntrance = TemporalHistoryPolicy::Decide(
        temporalRenderer.Active(), !pipelineSlots.ActiveEntrance().empty());
    if (temporalEntrance.showPause)
        EngageFallback(TemporalHistoryPolicy::FallbackId(), TemporalHistoryPolicy::PauseText());
    else
        ReleaseFallback(TemporalHistoryPolicy::FallbackId());
    if (!pipelineSlots.OriginalPathway())   // Original: no recorder, not even its environment read
        TraceFrameHookList(this, frameParams, frameSlotHooks, customHooks, submittedNls);   // P4 f test recorder (VP_HOOK_LIST_TRACE), after the moved body
}
