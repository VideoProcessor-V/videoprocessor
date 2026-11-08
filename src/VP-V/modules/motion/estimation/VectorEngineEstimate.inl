// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5 (doc 52 section 3.1 item f): the vector engine's estimate - the plane uploads for estimation
// (UploadDejudderPlanes, TakeNextPlanes), the matcher run (RunDejudderEstimate, the engine's Estimate) and the
// estimate ahead (EstimateAhead). Moved VERBATIM out of LibplaceboVideoRenderer.cpp; the only changes inside the
// bodies are the K4 pre-estimate hook (VectorEngine::Estimate wraps the matcher's Estimate call; with no hook
// registered it calls the matcher exactly as before and nothing else) and RunDejudderEstimate's `ahead` argument
// (default false at the declaration; true only from EstimateAhead's estimate step, for EstimateContext::ahead).
// These are out-of-line member definitions of the renderer's Impl: LibplaceboVideoRenderer.cpp includes this file
// ONCE, right after the Impl struct; no other translation unit may include it. Callers in the de-judder policy zone
// (UpdateDejudderEstimate, EntranceOnceFieldAhead, the extras) call the same Impl member names, unchanged.
// Render thread (renderMutex held), as before. The estimate state lives in VpMotion::EstimateState (EstimateState.h)
// and VpMotion::VectorEngineState, reached through the Impl aliases (HOOKS.md section 4).
#pragma once

#include "EngineKeeper.inl"

// The next frames' planes (B, or C ahead) in B's layout.
// upload (efficiency_fixes): filled with how the planes were made, for the reuse when this
// frame is shown; cleared first (a failed or luma-only upload is never reused).
bool LibplaceboVideoRenderer::Impl::UploadDejudderPlanes(const std::shared_ptr<const QueuedFrame::ConvertedPayload>& payloadPtr, pl_tex (&dst)[2],
	DejudderPlaneUpload* upload, uint64_t sequence)
{
	if (upload)
		upload->Clear();
	if (!payloadPtr)
		return false;
	const QueuedFrame::ConvertedPayload& payload = *payloadPtr;
	const DejudderUploadLayout& l = dejudderLayout;
	if (!l.valid || payload.Bytes() != l.pixelBytes)
		return false;
	auto record = [&](bool raw, const pl_plane* planes)
	{
		if (!upload)
			return;
		upload->record.valid = true;
		upload->record.payload = payloadPtr.get();
		// Gate r3 (2026-10-18, measured on the GeForce): the payload's source frame. It was never
		// recorded, so CanReuse (r.sequence == q.sequence) refused every reuse as "layout differs".
		upload->record.sequence = sequence;
		upload->record.width = l.width;
		upload->record.height = l.height;
		upload->record.chromaHeight = l.chromaHeight;
		upload->record.rowBytes = l.rowBytes;
		upload->record.raw = raw;
		upload->payload = payloadPtr;
		if (planes)
		{
			upload->planes[0] = planes[0];
			upload->planes[1] = planes[1];
		}
	};
	// native_rgb_ingest: an RGB raw frame gives the matcher its luma only
	// (dst[0]; dst[1] untouched), unpacked as the frame A was.
	if (payload.Raw() && !payload.dvTunnelRaw && GpuRgbUnpack::Supports(payload.rawEncoding))
	{
		if (!l.raw || !l.rgb || payload.rawEncoding != l.rawEncoding || !d3d11)
			return false;
		if (!rgbUnpack)
			rgbUnpack = std::make_unique<GpuRgbUnpack>(log, d3d11->gpu);
		EarlyUpload::Slot* slot = nullptr;
		const pl_tex early = EarlyTexture(payload, slot);
		bool unpacked = rgbUnpack->Unpack(payload.rawEncoding, l.bt2020, payload.rawFrame->GetData(), payload.rawRowBytes,
			l.width, l.height, dejudderRgbScratch, dst[0], true, early);
		if (slot)
			earlyUpload->Used(*slot);
		if (!unpacked && early)
			unpacked = rgbUnpack->Unpack(payload.rawEncoding, l.bt2020, payload.rawFrame->GetData(), payload.rawRowBytes,
				l.width, l.height, dejudderRgbScratch, dst[0], true);
		if (unpacked)
			return true;
		DebugLog::Log("Native RGB ingest: a de-judder frame's GPU unpack failed (%s); that frame skipped",
			rgbUnpack->Error().c_str());
		return false;
	}
	if (payload.Raw())
	{
		// capture_path: gpu; the route and format must be B's (A's layout).
		if (!l.raw || payload.rawEncoding != l.rawEncoding || payload.dvTunnelRaw != l.dvTunnelRaw)
			return false;
		if (!UnpackRawPayload(payload, l.width, l.height, dst, false))
			return false;
		record(true, nullptr);
		return true;
	}
	struct pl_plane_data planes[2]{};
	const BYTE* yPixels = payload.pixels.data();
	const BYTE* uvPixels = yPixels + l.rowBytes * static_cast<size_t>(l.height);
	planes[0].type = PL_FMT_UNORM;
	planes[0].width = l.width;
	planes[0].height = l.height;
	planes[0].component_size[0] = 16;
	planes[0].component_map[0] = PL_CHANNEL_Y;
	planes[0].pixel_stride = sizeof(uint16_t);
	planes[0].row_stride = l.rowBytes;
	planes[0].pixels = yPixels;
	planes[1] = planes[0];
	planes[1].width = (l.width + 1) / 2;
	planes[1].height = l.chromaHeight;
	planes[1].component_size[1] = 16;
	planes[1].component_map[0] = PL_CHANNEL_CB;
	planes[1].component_map[1] = PL_CHANNEL_CR;
	planes[1].pixel_stride = sizeof(uint16_t) * 2;
	planes[1].pixels = uvPixels;
	pl_plane uploaded[2]{};
	for (int plane = 0; plane < 2; ++plane)
		if (!pl_upload_plane(d3d11->gpu, &uploaded[plane], &dst[plane], &planes[plane]))
			return false;
	record(false, uploaded);
	return true;
}

// efficiency_fixes (UploadReuse.h): the frame about to be shown was the last frame's B,
// uploaded (or GPU-unpacked) for the motion estimate from this same payload in this same
// layout. Its planes are swapped in as the frame's own (bit-identical: the same bytes,
// route and formats) and `out` receives the planes the upload would have made. False:
// upload as before. YCbCr routes only (the RGB route uploads the matcher's luma alone).
bool LibplaceboVideoRenderer::Impl::TakeNextPlanes(const std::shared_ptr<const QueuedFrame::ConvertedPayload>& payload, uint64_t sourceSequence,
	int width, int height, int chromaHeight, size_t rowBytes, bool flipped, pl_plane* out)
{
	if (!payload)
		return false;
	UploadReuse::Record record = dejudderNextUpload.record;
	// The recorded payload's identity holds only while that payload is alive.
	const auto alive = dejudderNextUpload.payload.lock();
	record.payload = alive.get();
	UploadReuse::Request request;
	request.enabled = activeSettings.efficiencyFixes;
	request.payload = payload.get();
	request.sequence = sourceSequence;
	request.width = width;
	request.height = height;
	request.chromaHeight = chromaHeight;
	request.rowBytes = rowBytes;
	request.raw = payload->Raw();
	request.texturesPresent = dejudderNextTextures[0] && dejudderNextTextures[1];
	request.uploadedSequenceMatches = sourceSequence != 0 && dejudderNextUploadedSequence == sourceSequence;
	if (!UploadReuse::CanReuse(record, request))
	{
		// The next frame's planes were there but could not be taken: counted (and the first
		// few logged), so an on-screen run shows whether the saving engages.
		if (request.enabled && request.uploadedSequenceMatches && ++uploadsReuseMissed <= 5)
			DebugLog::Log("Efficiency: the next frame's planes not reused for frame %llu (%s)",
				static_cast<unsigned long long>(sourceSequence),
				!record.valid ? "not recorded" : record.payload != request.payload ? "another payload" :
				!request.texturesPresent ? "no planes" : "layout differs");
		return false;
	}
	std::swap(textures[0], dejudderNextTextures[0]);
	std::swap(textures[1], dejudderNextTextures[1]);
	if (request.raw)
	{
		// As the GPU unpack's planes are described below (UnpackRawPayload).
		out[0] = pl_plane{};
		out[0].texture = textures[0];
		out[0].components = 1;
		out[0].component_mapping[0] = PL_CHANNEL_Y;
		out[1] = pl_plane{};
		out[1].texture = textures[1];
		out[1].components = 2;
		out[1].component_mapping[0] = PL_CHANNEL_CB;
		out[1].component_mapping[1] = PL_CHANNEL_CR;
	}
	else
		for (int plane = 0; plane < 2; ++plane)
		{
			// pl_upload_plane's description of the same upload, on the swapped-in texture.
			out[plane] = dejudderNextUpload.planes[plane];
			out[plane].texture = textures[plane];
			out[plane].shift_x = 0.0f;
			out[plane].shift_y = 0.0f;
		}
	for (int plane = 0; plane < 2; ++plane)
		out[plane].flipped = flipped;
	// dejudderNextTextures now hold the previous frame's planes: nothing valid there.
	dejudderNextUploadedSequence = 0;
	dejudderNextUpload.Clear();
	if (++uploadsReused == 1 || uploadsReused % 2400 == 0)
		DebugLog::Log("Efficiency: the frame shown took the planes uploaded for the motion estimate (%llu frames, "
			"no second upload; %llu not reused)", uploadsReused, uploadsReuseMissed);
	return true;
}

// The matcher on (a, b): its settings from the profile, b's occlusion mask
// into the slot not holding a's.
bool LibplaceboVideoRenderer::Impl::RunDejudderEstimate(const Dejudder::Planes& a, const Dejudder::Planes& b, uint64_t aSequence, uint64_t bSequence,
	bool ahead)
{
	const uint64_t motionShadowEpoch = motionValidity.Epoch();   // shadow segment: as the callers' estimateEpoch
	// The matcher's settings from the profile, refinements included.
	auto wanted = [&](Dejudder::Settings settings)
	{
		Dejudder::Settings result = MatcherSettingsFromProfile::MatcherSettings(activeSettings, settings);
		result.sourceHz = dejudderSourceHz;   // the content rate (the film rate with pulldown)
		result.generalRates = activeSettings.generalRateFixes;   // fixed-constant batch B4 2026-10-18
		return result;
	};
	if (activeSettings.dejudderOcclusionFill && activeSettings.dejudderWarpRendered && !dejudderFillIgnoredLogged)
	{
		DebugLog::Log("De-judder occlusion fill: source warp path only; ignored with dejudder_warp_stage: rendered");
		dejudderFillIgnoredLogged = true;
	}
	if (!dejudder)
	{
		dejudder = std::make_unique<Dejudder::MotionWarp>(log, d3d11->gpu, wanted(Dejudder::Settings()));
		// Bug hunt 3 F4: logged, so a test sees that a second hiccup does not recreate it.
		if (++dejudderCreates <= 20 || dejudderCreates % 100 == 0)
			DebugLog::Log("Motion engine: created (%llu times this renderer; hiccup repair %s)",
				dejudderCreates, activeSettings.interpolationFillGaps ? "on: kept loaded and idle between gaps" : "off");
	}
	else
	{
		const Dejudder::Settings current = dejudder->GetSettings();
		const Dejudder::Settings next = wanted(current);
		const bool meaningChanged = current.matchTolerance != next.matchTolerance || current.minTrustedShare != next.minTrustedShare ||
			current.globalPrior != next.globalPrior || current.vectorSmooth != next.vectorSmooth ||
			current.motionScaleLo != next.motionScaleLo || current.motionScaleHi != next.motionScaleHi ||
			current.trustScaleFull != next.trustScaleFull || current.occlusionFill != next.occlusionFill ||
			current.estCandidates != next.estCandidates || current.estLambda != next.estLambda ||
			current.estQpel != next.estQpel || current.estStillZero != next.estStillZero || current.estGate != next.estGate ||
			current.estSplit != next.estSplit || current.estSplitSad != next.estSplitSad ||
			current.lowRes != next.lowRes || current.lowResAll != next.lowResAll ||
			current.holdFeatherPx != next.holdFeatherPx || current.holdHysteresis != next.holdHysteresis ||
			current.blockTaper != next.blockTaper || current.taperSad != next.taperSad ||
			current.motionRolloff != next.motionRolloff || current.rollLo != next.rollLo ||
			current.rollHi != next.rollHi || current.rollFloor != next.rollFloor ||
			current.objectStrength != next.objectStrength ||
			// The motion fixes (audit 2026-10-14: these never reached a live matcher).
			current.textureTolerance != next.textureTolerance || current.globalTwo != next.globalTwo ||
			current.regionalHold != next.regionalHold || current.cutHistogram != next.cutHistogram ||
			current.coarseRange != next.coarseRange || current.consistencyPixels != next.consistencyPixels ||
			current.fadeMatch != next.fadeMatch || current.fadeFloor != next.fadeFloor ||
			current.verifyPixels != next.verifyPixels || current.verifyShare != next.verifyShare ||
			current.pixelTrust != next.pixelTrust || current.pixelTrustLo != next.pixelTrustLo ||
			current.pixelTrustHi != next.pixelTrustHi || current.pixelTrustedVectors != next.pixelTrustedVectors ||
			current.landingCheck != next.landingCheck || current.pictureArea != next.pictureArea ||
			current.sourceWarpCatmull != next.sourceWarpCatmull ||
			current.referenceHeight != next.referenceHeight ||
			current.generalRates != next.generalRates ||   // fixed-constant batch B4 2026-10-18
			// The optical-flow engine and its camera estimate (audit 2026-10-16: never reached a live matcher).
			current.opticalFlow != next.opticalFlow || current.opticalFlowGlobal != next.opticalFlowGlobal ||
			current.opticalFlowGlobalFallback != next.opticalFlowGlobalFallback ||
			current.opticalFlowCostTrust != next.opticalFlowCostTrust || current.opticalFlowFbCheck != next.opticalFlowFbCheck;
		// The measured content rate (it follows the source; converts the per-frame values) and
		// the switch itself change no vector's meaning.
		if (meaningChanged || current.sourceHz != next.sourceHz || current.referenceRateHz != next.referenceRateHz ||
			current.historyFixes != next.historyFixes)
			dejudder->SetSettings(next);
		// Hunt 2 04 L3 (motion_history_fixes): a matcher setting that changes what the vectors
		// mean invalidates them (the consumers fall back until a fresh pair) and this pair is
		// estimated without the old history.
		if (meaningChanged && activeSettings.motionHistoryFixes)
		{
			RequestMotionInvalidate(MotionValidity::Cause::SettingsChange);
			dejudder->ForgetHistory();
		}
	}
	// Hunt 2 04 L3 (motion_history_fixes): the motion source (the optical-flow engine on or
	// off, the producer ahead wanted or not) changed while the matcher kept its history.
	{
		const int sourceKey = !activeSettings.opticalFlow ? 0 : activeSettings.motionAheadFrames > 0 ? 2 : 1;
		if (activeSettings.motionHistoryFixes && dejudderMotionSourceKey >= 0 && sourceKey != dejudderMotionSourceKey)
		{
			RequestMotionInvalidate(MotionValidity::Cause::MotionSourceChange);
			dejudder->ForgetHistory();
		}
		dejudderMotionSourceKey = sourceKey;
	}
	// 6c: B's occlusion mask into the slot not holding the mask for the
	// frame A (now showing, or B itself ahead), so A's survives until it
	// is rendered.
	int maskSlot = -1;
	if (activeSettings.occlusionMask)
		maskSlot = dejudderMaskSequence[0] == aSequence ? 1 : 0;
	// Black bars: the latest trusted active picture when it is this
	// raster's (else the matcher finds the bars itself).
	const int width = a.luma ? a.luma->params.w : 0, height = a.luma ? a.luma->params.h : 0;
	if (dejudderPicture.available && dejudderPicture.rasterWidth == width && dejudderPicture.rasterHeight == height &&
		dejudderPicture.right > dejudderPicture.left && dejudderPicture.bottom > dejudderPicture.top)
		dejudder->SetPictureArea(dejudderPicture.left, dejudderPicture.top, dejudderPicture.right, dejudderPicture.bottom);
	else
		dejudder->SetPictureArea(0, 0, 0, 0);
	// The frame numbers let a pair whose A is the last pair's B reuse its
	// planes (2026-10-15; a gap or resync forgets them).
	Dejudder::Planes sa = a, sb = b;
	sa.sequence = aSequence;
	sb.sequence = bSequence;
	if (motionProducer && motionProducer->Running() && !motionEngineWarming)
	{
		int engineW = 0, engineH = 0;
		if (dejudder->EngineInputSize(engineW, engineH))
			motionProducer->SetTargetSize(engineW, engineH);
		// MotionValidity step 1 D: a stored pair only for adjacent frames of the current
		// epoch (otherwise the matcher runs).
		MotionAhead::FlowPtr flow = motionValidity.PairValid(aSequence, bSequence, motionValidity.EpochOfLastKept(),
			motionValidity.Epoch()) ? vectorEngine.P2().Find(aSequence, bSequence) : nullptr;   // SK-MV: P2 read through the view
		if (flow) ++motionAheadHits; else ++motionAheadMisses;
		// Latch review 2026-10-17: the engine's whole-frame motion (z < 0.9 usable) for
		// source_resolution auto's fast-pan switch. Bug hunt 3 F1: one cell is 8 source px
		// per halving of the search input (4K 16, 8K 32), not 8 at every size.
		const int searchHalvings = dejudder->SearchHalvings();
		vectorEngine.NoteGlobalPair(flow != nullptr);   // VPV-HOOK
		vectorEngine.NoteEngineGlobal(aSequence, bSequence, flow ? flow->global[0] : nullptr, searchHalvings);   // K4: IVectorEngine::EngineGlobal (CPU record only)
		if (flow)
		{
			const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(flow->global[0][0]), static_cast<double>(flow->global[0][1]), searchHalvings),
				flow->global[0][2], flow->global[0][3], 0);
			if (sample.usable)
				sourceMonitor.SetMotion(sample.sourcePixels, bSequence);
		}
		// Motion intent (2026-10-17): the same whole-frame motion for this valid pair (the
		// vectors-valid state accepted it above), in picture heights per frame; read-only.
		if (flow && intentInterpreter->Active())
		{
			const int pictureHeight = dejudderPicture.available && dejudderPicture.rasterHeight == height &&
				dejudderPicture.bottom > dejudderPicture.top ? dejudderPicture.bottom - dejudderPicture.top : height;
			IntentInterpreter::VectorSample vectors;
			vectors.from = aSequence;
			vectors.to = bSequence;
			const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(flow->global[0][0]), static_cast<double>(flow->global[0][1]), searchHalvings),
				flow->global[0][2], flow->global[0][3], pictureHeight);
			vectors.valid = sample.intentValid;
			vectors.heightsPerFrame = sample.heightsPerFrame;
			intentInterpreter->NoteVectors(vectors);
		}
		dejudder->SetExternalFlow(std::move(flow));
		vectorEngine.P2().Prune(aSequence);
	}
	dejudderLiveFieldFrom = 0;   // the live outputs are being replaced
	motionSegment.Retract(VpMotion::Direction::Forward);
	dejudder->SetPassSampling(passTimerHook.Sampling());   // efficiency timers T3: polled, never drained
	// K4 (camera-guided matching): the pre-estimate hook binds the camera guidance right before the matcher's Estimate
	// and reads what it did right after. No hook registered (camera guidance off) = the matcher's Estimate only, as
	// before: nothing bound, no extra GPU work. Never for the warm-up's dummy A -> A estimate.
	const bool motionEstimated = vectorEngine.Estimate(aSequence, bSequence, ahead, dejudderSourceHz, motionShadowEpoch,
		!motionEngineWarming, *dejudder, [&]() { return dejudder->Estimate(sa, sb, maskSlot, adaptiveGrainMapWanted); },
		[this](const FallbackCatalog::Decision& decision) { SyncFallback(decision); });
	passTimerHook.AddGpuGroups("motion: ", dejudder->TakePassGpuMs());
	if (!motionEstimated)
	{
		// Bug hunt 3 F4: a texture the engine could not create (low video memory).
		if (dejudder->LastError().find("cannot create") != std::string::npos)
			motionEngineIdle.NoteAllocationFailure(static_cast<double>(GetTickCount64()) / 1000.0);
		if (++dejudderFailures == 1 || dejudderFailures % 2400 == 0)
			DebugLog::Log("De-judder: motion estimate failed (%s; %llu failures)",
				dejudder->LastError().c_str(), dejudderFailures);
		return false;
	}
	dejudderLiveFieldFrom = aSequence;
	if (pipelineSlots.Runs(PipelineSlots::Id::MotionVectorDelivery))
		motionSegment.Publish(VpMotion::Direction::Forward, aSequence, bSequence,
		MotionShadowField{ aSequence, bSequence, dejudder->Field(), dejudder->FrameTrust(), dejudder->SubField(), dejudder->Camera() },   // SK-MV: P3 (+P2/P2m by the source) published with its textures
		dejudder->ExternalFlowUsed() ? VpMotion::Source::EngineAhead :
		dejudder->OpticalFlowUsed() ? VpMotion::Source::EngineAtRender : VpMotion::Source::Matcher,
		motionShadowEpoch, 0, bSequence != aSequence + 1);
	if (motionEngineWarming)
		return true;   // r6 fix 1: warm-up only; nothing counted, logged or handed on
	if (maskSlot >= 0)
	{
		dejudderMaskSequence[maskSlot] = bSequence;
		dejudderMaskEpoch[maskSlot] = motionValidity.Epoch();
	}
	if (motionRecoveryLogPending)
	{
		DebugLog::Log("De-judder: estimating again on a fresh contiguous pair");
		motionRecoveryLogPending = false;
	}
	// The motion source: the optical-flow engine, or the block searches (logged on change).
	{
		const char* source = dejudder->ExternalFlowUsed() ? (dejudder->GlobalFromEngine() ?
				"optical-flow engine (computed ahead, camera from the engine)" : "optical-flow engine (computed ahead)") :
			dejudder->OpticalFlowUsed() ? "optical-flow engine" :
			!dejudder->GetSettings().opticalFlow ? "block searches (optical_flow off)" : "block searches (no engine)";
		if (motionSourceLogged != source)
		{
			motionSourceLogged = source;
			if (!dejudder->OpticalFlowUsed() && dejudder->GetSettings().opticalFlow && !dejudder->OpticalFlowError().empty())
				DebugLog::Log("De-judder motion: %s - %s", source, dejudder->OpticalFlowError().c_str());
			else
				DebugLog::Log("De-judder motion: %s", source);
		}
		// Silent-fallbacks batch: the matcher standing in for the optical-flow engine is visible.
		const bool flowFailed = !dejudder->OpticalFlowUsed() && !dejudder->ExternalFlowUsed() &&
			!dejudder->OpticalFlowError().empty();
		const bool producerStopped = motionProducer && !motionProducer->Running();
		SyncFallback(FallbackCatalog::Motion(dejudder->GetSettings().opticalFlow, flowFailed, producerStopped,
			flowFailed ? dejudder->OpticalFlowError() : producerStopped ? motionProducer->Error() : std::string()));
	}
	if (vectorDemand.estimate)
	{
		++vectorPairs;
		vectorEngine.NoteVectorsDelivered(false, dejudder->ExternalFlowUsed() ? "engine ahead" : dejudder->OpticalFlowUsed() ? "engine" : "matcher");   // VPV-HOOK
	}
	static GeneralRate::LogInterval motionAheadStatsLog = GeneralRate::LogInterval::ForFrames(2400);   // fixed-constant batch B7 2026-10-18: seconds, not frames
	if (motionProducer && dejudderEstimates + 1 >= 24 && motionAheadStatsLog.DueOr((dejudderEstimates + 1) == 24))   // as the other periodic logs (audit 2026-10-16)
	{
		std::string counts;
		for (size_t n = 0; n < queuedAheadCounts.size(); ++n)
		{
			const unsigned long long c = queuedAheadCounts[n].load(std::memory_order_relaxed);
			if (c) counts += (counts.empty() ? "" : " ") + std::to_string(n) + (n == 9 ? "+" : "") + ":" + std::to_string(c);
		}
		DebugLog::Log("Motion ahead: frames queued ahead when taken (count:frames) %s", counts.c_str());
		const auto ps = motionProducer->GetStats();
		const unsigned long long handoffs = motionHandoffCount.load(std::memory_order_relaxed);
		DebugLog::Log("Motion ahead: %llu frames, %llu pairs (last %.1f ms, luma %.1f ms, on the worker), %llu dropped; pairs taken %llu, missed %llu; render-loop hand-off mean %.3f ms, max %.3f ms",
			ps.frames, ps.pairs, ps.lastPairMs, ps.lumaMs, ps.dropped, motionAheadHits, motionAheadMisses,
			handoffs ? motionHandoffTotalMs.load(std::memory_order_relaxed) / handoffs : 0.0, motionHandoffMaxMs.load(std::memory_order_relaxed));
		DebugLog::Log("Motion ahead extras: %.2f ms per pair on the worker; engine trust weights %s; camera from the search when unclear %llu times",
			ps.extrasMs, dejudder->EngineWeightsUsed() ? "applied" : "not applied", dejudder->GlobalFallbacks());
	}
	static GeneralRate::LogInterval estimatesLog = GeneralRate::LogInterval::ForFrames(2400);   // fixed-constant batch B7 2026-10-18: seconds, not frames
	if (estimatesLog.DueOr(++dejudderEstimates == 1))
		DebugLog::Log("De-judder: motion estimated for %llu source frames (%dx%d, %s)",
			dejudderEstimates, dejudderLayout.width, dejudderLayout.height,
			dejudderLayout.rgb ? "RGB, luma from the unpack" : dejudderLayout.lossless422 ? "4:2:2" : "4:2:0");
	return true;
}

// Owner 2026-10-13 (estimate ahead): after an extra of A, with C (the
// frame after B) already queued, the next pair (B, C) is estimated in
// that extra's spare refresh, so B's own refresh holds only its render.
// The extras still to come read a copy of (A, B)'s field. Without C, or
// with anything not matching, B is estimated before its render as before.
// renderedPath: the extras of this frame really use the rendered warp (the
// cache exists). With the source warp they read the matcher's live field,
// which an ahead estimate would replace (audit 2026-10-14).
// timing (render-ahead gate, E): the estimate's GPU stages - the snapshot and the
// estimate. The next frame's upload is left out: its CPU copy runs between the
// timestamps with the GPU idle and was measured as GPU time (p95 20-37 ms on a
// 3050 Ti for an estimate of a few ms; render-ahead follow-up 2026-10-17).
// forEntranceOnce: B3's B ahead (any path: its consumers read the snapshot; ProcessedB).
bool LibplaceboVideoRenderer::Impl::EstimateAhead(uint64_t sourceSequence, uint64_t nextSourceSequence, bool renderedPath,
	AlphaFrameGpuTimingScope* timing, bool forEntranceOnce)
{
	Dejudder::AheadSchedule::Conditions c;
	c.enabled = activeSettings.dejudderEstimateAhead;
	c.renderedWarp = forEntranceOnce || (activeSettings.dejudderWarpRendered && renderedPath);
	c.active = dejudder && dejudderReason == DejudderPolicy::Reason::Active &&
		dejudderNextTextures[0] && (dejudderNextTextures[1] || dejudderLayout.rgb);
	c.source = sourceSequence;
	c.next = nextSourceSequence;
	c.after = dejudderAheadPayload ? dejudderAheadPayloadSequence : 0;
	c.estimated = dejudderEstimatedSequence;
	c.layoutMatches = dejudderAheadPayload && dejudderLayout.valid &&
		dejudderAheadPayload->Bytes() == dejudderLayout.pixelBytes &&
		dejudderAheadPayload->Raw() == dejudderLayout.raw &&
		(!dejudderLayout.raw || dejudderAheadPayload->rawEncoding == dejudderLayout.rawEncoding) &&
		dejudderAheadPayload->dvTunnelRaw == dejudderLayout.dvTunnelRaw &&
		dejudderAheadPayload->contract.sampleRange == dejudderLayout.contract.sampleRange &&
		dejudderAheadPayload->contract.colorDepth == dejudderLayout.contract.colorDepth &&
		dejudderAheadPayload->contract.bitShift == dejudderLayout.contract.bitShift;
	Dejudder::AheadSchedule::Steps steps;
	steps.snapshot = [this, timing]()
	{
		if (timing) timing->BeginStage(AlphaD3D11FrameGpuTimer::Stage::CoreRender);
		const bool done = dejudder->SnapshotForWarp();
		if (timing) timing->EndStage();
		return done;
	};
	steps.uploadAfter = [this]() { return UploadDejudderPlanes(dejudderAheadPayload, dejudderAheadTextures, &dejudderAheadUpload,
		dejudderAheadPayloadSequence); };
	steps.estimate = [this, timing](uint64_t bSequence, uint64_t cSequence)
	{
		Dejudder::Planes b, c;
		b.luma = dejudderNextTextures[0];
		b.chroma = dejudderLayout.rgb ? nullptr : dejudderNextTextures[1];
		b.chromaShiftX = 1;
		b.chromaShiftY = dejudderLayout.lossless422 ? 0 : 1;
		b.fullRange = dejudderLayout.fullRange;
		c = b;
		c.luma = dejudderAheadTextures[0];
		c.chroma = dejudderLayout.rgb ? nullptr : dejudderAheadTextures[1];
		if (timing) timing->BeginStage(AlphaD3D11FrameGpuTimer::Stage::CoreRender);
		const bool done = RunDejudderEstimate(b, c, bSequence, cSequence, true);   // ahead: EstimateContext::ahead for the K4 hook
		if (timing) timing->EndStage();
		return done;
	};
	return dejudderAhead.TryAhead(c, steps);
}
