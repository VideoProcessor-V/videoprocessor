// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VectorEngine.h"

#include <DebugLog.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>
#include <libplacebo/d3d11.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/MotionRecordPublish.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/motion/estimation/VectorEngineFallbackSync.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VpMotion
{
	namespace
	{
		// W2-C1: a validity epoch that moved this recently is a resync window (vectors invalid until the next pair); a time constant
		// of a recovery, not a rate.
		constexpr ULONGLONG kResyncWindowMs = 1000;

		// W2-C1: the producer part of the engine facts (as PlanProducer leaves it), then the state. Render thread.
		void RefreshEngineStatus(VectorEngine& e, ULONGLONG nowMs)
		{
			VectorDemand::EngineFacts& f = e.motionEngineFacts;
			f.opticalFlow = e.motionOpticalFlow;
			f.lowLatency = e.motionLowLatency;
			f.lowLatencyNoVectors = e.motionLowLatencyNoVectors;
			f.producerRunning = e.motionProducer && e.motionProducer->Running();
			f.producerRecovering = e.motionProducer && e.motionProducer->GetHealth().recovering;
			f.nvofStartError = e.motionProducerStartFailed;   // the last start failed (latched until one succeeds or the want ends)
			f.vramRelease = e.motionVramRelease;
			const uint64_t epoch = e.motionValidity.Epoch();
			if (e.motionStatusEpoch == 0)
				e.motionStatusEpoch = epoch;   // the first refresh seeds the epoch (MotionValidity::kInitialEpoch): no resync window
			else if (epoch != e.motionStatusEpoch)
			{
				e.motionStatusEpoch = epoch;
				e.motionStatusEpochAtMs = nowMs;
			}
			f.resyncWindow = e.motionStatusEpochAtMs != 0 && nowMs - e.motionStatusEpochAtMs < kResyncWindowMs;
			e.motionEngineStatus = VectorDemand::DeriveEngineStatus(f);
			if (e.motionCameraGated)
				e.motionEngineStatus.affected |= 1u << static_cast<int>(VectorDemand::Consumer::CameraFit);
		}
	}

	VectorDemand::MotionEngineWork VectorEngine::Work() const
	{
		VectorDemand::MotionEngineWork w = motionEngineWork;
		const std::shared_ptr<MotionAhead::Producer> producer = LoopProducer();
		w.producerPairs = motionEnginePairsBase + (producer ? producer->GetStats().pairs : 0ull);
		return w;
	}

	// docs/VP-nvof-ahead.md (owner 2026-10-16): the optical-flow depth for this frame - the configured
	// depth, off in low latency or without the engine, never more than the frames queued. Logged when
	// it changes (the matcher covers the rest). The expressions are the renderer's old inline block
	// (LibplaceboVideoRenderer::RenderLocked at cda5794e), decided by ProducerPlan.
	void VectorEngine::PlanProducer(const ProducerInputs& in)
	{
		// The producer runs while the de-judder does, with optical flow on, outside low latency and with a
		// depth set; it stops otherwise (the matcher covers all).
		ProducerPlan::LifecycleInput life;
		life.lowLatency = in.lowLatency->load(std::memory_order_acquire);
		life.opticalFlow = in.opticalFlow;
		life.motionAheadFrames = in.motionAheadFrames;
		motionAheadSetting = in.motionAheadFrames;   // SK-MV: kept for the no-pair cause
		// Owner 2026-10-16: the producer stays alive once wanted (starting it again would cost a device on
		// every queue refill); with no frames queued ahead it gets no frames, so it computes nothing it
		// could not use - the startup fill and an emptied queue alike.
		// Vector engine on demand: also while a consumer of vectors is served (forward, or backward pairs).
		life.vectorProducer = vectorDemand.producer;
		life.vectorLowLatencyAllowed = vectorDemand.lowLatencyAllowed;
		motionLowLatencyNoVectors = life.lowLatency && !vectorDemand.lowLatencyAllowed;
		motionOpticalFlow = in.opticalFlow;
		motionLowLatency = life.lowLatency;
		life.dejudderPresent = in.dejudderPresent;
		life.producerExists = motionProducer != nullptr;
		life.producerRunning = motionProducer && motionProducer->Running();
		life.startFailedLatched = motionProducerStartFailed;
		const ULONGLONG nowMs = GetTickCount64();
		// W2-C1: a failed start is retried on the recovery back-off (5 s, then 60 s), not every frame and not only on a settings change.
		life.retryDue = motionProducerStartFailed && motionProducerStartRetry.Recovering() && nowMs >= motionProducerStartRetry.RetryAtMs();
		// ... and a latch cleared by an unwanted frame does not skip the back-off (a want flip never restarts it early).
		life.backoffActive = motionProducerStartRetry.Recovering() && nowMs < motionProducerStartRetry.RetryAtMs();
		const bool startFailureInjected = !producerStartFailureForTesting_.empty();   // C1-4 test seam: empty in the product
		life.gpuAvailable = in.gpu != nullptr || startFailureInjected;
		const ProducerPlan::LifecycleOutput act = ProducerPlan::Lifecycle(life);
		if (act.action == ProducerPlan::Action::Start)
		{
			pl_d3d11 d3d = startFailureInjected ? nullptr : pl_d3d11_get(in.gpu);
			auto producer = std::make_unique<MotionAhead::Producer>();
			// The slow (best) preset: off the render path, and on the 7 real clips the seed with it removed
			// 41.9% of the judder vs the matcher's 41.3% (damage 0.35 vs 0.40; medium 41.2%, 0.37). The
			// render-time fallback keeps the matcher's preset (medium).
			const NvOpticalFlow::Preset preset = NvOpticalFlow::Preset::Slow;
			producer->SetSlot(p2Slot);   // SK-MV: the producer publishes P2 into the engine's slot (it owns no store)
			if (!startFailureInjected && d3d && d3d->device && producer->Start(d3d->device, preset))
			{
				motionProducerStartError.clear();
				motionProducerStartFailed = false;
				motionProducerStopLogged = false;
				if (motionProducerStartRetry.Recovering())
					motionProducerStartRetry.OnRestarted(nowMs);
				++motionEngineWork.producerStarts;
				PublishProducer(std::shared_ptr<MotionAhead::Producer>(std::move(producer)));   // hardening: atomic publish (HOOKS.md section 2)
			}
			else
			{
				const std::string startError = startFailureInjected ? producerStartFailureForTesting_ :
					producer->Error().empty() ? std::string("no render device") : producer->Error();
				// W2-C1 (lead 2026-11-11): logged once per failure episode (and when the reason changes), not at every retry.
				const bool logIt = !motionProducerStartFailed || startError != motionProducerStartError;
				motionProducerStartFailed = true;
				motionProducerStartError = startError;   // SK-MV: the real reason, shown
				const uint64_t retryMs = motionProducerStartRetry.OnStopped(nowMs);
				if (logIt)
					DebugLog::Log("Motion ahead: the producer did not start (%s); the de-judder takes the block searches (no optical-flow pair); retried in %llu s",
					startError.c_str(), static_cast<unsigned long long>(retryMs / 1000));
			}
		}
		else if (act.action == ProducerPlan::Action::Stop)
		{
			// Owner: the producer stays alive once wanted - kept while the settings still want it (a resync
			// or format change can take longer than any timer); stopped when they change.
			motionEnginePairsBase += motionProducer->GetStats().pairs;   // W2-C1: Work().producerPairs keeps them
			motionProducer->Stop();
			PublishProducer(nullptr);   // hardening: a loop copy keeps the stopped producer alive until its use ends
		}
		// Audit 2026-10-16: a failure is retried once the producer is wanted again (optical flow off and on,
		// low latency and back), and the look-ahead re-engages only after the refill - never on a stale
		// engagement.
		if (act.clearStartFailed)
		{
			motionProducerStartFailed = false;   // W2-C1: the back-off (motionProducerStartRetry) is kept: it ends only on a start
			motionProducerStopLogged = false;
		}
		if (act.disengage)
			motionAheadEngaged = false;
		if (motionProducer && motionProducer->Running())
			motionProducerStopLogged = false;   // W2-C1: running again (its own restart): a later stop is logged again
		else if (motionProducer && !motionProducerStopLogged)
		{
			motionProducerStopLogged = true;
			DebugLog::Log("Motion ahead: the producer stopped (%s); the de-judder estimates at render time",
				motionProducer->Error().c_str());
		}
		// SK-MV: the runtime one-engine guard (NvOpticalFlow.h Sessions): a second Engine session on a GPU is logged (a bug, never silent).
		const unsigned long long nvofViolations = NvOpticalFlow::Sessions::Violations();
		if (nvofViolations != nvofViolationsSeen)
		{
			nvofViolationsSeen = nvofViolations;
			DebugLog::Log("NVOF guard: a second NvOpticalFlow::Engine session was opened on one GPU (%llu in total) - the vector engine's producer is the only one allowed",
				nvofViolations);
		}
		// SK-MV: the standing cost is visible. While the de-judder keeps the engine warm (it is a consumer by the estimating reasons or
		// the standing demand) the producer's pairs per second and ms per pair are a status line, every 5 s, and the accessor for Ctrl+I.
		{
			const bool standing = motionProducer && motionProducer->Running() && (in.dejudderEstimating || in.dejudderCanEngage) && in.dejudderPresent;
			const auto nowSteady = std::chrono::steady_clock::now();
			if (in.dejudderCanEngage)
				motionStandingHookSeen = true;
			if (!standing)
			{
				// Without the host's hook line (one line, the P5b Z3 turn) the standing demand cannot cover the reasons that precede Active
				// (UnknownRates, PlaneFormat); the engine is warm only for Active / NoNextFrame / NextFrameDiffers. Said, never silent.
				const bool waiting = !motionStandingHookSeen && in.dejudderPresent && in.opticalFlow && motionProducer && motionProducer->Running();
				const std::string line = waiting ? "motion engine standing: waiting for host hook (P5b Z3 turn); warm for Active / NoNextFrame / NextFrameDiffers only" : std::string();
				if (line != motionStandingLine)
				{
					motionStandingLine = line;
					if (!line.empty())
						DebugLog::Log("%s", line.c_str());
				}
				motionStandingAt = nowSteady;
			}
			else if (motionStandingAt.time_since_epoch().count() == 0 || nowSteady - motionStandingAt >= std::chrono::seconds(motionStandingLine.empty() ? 1 : 5))   // the first figure after 1 s, then every 5 s
			{
				const MotionAhead::Producer::Stats ps = motionProducer->GetStats();
				if (motionStandingAt.time_since_epoch().count() != 0 && ps.pairs >= motionStandingPairs)
				{
					const double seconds = std::chrono::duration<double>(nowSteady - motionStandingAt).count();
					const unsigned long long pairs = ps.pairs - motionStandingPairs;
					const double sum = ps.pairMsSum - motionStandingMsSum;
					char line[160];
					std::snprintf(line, sizeof(line), "motion engine standing for de-judder: %.1f pairs/s, %.2f ms/pair", seconds > 0.0 ? pairs / seconds : 0.0,
						pairs ? sum / static_cast<double>(pairs) : 0.0);
					if (motionStandingLine != line)
						DebugLog::Log("%s", line);
					motionStandingLine = line;
				}
				motionStandingAt = nowSteady;
				motionStandingPairs = ps.pairs;
				motionStandingMsSum = ps.pairMsSum;
			}
		}
		// Frames are submitted only while a motion estimate runs for this source (the de-judder or the
		// interpolator); otherwise the producer stays alive, idle. Fixed-constant batch B3 2026-10-18
		// (general_rate_fixes): the depth as a time with a frame floor.
		ProducerPlan::DepthInput depth;
		depth.lowLatency = life.lowLatency;
		depth.opticalFlow = in.opticalFlow;
		depth.motionAheadFrames = in.motionAheadFrames;
		depth.generalRateFixes = in.generalRateFixes;
		depth.sourceHz = in.sourceHz;
		depth.vectorLowLatencyAllowed = vectorDemand.lowLatencyAllowed;
		depth.vectorBackward = vectorDemand.mode == VectorDemand::Mode::Backward;
		depth.vectorProducer = vectorDemand.producer;
		depth.dejudderEstimating = in.dejudderEstimating;
		// SK-MV: the standing demand while the de-judder is enabled AND eligible (DejudderPolicy::CanEngage). The estimating reasons
		// (Active / NoNextFrame / NextFrameDiffers) are already a consumer through dejudderEstimating; this widens it to the reasons
		// that become Active at a transient (UnknownRates, PlaneFormat) once the host sets dejudderCanEngage. An idle matcher kept loaded
		// for hiccup repair (Off / IneligibleCadence / VideoRateMaskOnly) is NOT standing: the idle engine is not run.
		depth.dejudderStanding = in.dejudderPresent && in.opticalFlow && in.dejudderCanEngage;
		depth.producerRunning = motionProducer && motionProducer->Running();
		depth.queuedFutureFrames = in.queuedFutureFrames->load(std::memory_order_acquire);
		const ProducerPlan::DepthOutput plan = ProducerPlan::Depth(depth);
		const MotionAhead::Decision& ahead = plan.ahead;
		motionAheadDepth.store(plan.storedDepth, std::memory_order_release);
		motionAheadBackward.store(plan.storedBackward, std::memory_order_release);
		motionAheadConfigured.store(plan.storedConfigured, std::memory_order_release);
		if (motionProducer)   // fixed-constant batch B3/B4 2026-10-18
		{
			motionProducer->SetDepth(ahead.configured);
			motionProducer->SetGeneralRates(in.generalRateFixes);
		}
		if (plan.disengage)
			motionAheadEngaged = false;   // no frames collected below: no stale engagement (audit 2026-10-16)
		char text[200];
		std::snprintf(text, sizeof(text), "Motion ahead: configured %d, queued ahead %d, depth %d, lead %d (%s)",
			ahead.configured, (std::min)(ahead.available, 99), ahead.depth, MotionAhead::LeadFor(ahead.depth, static_cast<size_t>((std::max)(0, ahead.available))),
			MotionAhead::Describe(ahead.reason));
		// Log the reason and depth, not every change of the queued count.
		char key[96];
		std::snprintf(key, sizeof(key), "%d|%d|%d", ahead.configured, ahead.depth, static_cast<int>(ahead.reason));
		// The queue level swings around the depth as frames arrive and are shown: an Active / ShallowQueue
		// flip is logged at most every 5 s (audit 2026-10-16: up to once per frame).
		const bool levelOnly = ahead.reason == MotionAhead::Reason::Active || ahead.reason == MotionAhead::Reason::ShallowQueue;
		const auto now = std::chrono::steady_clock::now();
		if (motionAheadLogged != key && (!levelOnly || now - motionAheadLoggedAt >= std::chrono::seconds(5)))
		{
			motionAheadLogged = key;
			motionAheadLoggedAt = now;
			DebugLog::Log("%s", text);
		}
		RefreshEngineStatus(*this, nowMs);   // W2-C1: the published state is same-frame (VectorEngineState.h, the one-frame lag)
	}

	// ---- P5a-4 -------------------------------------------------------------------------------------------------
	void VectorEngine::UpdateVectorEngine(const VectorDemand::Input& input, const UpdateFacts& facts, IVectorEngineHost& host)
	{
		const int cameraFit = static_cast<int>(VectorDemand::Consumer::CameraFit);
		const ULONGLONG now = GetTickCount64();
		// W2-C1: the camera fit (a starter) reads only the optical-flow seed (SeedFieldOf): with optical flow off, or while the producer's
		// failed start waits for its back-off, the matcher alone would give it nothing, so it does not start the engine (its own
		// "camera fit needs the optical-flow seed" status shows; the "motion engine" entry names it).
		const bool cameraWanted = cameraFitWanted_.load(std::memory_order_acquire) || input.wants[cameraFit];
		motionOpticalFlow = facts.opticalFlow;   // this frame's setting (PlanProducer refreshes it too)
		motionCameraGated = cameraWanted && (!motionOpticalFlow ||
			(motionProducerStartRetry.Recovering() && now < motionProducerStartRetry.RetryAtMs()));
		if ((cameraWanted && !input.wants[cameraFit]) || (motionCameraGated && input.wants[cameraFit]))
		{
			VectorDemand::Input merged = input;   // the camera fit asked for the field: one more consumer of the same decision
			merged.wants[cameraFit] = !motionCameraGated;
			vectorDemand = VectorDemand::Decide(merged);
		}
		else
			vectorDemand = VectorDemand::Decide(input);
		// W2-C1: refused starts are counted, and the engine state is derived from this decision.
		for (int i = 0; i < VectorDemand::kConsumerCount; ++i)
			if ((vectorDemand.refused & (1u << i)) != 0)
				++motionEngineWork.refusedStarts[i];
		motionEngineFacts.vectorDemand = facts.vectorDemandSetting;
		motionEngineFacts.decision = vectorDemand;
		motionEngineFacts.starterEngaged = input.starterEngaged || (vectorDemand.served & VectorDemand::StarterMask()) != 0;
		motionEngineFacts.frame = ++motionEngineFrame;
		RefreshEngineStatus(*this, now);
		if (!facts.vectorDemandSetting)
		{
			if (!vectorEngineLogged.empty())
			{
				vectorEngineLogged.clear();
				VectorEngineFallbackSync::Release(host);
			}
			return;
		}
		// What decides the output dither's want, logged when it changes (a dither that runs but is not served
		// is explained by one of the three).
		{
			const int ditherKey = (facts.sigmaDeltaRanLast ? 1 : 0) | (facts.ditherVectorsOwn ? 2 : 0) | (host.DitherVectorsSupported() ? 4 : 0);
			if (ditherKey != vectorDitherKeyLogged)
			{
				vectorDitherKeyLogged = ditherKey;
				DebugLog::Log("Vector engine: output dither inputs: sigma-delta ran %s, own search requested %s, vectors supported %s",
					(ditherKey & 1) ? "yes" : "no", (ditherKey & 2) ? "yes" : "no", (ditherKey & 4) ? "yes" : "no");
			}
		}
		const std::string line = VectorDemand::StatusLine(vectorDemand);
		if (line != vectorEngineLogged)
		{
			vectorEngineLogged = line;
			DebugLog::Log("%s", line.c_str());
			DiagnosticHistory::Global().RecordDecision("vector-engine", line.c_str(),
				static_cast<int64_t>(vectorDemand.mode), static_cast<int64_t>(vectorDemand.served),
				static_cast<double>(vectorDemand.unserved), 0.0);
			host.PublishStatus(true);
		}
		// The look-ahead stores nothing for this size while pairs are submitted: sustained = visible.
		if (motionProducer && motionProducer->Running() && vectorDemand.producer)
		{
			const uint64_t last = motionProducer->LastSubmitted();
			if (vectorProducerFirstSubmit == 0 && last != 0) vectorProducerFirstSubmit = last;
			const auto ps = motionProducer->GetStats();
			if (ps.pairs > 0) vectorProducerSkipSustained = false;
			else if (vectorProducerFirstSubmit != 0 && last >= vectorProducerFirstSubmit + 48) vectorProducerSkipSustained = true;
		}
		else
		{
			vectorProducerFirstSubmit = 0;
			vectorProducerSkipSustained = false;
		}
		VectorEngineFallbackSync::Sync(*this, host);
		if (vectorDemand.mode != VectorDemand::Mode::Off && (vectorStatsTick == 0 || now - vectorStatsTick >= 5000))
		{
			vectorStatsTick = now;
			DebugLog::Log("Vector engine: %llu pairs (ahead hits %llu, misses %llu, backward %llu)", vectorPairs,
				static_cast<unsigned long long>(motionAheadHits), static_cast<unsigned long long>(motionAheadMisses), vectorBackwardPairs);
			const FlowReadCounters fr = FlowReads();   // a CPU flow reader exists (FlowOf): its absences are visible, by reason
			if (fr.requests)
				DebugLog::Log("Vector engine: flow reads %llu, served %llu, absent %llu (no producer %llu, no pair %llu, invalidated %llu, size unknown %llu)",
					fr.requests, fr.served, fr.requests - fr.served, fr.noProducer, fr.noPair, fr.invalidated, fr.unscaled);
		}
	}

	void VectorEngine::NoteGlobalPair(bool hit)
	{
		if (!vectorDemand.Serves(VectorDemand::Consumer::SourceMonitorPan) && !vectorDemand.Serves(VectorDemand::Consumer::MotionIntent))
		{
			vectorGlobalMisses = vectorGlobalHits = 0;
			vectorGlobalMissSustained = false;
			return;
		}
		constexpr unsigned kRun = 24;   // consecutive pairs, either way, before the entry changes
		if (hit) { ++vectorGlobalHits; vectorGlobalMisses = 0; if (vectorGlobalHits >= kRun) vectorGlobalMissSustained = false; }
		else { ++vectorGlobalMisses; vectorGlobalHits = 0; if (vectorGlobalMisses >= kRun) vectorGlobalMissSustained = true; }
	}

	void VectorEngine::NoteVectorsDelivered(bool backward, const char* source)
	{
		const int mode = backward ? 1 : 0;
		if (vectorDeliveredLogged == mode)
			return;
		vectorDeliveredLogged = mode;
		DebugLog::Log("Vector engine: vectors delivered (%s, source %s)", backward ? "backward" : "forward", source);
	}

	void VectorEngine::SizeFor(bool lowResSearch, int width, int height, int& engineW, int& engineH, int& halvings)
	{
		halvings = DejudderPolicy::SearchHalvingsFor(lowResSearch, height);
		engineW = (width + 1) / 2;
		engineH = (height + 1) / 2;
		for (int i = 0; i < halvings; ++i) { engineW = (engineW + 1) / 2; engineH = (engineH + 1) / 2; }
	}

	void VectorEngine::ServeStatistics(uint64_t sequence, uint64_t nextSequence, int width, int height, const ServeFacts& facts,
		IVectorEngineHost& host)
	{
		const bool backward = vectorDemand.mode == VectorDemand::Mode::Backward;
		const bool forwardStats = vectorDemand.mode == VectorDemand::Mode::Forward && !vectorDemand.estimate && !facts.legacyConsumer &&
			(vectorDemand.Serves(VectorDemand::Consumer::SourceMonitorPan) || vectorDemand.Serves(VectorDemand::Consumer::MotionIntent));
		if ((!backward && !forwardStats) || !motionProducer || !motionProducer->Running() ||
			sequence == 0 || sequence == vectorBackwardSequence)
			return;
		vectorBackwardSequence = sequence;
		int engineW = 0, engineH = 0, halvings = 0;
		SizeFor(facts.lowResSearch, width, height, engineW, engineH, halvings);
		NoteFlowSourceWidth(width);
		motionProducer->SetTargetSize(engineW, engineH);
		MotionAhead::FlowPtr flow;
		uint64_t from = 0, to = 0;
		if (forwardStats)
		{
			from = sequence;
			to = nextSequence;
			if (nextSequence == sequence + 1 &&
				motionValidity.PairServes(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()))   // W1-C2a A1 / S2
				flow = FindCurrent(from, to);   // C2a-F 2: never a pair of another stream generation
			NoteGlobalPair(flow != nullptr);
		}
		else
			for (int age = 0; age < 2 && !flow && sequence > static_cast<uint64_t>(age + 1); ++age)
			{
				from = sequence - 1 - age;
				to = sequence - age;
				if (motionValidity.PairServes(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()))   // W1-C2a A1 / S2
					flow = FindCurrent(from, to);   // C2a-F 2
			}
		// SK-MV row 24: the frame-analysis record carries the shared engine's global, or "absent" (never a stale value). The store is
		// null while frame_analysis_store is off / the Original pathway runs.
		FrameAnalysis::Store* const recordStore = host_ ? VpFrameAnalysis::SourceFrameTap::StoreFor(host_) : nullptr;
		if (!flow)
		{
			++motionAheadMisses;
			if (recordStore && (!forwardStats || nextSequence > sequence))
				MotionRecord::Publish(recordStore, forwardStats ? nextSequence : sequence, MotionRecord::Absent());
			P2().Prune(forwardStats ? sequence : (sequence > 2 ? sequence - 2 : 0));
			return;
		}
		++motionAheadHits;
		if (backward) ++vectorBackwardPairs;
		++vectorPairs;
		NoteVectorsDelivered(backward, backward ? "engine" : "engine ahead");
		// W1-C4: the pair's ONE sample, read by the record and by the one feed (MotionRecordPublish.h).
		const MotionRecord::PairSample sample = MotionRecord::SampleOf(from, to, flow->global[0], halvings, facts.pictureHeight);
		if (recordStore)
			MotionRecord::Publish(recordStore, to, MotionRecord::SummaryOf(
				motionValidity.PairServes(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()), sample));   // W1-C2a A1 / S2
		MotionRecord::Feed(sample, vectorDemand.Serves(VectorDemand::Consumer::SourceMonitorPan),
			vectorDemand.Serves(VectorDemand::Consumer::MotionIntent), host);
		if (backward && facts.vectorDeliveryRuns)
			motionSegment.Publish(VpMotion::Direction::Backward, from, to, MotionShadowField{ from, to },
				VpMotion::Source::EngineAhead, motionValidity.Epoch(), 0, false);
		P2().Prune(forwardStats ? sequence : (sequence > 2 ? sequence - 2 : 0));
	}

	void VectorEngine::MotionShadowCheck(bool deliveryRuns, uint64_t sequence, uint64_t nextSequence, bool hostLive)
	{
		// Modularity sweep 2026-10-25: no shadow state while the delivery slot is off (Original pathway).
		if (!deliveryRuns)
			return;
		const auto d = motionSegment.Fetch(VpMotion::Consumer::Interpolator, VpMotion::Direction::Forward,
			sequence, nextSequence, 0);
		++motionShadowChecks;
		if (static_cast<bool>(d) == hostLive)
			return;
		if (++motionShadowMismatches == 1 || motionShadowMismatches % 2400 == 0)
			DebugLog::Log("Motion segment (shadow): mismatch source=%llu next=%llu host=%d segment=%d (%s; %llu of %llu)",
				static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(nextSequence), hostLive ? 1 : 0,
				d ? 1 : 0, VpMotion::Describe(d.miss), motionShadowMismatches, motionShadowChecks);
	}

	bool VectorEngine::TakeInvalidation(MotionValidity::Pending& pending)
	{
		pending = motionValidity.Consume(motionValiditySeen);
		if (!pending.changed)
			return false;
		motionInvalidations += pending.invalidations;
		return true;
	}

	void VectorEngine::LogInvalidation(const MotionValidity::Pending& pending)
	{
		const int cause = static_cast<int>(pending.cause);
		const ULONGLONG now = GetTickCount64();
		if (motionInvalidations <= 50 || now - motionInvalidateLogTick >= 5000 || cause != motionInvalidateLoggedCause)
		{
			// The text as before the MotionValidity wiring (the harness reads it): a frame gap names its own cause
			// ("missed frames", "backlog flush", "queue overflow drop", "pulldown lock change"), and a release
			// pending with other causes reads as the resync it is (the strongest request names the line, as the
			// old flag did).
			const char* detail = pending.cause == MotionValidity::Cause::Gap ?
				motionGapDetail.load(std::memory_order_relaxed) : nullptr;
			const char* text = pending.release && !MotionValidity::ReleasesResources(pending.cause) ?
				MotionValidity::Describe(MotionValidity::Cause::Resync) : detail ? detail : MotionValidity::Describe(pending.cause);
			DebugLog::Log("De-judder: motion invalidated (%s; %llu in total); the vectors' users fall back until a fresh pair",
				text, static_cast<unsigned long long>(motionInvalidations));
			motionInvalidateLogTick = now;
			motionInvalidateLoggedCause = cause;
			motionRecoveryLogPending = true;
		}
	}

	// ---- P5a-5: the host -> engine directory (Query("vpv.vector_engine.v1")) --------------------------------------
	namespace
	{
		class EngineDirectory : public IVectorEngineDirectory
		{
		public:
			IVectorEngine* EngineFor(const void* host) override
			{
				std::lock_guard<std::mutex> lock(mutex_);
				return Newest(host);
			}
			// f(engine) for the newest engine of `host`, under the lock (the engine cannot be removed meanwhile).
			template<class F>
			bool WithEngine(const void* host, F&& f)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				VectorEngine* engine = Newest(host);
				if (!engine)
					return false;
				f(*engine);
				return true;
			}
			void Add(const void* host, VectorEngine* engine)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				engines_.emplace_back(host, engine);
			}
			void Remove(const void* host, VectorEngine* engine)
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (auto it = engines_.begin(); it != engines_.end(); ++it)
					if (it->first == host && it->second == engine)
					{
						engines_.erase(it);
						return;
					}
			}

		private:
			// The newest registration of a host wins (a replacement renderer never shares its predecessor's key).
			VectorEngine* Newest(const void* host) const
			{
				for (auto it = engines_.rbegin(); it != engines_.rend(); ++it)
					if (it->first == host)
						return it->second;
				return nullptr;
			}

			std::mutex mutex_;
			std::vector<std::pair<const void*, VectorEngine*>> engines_;
		};

		EngineDirectory& Engines()
		{
			static EngineDirectory* directory = new EngineDirectory();   // never destroyed (see VectorEngine::Directory)
			return *directory;
		}
	}

	VectorEngine::VectorEngine(const void* host) : host_(host)
	{
		if (host_)
			Engines().Add(host_, this);
	}

	VectorEngine::VectorEngine(const void* host, InvalidateSink sink) : host_(host), invalidateSink_(sink)
	{
		if (host_)
			Engines().Add(host_, this);
	}

	VectorEngine::~VectorEngine()
	{
		// Contract (SetPreEstimateHook): the consumer clears its hook before the engine goes; say so if it did not.
		if (preEstimateHook_.load(std::memory_order_acquire) != nullptr)
			DebugLog::Log("Vector engine: destroyed with the pre-estimate hook still registered (the consumer must clear it first)");
		if (host_)
			Engines().Remove(host_, this);
	}

	IVectorEngineDirectory& VectorEngine::Directory()
	{
		return Engines();
	}

	bool VectorEngine::RequestInvalidationFor(const void* host, MotionValidity::Cause cause, const char* gapDetail)
	{
		if (!host)
			return false;
		return Engines().WithEngine(host, [&](VectorEngine& engine) {
			if (cause == MotionValidity::Cause::Gap)   // stored even when null, as the old dequeue code did
				engine.motionGapDetail.store(gapDetail, std::memory_order_relaxed);   // why, for the log
			if (engine.invalidateSink_)
				engine.invalidateSink_(engine.host_, cause);
			else
				engine.motionValidity.Invalidate(cause);
		});
	}

	bool VectorEngine::RegisterResyncFor(const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator)
	{
		if (!host)
			return false;
		return Engines().WithEngine(host, [&](VectorEngine& engine) {
			VpMotionResync::RegisterOn(indicator, engine.motionValidity, engine.motionResyncReaders);
		});
	}

	bool VectorEngine::BindResyncFor(const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator)
	{
		if (!host)
			return false;
		return Engines().WithEngine(host, [&](VectorEngine& engine) {
			if (indicator)
				engine.motionValidity.Bind(indicator);
			else
				engine.motionValidity.Unbind();
		});
	}

	bool VectorEngine::ResyncBoundFor(const void* host)
	{
		bool bound = false;
		if (host)
			Engines().WithEngine(host, [&](VectorEngine& engine) { bound = engine.motionValidity.Driven(); });
		return bound;
	}

	bool VectorEngine::ReleaseProducerFor(const void* host)
	{
		std::shared_ptr<MotionAhead::Producer> producer;
		if (host)
			Engines().WithEngine(host, [&](VectorEngine& engine) {
				producer = std::atomic_exchange(&engine.motionProducer, std::shared_ptr<MotionAhead::Producer>());
				// W2-C1: a GPU release is a recovery event: both latches and the start back-off are cleared, so the producer starts
				// again on the new device once wanted (the old single latch could block it until a settings change).
				engine.motionProducerStartFailed = false;
				engine.motionProducerStopLogged = false;
				engine.motionProducerStartRetry = MotionAhead::ProducerRecovery();
				if (producer)
					engine.motionEnginePairsBase += producer->GetStats().pairs;
			});
		if (!producer)
			return false;
		producer->Stop();   // joins its worker; a render-loop copy still held only sees a stopped producer
		return true;
	}

	// ---- P5a-5: records for IVectorEngine (render thread; CPU only, nothing logged) ----------------------------------
	void VectorEngine::NoteEstimated(bool done, uint64_t aSequence, uint64_t bSequence, uint64_t epochAtChoice)
	{
		if (!done || aSequence == 0 || bSequence <= aSequence)
			return;
		// An invalidation that landed between the pair's choice and here: the field is of an older epoch, never
		// recorded (the old record stays and is no longer current either).
		if (epochAtChoice == 0 || epochAtChoice != motionValidity.Epoch())
			return;
		if (epochAtChoice != field_.epoch || field_.from == 0)
			eraFirstFrom_ = aSequence;
		field_.from = aSequence;
		field_.to = bSequence;
		field_.epoch = epochAtChoice;
	}

	FallbackCatalog::Decision VectorEngine::NoteHookOutcome(bool failed, const std::string& what)
	{
		if (failed && (hookFailures_.failures < 20 || (hookFailures_.failures + 1) % 100 == 0))
			DebugLog::Log("Vector engine: the pre-estimate hook failed (%s; %llu failures); the plain estimate ran",
				what.c_str(), hookFailures_.failures + 1);
		return hookFailures_.Note(failed, what);
	}

	FallbackCatalog::Decision VectorEngine::ReleaseHookFailures()
	{
		hookFailures_.engaged = false;
		hookFailures_.goodSince = 0;
		return FallbackCatalog::Released(hookFailures_.id);
	}

	void VectorEngine::NoteEngineGlobal(uint64_t from, uint64_t to, const float* global, int searchHalvings)
	{
		// A caller without the pair's sample: sampled here (usable does not depend on the picture height).
		NoteEngineGlobal(from, to, global, searchHalvings, global && from != 0 && to > from &&
			MotionRecord::SampleOf(from, to, global, searchHalvings, 0).global.usable);
	}

	void VectorEngine::NoteEngineGlobal(uint64_t from, uint64_t to, const float* global, int searchHalvings, bool usable)
	{
		if (from == 0 || to <= from)
			return;
		GlobalRecord& r = global_[to % kGlobalRing];
		r = GlobalRecord();
		r.from = from;
		r.to = to;   // no producer pair: the slot says so (valid false), an older pair's record is not served for it
		if (!global)
			return;
		const double cell = DejudderPolicy::EngineCellSourcePixels(searchHalvings);
		r.epoch = motionValidity.Epoch();
		// Usable as the statistics consumers use it (W1-C4: the pair's one sample, MotionRecord::SampleOf: ambiguity z < 0.9).
		r.valid = usable;
		r.x = cell * static_cast<double>(global[0]);
		r.y = cell * static_cast<double>(global[1]);
	}

	// ---- P5a-5: IVectorEngine from today's state ---------------------------------------------------------------------
	uint64_t VectorEngine::LatestVerifiedFieldSeq() const
	{
		VerifiedField field;
		return FieldOf(field_.to, field) ? field_.to : 0;
	}

	uint64_t VectorEngine::FieldEra() const
	{
		return motionValidity.Epoch();
	}

	bool VectorEngine::FieldOf(uint64_t seq, VerifiedField& out) const
	{
		if (!matcher_ || seq == 0 || field_.from == 0 || seq != field_.to || matcher_->LiveFieldFrom() != field_.from ||
			!motionValidity.Current(field_.epoch))
			return false;
		const pl_tex fwd = matcher_->LiveField();
		if (!fwd)
			return false;
		out = VerifiedField();
		out.seq = field_.to;
		out.pairFrom = field_.from;
		out.pairTo = field_.to;
		out.fwd = fwd;
		out.gridW = fwd->params.w;
		out.gridH = fwd->params.h;
		out.blockSize = 16;          // one texel per 16x16 source block (MotionWarp::Field)
		out.scaleToSourcePx = 1.0f;  // the field's vectors are in source px
		// The stamp of the pair's later frame from the bound Indicator (resync on); unbound: the default Stamp.
		motionValidity.StampAt(field_.to, out.stamp);
		return true;
	}

	// F2: the engine's seed of the live pair (the camera fit's input; never the guided live field). The same validity,
	// epoch and lifetime rules as FieldOf (the last successful estimate, its epoch current, the matcher's live outputs still
	// that pair's), the same stamp, plus the matcher's own record that the seed fed this estimate. The seed's layout is
	// checked against the live field's grid (never assumed): a texel per 16 x 2^halvings source px, the seed covering the
	// live 16-px grid exactly (ceil(live / 2^halvings) texels per axis).
	bool VectorEngine::SeedFieldOf(uint64_t pairTo, SeedField& out) const
	{
		out = SeedField();   // absence NotSeeded unless the matcher says the live pair was skipped
		if (!matcher_ || pairTo == 0 || field_.from == 0 || pairTo != field_.to || matcher_->LiveFieldFrom() != field_.from ||
			!motionValidity.Current(field_.epoch))
			return false;
		IMatcherView::Seed s;
		if (!matcher_->LiveSeed(s))
		{
			if (s.skipped)
				out.absence = SeedAbsence::Skipped;
			return false;
		}
		if (!s.fwd || !s.bwd)
			return false;
		constexpr int kMaxHalvings = 4;   // 16K; MotionWarp halves while the input is about 1530 lines or taller
		if (s.halvings < 0 || s.halvings > kMaxHalvings || s.gridW <= 0 || s.gridH <= 0 || s.liveGridW <= 0 || s.liveGridH <= 0)
			return false;
		if (s.fwd->params.w != s.gridW || s.fwd->params.h != s.gridH || s.bwd->params.w != s.gridW || s.bwd->params.h != s.gridH)
			return false;
		const int per = 1 << s.halvings;   // live 16-px blocks per seed texel and axis
		if ((s.liveGridW + per - 1) / per != s.gridW || (s.liveGridH + per - 1) / per != s.gridH)
			return false;
		if (!(s.tolerance > 0.0f) || !(s.consistencyPx > 0.0f) || !(s.weightPenalty >= 0.0f))
			return false;
		out = SeedField();
		out.seq = field_.to;
		out.pairFrom = field_.from;
		out.pairTo = field_.to;
		out.fwd = s.fwd;
		out.bwd = s.bwd;
		out.gridW = s.gridW;
		out.gridH = s.gridH;
		out.blockSize = SeedField::kSourceBlockSize * per;
		out.scaleToSourcePx = 2.0f * static_cast<float>(per);   // half-size px of the searching instance -> source px
		out.sourceGridW = s.liveGridW;
		out.sourceGridH = s.liveGridH;
		out.sadTrustMax = s.tolerance;
		out.weightPenalty = s.weightPenalty;
		out.consistencyPx = s.consistencyPx;
		motionValidity.StampAt(field_.to, out.stamp);
		return true;
	}

	bool VectorEngine::GlobalFromEngine() const
	{
		return matcher_ && matcher_->MatcherGlobalFromEngine();
	}

	bool VectorEngine::EngineGlobal(uint64_t seq, GlobalVec& out) const
	{
		out = GlobalVec();
		const GlobalRecord& r = global_[seq % kGlobalRing];
		if (seq == 0 || seq != r.to || !r.valid || !motionValidity.Current(r.epoch))
			return false;
		out.valid = true;
		out.x = r.x;
		out.y = r.y;
		return true;
	}

	bool VectorEngine::FlowOf(uint64_t seq, FlowView& out) const
	{
		out = FlowView();
		std::lock_guard<std::mutex> lock(flowMutex_);
		++flowReads_.requests;
		auto absent = [this](unsigned long long& counter) {
			++counter;
			flowLease_.reset();   // nothing valid: hold nothing (the next view is built from the slot again)
			return false;
		};
		if (!LoopProducer() || motionLowLatencyNoVectors)
			return absent(flowReads_.noProducer);
		if (seq < 2)
			return absent(flowReads_.noPair);
		const uint64_t from = seq - 1, to = seq;
		// The one vectors-valid state decides first: a pair across a resync / gap / format change is never served.
		if (!motionValidity.PairServes(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()))   // W1-C2a S2
			return absent(flowReads_.invalidated);
		// The shared pointer is copied out of the slot before anything can Prune it: from here the pair cannot be freed under us.
		// C2a-F 2: a pair of another stream generation is refused (FindCurrent, VectorEngineState.h).
		bool stalePair = false;
		const MotionAhead::FlowPtr flow = FindCurrent(from, to, &stalePair);
		if (stalePair)
			return absent(flowReads_.invalidated);
		if (!flow || flow->gridW <= 0 || flow->gridH <= 0 || flow->halfW <= 0 ||
			flow->forward.size() < static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH) * 2)
			return absent(flowReads_.noPair);
		const int sourceWidth = flowSourceWidth_.load(std::memory_order_relaxed);
		if (sourceWidth <= 0)
			return absent(flowReads_.unscaled);
		const double scale = static_cast<double>(sourceWidth) / flow->halfW;   // source px per engine px (a ratio, never a constant)
		if (!flowLease_ || flowLease_->flow != flow || flowLease_->from != from || flowLease_->to != to || flowLease_->scale != scale)
		{
			auto lease = std::make_shared<FlowLease>();
			++flowReads_.conversions;   // W1-C3: once per pair id (a repeated FlowOf of the same pair reuses the lease)
			lease->computes = &flowTrustComputes_;
			lease->flow = flow;
			lease->from = from;
			lease->to = to;
			lease->scale = scale;
			const size_t cells = static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH);
			lease->x.resize(cells);
			lease->y.resize(cells);
			for (size_t i = 0; i < cells; ++i)
			{
				lease->x[i] = static_cast<float>(flow->forward[i * 2] / 32.0 * scale);
				lease->y[i] = static_cast<float>(flow->forward[i * 2 + 1] / 32.0 * scale);
			}
			flowLease_ = std::move(lease);
		}
		out.x = flowLease_->x.data();
		out.y = flowLease_->y.data();
		out.gridW = flow->gridW;
		out.gridH = flow->gridH;
		out.blockSize = 4.0 * scale;   // a cell is 4 x 4 engine px
		// The original payload, not a copy: the lease keeps `flow` (and so these arrays) alive until the next FlowOf call.
		const size_t flowCells = static_cast<size_t>(flow->gridW) * static_cast<size_t>(flow->gridH);
		out.rawForward = flow->forward.data();
		out.rawBackward = flow->backward.size() >= flowCells * 2 ? flow->backward.data() : nullptr;
		out.forwardCost = flow->forwardCost.size() >= flowCells ? flow->forwardCost.data() : nullptr;
		out.backwardCost = flow->backwardCost.size() >= flowCells ? flow->backwardCost.data() : nullptr;
		out.rawUnitsPerEnginePx = 32.0;
		out.enginePxToSource = scale;
		out.cellEnginePx = 4;
		out.engineW = flow->halfW;
		out.engineH = flow->halfH;
		out.from = from;
		out.to = to;
		// W1-C3: the producer's extras as they are (worked out once on its worker), never recomputed here.
		out.haveGlobal = flow->globalSet;
		out.globalGeneral = flow->globalGeneral;
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
			{
				out.global[d][k] = flow->global[d][k];
				out.second[d][k] = flow->second[d][k];
			}
		if (flow->blocksX > 0 && flow->blocksY > 0 &&
			flow->costWeight.size() == static_cast<size_t>(flow->blocksX) * static_cast<size_t>(flow->blocksY))
		{
			out.costWeight = flow->costWeight.data();
			out.blocksX = flow->blocksX;
			out.blocksY = flow->blocksY;
		}
		out.fbError = flow->fbError.size() == flowCells ? flow->fbError.data() : nullptr;
		out.extrasBlockPx = MotionAhead::Flow::kBlockPx;
		out.extrasCellPx = NvOpticalFlow::Engine::Grid;
		out.extrasUnitsPerEnginePx = 32.0;   // FbError reads S10.5
		out.trustMemo = flowLease_.get();
		++flowReads_.served;
		return true;
	}

	// W1-C3: the pair's cell trust, computed once per pair and tolerance (the same OpticalFlowTrust functions as every consumer used
	// to call itself: the producer's CostWeights / FbError when it worked them out, else the same maths on the raw fields).
	const float* VectorEngine::FlowLease::CellTrust(float toleranceEnginePx) const
	{
		std::lock_guard<std::mutex> lock(trustMutex);
		for (const TrustEntry& e : trusts)
			if (std::memcmp(&e.tolerance, &toleranceEnginePx, sizeof(float)) == 0)   // bit-exact: one entry per value, a NaN included (the list stays bounded)
				return e.ok ? e.trust.data() : nullptr;
		trusts.emplace_back();
		TrustEntry& entry = trusts.back();
		entry.tolerance = toleranceEnginePx;
		std::vector<float>& trust = entry.trust;
		bool& trustOk = entry.ok;
		if (computes)
			computes->fetch_add(1, std::memory_order_relaxed);
		if (!flow)
			return nullptr;
		const MotionAhead::Flow& f = *flow;
		const size_t cells = static_cast<size_t>(f.gridW > 0 ? f.gridW : 0) * static_cast<size_t>(f.gridH > 0 ? f.gridH : 0);
		const int grid = NvOpticalFlow::Engine::Grid;
		const int blockPx = MotionAhead::Flow::kBlockPx;
		const int blocksX = (f.halfW + blockPx - 1) / blockPx, blocksY = (f.halfH + blockPx - 1) / blockPx;
		std::vector<float> blockCostLocal, cellErrorLocal;
		const std::vector<float>* blockCost = &f.costWeight;
		if (!(f.blocksX == blocksX && f.blocksY == blocksY && f.costWeight.size() == static_cast<size_t>(blocksX) * static_cast<size_t>(blocksY)))
		{
			if (!OpticalFlowTrust::CostWeights(f.forwardCost, f.gridW, f.gridH, grid, blockPx, blocksX, blocksY, blockCostLocal))
				return nullptr;
			blockCost = &blockCostLocal;
		}
		const std::vector<float>* cellError = &f.fbError;
		if (cells == 0 || f.fbError.size() != cells)
		{
			if (!OpticalFlowTrust::FbError(f.forward, f.backward, f.gridW, f.gridH, grid, cellErrorLocal))
				return nullptr;
			cellError = &cellErrorLocal;
		}
		trustOk = OpticalFlowTrust::CellTrust(*blockCost, blocksX, blocksY, *cellError, f.gridW, f.gridH, grid, blockPx, toleranceEnginePx, trust);
		return trustOk ? trust.data() : nullptr;
	}

	VectorEngine::FlowReadCounters VectorEngine::FlowReads() const
	{
		std::lock_guard<std::mutex> lock(flowMutex_);
		FlowReadCounters r = flowReads_;
		r.trustComputes = flowTrustComputes_.load(std::memory_order_relaxed);
		return r;
	}

	bool VectorEngine::PairSpansSlip(uint64_t from, uint64_t to) const
	{
		constexpr uint64_t kLongestKnownSpan = 64;
		if (from == 0 || to <= from || to - from > kLongestKnownSpan)
			return true;
		// No estimate in the current epoch: where its run starts is unknown.
		if (field_.from == 0 || !motionValidity.Current(field_.epoch) || from < eraFirstFrom_)
			return true;
		for (uint64_t k = from; k < to; ++k)
			if (!motionValidity.PairValidAt(k, k + 1))
				return true;
		return false;
	}
}
