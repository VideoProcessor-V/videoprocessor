// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the vector engine behind the renderer. It owns the estimation-side state
// (VectorEngineState) and the logic that used to be member functions of the renderer's Impl:
//   P5a-3  the optical-flow producer's lifecycle and the look-ahead depth (PlanProducer);
//   P5a-4  the vector-demand decision, its statistics service and the invalidation requests
//          (UpdateVectorEngine, ServeStatistics, TakeInvalidation / LogInvalidation, the global-pair and shadow checks);
//   P5a-5  the matcher estimate's K4 pre-estimate hook (Estimate) and IVectorEngine (vpv.vector_engine.v1) from the
//          state today's renderer has (the live matcher field, the validity epoch, the engine's whole-frame motion).
// The renderer holds one member of this type (`vectorEngine`) and calls it at the sites the code used
// to sit at. What the engine needs from the renderer comes in as plain values (ProducerInputs, the
// VectorDemand::Input) or through the small IVectorEngineHost interface the renderer implements with
// an adapter; the engine never sees the renderer's Impl.
// C++14: included by VideoProcessor-Lib code.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/ProducerPlan.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/VectorEngineState.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <IntentInterpreterObserver.h>

#include <libplacebo/gpu.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace VpMotion
{
	// What the engine needs from the renderer besides plain values: the renderer's fallback registry, its stats
	// overlay, the source monitor's pan input and the intent interpreter's vector input. Implemented for the
	// renderer by VectorEngineImplHost (VectorEngineHost.h); tests use a fake.
	class IVectorEngineHost
	{
	public:
		virtual ~IVectorEngineHost() {}
		virtual void SyncFallback(const FallbackCatalog::Decision& decision) = 0;
		virtual void PublishStatus(bool force) = 0;
		virtual bool DitherVectorsSupported() const = 0;
		virtual void NoteSourceMonitorMotion(double sourcePixelsPerFrame, uint64_t sequence) = 0;
		virtual bool IntentActive() const = 0;
		virtual void NoteIntentVectors(const IntentInterpreter::VectorSample& sample) = 0;
	};

	// What the engine reads of the matcher whose type it does not see (EstimateState implements it; tests use a fake).
	// Render thread.
	class IMatcherView
	{
	public:
		virtual ~IMatcherView() {}
		virtual pl_tex LiveField() const = 0;             // the matcher's block field of its last Estimate (null = none)
		virtual uint64_t LiveFieldFrom() const = 0;       // the A of the pair the live outputs hold (0 = none)
		virtual bool MatcherGlobalFromEngine() const = 0; // the last Estimate's D1 came from the optical-flow engine
	};

	// K4: the guidance sink handed to the pre-estimate hook, 1:1 onto the matcher's SetCameraGuidance.
	template<class Matcher>
	class MatcherGuidanceSink : public IGuidanceSink
	{
	public:
		explicit MatcherGuidanceSink(Matcher& matcher) : matcher_(matcher) {}
		void SetCameraGuidance(std::shared_ptr<const CameraModel::GuidanceFrame> frame) override
		{
			matcher_.SetCameraGuidance(std::move(frame));
		}

	private:
		Matcher& matcher_;
	};

	class VectorEngine : public VectorEngineState, public IVectorEngine
	{
	public:
		// host: the renderer's RendererExtension host key; non-null registers this engine in the directory that
		// Query(kVectorEngineQueryId) answers with (until destruction). Default: unregistered (tests).
		VectorEngine() {}
		explicit VectorEngine(const void* host);
		~VectorEngine() override;
		VectorEngine(const VectorEngine&) = delete;
		VectorEngine& operator=(const VectorEngine&) = delete;
		// The process-wide host -> engine directory (never destroyed: a renderer may outlive static teardown).
		static IVectorEngineDirectory& Directory();

		// What the producer plan reads from the renderer, as plain values. The two atomics are read
		// at the points the old inline block read them (low latency first, the queue depth after the
		// producer was acted on).
		struct ProducerInputs
		{
			const std::atomic<bool>* lowLatency = nullptr;
			const std::atomic<size_t>* queuedFutureFrames = nullptr;
			bool opticalFlow = true;            // the optical_flow setting
			int motionAheadFrames = 0;          // the motion_ahead_frames setting
			bool generalRateFixes = true;       // the general_rate_fixes setting
			bool dejudderPresent = false;       // the de-judder's matcher exists
			bool dejudderEstimating = false;    // its reason is Active / NoNextFrame / NextFrameDiffers
			double sourceHz = 0.0;              // the measured content rate (0 = unmeasured)
			pl_gpu gpu = nullptr;               // the device the engine needs; null = none
		};

		// Render thread, renderMutex held, once per rendered source frame: starts, keeps or stops the
		// optical-flow producer and publishes the look-ahead depth the render loop submits to
		// (motionAheadDepth / Backward / Configured), with the one "Motion ahead:" log line.
		void PlanProducer(const ProducerInputs& in);

		// ---- P5a-4: the vector-demand decision and its statistics service ------------------------------------
		struct UpdateFacts
		{
			bool vectorDemandSetting = false;   // the vector_demand setting
			bool sigmaDeltaRanLast = false;     // the sigma-delta quantiser ran in the last frame's output pass
			bool ditherVectorsOwn = false;      // the dither_vectors_own setting
		};
		// Decides for this source frame (from the renderer's facts, VectorDemand::Input), then keeps the engine's state
		// visible: fallback entry, status line, Ctrl+Alt+D record and the logs (on change, once per mode, every 5 s
		// while on). Render thread.
		void UpdateVectorEngine(const VectorDemand::Input& input, const UpdateFacts& facts, IVectorEngineHost& host);

		struct ServeFacts
		{
			bool legacyConsumer = false;          // a de-judder / interpolator estimate takes this frame's pair
			bool lowResSearch = true;             // dejudder_low_res != "off"
			bool vectorDeliveryRuns = true;       // the motion.vector_delivery slot runs
			int pictureHeight = 0;                // the trusted picture's height (the raster's when none)
		};
		// Backward pairs (no frame ahead): once per new source frame the statistics consumers read the pair (n-1, n)
		// if the producer has it, else the frame-old (n-2, n-1); statistics only, never a field. Forward without a
		// render-time estimate: statistics consumers only. Render thread.
		void ServeStatistics(uint64_t sequence, uint64_t nextSequence, int width, int height, const ServeFacts& facts,
			IVectorEngineHost& host);

		// Whole-frame motion for the statistics consumers: a producer pair (hit) or none (miss). A miss run is visible
		// (never log-only): hysteresis, so a single missed pair does not flap the entry.
		void NoteGlobalPair(bool hit);
		// Once per mode: the first vectors the engine delivered and where they came from.
		void NoteVectorsDelivered(bool backward, const char* source);
		// Engine size the producer works at for a picture of this size, as the matcher would use it (halvings of the
		// search input), and the halvings themselves for the pixel scaling.
		static void SizeFor(bool lowResSearch, int width, int height, int& engineW, int& engineH, int& halvings);
		// S0 shadow step: the segment's answer against the host's own predicate; nothing is delivered from it.
		void MotionShadowCheck(bool deliveryRuns, uint64_t sequence, uint64_t nextSequence, bool hostLive);

		// ---- P5a-4 (item d): the apply point of the one vectors-valid state, split at the renderer's own consumers ----
		// Render thread, before an estimate: consumes the epoch requests made from any thread since the last look and
		// counts them. False = nothing changed (the caller returns). The caller then makes every consumer of older
		// vectors forget (they are the renderer's: matcher, ahead state, adaptive / deband / detail histories) and
		// calls LogInvalidation.
		bool TakeInvalidation(MotionValidity::Pending& pending);
		// The first 50 invalidations are all logged; then at most one line per 5 s, but a new cause is always logged.
		void LogInvalidation(const MotionValidity::Pending& pending);

		// ---- P5a-5: the matcher estimate and its K4 pre-estimate hook (render thread) ---------------------------
		// The estimate state binds itself as the engine's view of the matcher (null = unbound).
		void BindMatcher(const IMatcherView* matcher) { matcher_ = matcher; }
		// Runs the matcher's estimate of the pair (aSequence, bSequence). With a pre-estimate hook registered and
		// `hookable` (not the warm-up's dummy estimate): BeforeEstimate(ctx, sink) right before, AfterEstimate(pairSeq,
		// guidanceUsed, countsTex) right after. With no hook it is `estimate()` and nothing else (no sink, no matcher
		// call, no GPU work). Either way a successful estimate is recorded for FieldOf, but only when the validity
		// epoch is still `epochAtChoice` (the epoch the caller chose the pair under) after the estimate. ahead: called
		// from the estimate ahead (EstimateContext::ahead; today that is the render thread too). sourceHz: the content
		// rate for nextPairSeconds (<= 0 unknown). syncFallback(FallbackCatalog::Decision): the renderer's fallback
		// registry, called only while a hook is registered or a hook failure is still shown.
		// A hook that throws never stops the estimate: BeforeEstimate throwing = the guidance is cleared and the plain
		// estimate runs (AfterEstimate is then skipped); AfterEstimate throwing is contained. Each failure is counted,
		// logged and raised as the "vector_engine_hook" fallback entry (released after 8 hooked estimates without one).
		// An exception from the estimate itself is rethrown as before, after AfterEstimate(pairSeq, false, null).
		template<class Matcher, class EstimateFn, class SyncFallbackFn>
		bool Estimate(uint64_t aSequence, uint64_t bSequence, bool ahead, double sourceHz, uint64_t epochAtChoice, bool hookable,
			Matcher& matcher, EstimateFn&& estimate, SyncFallbackFn&& syncFallback)
		{
			IPreEstimateHook* hook = hookable ? preEstimateHook_.load(std::memory_order_acquire) : nullptr;
			if (!hook)
			{
				if (hookFailures_.engaged)
					syncFallback(ReleaseHookFailures());   // the hook went (camera guidance off): nothing left to show
				const bool done = estimate();
				NoteEstimated(done, aSequence, bSequence, epochAtChoice);
				return done;
			}
			EstimateContext ctx;
			ctx.pairSeq = bSequence;
			ctx.ahead = ahead;
			ctx.nextPairSeconds = sourceHz > 0 && bSequence > aSequence ? static_cast<double>(bSequence - aSequence) / sourceHz : 0.0;
			MatcherGuidanceSink<Matcher> sink(matcher);
			std::string failure;
			bool before = true;
			try { hook->BeforeEstimate(ctx, sink); }
			catch (const std::exception& e) { failure = std::string("BeforeEstimate: ") + e.what(); before = false; }
			catch (...) { failure = "BeforeEstimate: unknown exception"; before = false; }
			if (!before)
				matcher.SetCameraGuidance(nullptr);   // whatever it bound: the plain estimate runs
			bool done = false;
			try { done = estimate(); }
			catch (...)
			{
				if (before)
				{
					try { hook->AfterEstimate(ctx.pairSeq, false, nullptr); }
					catch (...) { if (failure.empty()) failure = "AfterEstimate: exception after a failed estimate"; }
				}
				syncFallback(NoteHookOutcome(!failure.empty(), failure));
				throw;
			}
			if (before)
			{
				const auto use = matcher.CameraGuidanceUsed();
				const bool used = !(use.rejected && use.rejected[0]) &&
					(use.modelGlobal || use.staticFromModel || use.matcherSkipped || use.narrowPass || use.e1Pass);
				try { hook->AfterEstimate(ctx.pairSeq, used, used && (use.narrowPass || use.e1Pass) ? matcher.CameraGuidanceCounts() : nullptr); }
				catch (const std::exception& e) { failure = std::string("AfterEstimate: ") + e.what(); }
				catch (...) { failure = "AfterEstimate: unknown exception"; }
			}
			syncFallback(NoteHookOutcome(!failure.empty(), failure));
			NoteEstimated(done, aSequence, bSequence, epochAtChoice);
			return done;
		}
		unsigned long long PreEstimateHookFailures() const { return hookFailures_.failures; }
		bool PreEstimateHooked() const { return preEstimateHook_.load(std::memory_order_acquire) != nullptr; }
		// The optical-flow engine's whole-frame motion of the pair the matcher took ahead (flow->global[0], D1 format:
		// 1/8-size texels, z ambiguity, w written); null = no producer pair. CPU record for EngineGlobal only.
		void NoteEngineGlobal(uint64_t from, uint64_t to, const float* global, int searchHalvings);

		// ---- IVectorEngine (vpv.vector_engine.v1), from today's state; render thread -----------------------------
		// The verified field = the matcher's live block field (rgba32f: vx, vy in source px, trust, match error; one
		// texel per 16x16 source block) of the last successful estimate, while the live outputs still hold that pair
		// in the current validity epoch. seq = the pair's later frame (pairTo). Absent (0 / false) otherwise.
		uint64_t LatestVerifiedFieldSeq() const override;
		uint64_t FieldEra() const override;                  // the validity epoch
		bool FieldOf(uint64_t seq, VerifiedField& out) const override;
		float FieldScale() const override { return 1.0f; }   // the field is in source px
		bool GlobalFromEngine() const override;
		bool EngineGlobal(uint64_t seq, GlobalVec& out) const override;
		// True unless every adjacent pair in [from, to] is valid in the current epoch and lies at or after the first
		// estimate made in it; unknown = true (conservative).
		bool PairSpansSlip(uint64_t from, uint64_t to) const override;
		bool FlowIsCpuResident() const override { return false; }   // absent: no CPU field view is kept today
		bool FlowOf(uint64_t, FlowView&) const override { return false; }
		// Contract: set AND clear on the render thread or under renderMutex (an estimate reads the pointer once and
		// calls it without a lock); the hook object may be destroyed only after the clear has returned.
		void SetPreEstimateHook(IPreEstimateHook* hook) override { preEstimateHook_.store(hook, std::memory_order_release); }
		// Reservations (RefinedFieldOf, SubmitLongPair ...): IVectorEngine's "absent" defaults.

	private:
		void NoteEstimated(bool done, uint64_t aSequence, uint64_t bSequence, uint64_t epochAtChoice);
		FallbackCatalog::Decision NoteHookOutcome(bool failed, const std::string& what);   // counts and logs a failure
		FallbackCatalog::Decision ReleaseHookFailures();

		const void* host_ = nullptr;
		const IMatcherView* matcher_ = nullptr;
		std::atomic<IPreEstimateHook*> preEstimateHook_{ nullptr };
		struct FieldRecord { uint64_t from = 0, to = 0, epoch = 0; };
		FieldRecord field_;                 // the last successful estimate (render thread)
		uint64_t eraFirstFrom_ = 0;         // the A of the first successful estimate in field_.epoch
		struct GlobalRecord { uint64_t from = 0, to = 0, epoch = 0; bool valid = false; double x = 0, y = 0; };
		static constexpr size_t kGlobalRing = 16;   // the link may ask about a pair a frame or so after it was taken
		GlobalRecord global_[kGlobalRing];          // slot = to % kGlobalRing
		FallbackCatalog::LookAheadFailures hookFailures_ = FallbackCatalog::PreEstimateHookFailures();
	};
}
