// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5b-4 (doc 59 section 2.1 items b, c, g): the phase picture, the per-vsync timeline helpers, the de-judder
// status line and the de-judder release. Out-of-line member definitions of the renderer's Impl, included ONCE by
// LibplaceboVideoRenderer.cpp right after the Impl struct. Moved VERBATIM; the Impl keeps one declaration each
// (same names, same default arguments). New: VpStageBFrame (item g: Stage B's no-refresh frames and the phase
// picture, one call at today's position in RenderLocked) and DrawVpPhasePicture (the draw piece inside the render
// callback; it stays IN PLACE as one call until P4 moves the hook block - module README open item).
// Render thread (RenderLocked, renderMutex held); ReleaseDejudder also from RetireOutput (stop path, renderMutex).
#pragma once

#include <VP-V/modules/motion/consumers/TimelinePlanner.h>

bool LibplaceboVideoRenderer::Impl::RunVpPhaseInterpolation()
{
	if (!frameInterpolator)
		frameInterpolator = std::make_unique<Interpolator::FrameInterpolator>(log, d3d11->gpu);
	frameInterpolator->SetGrain(activeSettings.interpolationGrain);
	const bool ran = frameInterpolator->Run(vpPhaseA, vpPhaseB, vpPhaseMotion, vpPhaseT, activeSettings.interpolationFallback,
		vpPhasePlanes);
	SyncFallback(motionConsumerFailures.Note(GetTickCount64(), ran ? nullptr : "interpolation failed"));   // D3
	if (!ran)
	{
		if (++interpolationFailures == 1 || interpolationFailures % 2400 == 0)
			DebugLog::Log("Interpolation: failed (%s; %llu failures); the source frame is shown",
				frameInterpolator->LastError().c_str(), interpolationFailures);
		return false;
	}
	return true;
}

double LibplaceboVideoRenderer::Impl::TimelineDisplayHz() const
{
	const double measured = presentationTelemetry.Snapshot().measuredDisplayHz;
	return timelineDisplayRateHold.Update(measured, renderLoadDisplayRefreshHz.load(std::memory_order_acquire),
		activeSettings.timingContinuityFixes);
}

bool LibplaceboVideoRenderer::Impl::VpTimelineWanted() const
{
	// A queue momentarily without B keeps the schedule (A is held, never waited for), so a
	// shallow queue does not restart the phases.
	const bool keep = dejudderPolicy.interpolationDecision.reason == InterpolationPolicy::Reason::NoNextFrame && dejudderPolicy.vpTimeline.Initialized();
	if (vpGapFillStandalone && !lowLatencyQueueActive.load(std::memory_order_acquire))
		return true;   // hiccup repair: this gap pair only
	return activeSettings.interpolation != InterpolationPolicy::Mode::Off && (interpolating || keep) &&
		!lowLatencyQueueActive.load(std::memory_order_acquire);
}

void LibplaceboVideoRenderer::Impl::PrepareVpPhasePicture(uint64_t sourceSequence, uint64_t nextSourceSequence, bool lossless422, bool fullRange)
{
	vpPhasePlanesValid = false;
	if (!interpolating || !(vpOutputPhase > 0.0) || !textures[0] || !textures[1])
		return;
	if (nextSourceSequence == 0 || dejudderNextUploadedSequence != nextSourceSequence || !dejudderNextTextures[0] ||
		!dejudderNextTextures[1])
	{
		++vpHeldPictures;   // no B: A, never waiting
		return;
	}
	Dejudder::Planes a;
	a.luma = textures[0];
	a.chroma = textures[1];
	a.chromaShiftX = 1;
	a.chromaShiftY = lossless422 ? 0 : 1;
	a.fullRange = fullRange;
	Dejudder::Planes b = a;
	b.luma = dejudderNextTextures[0];
	b.chroma = dejudderNextTextures[1];
	Interpolator::Motion motion;
	// B3 (i): after B's field was estimated ahead, A's (A, B) inputs are the snapshot.
	const bool snapshot = !DejudderLiveFieldFor(sourceSequence) && EntranceOnce() && EntranceOnceSnapshotFor(sourceSequence);
	// WIP DEBUG (near-black residual, 2026-10-24): VP_DEBUG_PHASE_NOVECTORS=1 forces the no-vector path.
	const bool forceNoVectors = debugPhaseNoVectors;   // C10: read once at construction
	const bool vectors = (DejudderLiveFieldFor(sourceSequence) || snapshot) && !forceNoVectors;
	// VPV-HOOK
	vectorEngine.MotionShadowCheck(pipelineSlots.Runs(PipelineSlots::Id::MotionVectorDelivery), sourceSequence, nextSourceSequence,
		DejudderLiveFieldFor(sourceSequence));
	// VPV-HOOK-END
	vpPhaseVectors = vectors;
	if (vectors)
	{
		// P5c: the P3 field view (R4 snapshot handle, else R0 live; the validity is decided above).
		if (snapshot)
		{
			const auto snapSet = vectorEstimate.Snapshot();
			motion.field = snapSet.Field();
			motion.frameTrust = snapSet.FrameTrust();
			motion.subField = snapSet.SubField();
		}
		else
		{
			const VpMotion::P3FieldSet liveSet = vectorEstimate.Live();
			motion.field = liveSet.field;
			motion.frameTrust = liveSet.frameTrust;
			motion.subField = liveSet.subField;
		}
		motion.minTrustedShare = vectorEstimate.MinTrustedShare();
		for (int slot = 0; slot < 2; ++slot)
			if (dejudderMaskSequence[slot] == nextSourceSequence && motionValidity.Current(dejudderMaskEpoch[slot]))
				motion.occlusionB = vectorEstimate.Occlusion(slot);
	}
	// WIP DEBUG (near-black residual, 2026-10-24; VP_DEBUG_PHASE_AB=1 only): are A's and B's raw
	// planes bit-identical when the source repeats a frame bit-exactly? Logs the differing luma
	// samples and the field's first vector for the first 40 pictures between frames.
	{
		static int debugCount = 0;
		if (debugCount < 40 && debugPhaseAb &&
			a.luma && b.luma && a.luma->params.w == b.luma->params.w && a.luma->params.h == b.luma->params.h)
		{
			++debugCount;
			EnsureRebaseDispatch();
			// A host-readable copy of each luma plane (a plain copy pass), then a byte compare.
			auto readBack = [&](pl_tex src, std::vector<uint8_t>& out) {
				pl_tex_params tp{};
				tp.w = src->params.w; tp.h = src->params.h; tp.format = src->params.format;
				if (src->params.format->type == PL_FMT_FLOAT)   // the field: a renderable, readable float format
					tp.format = pl_find_fmt(d3d11->gpu, PL_FMT_FLOAT, 4, 32, 32,
						static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				if (!tp.format) return false;
				tp.sampleable = true; tp.renderable = true; tp.host_readable = true;
				pl_tex copy = pl_tex_create(d3d11->gpu, &tp);
				if (!copy) return false;
				pl_shader sh = pl_dispatch_begin(rebaseDispatch);
				pl_sample_src sample{};
				sample.tex = src;
				sample.components = src->params.format->num_components;
				bool ok = pl_shader_sample_direct(sh, &sample);
				if (ok)
				{
					pl_dispatch_params dp{};
					dp.shader = &sh;
					dp.target = copy;
					ok = pl_dispatch_finish(rebaseDispatch, &dp);
				}
				else
					pl_dispatch_abort(rebaseDispatch, &sh);
				if (ok)
				{
					out.resize(static_cast<size_t>(tp.w) * tp.h * tp.format->texel_size);
					pl_tex_transfer_params t{};
					t.tex = copy;
					t.ptr = out.data();
					ok = pl_tex_download(d3d11->gpu, &t);
				}
				pl_tex_destroy(d3d11->gpu, &copy);
				return ok; };
			std::vector<uint8_t> da, db;
			size_t differing = 0;
			const bool ok = readBack(a.luma, da) && readBack(b.luma, db) && da.size() == db.size();
			if (ok)
				for (size_t i = 0; i < da.size(); ++i) differing += da[i] != db[i] ? 1 : 0;
			// The field's vectors (rgba32f: vx, vy, trust, error) - mean and max length of trusted blocks.
			double meanLen = 0.0, maxLen = 0.0, gx = 0.0, gy = 0.0;
			size_t trusted = 0, blocks = 0;
			if (motion.field)
			{
				std::vector<uint8_t> df;
				if (readBack(motion.field, df) && df.size() == static_cast<size_t>(motion.field->params.w) * motion.field->params.h * 16)
				{
					const float* f = reinterpret_cast<const float*>(df.data());
					blocks = df.size() / 16;
					for (size_t i = 0; i < blocks; ++i)
						if (f[i * 4 + 2] > 0.5f)
						{
							const double l = std::sqrt(double(f[i * 4]) * f[i * 4] + double(f[i * 4 + 1]) * f[i * 4 + 1]);
							meanLen += l; maxLen = (std::max)(maxLen, l); gx += f[i * 4]; gy += f[i * 4 + 1]; ++trusted;
						}
					if (trusted) { meanLen /= trusted; gx /= trusted; gy /= trusted; }
				}
			}
			DebugLog::Log("DEBUG phase A/B: sequence %llu next %llu phase %.3f vectors %d: luma bytes differing %zu of %zu%s; field: %zu of %zu blocks trusted, |v| mean %.3f max %.3f, mean v (%.3f, %.3f)",
				static_cast<unsigned long long>(sourceSequence), static_cast<unsigned long long>(nextSourceSequence),
				vpOutputPhase, vectors ? 1 : 0, differing, da.size(), ok ? "" : " (readback failed)", trusted, blocks, meanLen, maxLen, gx, gy);
		}
	}
	// Bug hunt 3 F5: a standalone gap pair on an active de-judder at its strength.
	vpPhaseA = a;
	vpPhaseB = b;
	vpPhaseMotion = motion;
	vpPhaseT = static_cast<float>(vpOutputPhase) * (vpGapFillStandalone ? dejudderPolicy.vpGapPhaseScale : 1.0f);
	// B3: made in the draw from the processed planes (the same inputs).
	vpPhaseDeferred = EntranceOnce();
	if (!vpPhaseDeferred && !RunVpPhaseInterpolation())
		return;
	vpPhasePlanesValid = true;
	++vpPhasePictures;
	if (!vectors)
		++vpHeldPictures;
	static GeneralRate::LogInterval picturesLog = GeneralRate::LogInterval::ForFrames(2400);   // fixed-constant batch B7 2026-10-18: seconds, not frames
	if (picturesLog.DueOr(vpPhasePictures == 1))
		LogVpTimeline("pictures");
}

bool LibplaceboVideoRenderer::Impl::VpPhaseDrawn(const pl_frame& image) const
{
	return vpPhasePlanesValid && image.num_planes == 2 && image.planes[0].texture == textures[0] &&
		image.planes[1].texture == textures[1];
}

void LibplaceboVideoRenderer::Impl::LogVpTimeline(const char* what)
{
	// M2 P5b-5: the format and arguments are VpMotion::VpTimelineLine's (TimelinePlanner.h, CPU-tested).
	VpMotion::VpTimelineCounters counters;
	counters.phasePictures = vpPhasePictures;
	counters.heldPictures = vpHeldPictures;
	counters.skippedFrames = vpSkippedFrames;
	counters.filmUpdates = dejudderPolicy.vpFilmUpdates;
	counters.gapPairs = vpGapPairs;
	DebugLog::Log("%s", VpMotion::VpTimelineLine(what, vpOutputPhase, counters, dejudderPolicy.vpTimeline).c_str());
}

void LibplaceboVideoRenderer::Impl::PublishDejudderStatus(bool force)
{
	const ULONGLONG now = GetTickCount64();
	if (!force && dejudderStatusTick != 0 && now - dejudderStatusTick < 500)
		return;
	dejudderStatusTick = now;
	std::string text;
	const bool on = activeSettings.dejudderStrength > 0.0 || activeSettings.dejudderStrengthEven > 0.0 ||
		activeSettings.occlusionMask || activeSettings.interpolation != InterpolationPolicy::Mode::Off;
	if (on)
	{
		char line[260];
		const DejudderExtraStats& st = dejudderExtraStats;
		// R1: the sigma-delta's GPU time per extra, in the mode in use.
		char dither[64] = "";
		if (st.ditherMsExternal > 0.0 || st.ditherMsOwn > 0.0)
			std::snprintf(dither, sizeof(dither), ", dither %.2f ms (%s)",
				st.ditherLastExternal ? st.ditherMsExternal : st.ditherMsOwn,
				st.ditherLastExternal ? "de-judder vectors" : "own search");
		if (activeSettings.interpolation != InterpolationPolicy::Mode::Off && !interpolating &&
			dejudderReason != DejudderPolicy::Reason::Active)
			std::snprintf(line, sizeof(line), "interpolation: %s", InterpolationPolicy::Name(dejudderPolicy.interpolationDecision.reason));
		else if (dejudderReason != DejudderPolicy::Reason::Active)
			std::snprintf(line, sizeof(line), "%s", DejudderPolicy::Name(dejudderReason));
		else if (interpolating && vpTimelineActive)
			std::snprintf(line, sizeof(line), "interpolating per refresh (%s), ratio %.3f, %llu pictures, %llu held, %llu frames skipped, "
				"%llu gap pairs, %llu restarts", InterpolationPolicy::Name(activeSettings.interpolationFallback),
				dejudderPolicy.interpolationDecision.ratio, vpPhasePictures, vpHeldPictures, vpSkippedFrames, vpGapPairs, dejudderPolicy.vpTimeline.Restarts());
		else if (interpolating)
			std::snprintf(line, sizeof(line), "interpolating (%s), ratio %.3f, +%d pictures %.1f ms, late %llu, full %llu, held %llu%s",
				InterpolationPolicy::Name(activeSettings.interpolationFallback), st.cadenceRatio, st.lastExtrasPerFrame,
				st.lastMs, st.late + st.nextFrameLate, st.swapchainFull, st.holds, dither);
		else if (!dejudderExtrasAllowed)
			std::snprintf(line, sizeof(line), "mask only (cadence %.2f)", st.cadenceRatio);
		else
			std::snprintf(line, sizeof(line), "s=%.2f, cadence %.2f, +%d extras %.1f ms, late %llu, full %llu, held %llu%s",
				activeSettings.dejudderStrength, st.cadenceRatio, st.lastExtrasPerFrame, st.lastMs,
				st.late + st.nextFrameLate, st.swapchainFull, st.holds, dither);
		text = line;
		// Estimate ahead: the share of the matcher runs done in an extra's
		// spare time instead of before the frame's own render.
		if (activeSettings.dejudderEstimateAhead && dejudderAhead.Hits() + dejudderAhead.Sequential() > 0)
		{
			char ahead[128];
			std::snprintf(ahead, sizeof(ahead), ", ahead %.0f%%%s%s", 100.0 * static_cast<double>(dejudderAhead.Hits()) /
				static_cast<double>(dejudderAhead.Hits() + dejudderAhead.Sequential()),
				dejudderAhead.LastReason() != Dejudder::AheadSchedule::Reason::Ahead ? " (last: " : "",
				dejudderAhead.LastReason() != Dejudder::AheadSchedule::Reason::Ahead ?
					(std::string(Dejudder::AheadSchedule::Name(dejudderAhead.LastReason())) + ")").c_str() : "");
			text += ahead;
		}
		// Render-ahead: queued, its lead, late frames and the gate's level.
		if (RenderAheadActive() && renderAheadFrames > 0)
		{
			char ahead[160];
			std::snprintf(ahead, sizeof(ahead), ", render-ahead lead %.1f ms, late %llu, dropped %llu, gate level %d",
				renderAheadLead * 1000.0, renderAheadLate, renderAheadDropped, renderAheadPresenter.gate.Level());
			text += ahead;
		}
		// Efficiency: new frames whose sigma-delta took the de-judder's
		// vectors (its own search skipped).
		if (ditherVectorsNextUsed > 0)
		{
			char next[64];
			std::snprintf(next, sizeof(next), ", dither new-frame vectors %llu",
				static_cast<unsigned long long>(ditherVectorsNextUsed));
			text += next;
		}
		if (activeSettings.dejudderPulldown)
		{
			char pulldown[128];
			const int cycle = pulldownCadence >> 16, unique = pulldownCadence & 0xffff;
			if (pulldownCadence != 0 && unique > 0 && pulldownFilmHz > 0.0)
				std::snprintf(pulldown, sizeof(pulldown), ", pulldown %s, %llu repeats skipped",
					Pulldown::Describe(pulldownFilmHz, pulldownFilmHz * cycle / unique, unique, cycle).c_str(),
					static_cast<unsigned long long>(pulldownSkipped));
			else
				std::snprintf(pulldown, sizeof(pulldown), ", pulldown: none found");
			text += pulldown;
		}
		// "mask on" only when the mask was used within the last second.
		if (activeSettings.occlusionMask)
			text += dejudderMaskUsedTick != 0 && now - dejudderMaskUsedTick < 1000 ? std::string(", mask on") :
				", " + (dejudderMaskState.empty() ? std::string("mask waiting") : dejudderMaskState);
		// Planner 2026-10-16: the motion source on the stats line (NVIDIA optical-flow engine,
		// Turing or later, driver 528+; without it the block searches, as before).
		if (!motionSourceLogged.empty())
			text += ", motion: " + motionSourceLogged;
	}
	if (activeSettings.vectorDemand)
	{
		const std::string vectorLine = VectorDemand::StatusLine(vectorDemand);
		if (!vectorLine.empty())
			text += (text.empty() ? std::string() : std::string("; ")) + vectorLine;
	}
	std::lock_guard<std::mutex> guard(dejudderStatusMutex);
	if (repeatReuse.Active())
		text += (text.empty() ? std::string() : std::string("; ")) + repeatReuse.StatusLine();
	dejudderStatus = std::move(text);
}

void LibplaceboVideoRenderer::Impl::ReleaseDejudder()
{
	dejudderMaskSequence[0] = dejudderMaskSequence[1] = 0;
	frameInterpolator.reset();
	dejudder.reset();
	VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserMotionMatcher);   // W2-C1 C1-3: the matcher's memory is freed
	dejudderLiveFieldFrom = 0;
	motionSegment.Retract(VpMotion::Direction::Forward);
	vectorEstimate.dejudderMotionSourceKey = -1;
	// 6d: the cache and the rendered warp hold textures (and a dispatch)
	// of this device (audit stage10-6d blocker 1).
	dejudderCache.Release();
	repeatReuse.Release();   // WP-24: its probe hook while the GPU exists; its fallback entry
	dejudderRenderedWarp.reset();
	dejudderVectorField.reset();
	// The next-frame vector field owns a texture of this device too (the sigma-delta's vectors on B's first
	// present): released with the device, never left for ~Impl (access violation after the GPU was gone).
	dejudderNextVectorField.reset();
	dejudderNextVectors = nullptr;
	dejudderNextVectorsSequence = 0;
	ditherNextGate.Clear();
	for (pl_tex& tex : dejudderNextTextures)
		if (tex && d3d11) pl_tex_destroy(d3d11->gpu, &tex);
	dejudderNextUploadedSequence = 0;
	dejudderNextUpload.Clear();
	dejudderAheadUpload.Clear();
	for (pl_tex& tex : dejudderAheadTextures)
		if (tex && d3d11) pl_tex_destroy(d3d11->gpu, &tex);
	if (dejudderRgbScratch && d3d11) pl_tex_destroy(d3d11->gpu, &dejudderRgbScratch);
	dejudderAhead.Reset();
	dejudderAheadPayload.reset();
	dejudderAheadPayloadSequence = 0;
	dejudderLayout.valid = false;
}

// Item g (RenderLocked, before the target is acquired): Stage B's frame that no refresh shows (true: RenderLocked
// returns true, as before) and the phase picture of this draw. phaseUploadable = !nativeRgbUpload && !rgbRawUpload,
// fullRange = image.repr.levels == PL_COLOR_LEVELS_FULL (evaluated by the caller; both pure).
// PHASE 1 (refs/107 s2.4 item 1; owner E3): the five Impl bodies of this file that a Bill call site reaches (VpStageBFrame,
// DrawVpPhasePicture, PlanVpOutputs, RegisterDejudderCacheFrame, NoteTimelinePlanned) return at once in the Original
// pathway (`// VPV-GATED`; the settings seam, CoreSegments.inl UpdatePipelineSlots, released the timeline and the phase
// picture when the pathway was entered), so each call site does nothing there whatever its own guard says.
bool LibplaceboVideoRenderer::Impl::VpStageBFrame(uint64_t sourceSequence, uint64_t nextSourceSequence, bool phaseUploadable,   // VPV-GATED
	bool lossless422Upload, bool fullRange)
{
	if (pipelineSlots.OriginalPathway()) return false;
	vpPhasePlanesValid = false;
	if (vpTimelineActive)
	{
		// Stage B: no refresh in this frame's interval (the source is faster): noted for
		// the motion run (contiguity), but not uploaded (the last planes are reused), not
		// estimated and not presented (as smooth motion skips).
		if (vpSkipOutput)
		{
			// VP deband's hold carries over the frame (unless the motion was invalidated,
			// which already forgot it).
			debander.CarryHold(sourceSequence);
			// Hunt 2 (06-interp L4): a pulldown film update (the same film time) is shown with
			// the next film frame; counted apart from the frames a faster source skips.
			if (dejudderPolicy.vpSkipIsUpdate)
			{
				if (++dejudderPolicy.vpFilmUpdates == 1)
					LogVpTimeline("first pulldown film update (shown with the next film frame)");
			}
			else if (++vpSkippedFrames == 1)
				LogVpTimeline("first source frame skipped (the source is faster than the display)");
			return true;
		}
		if (phaseUploadable)
			PrepareVpPhasePicture(sourceSequence, nextSourceSequence, lossless422Upload,
				fullRange);
	}
	return false;
}

// The draw piece: inside RenderLocked's render callback (re-entered on an NLS hook retry), after the temporal
// renderer's own draws; the picture at this refresh's phase in place of A's planes (a composed picture keeps A:
// hold), or B3's processed picture. entranceOnceDone / bAlreadyA / bAsADone are the callback's (P4).
template<class AsADone>
bool LibplaceboVideoRenderer::Impl::DrawVpPhasePicture(const pl_frame& source, const pl_frame& image, pl_frame& frame,   // VPV-GATED
	uint64_t sourceSequence, uint64_t nextSourceSequence, bool& entranceOnceDone, bool& bAlreadyA, const AsADone& bAsADone)
{
    if (pipelineSlots.OriginalPathway()) return false;
    bool phasePicture = vpPhasePlanesValid && source.num_planes == 2 &&
        source.planes[0].texture == textures[0] && source.planes[1].texture == textures[1];
    if (phasePicture && vpPhaseDeferred)
    {
        const OwnedEntrance* heldA = nullptr;
        entranceOnceDone = EntranceOncePhasePicture(source, sourceSequence, nextSourceSequence, &image, frame, &heldA);
        bAlreadyA = bAsADone();
        if (!entranceOnceDone && bAlreadyA && heldA && !(rebaseFade < 1.0 && !temporalRenderer.Active()))
        {
            frame = EntranceSegment::OwnedFrame(*heldA, source);   // the processed A, held (no stage runs)
            entranceOnceDone = true;
        }
        else if (!entranceOnceDone && !RunVpPhaseInterpolation())
            phasePicture = false;   // A (never waiting)
    }
    if (phasePicture && !entranceOnceDone)
    {
        frame.planes[0].texture = vpPhasePlanes.luma;
        frame.planes[1].texture = vpPhasePlanes.chroma;
    }
	return phasePicture;
}

// M2 P5b-5 (item h): the motion-engine either/or (owner 2026-10-15, motion_engine): VP's motion handling or
// libplacebo's smooth motion, chosen by the profile rules (deinterlacing is independent of it), and no mixing for
// a file frame shown without motion (ABI 37). The render loop's old statements (m_impl-> dropped); Bill's
// TemporalRenderer::WillMix / Begin then read the returned settings as before. Render loop, renderMutex held.
MotionConfig::Settings LibplaceboVideoRenderer::Impl::ResolveMotionEngine(bool currentFileMotionOff)
{
	MotionConfig::Settings motionSettings=activeSettings.motion;
	{
	    const auto engine=DejudderPolicy::ResolveEngine(motionSettings.engine,
	        DejudderPolicy::Configured(activeSettings.dejudderStrength,activeSettings.dejudderStrengthEven),
	        motionSettings.smoothMotion!="off");
	    const bool smooth=engine==DejudderPolicy::MotionEngine::SmoothMotion;
	    if(!smooth)motionSettings.smoothMotion="off";
	    if(smooth!=smoothMotionEngine||!dejudderPolicy.motionEngineLogged) {
	        if(smooth!=smoothMotionEngine)
	            RequestMotionInvalidate(MotionValidity::Cause::SmoothMotionMix);
	        smoothMotionEngine=smooth;
	        dejudderPolicy.motionEngineLogged=true;
	        DebugLog::Log("Motion engine: %s (motion_engine=%s)",smooth?"libplacebo smooth motion; VP's de-judder off":
	            "VP (de-judder); smooth motion off",motionSettings.engine.c_str());
	        DiagnosticHistory::Global().RecordDecision("motion-engine",
	            smooth?"libplacebo smooth motion":"VP de-judder",smooth?1:0);
	    }
	}
	// ABI 37: a file frame shown without motion (playback rate != 1, paused)
	// is not mixed by smooth motion either.
	if(currentFileMotionOff)motionSettings.smoothMotion="off";
	return motionSettings;
}

namespace
{
	// The renderer side of VpMotion::PlanVpTimeline (the old statements, at the old points).
	template<class ImplT>
	struct ImplTimelineHost
	{
		ImplT& impl;
		bool TakeReanchor()
		{
			return (VpResync::TakeIfDriving(impl.resync,impl.resyncDrive,VpResync::Reader::Timeline)&
				(VpIndicator::Bit(VpIndicator::kContinuity)|VpIndicator::Bit(VpIndicator::kTiming)))!=0;
		}
		bool TemporalActive() const { return impl.temporalRenderer.Active(); }
		bool VpTimelineWanted() const { return impl.VpTimelineWanted(); }
		double QpcFrequency() const
		{
			LARGE_INTEGER qpcFrequencyTimeline{};
			return QueryPerformanceFrequency(&qpcFrequencyTimeline) && qpcFrequencyTimeline.QuadPart > 0 ?
				static_cast<double>(qpcFrequencyTimeline.QuadPart) : 0.0;
		}
		uint64_t ReaderGeneration(uint64_t frameGeneration, uint64_t sourceSequence) const
		{
			return VpResync::ReaderGeneration(impl.resync,frameGeneration,sourceSequence);
		}
		double TimelineDisplayHz() const { return impl.TimelineDisplayHz(); }
		void LogVpTimeline(const char* what) { impl.LogVpTimeline(what); }
	};
}

// M2 P5b-5 (item h, R10 PlanOutputs; DejudderExtension calls it through the binding): the smooth-motion mixing
// log (moved from just before the render loop's motionOutputs copy), then VP's per-vsync timeline (exclusive
// with smooth motion, never in low latency): this frame's refreshes, each at its true phase between A and B.
// Render loop, renderMutex held. True: handled (the plan is the timeline's, or the host's list left as it was).
bool LibplaceboVideoRenderer::Impl::PlanVpOutputs(RendererExtension::OutputPlan& plan)   // VPV-GATED
{
	if (pipelineSlots.OriginalPathway()) return false;   // PHASE 1: not planned, Bill's list is drawn
	if(temporalRenderer.Mixing()!=dejudderPolicy.smoothMotionMixingLogged) {
	    RequestMotionInvalidate(MotionValidity::Cause::SmoothMotionMix);
	    dejudderPolicy.smoothMotionMixingLogged=temporalRenderer.Mixing();
	    DebugLog::Log("Motion: smooth motion mixing %s (%s)",dejudderPolicy.smoothMotionMixingLogged?"on":"off",
	        temporalRenderer.status.c_str());
	}
	ImplTimelineHost<Impl> host{ *this };
	VpMotion::PlanVpTimeline(activeSettings, dejudderPolicy, plan, host);
	return true;
}

// M2 P5b-6 (item i): the de-judder rendered-picture cache and the extras' geometry for this frame (RenderLocked,
// after the hook list is built, at today's position: the registration order inside the frame's hooks is unchanged).
// Owner change: stays IN PLACE as this one call; retired when P4 moves the hook block (CollectHooks). The old
// statements; nlsSubmitted = (submittedNls != nullptr), evaluated by the caller.
void LibplaceboVideoRenderer::Impl::RegisterDejudderCacheFrame(const pl_frame& target, const pl_frame& renderImage,   // VPV-GATED
	const SubtitleCutPasteGeometry& relocatedSubtitle, bool subtitleCompositionSucceeded, bool nlsSubmitted, bool cadenceRepeat,
	uint64_t sourceSequence)
{
    if (pipelineSlots.OriginalPathway()) return;   // PHASE 1: no cache stage, no geometry (Bill's hooks only)
    dejudderSubtitleComposed = subtitleCompositionSucceeded;
    {
        const pl_tex framebuffer = target.planes[0].texture;
        const SubtitleBoxRect subtitleHolds[] = { relocatedSubtitle.source, relocatedSubtitle.generatedCleanup,
            relocatedSubtitle.destination, relocatedSubtitle.layoutCoverage };
        DejudderCacheStage::Frame cacheFrame;
        cacheFrame.extrasMayFollow = dejudderReason == DejudderPolicy::Reason::Active &&
            dejudderExtrasAllowed && dejudder && !cadenceRepeat && sourceSequence != 0;
        cacheFrame.geometryForVectors = !cacheFrame.extrasMayFollow && vectorDemand.Serves(VectorDemand::Consumer::SigmaDelta) &&
            dejudder && !cadenceRepeat && sourceSequence != 0 && !interpolating && !vpTimelineActive;   // vector engine: the sigma-delta's vectors need the geometry
        cacheFrame.sourceSequence = sourceSequence;
        cacheFrame.sourceCrop = renderImage.crop;
        cacheFrame.targetCrop = target.crop;
        cacheFrame.framebufferW = framebuffer ? static_cast<float>(framebuffer->params.w) : 0.0f;
        cacheFrame.framebufferH = framebuffer ? static_cast<float>(framebuffer->params.h) : 0.0f;
        cacheFrame.nlsSubmitted = nlsSubmitted;
        cacheFrame.warpRendered = activeSettings.dejudderWarpRendered;
        cacheFrame.interpolating = interpolating;
        cacheFrame.subtitleHolds = subtitleCompositionSucceeded ? subtitleHolds : nullptr;
        cacheFrame.numSubtitleHolds = 4;
        dejudderCacheStage.Register(cacheFrame, [this]() { return KnownNlsMapping(); }, frameHooks);
    }
}
// M2 P5b-5 (audit F1): after the R10 call. Not planned (no binding / hub, or the plan threw): no stale timeline
// flags, Bill's draw of the host's list, and the visible "timeline" fallback; planned: released.
void LibplaceboVideoRenderer::Impl::NoteTimelinePlanned(bool planned)   // VPV-GATED
{
	if (pipelineSlots.OriginalPathway()) return;   // PHASE 1: no timeline, no "timeline" fallback line (released at the seam)
	if (VpMotion::TimelineNotPlanned(dejudderPolicy, planned))
		EngageFallback("timeline", "de-judder module did not plan");
	else
		ReleaseFallback("timeline");
}

// M2 P5b-5 (audit F5): the hold fraction alone under the hold's lock (no copy of the whole plan per frame).
double LibplaceboVideoRenderer::CurrentQueueHoldFraction() const
{
	std::lock_guard<std::mutex> guard(m_queueHoldMutex);
	return m_queueHold.fraction;
}
