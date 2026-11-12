// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// motion.estimation / DITHER VECTOR SOURCE, renderer side (W2-C8): out-of-line member definitions of the renderer's Impl,
// included ONCE by VP-V/modules/motion/consumers/PhaseTimeline.inl (itself included once by the renderer after the Impl class;
// no new line in the renderer file). The body of BuildSigmaDeltaVectorsWithoutExtras moved here from the renderer: the
// sigma-delta's vectors for a frame whose de-judder extras do not run, chosen by the ONE chain of DitherVectorSource.h
// (shared pair -> matcher field -> on-demand pair -> own search -> static), mapped to output blocks by the motion module's
// DejudderRendered::VectorField::Run exactly as before, each step counted and visible (status segment + fallback entries).
// Render thread only. The extras' own VPVEC binding (PresentDejudderExtras) is unchanged here: it moves once, with P5c-3.

// The sigma-delta's vectors on a frame whose extras do not run: B's first present takes the pair's field at the full vector
// (previousShift 0), exactly as the extras path builds them (payback of the own search).
void LibplaceboVideoRenderer::Impl::BuildSigmaDeltaVectorsWithoutExtras(uint64_t sourceSequence, uint64_t nextSourceSequence)
{
	dejudderNextVectorsSequence = 0;
	ditherNextGate.Clear();
	namespace DV = VpMotion::DitherVectors;
	DV::Facts facts;
	facts.slotRuns = pipelineSlots.Runs(PipelineSlots::Id::OutputDither);
	facts.patch = SigmaDeltaVectorsSupported();
	facts.ownSetting = activeSettings.ditherVectorsOwn;
	double motionCompensation = 0.0;
	facts.motionCompensation = customHooks.QuantizerParameter("motion_compensation", motionCompensation) && motionCompensation >= 0.5;
	facts.served = vectorDemand.Serves(VectorDemand::Consumer::SigmaDelta);
	facts.sequences = sourceSequence != 0 && nextSourceSequence != 0;
	facts.timelineDrew = interpolating || vpTimelineActive;   // the timeline / hiccup repair drew refreshes: A was not at shift 0
	facts.matcher = dejudder != nullptr;
	facts.geometryValid = dejudderGeometryValid;
	if (facts.sequences && facts.matcher)
	{
		VpMotion::VerifiedField field;
		facts.engineField = vectorEngine.FieldOf(nextSourceSequence, field) && field.pairFrom == sourceSequence;
		facts.pairServes = motionValidity.PairServes(sourceSequence, nextSourceSequence, dejudderEstimatedEpoch, dejudderEstimatedEpoch);
		facts.span = motionValidity.SpanStatus(sourceSequence, nextSourceSequence);
		facts.onDemandServed = vectorEngine.OnDemandServed(sourceSequence, nextSourceSequence);
		facts.onDemandCause = vectorEngine.OnDemandLastCause();
		facts.matcherLive = DejudderLiveFieldFor(sourceSequence);
	}
	DV::Choice choice = DV::Choose(facts);
	bool mapped = false;
	if (choice.Reuses() && d3d11 && d3d11->gpu)
	{
		const int w = static_cast<int>(std::lround(std::fabs(dejudderGeometry.targetCrop.x1 - dejudderGeometry.targetCrop.x0)));
		const int h = static_cast<int>(std::lround(std::fabs(dejudderGeometry.targetCrop.y1 - dejudderGeometry.targetCrop.y0)));
		if (w > 0 && h > 0)
		{
			if (!dejudderNextVectorField)
				dejudderNextVectorField = std::make_unique<DejudderRendered::VectorField>(log, d3d11->gpu);
			const Dejudder::Settings matcher = RenderedWarpSettings(h);
			pl_tex next = nullptr;
			// Steps 1-3 read the same matcher outputs (FieldOf's field IS the matcher's live field). A direct read: this file is
			// motion.estimation's own (the P5c view rule covers readers outside the module), so it holds with or without P5c.
			if (dejudderNextVectorField->Run(dejudder->Field(), dejudder->FrameTrust(), dejudder->GetSettings().minTrustedShare, 0.0f,
					dejudderGeometry, w, h, next, dejudder->SubField(), &matcher, dejudder->Camera(), true) && next)
			{
				dejudderNextVectors = next;
				dejudderNextVectorsSequence = nextSourceSequence;
				ditherNextGate.Bind(sourceSequence, nextSourceSequence, dejudderEstimatedEpoch);
				dejudderNextVectorsGeometry = dejudderGeometry;
				dejudderNextVectorsW = w;
				dejudderNextVectorsH = h;
				mapped = true;
			}
		}
		else
			mapped = false;   // an empty destination: nothing to map (counted as MapFailed, as the own search covers B)
	}
	DV::Tracker& tracker = DV::Registry::ForHost(this);
	const unsigned long long nowMs = GetTickCount64();
	choice = tracker.Note(choice, mapped, nowMs);
	for (const FallbackCatalog::Decision& decision : tracker.Decisions(nowMs))
		SyncFallback(decision);
	if (tracker.TakeChanged())
		DebugLog::Log("Sigma-delta vectors on the new frame's present: %s", tracker.StatusSegment().c_str());
}
