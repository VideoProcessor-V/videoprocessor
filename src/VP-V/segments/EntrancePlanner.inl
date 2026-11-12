// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P4 e-i (refs/110 s1.2): the entrance planner, moved VERBATIM out of LibplaceboVideoRenderer.cpp (RenderLocked): the re-base factor
// for this frame (Stage 12 step A, with the native source monitor of step B, the kernel match, the failure pause, the FX-RB warm-up
// hold, the crossfade and the R4a decision product P0), VP's debander parameters (Stage 12 2b/2d) and the detail pulse /
// texture restore parameters. One out-of-line member of the renderer's Impl; the body keeps its own brace block (its local
// std::string state shadows the state parameter after the P0 lines, exactly as in the original). Inputs are the same-name
// references; the outputs are Impl members only (rebaseFactor/X, rebaseFade, rebaseFrom*, debandThreshold, vpDebandParams,
// detailPulseParams, textureRestoreParams, the *Runs / *Inactive / *Logged strings, rebaseP0Status, detailPulseAheadSequence).
// Included ONCE by LibplaceboVideoRenderer.cpp right after the Impl struct; not a stand-alone header. Render thread (renderMutex
// held), once per picture, directly after RendererExtension::NotifyEntrance, as before.
//
// Entrance stage order (named positions; a declaration only, no behaviour): re-base -> repair stack (deband, detail pulse,
// texture restore) -> TdApply. TdApply is the reserved position where the temporal-detail stage applies td.delta (Products.h
// kTdDelta / kTdMaps): every TD method writes td.delta, this slot applies it, and the slot never moves.
#pragma once

void LibplaceboVideoRenderer::Impl::PlanEntrance(pl_frame& renderImage, pl_frame& target, const pl_frame& image, uint64_t sourceSequence,
	double captureRateHz, double nominalSourceRateHz, const VideoState& state)
{
// Stage 12 step A: the re-base factor for this frame. Geometry above
// stays in native pixels; the factor applies where libplacebo samples
// the source (the render call, the de-judder's source-warp extras) and
// where the scaling gates compare source and destination heights.
{
	const pl_tex luma = renderImage.num_planes > 0 ? renderImage.planes[0].texture : nullptr;
	const int nativeHeight = luma ? luma->params.h : 0;
	// Step B: the native, unwarped, not re-based frame, once per new source frame.
	if (sourceSequence != sourceMonitorSequence && d3d11 && d3d11->gpu)
	{
		sourceMonitorSequence = sourceSequence;
		if (!sourceMonitorHistorySet)
		{
			// The auto history beside the log, one file per session, newest 5 kept (logs\ when the log path is not known here).
			std::string logPath = DebugLog::GetLogFilePath();
			if (logPath.empty()) logPath = "logs\\vp.log";
			const size_t slash = logPath.find_last_of("\\/");
			const std::string folder = slash == std::string::npos ? std::string(".") : logPath.substr(0, slash);
			CreateDirectoryA(folder.c_str(), nullptr);
			sourceMonitor.SetHistoryFolder(folder);
			sourceMonitorHistorySet = true;
		}
		if (!rebaseDispatch) rebaseDispatch = pl_dispatch_create(log, d3d11->gpu);
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		// R3 (resync_event on): the worker's copies carry the frame's stream key (copies from
		// before a FULL are dropped), and a FULL resets the evidence on the frame that carries it.
		if (resync.Driving())
		{
			const std::shared_ptr<VpIndicator::Indicator> indicator = resync.GetIndicator();
			sourceMonitor.SetStreamKey(indicator ? VpResync::Drive::FullKey(*indicator, sourceSequence) : 0);
			if ((resyncDrive.Take(VpResync::Reader::SourceMonitor) & VpIndicator::Bit(VpIndicator::kFull)) != 0)
				sourceMonitor.Discontinuity("resync (FULL)");
		}
		else if (sourceMonitor.StreamKey() != 0)
			sourceMonitor.SetStreamKey(0);
		sourceMonitor.Frame(d3d11->gpu, rebaseDispatch, renderImage, sourceSequence, now);
		if (renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION && !sourceMonitorDvLogged)
		{
			// 2026-10-17: no longer skipped as a packed signal.
			DebugLog::Log("Source resolution on Dolby Vision: measured on the decoded intensity (I) plane; a re-base "
				"resamples the IPT planes before libplacebo's Dolby Vision decode (as YCbCr planes before their matrix)");
			sourceMonitorDvLogged = true;
		}
	}
	double wanted = 1.0, wantedX = 1.0;
	sourceMonitor.SetSelection(activeSettings.sourceResolution);
	sourceMonitor.SetTextGuard(activeSettings.sourceResolutionTextGuard);
	sourceMonitor.SetMixedGuard(activeSettings.sourceResolutionMixedGuard);
	sourceMonitor.SetCodingFixes(activeSettings.sourceCodingFixes);
	sourceMonitor.SetLatchFixes(activeSettings.sourceResolutionLatchFixes);
	sourceMonitor.SetCutFixes(activeSettings.sourceResolutionCutFixes);
	sourceMonitor.SetEngageTiming(activeSettings.sourceResolutionEngageMaxWait, activeSettings.sourceResolutionCrossfade,
		activeSettings.sourceResolutionReleaseFade);
	sourceMonitor.SetEvidence(activeSettings.sourceResolutionEvidenceShots, activeSettings.sourceResolutionEvidenceSeconds);
	sourceAutoActive.store(activeSettings.sourceResolution == "auto", std::memory_order_release);
	if (activeSettings.sourceResolution == "auto")
	{
		std::string why;
		// Per axis (owner 2026-10-16): the uniform factor when engaged, else the width
		// and height factors of a source upscaled in one direction.
		sourceMonitor.AutoFactors(luma ? luma->params.w : 0, nativeHeight, activeSettings.sourceResolutionAutoMinConfidence,
			why, wantedX, wanted, -1.0, sourceSequence);
		if (!why.empty())
		{
			const SourceResolutionMonitor::Snapshot snap = sourceMonitor.Read();
			DebugLog::Log("Source resolution auto: %s; %s", why.c_str(),
				SourceResolutionMonitor::Describe(snap, sourceMonitor.AutoRatio(), true).c_str());
		}
	}
	else
	{
		wanted = SourceRebase::Factor(activeSettings.sourceResolution, nativeHeight);
		wantedX = SourceRebase::FactorX(activeSettings.sourceResolution, luma ? luma->params.w : 0, nativeHeight);
	}
	// The identified player kernel for an exact 2:1 luma re-base (logged on change).
	{
		const int identified = activeSettings.rebaseKernelMatch ? sourceMonitor.Read().kernel : -1;
		if (identified != sourceRebase.Kernel())
		{
			sourceRebase.SetKernel(identified);
			DebugLog::Log("Re-base kernel: %s", identified >= 0 ?
				(KernelInverse::Candidates()[static_cast<size_t>(identified)].name + " identified: its inverse for 2:1 luma").c_str() :
				(activeSettings.rebaseKernelMatch ? "none identified (the polar filter)" : "off (rebase_kernel_match off)"));
		}
	}
	const bool temporal = temporalRenderer.Active();
	// Audit 2026-10-16: a failed re-base renders the native frame while the blend, the SSIM gate
	// and the debander were set up for the re-based one; after a failure the re-base pauses for
	// 5 s (then retries), so a persistent failure costs one frame, not every frame.
	if (sourceRebase.Failures() != rebaseFailuresSeen)
	{
		// 5 s, doubling to 60 s while it keeps failing (one native frame per retry).
		const bool recent = SteadyClock::now() < rebasePausedUntil + std::chrono::seconds(rebasePauseSeconds * 2);
		rebasePauseSeconds = recent ? (std::min)(60, rebasePauseSeconds * 2) : 5;
		rebaseFailuresSeen = sourceRebase.Failures();
		rebasePausedUntil = SteadyClock::now() + std::chrono::seconds(rebasePauseSeconds);
	}
	const bool rebasePaused = SteadyClock::now() < rebasePausedUntil;
	const double previousRebase = rebaseFactor, previousRebaseX = rebaseFactorX;
	rebaseFactor = wanted < 1.0 && !temporal && !rebasePaused ? wanted : 1.0;
	rebaseFactorX = wantedX < 1.0 && !temporal && !rebasePaused ? wantedX : 1.0;
	if (!pipelineSlots.OriginalPathway()) // VPV-HOOK
	{ // VPV-HOOK
		// FX-RB: hold a re-base on the current path until a worker has its shaders // VPV-HOOK
		const VpRebaseWarmup::Context warmContext{ renderImage, target, renderParams, d3d11->device, cache, // VPV-HOOK
			shaderCacheEnabled, nlsPreparationRevision, nlsPreparationNoCompute, sourceRebase.Kernel() }; // VPV-HOOK
		const VpRebaseWarmup::Controller::Outcome warm = rebaseWarmup.Frame(luma && // VPV-HOOK
			!pipelineSlots.OriginalPathway() && pipelineSlots.Runs(PipelineSlots::Id::ScalingRebase), // VPV-HOOK
			warmContext, rebaseFactorX, rebaseFactor, previousRebaseX, previousRebase); // VPV-HOOK
		if (warm.hold) { rebaseFactorX = warm.x; rebaseFactor = warm.y; } // VPV-HOOK
		if (warm.fallbackChanged) SyncFallback(warm.fallback); // VPV-HOOK
	} // VPV-HOOK
	// Silent-fallbacks batch: with scaling.rebase switched off the planes stay native, so every
	// downstream stage (debander, detail pulse, texture restore, adaptive upscale) uses factor 1.
	{
		const FallbackCatalog::RebaseFactors effective = FallbackCatalog::EffectiveRebaseFactors(
			pipelineSlots.Runs(PipelineSlots::Id::ScalingRebase), rebaseFactorX, rebaseFactor);
		rebaseFactorX = effective.x;
		rebaseFactor = effective.y;
	}
	SyncFallback(FallbackCatalog::RebasePaused((wanted < 1.0 || wantedX < 1.0) && rebasePaused,
		rebasePauseSeconds, sourceRebase.LastError()));
	// Latch review 2026-10-17: a crossfade between the path being left and this one (auto
	// only; never while smooth motion / deinterlacing runs or the re-base is paused).
	rebaseFade = 1.0;
	if (activeSettings.sourceResolution == "auto" && activeSettings.sourceResolutionLatchFixes && !temporal && !rebasePaused)
	{
		const SourceResolutionMonitor::Monitor::Transition tr = sourceMonitor.AutoTransition();
		if (tr.Active() && std::fabs(tr.toX - rebaseFactorX) < 1e-9 && std::fabs(tr.toY - rebaseFactor) < 1e-9)
		{
			rebaseFade = tr.weight;
			rebaseFromFactor = tr.fromY;
			rebaseFromFactorX = tr.fromX;
		}
	}
	// R4a (resync_event on): the re-base decision product P0. The factors are decided as above;
	// P0 times each axis's crossfade in SOURCE time (frames x the source period: the same frames
	// give the same weights), re-latches only the axis that changed, goes native without a fade
	// on a FULL, and writes the per-axis re-base stamps through the Indicator (from the next
	// frame: never retroactive). Off/shadow: the monitor's wall-clock fade above, unchanged.
	if (resync.Driving())
	{
		if ((resyncDrive.Take(VpResync::Reader::RebaseDecision) & VpIndicator::Bit(VpIndicator::kFull)) != 0)
			rebaseDecision.Reset();
		const bool fadeAllowed = activeSettings.sourceResolution == "auto" && activeSettings.sourceResolutionLatchFixes &&
			!temporal && !rebasePaused;
		double fadeUs = 0.0;
		if (fadeAllowed)
		{
			const SourceResolutionMonitor::Monitor::Transition tr = sourceMonitor.AutoTransition();
			if (tr.Active() && std::fabs(tr.toX - rebaseFactorX) < 1e-9 && std::fabs(tr.toY - rebaseFactor) < 1e-9)
				fadeUs = tr.duration * 1.0e6;   // the length the monitor asks for; its clock is not used
		}
		const double p0RateHz = SourceRateHz(state) > 0.0 ? SourceRateHz(state) : captureRateHz;
		const VpRebase::Decision p0 = rebaseDecision.Update(sourceSequence, p0RateHz > 0.0 ? 1.0e6 / p0RateHz : 0.0,
			rebaseFactorX, rebaseFactor, sourceRebase.Kernel(), fadeUs, fadeUs);
		resync.OnRebaseDecision(p0.StampH(), p0.StampV(), sourceSequence + 1);
		const VpRebase::Decision::Blend blend = p0.TwoPicture();
		rebaseFade = 1.0;
		if (fadeAllowed && blend.Active())
		{
			rebaseFade = blend.weight;
			rebaseFromFactor = blend.fromY;
			rebaseFromFactorX = blend.fromX;
		}
		rebaseP0Status.Publish(p0);
	}
	else
		rebaseP0Status.Clear();
	// MotionValidity step 1 (RebaseChange): the motion field belongs to the native
	// frame and survives a factor change, but VP deband's hold weights are
	// plane-sized: they start over (no motion invalidation, so this frame's
	// estimate and its consumers are unchanged).
	if (previousRebase != rebaseFactor || previousRebaseX != rebaseFactorX)
	{
		debander.ForgetHold();
		detailPulse.Forget();      // the planes change size: both histories restart
		textureRestore.Forget();
		detailPulseAheadSequence = 0;
		// Report-a-problem: a = native height, b = source sequence, x/y = factor X/Y.
		DiagnosticHistory::Global().RecordDecision("srcres-factor", activeSettings.sourceResolution.c_str(),
			nativeHeight, static_cast<int64_t>(sourceSequence), rebaseFactorX, rebaseFactor);
	}
	std::string state;
	if ((wanted < 1.0 || wantedX < 1.0) && rebasePaused)
		state = "Re-base: paused " + std::to_string(rebasePauseSeconds) + " s after a failure (" + sourceRebase.LastError() +
			", " + std::to_string(rebaseFailuresSeen) + " so far)";
	else if ((wanted < 1.0 || wantedX < 1.0) && temporal)
		state = "Re-base: not applied while smooth motion or deinterlacing runs (source_resolution " +
			activeSettings.sourceResolution + ")";
	else if ((rebaseFactor < 1.0 || rebaseFactorX < 1.0) && luma)
		state = "Re-base: " + std::to_string(luma->params.w) + "x" + std::to_string(nativeHeight) + " -> " +
			std::to_string(SourceRebase::Scaled(luma->params.w, rebaseFactorX)) + "x" +
			std::to_string(SourceRebase::Scaled(nativeHeight, rebaseFactor)) + " (source_resolution " +
			activeSettings.sourceResolution + ")";
	else if (!rebaseLogged.empty())
		state = "Re-base: off (source_resolution " + activeSettings.sourceResolution + ")";
	if (!state.empty() && state != rebaseLogged)
	{
		rebaseLogged = state;
		DebugLog::Log("%s", state.c_str());
	}
	// Stage 12 §2b/§2d: VP's debander for this frame - strength from
	// the readings (the content's level step; the bitrate quality),
	// held along the de-judder's vectors when they exist for it.
	debandThreshold = 0.0;
	vpDebandParams = VpDebander::Params();
	std::string debandState;
	// VP's debander skipped on the raw DV tunnel planes, visibly (DvHostStage, WP-14); nothing to
	// skip (or list) while the repair.deband slot is off (modularity sweep).
	const bool dvVpDebandSkipped = dvStage.SkipVpDeband(renderImage.repr.sys == PL_COLOR_SYSTEM_DOLBYVISION,
		activeSettings.debandVp != "off" && pipelineSlots.Runs(PipelineSlots::Id::RepairDeband), debandState);
	if (activeSettings.debandVp != "off" && luma && !dvVpDebandSkipped)
	{
		const SourceResolutionMonitor::Snapshot snap = sourceMonitor.Read();
		const int depth = renderImage.repr.bits.color_depth > 0 ? renderImage.repr.bits.color_depth : 10;
		const bool useQuality = activeSettings.lowBitrateHandling != "off";
		double quality = snap.reading ? snap.bitrateQuality : 1.0;
		if (activeSettings.lowBitrateHandling == "on") quality = (std::min)(quality, 0.4);
		const double bits = snap.reading ? snap.effectiveBits : 0.0;
		debandThreshold = VpDebander::Strength(bits, depth, quality, useQuality, activeSettings.sourceCodingFixes);
		if (activeSettings.debandVp == "on")
			debandThreshold = (std::max)(debandThreshold, 1.5 * std::pow(2.0, (std::max)(0, depth - 8)));
		if (temporal) debandThreshold = 0.0;
		vpDebandParams.thresholdCodes = debandThreshold;
		vpDebandParams.colorDepth = depth;
		vpDebandParams.radiusScale = nativeHeight * rebaseFactor / 1080.0;
		// Fixed-constant batch B6 (2026-10-18, sweep #18, general_rate_fixes): the hold's
		// per-frame weight keeps its time constant above 60 Hz (unchanged at and below).
		vpDebandParams.holdAlpha = GeneralRate::PerFrameWeight(vpDebandParams.holdAlpha, dejudderSourceHz,
			activeSettings.generalRateFixes);
		vectorEstimate.LiveFieldInto(pipelineSlots.OriginalPathway(), sourceSequence, activeSettings.motionHistoryFixes,   // VPV-HOOK P5c: R1 (VPV-GATED)
			vpDebandParams.field, vpDebandParams.grainError);   // VPV-HOOK
		vpDebandParams.fieldScale = rebaseFactor > 0.0 ? 1.0 / rebaseFactor : 1.0;
		vpDebandParams.fieldScaleX = rebaseFactorX > 0.0 ? 1.0 / rebaseFactorX : 1.0;
		vpDebandParams.sequence = sourceSequence;
		vpDebandParams.grainGuard = activeSettings.debandVpGrainGuard;
		vpDebandParams.grainSplit = activeSettings.debandVpGrainSplit;
		char text[200];
		if (temporal)
			snprintf(text, sizeof(text), "VP deband: not applied while smooth motion or deinterlacing runs (deband_vp %s)", activeSettings.debandVp.c_str());
		else if (debandThreshold <= 0.0)
			snprintf(text, sizeof(text), "VP deband: idle (deband_vp %s; %.1f-bit content, bitrate quality %.1f)", activeSettings.debandVp.c_str(), bits, quality);
		else
			snprintf(text, sizeof(text), "VP deband: threshold %.0f codes (deband_vp %s; %.1f-bit content, bitrate quality %.1f)",
				debandThreshold, activeSettings.debandVp.c_str(), bits, quality);
		debandState = text;
	}
	else if (!dvVpDebandSkipped && !debandLogged.empty())
		debandState = "VP deband: off";
	if (!debandState.empty() && debandState != debandLogged)
	{
		debandLogged = debandState;
		DebugLog::Log("%s", debandState.c_str());
	}
	// detail_pulse_stabilise / texture_restore (docs/VP-detail-restore.md): the parameters
	// for this frame, after the re-base factor is known. Off = never called (byte-identical
	// to today); not applied while smooth motion or deinterlacing runs (as the debander).
	// The source-resolution monitor above has already read the original frame.
	{
		const bool wasPulse = detailPulseRuns, wasRestore = textureRestoreRuns;
		detailPulseRuns = textureRestoreRuns = false;
		detailPulseParams = DetailPulse::Params();
		textureRestoreParams = TextureRestore::Params();
		std::string pulseState, restoreState;
		if ((activeSettings.detailPulseStabilise || activeSettings.textureRestore) && luma && !temporal)
		{
			const SourceResolutionMonitor::Snapshot snap = sourceMonitor.Read();
			const int depth = renderImage.repr.bits.color_depth > 0 ? renderImage.repr.bits.color_depth : 10;
			// The content rate the de-judder and the interpolator schedule with: the pulldown
			// film rate, else the measured capture rate, else the nominal rate; 0 = unknown.
			double contentHz = pulldownFilmHz > 0.0 ? pulldownFilmHz : vpCaptureRateHz > 0.0 ? vpCaptureRateHz : nominalSourceRateHz;
			if (!std::isfinite(contentHz) || contentHz <= 0.0) contentHz = 0.0;
			// The de-judder's vectors only under the debander's validity condition: this source frame's
			// live field (DejudderLiveFieldFor: the current motion epoch, and with motion_history_fixes the live-field sequence).
			// P5c W1-G: R1 (the gate lacked the motion_history_fixes clause; unchanged with it off).   // VPV-HOOK
			pl_tex field = vectorEstimate.LiveFieldOnVp(pipelineSlots.OriginalPathway(), sourceSequence, activeSettings.motionHistoryFixes);   // VPV-HOOK (VPV-GATED)
			const double fieldScale = rebaseFactor > 0.0 ? 1.0 / rebaseFactor : 1.0;
			const double fieldScaleX = rebaseFactorX > 0.0 ? 1.0 / rebaseFactorX : 1.0;
			const double quality = snap.reading ? snap.bitrateQuality : 1.0;
			const double lines = nativeHeight * rebaseFactor;
			if (activeSettings.detailPulseStabilise)
			{
				detailPulseRuns = true;
				DetailPulse::Params& p = detailPulseParams;
				p.cap = (std::max)(DetailPulse::kCapMin, (std::min)(DetailPulse::kCapMax, activeSettings.detailPulseCap));
				p.fast = activeSettings.detailPulseFast;
				p.colorDepth = depth;
				p.planeLines = lines;
				p.sourceHz = contentHz;
				p.bitrateQuality = quality;
				p.field = field;
				p.fieldScale = fieldScale;
				p.fieldScaleX = fieldScaleX;
				p.sequence = sourceSequence;
				// Hunt 3 F8 / F9: a one-plane RGB picture (an RGB capture, the subtitle move's
				// composed picture) is stabilised on its luma in the source's codes.
				p.lumaSystem = image.repr.sys;
				p.codeLevels = image.repr.levels;
				char text[200];
				if (detailPulseInactive.empty())
					snprintf(text, sizeof(text), "Detail pulse stabilise: on (cap %.2f)", p.cap);
				else
					snprintf(text, sizeof(text), "Detail pulse stabilise: inactive (%s)", detailPulseInactive.c_str());
				pulseState = text;
			}
			if (activeSettings.textureRestore)
			{
				textureRestoreRuns = true;
				TextureRestore::Params& p = textureRestoreParams;
				p.cap = (std::max)(TextureRestore::kCapMin, (std::min)(TextureRestore::kCapMax, activeSettings.textureRestoreCap));
				p.colorDepth = depth;
				p.planeLines = lines;
				p.sourceHz = contentHz;
				p.bitrateQuality = quality;
				p.field = InterpolationTimeline::OnePeriodField(dejudderEstimatedPeriods) ? field : nullptr;   // Bug hunt 3 F10
				p.fieldScale = fieldScale;
				p.fieldScaleX = fieldScaleX;
				p.sequence = sourceSequence;
				char text[200];
				if (textureRestoreInactive.empty())
					snprintf(text, sizeof(text), "Texture restore: statistical (cap %.2f)", p.cap);
				else
					snprintf(text, sizeof(text), "Texture restore: inactive (%s)", textureRestoreInactive.c_str());
				restoreState = text;
			}
		}
		else if (temporal)
		{
			if (activeSettings.detailPulseStabilise)
				pulseState = "Detail pulse stabilise: not applied while smooth motion or deinterlacing runs";
			if (activeSettings.textureRestore)
				restoreState = "Texture restore: not applied while smooth motion or deinterlacing runs";
		}
		if (!activeSettings.detailPulseStabilise && !detailPulseLogged.empty())
			pulseState = "Detail pulse stabilise: off";
		if (!activeSettings.textureRestore && !textureRestoreLogged.empty())
			restoreState = "Texture restore: off";
		// A stage that stopped running (switched off, or a temporal mode) starts over when it returns.
		if (wasPulse && !detailPulseRuns) detailPulse.Forget();
		if (wasRestore && !textureRestoreRuns) textureRestore.Forget();
		if (!detailPulseRuns) detailPulseAheadSequence = 0;
		if (!detailPulseRuns) detailPulseInactive.clear();
		if (!textureRestoreRuns) textureRestoreInactive.clear();
		if (!pulseState.empty() && pulseState != detailPulseLogged)
		{
			detailPulseLogged = pulseState;
			DebugLog::Log("%s", pulseState.c_str());
		}
		if (!restoreState.empty() && restoreState != textureRestoreLogged)
		{
			textureRestoreLogged = restoreState;
			DebugLog::Log("%s", restoreState.c_str());
		}
	}
}
}

// The Original pathway never calls PlanEntrance (guarded at the call site), so a live switch to it would leave the last VP-V plan
// in the members the renderer's other consumers read (the repeat-reuse extension, the adaptive frame, the crossfade branches).
// The settings seam (UpdatePipelineSlots) calls this once when Original is selected: native factors, no crossfade, no debander or
// detail-restore parameters. Idempotent; all values are the members' initial values.
void LibplaceboVideoRenderer::Impl::ResetEntrancePlan()
{
	rebaseFactor = rebaseFactorX = 1.0;
	rebaseFade = 1.0;
	debandThreshold = 0.0;
	vpDebandParams = VpDebander::Params();
	detailPulseRuns = textureRestoreRuns = false;
	detailPulseParams = DetailPulse::Params();
	textureRestoreParams = TextureRestore::Params();
}
