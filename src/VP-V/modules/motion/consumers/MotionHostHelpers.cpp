// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "MotionHostHelpers.h"

#include <DebugLog.h>
#include <GeneralRateLog.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <VP-V/modules/input/signal/ResyncCallSites.h>
#include <VP-V/modules/motion/consumers/DejudderHistory.h>
#include <VP-V/modules/motion/estimation/DitherVectorSource.h>
#include <VP-V/modules/output/dither/DitherSlot.h>
#include <VP-V/modules/output/dither/SigmaDeltaVectors.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/sampling.h>

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
			return GeneralRate::NoteFallback(fallback, customHookRefreshHz > 0.0, "custom hook frame time", 24.0) ? // GENERAL-REF: measured-rate fallback reported via GeneralRate::NoteFallback
				static_cast<float>(1.0 / customHookRefreshHz) : 1.0f / 24.0f; // GENERAL-REF: measured-rate fallback reported via GeneralRate::NoteFallback
		}

		::Dejudder::Settings RenderedWarpSettings(const ::Dejudder::MotionWarp& dejudder, int outputHeight)
		{
			::Dejudder::Settings s = dejudder.EffectiveSettings();
			const float refH = dejudder.GetSettings().referenceHeight;
			s.holdFeatherPx = dejudder.GetSettings().holdFeatherPx *
				(outputHeight > 0 && refH > 0.0f ? static_cast<float>(outputHeight) / refH : 1.0f);
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

		void PrepareDejudderCamera(VpMotion::DejudderCameraState& bindings, const void* host, uint64_t sourceSequence, float phaseT)
		{
			// K9-3 (B4, default OFF): when camera_use_dejudder is enabled, evaluate P9 camera availability / phase weight
			// (strictest policy) for visible status and late fallback; off = zero work, byte-identical.
			if (!bindings.config.enabled)
				return;
			const auto* dir = static_cast<const CameraLink::ICameraLinkDirectory*>(
				Vpv::Hub::ExtensionHub::Instance().Query(CameraLink::kCameraDirectoryQueryId));
			const CameraLink::ICameraLink* camLink = dir ? dir->LinkFor(host) : nullptr;
			bindings.Prepare(camLink, sourceSequence, static_cast<double>(phaseT));
		}

		void AppendDejudderStatusExtras(std::string& text, bool pulldown, uint32_t pulldownCadence, double pulldownFilmHz,
			unsigned long long pulldownSkipped, bool occlusionMask, uint64_t maskUsedTick, uint64_t now,
			const std::string& maskState, const VpMotion::DejudderCameraState& cameraBindings)
		{
			if (pulldown)
			{
				char buf[128];
				const int cycle = static_cast<int>(pulldownCadence >> 16), unique = static_cast<int>(pulldownCadence & 0xffffu);
				if (pulldownCadence != 0 && unique > 0 && pulldownFilmHz > 0.0)
					std::snprintf(buf, sizeof(buf), ", pulldown %s, %llu repeats skipped",
						Pulldown::Describe(pulldownFilmHz, pulldownFilmHz * cycle / unique, unique, cycle).c_str(),
						pulldownSkipped);
				else
					std::snprintf(buf, sizeof(buf), ", pulldown: none found");
				text += buf;
			}
			// "mask on" only when the mask was used within the last second.
			if (occlusionMask)
				text += maskUsedTick != 0 && now - maskUsedTick < 1000 ? std::string(", mask on") :
					", " + (maskState.empty() ? std::string("mask waiting") : maskState);
			// K9-3 (B4, default OFF): visible camera P9 weight / causal / late fallback suffix (empty when off).
			if (cameraBindings.config.enabled)
				text += cameraBindings.StatusSuffix();
		}

		bool RunVpPhaseInterpolationHost(std::unique_ptr<Interpolator::FrameInterpolator>& frameInterpolator,
			pl_log log, pl_gpu gpu, const VpvRenderer::Fields& activeSettings,
			const ::Dejudder::Planes& a, const ::Dejudder::Planes& b, const Interpolator::Motion& motion,
			float t, ::Dejudder::Planes& out, VpMotion::VectorEngine& vectorEngine,
			Dejudder::ExtrasState& dejudderExtras, uint64_t nowTickMs,
			const std::function<void(const FallbackCatalog::Decision&)>& syncFallback)
		{
			if (!frameInterpolator)
				frameInterpolator = std::make_unique<Interpolator::FrameInterpolator>(log, gpu);
			frameInterpolator->SetGrain(activeSettings.interpolationGrain);
			const std::shared_ptr<VpSkeleton::SkeletonMemo> memo = vectorEngine.SkeletonMemoFor(dejudderExtras.vpPhaseSeqA);
			const bool ran = frameInterpolator->Run(a, b, motion, t, activeSettings.interpolationFallback, out,
				0, 0, memo.get(), dejudderExtras.vpPhaseSeqA, dejudderExtras.vpPhaseSeqB);
			if (syncFallback)
			{
				syncFallback(dejudderExtras.motionConsumerFailures.Note(nowTickMs, ran ? nullptr : "interpolation failed"));
				const std::string& bmWhy = frameInterpolator->LastBandMemoFallback();
				syncFallback(bmWhy.empty()
					? FallbackCatalog::Released("product.interpolator")
					: FallbackCatalog::ProductFallback("product.interpolator", bmWhy));
			}
			if (!ran)
			{
				if (++dejudderExtras.interpolationFailures == 1 || dejudderExtras.interpolationFailures % 2400 == 0)
					DebugLog::Log("Interpolation: failed (%s; %llu failures); the source frame is shown",
						frameInterpolator->LastError().c_str(), dejudderExtras.interpolationFailures);
				return false;
			}
			return true;
		}

		void SyncEstimatorFallbacks(const ::Dejudder::MotionWarp& dejudder,
			const std::function<void(const FallbackCatalog::Decision&)>& syncFallback)
		{
			if (!syncFallback)
				return;
			syncFallback(dejudder.LastPairGlobalFallback());
			// POD1-M7: surface U7 scene-verdict fallback visibly in the host FallbackStatus registry.
			const std::string& u7Why = dejudder.LastU7SceneFallback();
			syncFallback(u7Why.empty()
				? FallbackCatalog::Released("motion.u7_scene")
				: FallbackCatalog::Engaged("motion.u7_scene", u7Why));
		}

		void LogDebugPhaseAb(pl_gpu gpu, pl_dispatch dispatch, pl_tex aLuma, pl_tex bLuma, pl_tex motionField,
			uint64_t sourceSequence, uint64_t nextSourceSequence, double vpOutputPhase, bool vectors)
		{
			if (!gpu || !dispatch || !aLuma || !bLuma ||
				aLuma->params.w != bLuma->params.w || aLuma->params.h != bLuma->params.h)
				return;
			auto readBack = [&](pl_tex src, std::vector<uint8_t>& out) {
				pl_tex_params tp{};
				tp.w = src->params.w; tp.h = src->params.h; tp.format = src->params.format;
				if (src->params.format->type == PL_FMT_FLOAT)
					tp.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
						static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				if (!tp.format) return false;
				tp.sampleable = true; tp.renderable = true; tp.host_readable = true;
				pl_tex copy = pl_tex_create(gpu, &tp);
				if (!copy) return false;
				pl_shader sh = pl_dispatch_begin(dispatch);
				pl_sample_src sample{};
				sample.tex = src;
				sample.components = src->params.format->num_components;
				bool ok = pl_shader_sample_direct(sh, &sample);
				if (ok)
				{
					pl_dispatch_params dp{};
					dp.shader = &sh;
					dp.target = copy;
					ok = pl_dispatch_finish(dispatch, &dp);
				}
				else
					pl_dispatch_abort(dispatch, &sh);
				if (ok)
				{
					out.resize(static_cast<size_t>(tp.w) * tp.h * tp.format->texel_size);
					pl_tex_transfer_params t{};
					t.tex = copy;
					t.ptr = out.data();
					ok = pl_tex_download(gpu, &t);
				}
				pl_tex_destroy(gpu, &copy);
				return ok;
			};
			std::vector<uint8_t> da, db;
			size_t differing = 0;
			const bool ok = readBack(aLuma, da) && readBack(bLuma, db) && da.size() == db.size();
			if (ok)
				for (size_t i = 0; i < da.size(); ++i) differing += da[i] != db[i] ? 1 : 0;
			double meanLen = 0.0, maxLen = 0.0, gx = 0.0, gy = 0.0;
			size_t trusted = 0, blocks = 0;
			if (motionField)
			{
				std::vector<uint8_t> df;
				if (readBack(motionField, df) && df.size() == static_cast<size_t>(motionField->params.w) * motionField->params.h * 16)
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
}
