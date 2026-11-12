// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P4-5 (doc 52 section 4.1 items a, b): the entrance host's B3 helpers (entrance_once field ahead, the phase picture, the de-judder
// extra), the source-resolution crossfade mix and the entrance GPU drain call, moved VERBATIM out of LibplaceboVideoRenderer.cpp
// (only the signatures gained the LibplaceboVideoRenderer::Impl:: qualification; default arguments stay on the declarations in Impl).
// Out-of-line member definitions of the renderer's Impl: included ONCE by LibplaceboVideoRenderer.cpp right after the Impl struct; not a
// stand-alone header. Render thread (renderMutex held), as before.
#pragma once

void LibplaceboVideoRenderer::Impl::DrainEntranceBeforeRender()
{
	EntranceGpuDrain::Mode mode = EntranceGpuDrain::Mode::Off;
	if (!pipelineSlots.OriginalPathway()) EntranceGpuDrain::Parse(activeSettings.entranceGpuDrain, mode);
	pl_gpu gpu = d3d11 ? d3d11->gpu : nullptr;
	std::string summary;
	entranceDrain.BeforeRender(gpu ? mode : EntranceGpuDrain::Mode::Off, pipelineSlots.Runs(PipelineSlots::Id::ScalingRebase),
		[gpu]() { pl_gpu_flush(gpu); }, [gpu]() { pl_gpu_finish(gpu); }, &summary);
	if (!summary.empty()) DebugLog::Log("%s", summary.c_str());
}

bool LibplaceboVideoRenderer::Impl::EntranceOnceFieldAhead(uint64_t aSequence, uint64_t bSequence, std::string& why)
{
	if (!dejudder)
	{
		why = "no motion estimate";
		return false;
	}
	if (EntranceOnceSnapshotFor(aSequence) && dejudderLiveFieldFrom == bSequence)
		return true;   // already estimated ahead for this A
	if (entranceOnceAheadFailedFor == aSequence)
	{
		why = entranceOnceAheadFailure;   // tried once for this A (never every refresh)
		return false;
	}
	const bool snapshotInUse = EstimateAhead(aSequence, bSequence, true, nullptr, true);
	if (snapshotInUse)
	{
		entranceOnceSnapFor = aSequence;
		entranceOnceSnapEpoch = motionValidity.Epoch();
		entranceOnceAheadNow = true;
	}
	if (snapshotInUse && dejudderAhead.LastReason() == Dejudder::AheadSchedule::Reason::Ahead && dejudderLiveFieldFrom == bSequence)
		return true;
	why = Dejudder::AheadSchedule::Name(dejudderAhead.LastReason());
	// A failure after the snapshot (upload, estimate) is not retried for this A (it would redo
	// the copy every refresh); a skip before it (C not queued yet) is retried on the next picture.
	if (snapshotInUse)
	{
		entranceOnceAheadFailedFor = aSequence;
		entranceOnceAheadFailure = why;
	}
	return false;
}

bool LibplaceboVideoRenderer::Impl::EntranceOncePhasePicture(const pl_frame& source, uint64_t sequence, uint64_t nextSequence, const pl_frame* image,
	pl_frame& frame, const OwnedEntrance** heldA)
{
	entrance.SetCurrentA(sequence);
	if (heldA) *heldA = nullptr;
	if (EntranceOnceCrossfading())
	{
		entrance.OnceFallback("source-resolution crossfade");
		return false;
	}
	const OwnedEntrance* pa = entrance.ProcessedA(source, sequence, nextSequence, image);
	if (!pa)
		return false;
	pl_frame rawB = source;
	rawB.planes[0].texture = vpPhaseB.luma;
	rawB.planes[1].texture = vpPhaseB.chroma;
	const OwnedEntrance* pb = entrance.ProcessedB(rawB, nextSequence, sequence);
	// (i) the estimate ahead replaced the live (A, B) field: A's picture reads the snapshot.
	if (vpPhaseVectors && EntranceOnceSnapshotFor(sequence) && dejudderLiveFieldFrom != sequence)
	{
		const auto snapSet = vectorEstimate.Snapshot();   // P5c: R4 of the P3 field view
		vpPhaseMotion.field = snapSet.Field();
		vpPhaseMotion.frameTrust = snapSet.FrameTrust();
		vpPhaseMotion.subField = snapSet.SubField();
	}
	if (heldA && !pa->identity) *heldA = pa;
	if (!pb)
		return false;
	if (pa->identity && pb->identity)
		return false;   // no stage changes the picture: today's path draws the same picture
	const pl_frame drawnA = EntranceSegment::OwnedFrame(*pa, source), drawnB = EntranceSegment::OwnedFrame(*pb, rawB);
	if (drawnA.num_planes != 2 || drawnB.num_planes != 2)
	{
		entrance.OnceFallback("processed picture not in two planes");
		return false;
	}
	if (entrance.ForcedFallback())
	{
		entrance.OnceFallback("test: forced late fallback");
		return false;
	}
	Dejudder::Planes a = vpPhaseA, b = vpPhaseA, out;
	a.luma = drawnA.planes[0].texture;
	a.chroma = drawnA.planes[1].texture;
	b.luma = drawnB.planes[0].texture;
	b.chroma = drawnB.planes[1].texture;
	if (!frameInterpolator)
		frameInterpolator = std::make_unique<Interpolator::FrameInterpolator>(log, d3d11->gpu);
	frameInterpolator->SetGrain(activeSettings.interpolationGrain);
	if (!frameInterpolator->Run(a, b, vpPhaseMotion, vpPhaseT, activeSettings.interpolationFallback, out,
		vpPhaseA.luma->params.w, vpPhaseA.luma->params.h))
	{
		entrance.OnceFallback("interpolation on the processed planes failed (" + frameInterpolator->LastError() + ")");
		return false;
	}
	frame = drawnA;
	frame.planes[0].texture = out.luma;
	frame.planes[1].texture = out.chroma;
	entrance.OnceWorking();
	return true;
}

bool LibplaceboVideoRenderer::Impl::EntranceOnceExtra(const pl_frame& image, uint64_t sequence, uint64_t nextSequence, bool interpolating,
	const Dejudder::Planes& a, Dejudder::Planes& processedA, Dejudder::Planes& processedB, const pl_frame*& processedFrame)
{
	processedFrame = nullptr;
	entrance.SetCurrentA(sequence);
	ReleaseFallback("entrance once fill");   // engaged again below only when this extra warps processed planes
	if (EntranceOnceCrossfading())
	{
		entrance.OnceFallback("source-resolution crossfade");
		return false;
	}
	if (image.num_planes != 2 || image.planes[0].texture != a.luma || image.planes[1].texture != a.chroma)
	{
		entrance.OnceFallback("composed picture");
		return false;
	}
	const OwnedEntrance* pa = entrance.ProcessedA(image, sequence, nextSequence, &image);
	if (!pa)
		return false;
	const OwnedEntrance* pb = nullptr;
	if (interpolating)
	{
		if (!dejudderNextTextures[0] || !dejudderNextTextures[1])
		{
			entrance.OnceFallback("the next frame's planes are not uploaded");
			return false;
		}
		pl_frame rawB = image;
		rawB.planes[0].texture = dejudderNextTextures[0];
		rawB.planes[1].texture = dejudderNextTextures[1];
		pb = entrance.ProcessedB(rawB, nextSequence, sequence);
		if (!pb)
			return false;
	}
	if (pa->identity && (!pb || pb->identity))
		return false;   // no stage changes the picture: today's path draws the same picture
	entranceOnceExtraFrame = EntranceSegment::OwnedFrame(*pa, image);
	pl_frame drawnB{};
	if (pb)
	{
		pl_frame rawB = image;
		rawB.planes[0].texture = dejudderNextTextures[0];
		rawB.planes[1].texture = dejudderNextTextures[1];
		drawnB = EntranceSegment::OwnedFrame(*pb, rawB);
	}
	if (entranceOnceExtraFrame.num_planes != 2 || (pb && drawnB.num_planes != 2))
	{
		entrance.OnceFallback("processed picture not in two planes");
		return false;
	}
	processedA = a;
	processedA.luma = entranceOnceExtraFrame.planes[0].texture;
	processedA.chroma = entranceOnceExtraFrame.planes[1].texture;
	if (pb)
	{
		processedB = a;
		processedB.luma = drawnB.planes[0].texture;
		processedB.chroma = drawnB.planes[1].texture;
	}
	// Audit MED 5: the source warp's occlusion fill reads B at the estimate size; on processed
	// planes it is off - shown, never silent.
	if (!interpolating && dejudder && dejudder->GetSettings().occlusionFill)
		EngageFallback("entrance once fill", "the source warp's occlusion fill is off on processed planes");
	else
		ReleaseFallback("entrance once fill");
	processedFrame = &entranceOnceExtraFrame;
	return true;
}

bool LibplaceboVideoRenderer::Impl::RenderCrossfadeMix(const pl_frame& from, const pl_frame& to, const pl_frame& destination,
	const pl_render_params& parameters, const pl_color_space* historyColor, double weight)
{
	const pl_hook* late = extendedRangeHooks.LateRestore();
	bool ok = crossfadeMixer.RenderFrom(renderer, d3d11->gpu, from, destination, parameters, late);
	if (ok)
	{
		std::vector<const pl_hook*> mixed = crossfadeMixer.WithMix(parameters.hooks, parameters.num_hooks, late, weight);
		// The mix is a texture-input PRE_OUTPUT hook: libplacebo 7.360 then drops the output
		// encoding unless the repair runs first at OUTPUT (OutputEncodingRepair.h).
		const pl_render_errors errors = pl_renderer_get_errors(renderer);
		if (std::find(mixed.begin(), mixed.end(), outputEncodingRepair.Get()) == mixed.end() &&
			OutputEncodingRepair::Needed(mixed, &errors, destination.lut ? destination.lut_type : PL_LUT_UNKNOWN))
		{
			outputEncodingRepair.SetTarget(destination.repr);
			mixed.insert(mixed.begin(), outputEncodingRepair.Get());
		}
		pl_render_params withMix = parameters;
		withMix.hooks = mixed.data();
		withMix.num_hooks = static_cast<int>(mixed.size());
		const unsigned long long before = crossfadeMixer.Mixed();
		++rebaseFadeRender;
		ok = temporalRenderer.Render(renderer, to, destination, withMix, pictureProcessingEpoch, historyColor) && crossfadeMixer.Mixed() > before;
	}
	const int state = ok ? 1 : 0;
	if (state != crossfadeMixLogged)
	{
		crossfadeMixLogged = state;
		if (ok)
			DebugLog::Log("Source resolution crossfade: mixed in the output signal domain (VP hooks; BTB/WTW carried, "
				"the late restore per path)");
		else
			DebugLog::Log("Source resolution crossfade: VP's signal-domain mix failed (%llu failures); libplacebo's frame "
				"mixer instead (it clips BTB while the fade runs)", crossfadeMixer.Failures());
	}
	return ok;
}

// M2 P4 g (refs/110 s1.4): the frame's draw - the second lambda of NlsHookRecovery::RenderOwned in RenderLocked (merged output
// segments, repeat reuse, the phase picture, the entrance and the re-base crossfade, then Bill's temporal render) - moved VERBATIM
// (dedent only); the lambda's captures became parameters. RenderOwned calls it once, and a second time after an NLS hook failure
// (the retry), with `parameters` non-const as before. Render thread, renderMutex held, as before.
bool LibplaceboVideoRenderer::Impl::DrawFrame(const pl_frame& source, const pl_frame& destination, pl_render_params& parameters, const pl_frame& image, uint64_t sourceSequence, uint64_t nextSourceSequence, bool cadenceRepeat, const pl_render_errors& renderErrors, const TemporalHistoryPolicy::Decision& temporalEntrance, bool extendedRangeRelabelled)
{
    // output_segments_merged: S2/S3 as one hook each for this draw only (restored after).
    OutputSegments::Merger::Scope mergedOutput(outputMerger.get(), parameters, &renderErrors);
    // WP-24 repeat reuse, call 2 of 3 (upstreamable): a reusable repeat runs the OUTPUT stage only.
    if (repeatReuse.TryDrawRepeat(repeatReuse.Key(), renderer, destination, parameters))
        return true;
    // Stage 12 step A: the planes libplacebo samples, re-based.
    pl_frame frame = source;
    // Bill merge (owner decision 1, TemporalHistoryPolicy): Bill's temporal renderer
    // renders its own captured history; the entrance stages do not run on its draws and
    // it gets the captured picture's own crop (never a re-based one).
    if (!temporalEntrance.runEntrance)
    {
        return temporalRenderer.Render(renderer,frame,destination,parameters,pictureProcessingEpoch,
            extendedRangeRelabelled ? &source.color : nullptr);
    }
    // Stage B: the picture at this refresh's phase in place of A's planes (a
    // composed picture keeps A: hold). Everything after runs on it unchanged.
    entrance.NoteSourceFrame(sourceSequence);
    // B3 (pipeline_entrance_once): the picture from the processed A and B, no stages after.
    bool entranceOnceDone = false;
    // Audit HIGH 1: B was processed as its own A - A's pictures must not advance the
    // stateful stages again (the per-picture path for any late fallback).
    // (Evaluated after the B3 picture: its first call may process B as B's A.)
    auto bAsADone = [&]() { return EntranceOnce() && entrance.BAsAFor() == sourceSequence; };
    bool bAlreadyA = bAsADone();
    const bool phasePicture = !pipelineSlots.OriginalPathway() && DrawVpPhasePicture(source, image, frame, sourceSequence, nextSourceSequence,   // VPV-HOOK M2 P5b-4
        entranceOnceDone, bAlreadyA, bAsADone);   // Stage B's picture at this refresh's phase (PhaseTimeline.inl; in place until P4)
    // Latch review 2026-10-17: during a crossfade the path being left, drawn with the
    // other's weight (below); the frame before the entrance stages is kept for it.
    const pl_frame unrebased = frame;
    // Pipeline skeleton (B3/R2): the entrance stages in the registry's order - re-base,
    // VP deband, detail pulse + texture restore (docs/VP-detail-restore.md), all before
    // libplacebo's scaling and never on renderImage (the source-resolution monitor read
    // the original frame above). A cadence repeat of a picture drawn as itself reuses
    // its entrance result (the modules would recompute the same picture).
    if (!entranceOnceDone)
    {
        // S1, one call: the main draw (cached / B3 ring), or - B already processed as its
        // own A - the per-picture stages, never advancing (audit HIGH 1).
        EntranceSegment::Request rq;
        rq.sequence = sourceSequence;
        rq.source = &source;
        rq.image = &image;
        rq.nextSequence = nextSourceSequence;
        rq.phasePicture = phasePicture;
        rq.cadenceRepeat = cadenceRepeat;
        rq.perPicture = bAlreadyA && phasePicture;
        (void)entrance.Run(rq, frame);
    }
    if (rebaseFade < 1.0 && !temporalRenderer.Active())
    {
        // The crossfade (owner 2026-10-17): both paths of this one frame, exact at both
        // ends. Hunt 3 (lead): the path being left carries the same stages (VP deband per
        // picture, the detail pulse's and texture restore's decision of this frame on its
        // own bands), so none drops out in one step when the fade starts.
        pl_frame from = unrebased;
        EntranceSegment::Request fromRq;
        fromRq.path = EntranceSegment::Path::CrossfadeFrom;
        fromRq.sequence = sourceSequence;
        const bool fromReady = entrance.Run(fromRq, from);
        DrainEntranceBeforeRender();   // entrance_gpu_drain: after the last entrance run (the path left)
        if (fromReady)
        {
            // Hunt 3 F1: mixed in the output signal domain by VP's PRE_OUTPUT hooks
            // (CrossfadeMix.h): BTB/WTW carried exactly, the extended-range late restore
            // (calibration LUT, wide gamut) once per path before the mix.
            if (RenderCrossfadeMix(from, frame, destination, parameters,
                extendedRangeRelabelled ? &source.color : nullptr, rebaseFade))
                return true;
            // The fallback: libplacebo's frame mixer (linear light: BTB clipped while it runs).
            float timestamps[2];
            SourceResolutionMonitor::Monitor::FadeMixTimestamps(rebaseFade, timestamps);
            const pl_frame* frames[2] = { &from, &frame };
            ++rebaseFadeRender;
            const uint64_t signatures[2] = { (rebaseFadeRender << 1) | 0u, (rebaseFadeRender << 1) | 1u };
            pl_frame_mix mix{};
            mix.num_frames = 2;
            mix.frames = frames;
            mix.signatures = signatures;
            // float in libplacebo 7.360, double in the beta build.
            using MixTimestamp = std::remove_const_t<std::remove_pointer_t<decltype(pl_frame_mix{}.timestamps)>>;
            const MixTimestamp mixTimestamps[2] = { static_cast<MixTimestamp>(timestamps[0]), static_cast<MixTimestamp>(timestamps[1]) };
            mix.timestamps = mixTimestamps;
            mix.vsync_duration = 1e-3f;   // far below the 1.0 between the two: no mixer blur
            pl_render_params fade = parameters;
            fade.frame_mixer = &pl_filter_triangle;
            fade.preserve_mixing_cache = false;
            if (pl_render_image_mix(renderer, &mix, &destination, &fade))
                return true;
            if (!rebaseFadeFailedLogged)
            {
                DebugLog::Log("Source resolution: the crossfade could not be rendered; the change is shown at once");
                rebaseFadeFailedLogged = true;
            }
        }
    }
    DrainEntranceBeforeRender();   // entrance_gpu_drain: once, after the draw's last entrance run
    return temporalRenderer.Render(renderer,frame,destination,parameters,pictureProcessingEpoch,
        extendedRangeRelabelled ? &source.color : nullptr);   // Bill's epoch + VP-V history relabel
}

// M2 P4 g (refs/110 s1.4; hook approvals HA-P5c-5039/5104/5191, expiry P4-g): the three P5c transitional view reads moved VERBATIM
// (dedent only) out of LibplaceboVideoRenderer.cpp: EntranceHost::FieldOf (R2 LiveFrom), EntranceOnceSnapshotFor (R4 Snapshot) and
// DejudderLiveFieldFor (R1 LiveValidFor). Only the signatures gained the qualification. Render thread, as before.
auto LibplaceboVideoRenderer::Impl::EntranceHost::FieldOf(uint64_t sequence) const -> Field
{
	Field f;
	const VpMotion::P3FieldSet live = r.vectorEstimate.LiveFrom(sequence);   // P5c: rule R2 of the P3 field view   // VPV-HOOK
	f.field = live.field;   // VPV-HOOK
	f.grainError = live.grainError;   // VPV-HOOK
	return f;
}

bool LibplaceboVideoRenderer::Impl::EntranceOnceSnapshotFor(uint64_t aSequence) const
{
	return dejudder && aSequence != 0 && entranceOnceSnapFor == aSequence && motionValidity.Current(entranceOnceSnapEpoch) &&
		vectorEstimate.Snapshot().Usable();   // P5c: R4, the host keeps snapFor / epoch   // VPV-HOOK
}

// Whether the matcher's live outputs (field, trust, grain error) are this source
// frame's (sequence, sequence + 1) pair of the current motion epoch (rule R1 of the P3 field view, EstimateState).   // VPV-HOOK
bool LibplaceboVideoRenderer::Impl::DejudderLiveFieldFor(uint64_t sequence) const { return vectorEstimate.LiveValidFor(sequence, activeSettings.motionHistoryFixes); }   // VPV-HOOK
