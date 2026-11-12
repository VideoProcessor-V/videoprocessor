// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5b-3 (doc 59 section 2.1 items d, e, f): the renderer side of the de-judder policy. Out-of-line member
// definitions of the renderer's Impl, included ONCE by LibplaceboVideoRenderer.cpp right after the Impl struct
// (with the other module .inl files). Moved VERBATIM out of LibplaceboVideoRenderer.cpp:
//   - RunDejudderPolicy = the old UpdateDejudderEstimate body (the Impl keeps a one-line forwarder with the same
//     15 arguments, packed into DejudderEstimateCall with the same values); its arguments are unpacked into
//     references of the old names, so the moved statements are unchanged. The decision part in the middle is
//     VpMotion::RunDejudderDecision (DejudderPolicyRunner.h, CPU-tested over the recorded rows) with
//     ImplDecisionHost below: each host call is the old statement at the old point.
//   - LogInterpolationDecision (via VpMotion::LogInterpolationDecisionFor), LogDejudderAhead, DejudderHold,
//     FlushDejudderHolds: the old bodies.
// Render thread (RenderLocked, renderMutex held); FlushDejudderHolds also from the shutdown path (renderMutex).
#pragma once

#include <VP-V/modules/motion/consumers/DejudderPolicyRunner.h>
#include <VP-V/modules/motion/estimation/DitherVectorSource.h>

namespace
{
	// The renderer side of RunDejudderDecision (the old statements, at the old points). A template on the Impl
	// type (named only inside its member functions).
	template<class ImplT, class CallT>
	struct ImplDecisionHost
	{
		ImplT& impl;
		const CallT& call;
		int Width() const { return call.width; }
		int Height() const { return call.height; }
		bool WarmPending() const { return impl.motionEngineWarmPending; }
		void SetWarmPending(bool v) { impl.motionEngineWarmPending = v; }
		bool LowLatency() const { return impl.lowLatencyQueueActive.load(std::memory_order_acquire); }
		AlphaPresentationSnapshot telemetry{};
		void SnapshotTelemetry() { telemetry = impl.presentationTelemetry.Snapshot(); }
		double DecisionDisplayHz() const
		{
			return telemetry.measuredDisplayHz > 0.0 ? telemetry.measuredDisplayHz :
				impl.RenderAheadActive() && impl.dejudderDisplayRate.openLoop ? impl.dejudderDisplayRate.hz : impl.customHookRefreshHz;
		}
		bool NextMatches() const
		{
			const auto& next = call.next;
			const auto& current = call.current;
			const auto& contract = call.contract;
			return next && current &&
				Vpv::Capture::BytesOf(*next) == Vpv::Capture::BytesOf(*current) && Vpv::Capture::IsRaw(*next) == Vpv::Capture::IsRaw(*current) &&
				(!Vpv::Capture::IsRaw(*next) || Vpv::Capture::RawEncodingOf(*next) == Vpv::Capture::RawEncodingOf(*current)) &&
				Vpv::Capture::DvTunnelRawOf(*next) == Vpv::Capture::DvTunnelRawOf(*current) &&
				next->contract.sampleRange == contract.sampleRange &&
				next->contract.colorDepth == contract.colorDepth &&
				next->contract.bitShift == contract.bitShift &&
				// Bug hunt 2026-10-17: on the DV tunnel never pair an untrusted B
				// (black frame) or a DV picture with an SDR fallback.
				// By the capture encoding (every payload), not rawEncoding (raw only).
				DejudderPolicy::DolbyVisionNextPairs(impl.dejudderDolbyVisionTunnel, *current, *next,
					impl.activeSettings.motionHistoryFixes);   // Hunt 2 02-dv L2: byte-equal DM matrices
		}
		bool PlaneFormatsUsable(bool nativeRgb) const
		{
			const auto usable = [](pl_tex tex)
			{
				return tex && tex->params.sampleable && (tex->params.format->caps & PL_FMT_CAP_LINEAR) &&
					(tex->params.format->caps & PL_FMT_CAP_STORABLE);
			};
			return nativeRgb || (impl.dejudderRgbLuma ? usable(impl.dejudderRgbLuma) :
				(usable(impl.textures[0]) && usable(impl.textures[1])));
		}
		bool TemporalMixing() const { return impl.temporalRenderer.Mixing(); }
		double IntentMotionScale(uint64_t sourceSequence) const { return impl.intentInterpreter->AppliedMotionScale(sourceSequence); }
		double TimelineDisplayHz() const { return impl.TimelineDisplayHz(); }
		void LogInterpolationDecision(const InterpolationPolicy::Input& ii) { impl.LogInterpolationDecision(ii); }
		void SetCadenceRatio(double ratio) { impl.dejudderExtraStats.cadenceRatio = ratio; }
		ULONGLONG Tick() const { return GetTickCount64(); }
		void RecordDecision(const char* kind, const char* name, int64_t a, int64_t b, double x, double y)
		{
			DiagnosticHistory::Global().RecordDecision(kind, name, a, b, x, y);
		}
		template<class... Args> void Log(const char* format, Args... args) { DebugLog::Log(format, args...); }
		void PublishDejudderStatus(bool force) { impl.PublishDejudderStatus(force); }
		void ClearStatusIfPublished()
		{
			if (impl.dejudderStatusTick != 0)
			{
				impl.dejudderStatusTick = 0;
				std::lock_guard<std::mutex> guard(impl.dejudderStatusMutex);
				impl.dejudderStatus.clear();
			}
		}
		void PollMotionEngineBudget(int width, int height, bool held) { impl.PollMotionEngineBudget(width, height, held); }
		MotionEngineIdle::Keeper& IdleKeeper() { return impl.motionEngineIdle; }
		unsigned long long& IdleReleasesLogged() { return impl.motionEngineIdleReleasesLogged; }
		void WarmIdleMotionEngine() { impl.WarmIdleMotionEngine(call.contract, call.lossless422, call.nativeRgb); }
		void ReleaseDejudder() { impl.ReleaseDejudder(); }
	};
	// LogInterpolationDecision's clock and log.
	struct DejudderLogHost
	{
		ULONGLONG Tick() const { return GetTickCount64(); }
		template<class... Args> void Log(const char* format, Args... args) { DebugLog::Log(format, args...); }
	};
}

// Stage 6b-1: decide whether the de-judder runs for this source frame and,
// if so, upload B (the next frame) and estimate the motion A->B once per
// source frame. Nothing is presented differently yet (6b-2). Off: B is
// not uploaded, nothing is estimated, and the matcher is freed.
void LibplaceboVideoRenderer::Impl::RunDejudderPolicy(const DejudderEstimateCall& call)
{
	const std::shared_ptr<const QueuedFrame::ConvertedPayload>& next = call.next;
	const VideoFrameFormatterOutputContract& contract = call.contract;
	const int width = call.width, height = call.height;
	const size_t rowBytes = call.rowBytes;
	const int chromaHeight = call.chromaHeight;
	const bool lossless422 = call.lossless422, flipped = call.flipped;
	const uint64_t sourceSequence = call.sourceSequence, nextSourceSequence = call.nextSourceSequence;
	const bool cadenceRepeat = call.cadenceRepeat, nativeRgb = call.nativeRgb, interlaced = call.interlaced;
	const double sourceHz = call.sourceHz;
	// MotionValidity step 1 C: the kept run. Cadence repeats and smooth motion's
	// extra draws carry the same sequence (Repeat: the run continues); a hole
	// nothing else reported (a drop) invalidates here, before this estimate.
	const MotionValidity::Continuity keptContinuity = motionValidity.NoteKept(sourceSequence, MotionValidity::Cause::DriftDrop);
	ApplyMotionInvalidate();
	// W2-C8 (owner 2026-11-11 22:39, doc 91:108): the sigma-delta's motion compensation for this frame's renders - on (the
	// configured default) only while the pair that ends here is valid and the engine runs; else off, visibly (DitherVectorSource.h).
	{
		namespace DV = VpMotion::DitherVectors;
		double configured = 0.0;
		DV::CompensationFacts cf;
		// A loaded sigma-delta always notes the decision (a rule set to 0 shows as "rule", status only); only a configured one is
		// switched per frame. Invariant: a hook reload (Ensure, on a template change) re-applies the rule value 1 until the next
		// frame's gate - the same moment the shader's instance_tag resets its carry, so no stale compensation crosses it.
		if (customHooks.QuantizerConfigured("motion_compensation", configured))
		{
			cf.configured = configured >= 0.5;
			cf.engineOff = DV::EngineFullyOff(activeSettings.vectorDemand, MotionVectorsRun());
			cf.continuity = keptContinuity;
			cf.keptPairValid = sourceSequence > 1 && motionValidity.KeptPairValid(sourceSequence - 1, sourceSequence);
			if (sourceSequence > 1)
				cf.span = motionValidity.SpanStatus(sourceSequence - 1, sourceSequence);
			cf.invalidations = motionValidity.Total();
			cf.lastCause = motionValidity.LastCause();
			const DV::Compensation compensation = DV::DecideCompensation(cf);
			if (cf.configured)
				customHooks.SetQuantizerParameter("motion_compensation", compensation.on ? 1.0 : 0.0);
			DV::Tracker& tracker = DV::Registry::ForHost(this);
			const unsigned long long nowMs = GetTickCount64();
			tracker.NoteCompensation(compensation, nowMs);
			// The vector chain (BuildSigmaDeltaVectorsWithoutExtras) is asked only while the engine runs (its call site's
			// vectorDemand.mode != Off). With the engine off the sigma-delta searches itself (step 4, "not served"): say so per
			// frame here, so that state is never without a status line (GPU A/B 2026-11-12: no segment in that case).
			if (vectorDemand.mode == VectorDemand::Mode::Off)
			{
				DV::Facts vf;
				vf.slotRuns = pipelineSlots.Runs(PipelineSlots::Id::OutputDither);
				vf.patch = SigmaDeltaVectorsSupported();
				vf.ownSetting = activeSettings.ditherVectorsOwn;
				vf.motionCompensation = cf.configured;
				vf.served = false;
				tracker.Note(DV::Choose(vf), false, nowMs);
			}
			if (compensation.Automatic())
				for (const FallbackCatalog::Decision& decision : tracker.Decisions(nowMs))
					SyncFallback(decision);
		}
	}
	dejudderSourceHz = sourceHz;
	// Vector engine on demand: what wants vectors this frame (previous-frame state), and backward pairs.
	// VPV-HOOK
	{
	auto vectorHost = VpMotion::MakeVectorEngineHost(*this);
	VpMotion::VectorEngine::UpdateFacts vectorFacts;
	vectorFacts.vectorDemandSetting = activeSettings.vectorDemand; vectorFacts.sigmaDeltaRanLast = sigmaDeltaRanLast;
	vectorFacts.ditherVectorsOwn = activeSettings.ditherVectorsOwn;
	vectorFacts.opticalFlow = activeSettings.opticalFlow;   // W2-C1: the camera fit's gate (this frame)
	// W2-C1 (B9): a starter runs the engine - the de-judder in its standing class (its extras configured, matcher held, a reason
	// that can act now or at the next transient, or a cadence being acquired), the interpolator, or hiccup repair filling a gap.
	// Reuse-only consumers (sigma-delta, detail pulse, deband hold, source monitor, the occlusion mask, ...) are served only then
	// (previous-frame state, like the wants). The mask's own matcher never counts: it would keep itself served.
	VectorDemand::Input vectorInput = VectorDemandInput(interlaced);
	const bool dejudderStanding = dejudder && (activeSettings.dejudderStrength > 0.0 || activeSettings.dejudderStrengthEven > 0.0) &&
		DejudderPolicy::CanEngageOrAcquiring(dejudderReason, pulldownAcquiring);
	vectorInput.starterEngaged = dejudderStanding || interpolating || vpGapFillStandalone;
	// W2-C1 (coordinator 2026-11-11): an engine that exists without a starter is named in the status (kept idle for hiccup repair:
	// fill_gaps on outside the low-latency queue, where the keeper keeps it; otherwise held until its release).
	vectorInput.engineLoaded = dejudder && !vectorInput.starterEngaged;
	vectorInput.engineLoadedForHiccupRepair = vectorInput.engineLoaded && activeSettings.interpolationFillGaps &&
		!lowLatencyQueueActive.load(std::memory_order_acquire);
	// The governor's release is a state of this frame: a held matcher is not released (the keeper's latch alone would stay set).
	vectorEngine.NoteVramRelease(motionEngineIdle.Pressured() && !dejudder);
	vectorEngine.UpdateVectorEngine(vectorInput, vectorFacts, vectorHost);
	if (vectorDemand.mode != VectorDemand::Mode::Off && !cadenceRepeat)
	{
		VpMotion::VectorEngine::ServeFacts serve;
		serve.legacyConsumer = (dejudder && (dejudderReason == DejudderPolicy::Reason::Active ||
			dejudderReason == DejudderPolicy::Reason::NoNextFrame || dejudderReason == DejudderPolicy::Reason::NextFrameDiffers)) || interpolating;
		serve.lowResSearch = activeSettings.dejudderLowRes != "off";
		serve.vectorDeliveryRuns = pipelineSlots.Runs(PipelineSlots::Id::MotionVectorDelivery);
		serve.pictureHeight = dejudderPicture.available && dejudderPicture.rasterHeight == height &&
			dejudderPicture.bottom > dejudderPicture.top ? dejudderPicture.bottom - dejudderPicture.top : height;
		vectorEngine.ServeStatistics(sourceSequence, nextSourceSequence, width, height, serve, vectorHost);
	}
	}
	// VPV-HOOK-END
	// The decision (DejudderPolicyRunner.h): the policy, the interpolation decision, the gap repair, the status and
	// reason logs, the idle engine keeper; then today's returns.
	bool analysisEstimate = false;   // W2-C1: the estimate runs for an analysis consumer, not the de-judder (MotionEngineWork)
	{
		VpMotion::DecisionFacts facts;
		facts.sourceHz = sourceHz;
		facts.nativeRgb = nativeRgb;
		facts.interlaced = interlaced;
		facts.cadenceRepeat = cadenceRepeat;
		facts.sourceSequence = sourceSequence;
		facts.haveNext = next != nullptr;
		facts.adaptiveGrainMapWanted = adaptiveGrainMapWanted;
		facts.vpGapFillStandalone = vpGapFillStandalone;
		facts.dejudderHeld = dejudder != nullptr;
		facts.nextTextureHeld = dejudderNextTextures[0] != nullptr;
		facts.dejudderRgbLuma = dejudderRgbLuma != nullptr;
		facts.smoothMotionEngine = smoothMotionEngine;
		facts.pulldownFilmHz = pulldownFilmHz;
		facts.dejudderEstimatedSequence = dejudderEstimatedSequence;
		facts.vectorDemand = &vectorDemand;
		ImplDecisionHost<Impl, DejudderEstimateCall> host{ *this, call };
		if (VpMotion::RunDejudderDecision(activeSettings, dejudderPolicy, facts, host, &analysisEstimate) != VpMotion::DecisionOutcome::PairCheck)
			return;
	}
	// MotionValidity step 1 D: (A, B) only as adjacent frames of the current epoch
	// (the dequeue already drops B after a gap, a repeat, a film update or a
	// different state).
	// Gap fill: the pair the render loop offered across a short loss (a dropped queued frame
	// leaves a sequence hole), in the current epoch.
	const bool fillPair = vpFillNextSequence != 0 && vpFillNextSequence == nextSourceSequence &&
		nextSourceSequence > sourceSequence &&
		nextSourceSequence - sourceSequence <= static_cast<uint64_t>((std::max)(vpFillMaxSpan, 1)) &&
		motionValidity.Current(motionValidity.EpochOfLastKept()) &&
		motionValidity.SpanStatus(sourceSequence, nextSourceSequence, true) !=
			::MotionValidity::State::SpanState::Invalid;   // W1-C2a MUST 1 / C2a-F 1: no FULL / settings[motion] across
	if (!fillPair && !motionValidity.PairServes(sourceSequence, nextSourceSequence, motionValidity.EpochOfLastKept(),
		motionValidity.Epoch()))   // W1-C2a A1 / S2
		return;
	// Stage B: a frame no refresh shows (a source faster than the display) needs no
	// pair (the producer still computes its pairs ahead).
	if (vpTimelineActive && vpSkipOutput)
		return;
	// B's layout, for C (estimate ahead) to match.
	dejudderLayout.valid = true;
	dejudderLayout.width = width;
	dejudderLayout.height = height;
	dejudderLayout.chromaHeight = chromaHeight;
	dejudderLayout.rowBytes = rowBytes;
	dejudderLayout.pixelBytes = Vpv::Capture::BytesOf(*next);
	dejudderLayout.raw = Vpv::Capture::IsRaw(*next);
	dejudderLayout.rawEncoding = Vpv::Capture::IsRaw(*next) ? Vpv::Capture::RawEncodingOf(*next) : VideoFrameEncoding::UNKNOWN;
	dejudderLayout.dvTunnelRaw = Vpv::Capture::DvTunnelRawOf(*next);
	dejudderLayout.rgb = dejudderRgbLuma != nullptr;
	dejudderLayout.bt2020 = dejudderRgbBt2020;
	dejudderLayout.lossless422 = lossless422;
	dejudderLayout.fullRange = contract.sampleRange == VideoFrameSampleRange::FULL;
	dejudderLayout.contract = contract;
	// Owner 2026-10-13 (estimate ahead): this pair (A, B) was estimated in
	// the last frame's extras, with B the same frame: the field, the trust,
	// A's mask and the grain are ready, and B's planes are the ahead ones.
	// E12 (B3-Z Z2): a hit only for an entry made in the current epoch, stamped with the entry's own epoch.
	if (dejudderAhead.Consume(sourceSequence, nextSourceSequence, motionValidity.Epoch()))
	{
		std::swap(dejudderNextTextures[0], dejudderAheadTextures[0]);
		std::swap(dejudderNextTextures[1], dejudderAheadTextures[1]);
		std::swap(dejudderNextUpload, dejudderAheadUpload);   // efficiency_fixes: the records follow
		dejudderNextUploadedSequence = nextSourceSequence;
		dejudderEstimatedSequence = sourceSequence;
		dejudderEstimatedEpoch = dejudderAhead.ConsumedEpoch();
		dejudderAhead.NoteEpoch(dejudderEstimatedEpoch);   // E12: the next entry is made in this field's epoch
		dejudderEstimatedPeriods = 1;   // estimated ahead: an adjacent pair
		LogDejudderAhead();
		return;
	}
	LogDejudderAhead();
	// B: the same plane layout as A's upload above.
	dejudderNextUploadedSequence = 0;
	if (!UploadDejudderPlanes(next, dejudderNextTextures, &dejudderNextUpload, nextSourceSequence))
	{
		motionEngineIdle.NoteAllocationFailure(static_cast<double>(GetTickCount64()) / 1000.0);   // Bug hunt 3 F4
		if (++dejudderFailures == 1 || dejudderFailures % 2400 == 0)
			DebugLog::Log("De-judder: uploading the next frame failed; off for this frame (%llu failures)",
				dejudderFailures);
		return;
	}
	dejudderNextUploadedSequence = nextSourceSequence;
	(void)flipped;   // A and B share the orientation; the warp keeps it.
	Dejudder::Planes a, b;
	a.luma = dejudderRgbLuma ? dejudderRgbLuma : textures[0];
	a.chroma = dejudderRgbLuma ? nullptr : textures[1];
	a.chromaShiftX = 1;
	a.chromaShiftY = lossless422 ? 0 : 1;
	a.fullRange = contract.sampleRange == VideoFrameSampleRange::FULL;   // delta 5, D5-3
	b = a;
	b.luma = dejudderNextTextures[0];
	b.chroma = dejudderRgbLuma ? nullptr : dejudderNextTextures[1];
	const uint64_t estimateEpoch = motionValidity.Epoch();
	if (RunDejudderEstimate(a, b, sourceSequence, nextSourceSequence))
	{
		if (analysisEstimate)
			++vectorEngine.WorkCounters().analysisEstimates;
		dejudderEstimatedSequence = sourceSequence;
		dejudderEstimatedEpoch = estimateEpoch;
		dejudderAhead.NoteEpoch(estimateEpoch);   // E12: an entry made ahead of this field keeps its epoch
		// Bug hunt 3 F10: a gap pair's field spans the missed periods too.
		dejudderEstimatedPeriods = InterpolationTimeline::FieldPeriods(fillPair && vpFillGapFrames > 0, vpFillGapFrames);
	}
}

// Interpolation: the decision when it changes (at most one line per 5 s), with the
// measured rates it came from (VpMotion::LogInterpolationDecisionFor: the old statements).
void LibplaceboVideoRenderer::Impl::LogInterpolationDecision(const InterpolationPolicy::Input& in)
{
	DejudderLogHost host;
	VpMotion::LogInterpolationDecisionFor(activeSettings, dejudderPolicy, interpolatedPresents, in, host);
}

void LibplaceboVideoRenderer::Impl::LogDejudderAhead()
{
	if (!activeSettings.dejudderEstimateAhead)
		return;
	const ULONGLONG now = GetTickCount64();
	if (dejudderAheadLogTick != 0 && now - dejudderAheadLogTick < 5000)
		return;
	dejudderAheadLogTick = now;
	using Reason = Dejudder::AheadSchedule::Reason;
	DebugLog::Log("De-judder estimate ahead: %llu frames ahead, %llu before the frame (%llu estimated ahead for a "
		"pair that did not come, %llu of them from an older epoch); not ahead: no frame after next %llu, source "
		"warp %llu, other %llu; last: %s",
		dejudderAhead.Hits(), dejudderAhead.Sequential(), dejudderAhead.Misses(), dejudderAhead.EpochMisses(),
		dejudderAhead.Count(Reason::NoAfter), dejudderAhead.Count(Reason::SourceWarp),
		dejudderAhead.Count(Reason::AfterNotNew) + dejudderAhead.Count(Reason::NotEstimated) +
			dejudderAhead.Count(Reason::Layout) + dejudderAhead.Count(Reason::SnapshotFailed) +
			dejudderAhead.Count(Reason::UploadFailed) + dejudderAhead.Count(Reason::EstimateFailed) +
			dejudderAhead.Count(Reason::NoNext) + dejudderAhead.Count(Reason::NotActive),
		Dejudder::AheadSchedule::Name(dejudderAhead.LastReason()));
}

void LibplaceboVideoRenderer::Impl::DejudderHold(const char* why)
{
	// Report-a-problem: one repeated-stage frame row per hold and a decision when the
	// reason changes; the hold lines (VP diagnostics adapter).
	VpDiagnostics::RecordDejudderHold({ why, diagExtrasCounter, diagExtrasSequence, RenderAheadActive(),
		dejudderDisplayRate.openLoop, diagRendererGeneration, dejudderExtraStats.holds }, diagLastHoldCode);
	++dejudderExtraStats.holds;
	dejudderExtraStats.lastHold = why;
	dejudderHoldLog.OnHold(why, dejudderExtraStats.holds);
	// Render-ahead: every hold also shows on the "Render-ahead:" line with the gate's level.
	if (RenderAheadActive())
		LogRenderAhead(dejudderExtraStats.cadenceRatio, renderAheadGate.Level(), why);
}

// The last hold lines at teardown, so the last holds are never lost.
void LibplaceboVideoRenderer::Impl::FlushDejudderHolds() { dejudderHoldLog.Flush(dejudderExtraStats.holds); }
