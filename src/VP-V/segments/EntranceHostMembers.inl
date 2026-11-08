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
		vpPhaseMotion.field = dejudder->SnapField();
		vpPhaseMotion.frameTrust = dejudder->SnapFrameTrust();
		vpPhaseMotion.subField = dejudder->SnapSubField();
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
