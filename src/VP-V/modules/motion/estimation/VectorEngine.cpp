// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include "VectorEngine.h"

#include <DebugLog.h>
#include <VP-V/modules/control/diagnostics/DiagnosticHistory.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>
#include <libplacebo/d3d11.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/VectorEngineFallbackSync.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VpMotion
{
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
		// Owner 2026-10-16: the producer stays alive once wanted (starting it again would cost a device on
		// every queue refill); with no frames queued ahead it gets no frames, so it computes nothing it
		// could not use - the startup fill and an emptied queue alike.
		// Vector engine on demand: also while a consumer of vectors is served (forward, or backward pairs).
		life.vectorProducer = vectorDemand.producer;
		life.vectorLowLatencyAllowed = vectorDemand.lowLatencyAllowed;
		life.dejudderPresent = in.dejudderPresent;
		life.producerExists = motionProducer != nullptr;
		life.producerRunning = motionProducer && motionProducer->Running();
		life.startFailedLatched = motionProducerFailedLogged;
		life.gpuAvailable = in.gpu != nullptr;
		const ProducerPlan::LifecycleOutput act = ProducerPlan::Lifecycle(life);
		if (act.action == ProducerPlan::Action::Start)
		{
			pl_d3d11 d3d = pl_d3d11_get(in.gpu);
			auto producer = std::make_unique<MotionAhead::Producer>();
			// The slow (best) preset: off the render path, and on the 7 real clips the seed with it removed
			// 41.9% of the judder vs the matcher's 41.3% (damage 0.35 vs 0.40; medium 41.2%, 0.37). The
			// render-time fallback keeps the matcher's preset (medium).
			const NvOpticalFlow::Preset preset = NvOpticalFlow::Preset::Slow;
			if (d3d && d3d->device && producer->Start(d3d->device, preset))
				PublishProducer(std::shared_ptr<MotionAhead::Producer>(std::move(producer)));   // hardening: atomic publish (HOOKS.md section 2)
			else
			{
				motionProducerFailedLogged = true;
				DebugLog::Log("Motion ahead: the producer did not start (%s); the de-judder estimates at render time",
					producer->Error().c_str());
			}
		}
		else if (act.action == ProducerPlan::Action::Stop)
		{
			// Owner: the producer stays alive once wanted - kept while the settings still want it (a resync
			// or format change can take longer than any timer); stopped when they change.
			motionProducer->Stop();
			PublishProducer(nullptr);   // hardening: a loop copy keeps the stopped producer alive until its use ends
			motionEngineReady.store(false, std::memory_order_release);
		}
		// Audit 2026-10-16: a failure is retried once the producer is wanted again (optical flow off and on,
		// low latency and back), and the look-ahead re-engages only after the refill - never on a stale
		// engagement.
		if (act.clearStartFailed)
			motionProducerFailedLogged = false;
		if (act.disengage)
			motionAheadEngaged = false;
		if (motionProducer && !motionProducer->Running() && !motionProducerFailedLogged)
		{
			motionProducerFailedLogged = true;
			DebugLog::Log("Motion ahead: the producer stopped (%s); the de-judder estimates at render time",
				motionProducer->Error().c_str());
		}
		motionEngineReady.store(motionProducer && motionProducer->Running() && motionProducer->EngineReady(), std::memory_order_release);
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
		std::snprintf(text, sizeof(text), "Motion ahead: configured %d, queued ahead %d, depth %d (%s)",
			ahead.configured, (std::min)(ahead.available, 99), ahead.depth, MotionAhead::Describe(ahead.reason));
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
	}

	// ---- P5a-4 -------------------------------------------------------------------------------------------------
	void VectorEngine::UpdateVectorEngine(const VectorDemand::Input& input, const UpdateFacts& facts, IVectorEngineHost& host)
	{
		vectorDemand = VectorDemand::Decide(input);
		const ULONGLONG now = GetTickCount64();
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
		motionProducer->SetTargetSize(engineW, engineH);
		MotionAhead::FlowPtr flow;
		uint64_t from = 0, to = 0;
		if (forwardStats)
		{
			from = sequence;
			to = nextSequence;
			if (nextSequence == sequence + 1 &&
				motionValidity.PairValid(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()))
				flow = motionProducer->Take(from, to);
			NoteGlobalPair(flow != nullptr);
		}
		else
			for (int age = 0; age < 2 && !flow && sequence > static_cast<uint64_t>(age + 1); ++age)
			{
				from = sequence - 1 - age;
				to = sequence - age;
				if (motionValidity.PairValid(from, to, motionValidity.EpochOfLastKept(), motionValidity.Epoch()))
					flow = motionProducer->Take(from, to);
			}
		if (!flow)
		{
			++motionAheadMisses;
			motionProducer->Prune(forwardStats ? sequence : (sequence > 2 ? sequence - 2 : 0));
			return;
		}
		++motionAheadHits;
		if (backward) ++vectorBackwardPairs;
		++vectorPairs;
		NoteVectorsDelivered(backward, backward ? "engine" : "engine ahead");
		const VectorDemand::GlobalSample g = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
			static_cast<double>(flow->global[0][0]), static_cast<double>(flow->global[0][1]), halvings),
			flow->global[0][2], flow->global[0][3], facts.pictureHeight);
		if (g.usable && vectorDemand.Serves(VectorDemand::Consumer::SourceMonitorPan))
			host.NoteSourceMonitorMotion(g.sourcePixels, to);
		if (vectorDemand.Serves(VectorDemand::Consumer::MotionIntent) && host.IntentActive())
		{
			IntentInterpreter::VectorSample vectors;
			vectors.from = from;
			vectors.to = to;
			vectors.valid = g.intentValid;
			vectors.heightsPerFrame = g.heightsPerFrame;
			host.NoteIntentVectors(vectors);
		}
		if (backward && facts.vectorDeliveryRuns)
			motionSegment.Publish(VpMotion::Direction::Backward, from, to, MotionShadowField{ from, to },
				VpMotion::Source::EngineAhead, motionValidity.Epoch(), 0, false);
		motionProducer->Prune(forwardStats ? sequence : (sequence > 2 ? sequence - 2 : 0));
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

	bool VectorEngine::ReleaseProducerFor(const void* host)
	{
		std::shared_ptr<MotionAhead::Producer> producer;
		if (host)
			Engines().WithEngine(host, [&](VectorEngine& engine) {
				producer = std::atomic_exchange(&engine.motionProducer, std::shared_ptr<MotionAhead::Producer>());
				engine.motionEngineReady.store(false, std::memory_order_release);
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
		// Usable as the statistics consumers use it (VectorDemand::SampleGlobal: ambiguity z < 0.9).
		r.valid = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(static_cast<double>(global[0]),
			static_cast<double>(global[1]), searchHalvings), global[2], global[3], 0).usable;
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
