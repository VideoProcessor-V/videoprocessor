// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "MotionHostHelpers.h"

#include <DebugLog.h>
#include <GeneralRateLog.h>
#include <VP-V/modules/input/signal/ResyncCallSites.h>
#include <VP-V/modules/motion/consumers/DejudderHistory.h>
#include <VP-V/modules/motion/estimation/DitherVectorSource.h>
#include <VP-V/modules/output/dither/DitherSlot.h>
#include <VP-V/modules/output/dither/SigmaDeltaVectors.h>

#include <cmath>

namespace Vpv
{
	namespace MotionHost
	{
		bool MotionVectorsRun(const PipelineSlots::Registry& pipelineSlots)
		{
			return pipelineSlots.Runs(PipelineSlots::Id::MotionEstimation) &&
				pipelineSlots.Runs(PipelineSlots::Id::MotionVectorDelivery);
		}

		bool DitherVectorsSupported(const PipelineSlots::Registry& pipelineSlots)
		{
			return DitherSlot::VectorsWanted(pipelineSlots.Runs(PipelineSlots::Id::OutputDither), SigmaDeltaVectorsSupported());
		}

		VectorDemand::Input BuildVectorDemandInput(const VectorDemandHost& h, bool interlaced)
		{
			using C = VectorDemand::Consumer;
			VectorDemand::Input in;
			in.enabled = h.activeSettings.vectorDemand;
			in.slotsRun = MotionVectorsRun(h.pipelineSlots);
			in.lowLatency = h.lowLatency;
			in.queueAheadCapacity = h.queueAheadCapacity;
			in.smoothMotion = h.smoothMotionEngine || h.temporalMixing;
			in.interlaced = interlaced;
			// Stable while smooth motion mixes (Active() flips with each drawn mix); interlaced-only temporal frames count.
			const bool temporal = h.smoothMotionEngine || (h.temporalActive && !h.temporalMixing);
			auto want = [&in](C c, bool on) { in.wants[static_cast<int>(c)] = on; };
			want(C::DejudderExtras, h.activeSettings.dejudderStrength > 0.0 || h.activeSettings.dejudderStrengthEven > 0.0);
			want(C::Interpolator, h.activeSettings.interpolation != InterpolationPolicy::Mode::Off);
			want(C::HiccupRepair, h.activeSettings.interpolationFillGaps);
			want(C::OcclusionMask, h.activeSettings.occlusionMask);
			want(C::SigmaDelta, h.sigmaDeltaRanLast && !h.activeSettings.ditherVectorsOwn && h.ditherVectorsSupported);
			want(C::GrainMap, h.adaptiveGrainMapWanted);
			want(C::DebandHold, h.activeSettings.debandVp != "off" && h.pipelineSlots.Runs(PipelineSlots::Id::RepairDeband) && !temporal);
			want(C::DetailPulse, (h.activeSettings.detailPulseStabilise || h.activeSettings.textureRestore) && !temporal);
			want(C::EntranceSegment, h.entranceOnce);
			want(C::SourceMonitorPan, h.activeSettings.sourceResolution == "auto");
			want(C::MotionIntent, h.motionIntentActive);
			return in;
		}

		bool SameGeometry(const DejudderRendered::Geometry& a, const DejudderRendered::Geometry& b)
		{
			auto same = [](const pl_rect2df& x, const pl_rect2df& y)
			{
				return std::fabs(x.x0 - y.x0) < 0.01f && std::fabs(x.y0 - y.y0) < 0.01f &&
					std::fabs(x.x1 - y.x1) < 0.01f && std::fabs(x.y1 - y.y1) < 0.01f;
			};
			if (!same(a.sourceCrop, b.sourceCrop) || !same(a.targetCrop, b.targetCrop) || a.nls.valid != b.nls.valid ||
				a.numHolds != b.numHolds)
				return false;
			if (a.nls.valid && (!same(a.nlsDomain, b.nlsDomain) || a.nls.curve != b.nls.curve || a.nls.geometry != b.nls.geometry ||
				a.nls.xScale != b.nls.xScale || a.nls.yScale != b.nls.yScale || a.nls.xProtect != b.nls.xProtect ||
				a.nls.yProtect != b.nls.yProtect))
				return false;
			for (int i = 0; i < a.numHolds && i < 4; ++i)
				if (!same(a.holds[i], b.holds[i]))
					return false;
			return true;
		}

		float AdaptiveFallbackFramePeriod(double customHookRefreshHz)
		{
			static GeneralRate::FallbackNotice fallback;   // fixed-constant batch B7 2026-10-18: logged
			return GeneralRate::NoteFallback(fallback, customHookRefreshHz > 0.0, "custom hook frame time", 24.0) ?
				static_cast<float>(1.0 / customHookRefreshHz) : 1.0f / 24.0f;
		}

		::Dejudder::Settings RenderedWarpSettings(const ::Dejudder::MotionWarp& dejudder, int outputHeight)
		{
			::Dejudder::Settings s = dejudder.EffectiveSettings();
			s.holdFeatherPx = dejudder.GetSettings().holdFeatherPx *
				(outputHeight > 0 ? static_cast<float>(outputHeight) / 2160.0f : 1.0f);
			return s;
		}

		double SourceRateHz(double pulldownFilmHz, double displayModeRefreshHz)
		{
			if (pulldownFilmHz > 0.0)
				return pulldownFilmHz;
			return displayModeRefreshHz;
		}

		void RequestMotionInvalidate(MotionValidity::State& motionValidity,
			VpFrameAnalysis::Host& frameAnalysis, MotionValidity::Cause cause)
		{
			motionValidity.Invalidate(cause);
			frameAnalysis.OnDiscontinuity(static_cast<int>(cause));   // WP-23: the one shared epoch
		}

		void ApplyResyncLibplaceboFlush(VpResync::Shadow& resync, VpResync::Drive& resyncDrive,
			pl_renderer renderer, bool originalPathway, unsigned long long& flushCount,
			uint64_t sourceSequence, const std::function<void()>& clearTemporal)
		{
			if ((VpResync::TakeIfDriving(resync, resyncDrive, VpResync::Reader::Libplacebo) & VpIndicator::Bit(VpIndicator::kFull)) == 0 ||
				!renderer || originalPathway)
				return;
			pl_renderer_flush_cache(renderer);
			if (clearTemporal)
				clearTemporal();   // retained frames borrow the flushed cache (as the DV matrix recycle)
			++flushCount;
			if (flushCount <= 20 || (flushCount & (flushCount - 1)) == 0)
				DebugLog::Log("Resync (on): FULL at source frame %llu - libplacebo frame-mix cache and detected peak flushed (%llu so far)",
					static_cast<unsigned long long>(sourceSequence), static_cast<unsigned long long>(flushCount));
		}

		void ApplyMotionInvalidate(MotionInvalidateHost& h)
		{
			MotionValidity::Pending pending;
			const bool epochMoved = h.vectorEngine.TakeInvalidation(pending);
			if (!VpMotion::TakeDejudderHistoryAction(h.resync.Driving(), epochMoved, pending, h.vectorEngine.motionResyncReaders))
				return;
			if (h.endSnapshotLease)
				h.endSnapshotLease();
			if (pending.release)
			{
				if (h.releaseDejudder)
					h.releaseDejudder();
				h.motionEngineWarmPending = true;   // H3 F4: a new source loads the idle engine again
			}
			else
			{
				if (h.dejudder)
					h.dejudder->ForgetHistory();
				h.dejudderAhead.Reset();
				if (h.clearAheadPayload)
					h.clearAheadPayload();
				h.dejudderExtras.dejudderMaskSequence[0] = h.dejudderExtras.dejudderMaskSequence[1] = 0;
			}
			h.dejudderExtras.dejudderMaskEpoch[0] = h.dejudderExtras.dejudderMaskEpoch[1] = 0;
			h.dejudderEstimatedSequence = 0;
			h.dejudderEstimatedEpoch = 0;
			h.dejudderLiveFieldFrom = 0;
			h.vectorEngine.motionSegment.Retract(VpMotion::Direction::Forward);
			h.adaptiveDownscale.ForgetMap();
			h.debander.ForgetHold();
			h.detailPulse.Forget();      // detail restore: both histories restart (docs/VP-detail-restore.md)
			h.textureRestore.Forget();
			if (epochMoved)
				h.vectorEngine.LogInvalidation(pending);
		}

		void BuildSigmaDeltaVectorsWithoutExtras(DitherVectorsHost& h, uint64_t sourceSequence, uint64_t nextSourceSequence)
		{
			h.dejudderExtras.dejudderNextVectorsSequence = 0;
			h.dejudderExtras.dejudderNextVectorsFrom = 0;   // GEM-SD-VECTORS MUST-2: cleared here, set when mapped (CoreSegments.inl CarryValid reads it)
			h.dejudderExtras.ditherNextGate.Clear();
			namespace DV = VpMotion::DitherVectors;
			DV::Facts facts;
			double motionCompensation = 0.0;
			// No sigma-delta loaded (no quantiser, or one without the parameter): nothing to decide (step None, not "static"). The
			// CONFIGURED value (rule / owner default 1), not this frame's validity override (RunDejudderPolicy's gate).
			const bool quantizer = h.customHooks.QuantizerConfigured("motion_compensation", motionCompensation);
			facts.slotRuns = h.pipelineSlots.Runs(PipelineSlots::Id::OutputDither) && quantizer;
			facts.patch = SigmaDeltaVectorsSupported();
			facts.ownSetting = h.activeSettings.ditherVectorsOwn;
			facts.motionCompensation = motionCompensation >= 0.5;
			facts.served = h.vectorDemand.Serves(VectorDemand::Consumer::SigmaDelta);
			facts.engineStateOff = h.vectorEngine.State().state == VectorDemand::MotionEngineState::Off;   // doc 91 D (lead 5 ruling)
			facts.sequences = sourceSequence != 0 && nextSourceSequence != 0;
			facts.timelineDrew = h.interpolating || h.vpTimelineActive;   // the timeline / hiccup repair drew refreshes: A was not at shift 0
			facts.matcher = h.dejudder != nullptr;
			facts.geometryValid = h.dejudderExtras.dejudderGeometryValid;
			if (facts.sequences && facts.matcher)
			{
				VpMotion::VerifiedField field;
				facts.engineField = h.vectorEngine.FieldOf(nextSourceSequence, field) && field.pairFrom == sourceSequence;
				facts.pairServes = h.motionValidity.PairServes(sourceSequence, nextSourceSequence, h.dejudderEstimatedEpoch, h.dejudderEstimatedEpoch);
				facts.span = h.motionValidity.SpanStatus(sourceSequence, nextSourceSequence);
				facts.onDemandServed = h.vectorEngine.OnDemandServed(sourceSequence, nextSourceSequence);
				facts.onDemandCause = h.vectorEngine.OnDemandLastCause();
				facts.matcherLive = h.liveFieldFor ? h.liveFieldFor(sourceSequence) : false;
			}
			DV::Choice choice = DV::Choose(facts);
			bool mapped = false;
			if (choice.Reuses() && h.d3d11 && h.d3d11->gpu)
			{
				const int w = static_cast<int>(std::lround(std::fabs(h.dejudderExtras.dejudderGeometry.targetCrop.x1 - h.dejudderExtras.dejudderGeometry.targetCrop.x0)));
				const int vh = static_cast<int>(std::lround(std::fabs(h.dejudderExtras.dejudderGeometry.targetCrop.y1 - h.dejudderExtras.dejudderGeometry.targetCrop.y0)));
				if (w > 0 && vh > 0)
				{
					if (!h.dejudderExtras.dejudderNextVectorField)
						h.dejudderExtras.dejudderNextVectorField = std::make_unique<DejudderRendered::VectorField>(h.log, h.d3d11->gpu);
					const ::Dejudder::Settings matcher = RenderedWarpSettings(*h.dejudder, vh);
					pl_tex next = nullptr;
					if (h.dejudderExtras.dejudderNextVectorField->Run(h.dejudder->Field(), h.dejudder->FrameTrust(), h.dejudder->GetSettings().minTrustedShare, 0.0f,
							h.dejudderExtras.dejudderGeometry, w, vh, next, h.dejudder->SubField(), &matcher, h.dejudder->Camera(), true) && next)
					{
						h.dejudderExtras.dejudderNextVectors = next;
						h.dejudderExtras.dejudderNextVectorsSequence = nextSourceSequence;
						h.dejudderExtras.dejudderNextVectorsFrom = sourceSequence;   // GEM-SD-VECTORS MUST-2: A, for the stamp check at B
						h.dejudderExtras.ditherNextGate.Bind(sourceSequence, nextSourceSequence, h.dejudderEstimatedEpoch);
						h.dejudderExtras.dejudderNextVectorsGeometry = h.dejudderExtras.dejudderGeometry;
						h.dejudderExtras.dejudderNextVectorsW = w;
						h.dejudderExtras.dejudderNextVectorsH = vh;
						mapped = true;
					}
				}
				else
					mapped = false;   // an empty destination: nothing to map (counted as MapFailed, as the own search covers B)
			}
			DV::Tracker& tracker = DV::Registry::ForHost(h.host);
			const unsigned long long nowMs = GetTickCount64();
			choice = tracker.Note(choice, mapped, nowMs);
			if (h.syncFallback)
				for (const FallbackCatalog::Decision& decision : tracker.Decisions(nowMs))
					h.syncFallback(decision);
			if (tracker.TakeChanged())
				DebugLog::Log("Sigma-delta vectors on the new frame's present: %s", tracker.StatusSegment().c_str());
		}
	}
}
