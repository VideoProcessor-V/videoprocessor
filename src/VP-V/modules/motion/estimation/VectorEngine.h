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

#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>
#include <VP-V/modules/motion/estimation/OnDemandEstimate.h>
#include <VP-V/modules/motion/estimation/ProducerPlan.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/matcher/MatcherPyramidSource.h>
#include <VP-V/modules/motion/estimation/VectorEngineState.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <IntentInterpreterObserver.h>

#include <libplacebo/gpu.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <exception>
#include <list>
#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VpSkeleton { class QueueWindowBoard; class SkeletonMemo; }   // core/QueueWindow.h, core/SkeletonAcquire.h

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
		// F2: the optical-flow seed of the last Estimate (MotionWarp::LastSeed) and the live field's grid; false = the
		// seed did not feed it. Default: none (fakes that predate the seed).
		struct Seed
		{
			pl_tex fwd = nullptr, bwd = nullptr;
			int gridW = 0, gridH = 0;          // seed texels
			int halvings = 0;                  // lowRes halvings of the search input
			float tolerance = 0.0f, weightPenalty = 0.0f, consistencyPx = 0.0f;   // the live trust rule (MotionWarp::SeedState)
			int liveGridW = 0, liveGridH = 0;  // the live field's 16-px source-block grid
			bool skipped = false;              // on a false return: the matcher skipped the pair (a cut / flash)
		};
		virtual bool LiveSeed(Seed& out) const { out = Seed(); return false; }
		// Y1b: the matcher's SNAPSHOT of the pair before the live one (captured by SnapshotForWarp ahead of an ahead estimate) and the A of the
		// pair it holds; null / 0 = none or the snapshot lease was lost (the implementer counts that: motion snapshot fallback). Defaults: none.
		virtual pl_tex SnapField() const { return nullptr; }
		virtual uint64_t SnapFieldFrom() const { return 0; }
		// Y1a / Z3: multi-slot snapshot/ahead view (slot 0 delegates to SnapField / SnapFieldFrom by default).
		virtual pl_tex SnapFieldAt(int slot) const { return slot == 0 ? SnapField() : nullptr; }
		virtual uint64_t SnapFieldFromAt(int slot) const { return slot == 0 ? SnapFieldFrom() : 0; }
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

	// RAII pin + matcher binding for SkeletonMemo across one Estimate call (exception-safe; skips live memo on warm-up).
	template<class Matcher>
	class MatcherSkeletonMemoScope
	{
	public:
		MatcherSkeletonMemoScope(Matcher& matcher, std::shared_ptr<VpSkeleton::SkeletonMemo> memo, uint64_t seq)
			: matcher_(matcher), memo_(std::move(memo))
		{
			matcher_.SetSkeletonMemo(memo_.get(), memo_ ? seq : 0);
		}
		~MatcherSkeletonMemoScope() { Release(); }
		void Release()
		{
			if (!active_)
				return;
			active_ = false;
			matcher_.SetSkeletonMemo(nullptr, 0);
			memo_.reset();
		}
		MatcherSkeletonMemoScope(const MatcherSkeletonMemoScope&) = delete;
		MatcherSkeletonMemoScope& operator=(const MatcherSkeletonMemoScope&) = delete;

	private:
		Matcher& matcher_;
		std::shared_ptr<VpSkeleton::SkeletonMemo> memo_;
		bool active_ = true;
	};

	class VectorEngine : public VectorEngineState, public IVectorEngine
	{
	public:
		// host: the renderer's RendererExtension host key; non-null registers this engine in the directory that
		// Query(kVectorEngineQueryId) answers with (until destruction). Default: unregistered (tests).
		VectorEngine() {}
		explicit VectorEngine(const void* host);
		// P5a-rest (items j, k): the host's own invalidation request (the renderer's RequestMotionInvalidate: the
		// validity epoch AND the frame-analysis store's shared epoch), called by RequestInvalidationFor. Fixed at
		// construction (never changes). Null: the engine invalidates its own validity state only (tests).
		using InvalidateSink = void (*)(const void* host, MotionValidity::Cause cause);
		VectorEngine(const void* host, InvalidateSink sink);
		~VectorEngine() override;
		VectorEngine(const VectorEngine&) = delete;
		VectorEngine& operator=(const VectorEngine&) = delete;
		// The process-wide host -> engine directory (never destroyed: a renderer may outlive static teardown).
		static IVectorEngineDirectory& Directory();
		// P5a-rest (items j, k; R9 OnQueue): an invalidation request for the newest engine registered under `host`,
		// from any thread (UI: Reset / ResetLiveQueue; render loop: the dequeue's gap, file discontinuity, format
		// change). For a Gap, gapDetail (a static string, may be null) is stored first, as the old dequeue code did,
		// for the invalidation log; other causes leave the stored detail alone. Runs under the directory's lock (a leaf lock: the sink only moves the two epochs), so
		// the engine cannot go meanwhile. False = no engine for `host` (nothing requested).
		static bool RequestInvalidationFor(const void* host, MotionValidity::Cause cause, const char* gapDetail);
		// P5a-rest (item l; doc 30 F4 "fix C"): the GPU release of `host` (RendererExtension GpuReleased, from
		// RetireOutput before the device goes) stops and drops that engine's optical-flow producer (its worker, its own
		// D3D11 device and VRAM) instead of keeping it until the Impl is destroyed, and clears the start latches and back-off (W2-C1). Keyed by
		// host: the release can run on the retirement worker after a replacement renderer has started (70b section 7),
		// whose engine is untouched. The producer is unpublished under the directory lock and stopped outside it. False =
		// no engine for `host` or no producer.
		static bool ReleaseProducerFor(const void* host);
		// W1-C2a A1 (doc 88 gap 1): the resync coordinator's "motion" FrameSeal provider for `host` (render thread, inside the
		// seal, before the new era's first Observe). Binds this engine's motionValidity to `indicator` and registers the
		// motion-history and vector-producer readers on it (VpMotion::Resync RegisterOn, the shared_ptr form); a null
		// Indicator unbinds and registers nothing. Under the directory lock. False = no engine for `host`.
		static bool RegisterResyncFor(const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator);
		// The bind half alone (no readers): resync_event turned off unbinds (null), turned on again over the SAME Indicator
		// (FrameSeal runs providers only on a new era) rebinds; the readers registered at that era still fire.
		static bool BindResyncFor(const void* host, const std::shared_ptr<VpIndicator::Indicator>& indicator);
		// Whether this engine's validity is bound to an Indicator (false = unbound: resync_event off, or no engine for `host`).
		// Tests and status. Unbinding applies a request still deferred to a seal (MotionValidity::State::Unbind).
		static bool ResyncBoundFor(const void* host);

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
			// SK-MV: the de-judder can engage on this source (DejudderPolicy::CanEngage: also UnknownRates / PlaneFormat). Set by the host's
			// hook block (one line, with the Z3 turn); until then false, and dejudderEstimating alone keeps the engine warm for the three
			// estimating reasons.
			bool dejudderCanEngage = false;
			double sourceHz = 0.0;              // the measured content rate (0 = unmeasured)
			pl_gpu gpu = nullptr;               // the device the engine needs; null = none
			// W2-C5b: optical_flow_on_demand for a host that does not note it (tests). The renderer notes it every frame through
			// NoteHostOnDemandSetting (rule 32: no hook line of its own), which then wins; PlanProducer publishes the result
			// (OnDemandSettingNow) for the walk's stage.
			OnDemand::Setting onDemand = OnDemand::Setting::Auto;
		};

		// Render thread, renderMutex held, once per rendered source frame: starts, keeps or stops the
		// optical-flow producer and publishes the look-ahead depth the render loop submits to
		// (motionAheadDepth / Configured), with the one "Motion ahead:" log line.
		void PlanProducer(const ProducerInputs& in);

		// ---- P5a-4: the vector-demand decision and its statistics service ------------------------------------
		struct UpdateFacts
		{
			bool vectorDemandSetting = false;   // the vector_demand setting
			bool sigmaDeltaRanLast = false;     // the sigma-delta quantiser ran in the last frame's output pass
			bool ditherVectorsOwn = false;      // the dither_vectors_own setting
			bool opticalFlow = true;            // W2-C1: the optical_flow setting (the camera fit's gate, this frame)
		};
		// Decides for this source frame (from the renderer's facts, VectorDemand::Input), then keeps the engine's state
		// visible: fallback entry, status line, Ctrl+Alt+D record and the logs (on change, once per mode, every 5 s
		// while on). Render thread.
		void UpdateVectorEngine(const VectorDemand::Input& input, const UpdateFacts& facts, IVectorEngineHost& host);

		// ---- W2-C1 (B9): the one engine state and its work counters ---------------------------------------------------
		// State: derived at the end of UpdateVectorEngine and PlanProducer (render thread; VectorEngineState.h, the one-frame lag).
		VectorDemand::MotionEngineStatus State() const override;
		// Work: the counters, with the producer's pairs (stopped producers' pairs kept in motionEnginePairsBase).
		VectorDemand::MotionEngineWork Work() const override;
		// B-prereq (a): the P2 slot's read-only view and the P2 CPU form's read counts (served / declined; atomics, any thread).
		VpMotion::P2View P2Pairs() const override;
		// The P2 CPU form's reader (VectorEngine.cpp: FindTo + the C2a-F 2 generation check + MakeP2PairRecord).
		bool P2RecordEndingAt(uint64_t to, VpMotion::P2PairRecord& out, std::string& why) const override;
		void NoteP2CpuFormRead(bool served) const override;
		// The fill's call: a decline also keeps its words for the status segment's "(last: ...)" (BBP-A-COUNTS-CTRLALTD).
		void NoteP2CpuFormRead(bool served, const std::string& why) const override;
		std::string P2CpuFormLastDecline() const;
		unsigned long long P2CpuFormServed() const;
		unsigned long long P2CpuFormDeclined() const;
		unsigned long long ShallowBackwardPairs() const;
		// The VRAM governor's release of the motion engine (the de-judder's keeper feeds it; render thread).
		void NoteVramRelease(bool released);
		// The keeper's counters (warm-ups, analysis estimates, matcher creates); render thread.
		VectorDemand::MotionEngineWork& WorkCounters();

		struct ServeFacts
		{
			bool legacyConsumer = false;          // a de-judder / interpolator estimate takes this frame's pair
			bool lowResSearch = true;             // dejudder_low_res != "off"
			int pictureHeight = 0;                // the trusted picture's height (the raster's when none)
			// Batch B fold: vectorDeliveryRuns (the motion.vector_delivery slot gate of the backward pair's segment publish) is
			// retired with the backward leg.
		};
		// Forward without a render-time estimate: once per new source frame the statistics consumers read the producer's
		// pair (n, n+1); statistics only, never a field. Batch B fold: the backward leg (competitive low latency, pairs
		// (n-1, n) / (n-2, n-1)) is retired: competitive is motion-free. Render thread.
		void ServeStatistics(uint64_t sequence, uint64_t nextSequence, int width, int height, const ServeFacts& facts,
			IVectorEngineHost& host);

		// Whole-frame motion for the statistics consumers: a producer pair (hit) or none (miss). A miss run is visible
		// (never log-only): hysteresis, so a single missed pair does not flap the entry.
		void NoteGlobalPair(bool hit);
		// Once: the first vectors the engine delivered and where they came from (forward; the backward mode is retired).
		void NoteVectorsDelivered(const char* source);
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
		void BindMatcher(const IMatcherView* matcher);
		// Runs the matcher's estimate of the pair (aSequence, bSequence). With a pre-estimate hook registered and
		// `hookable` (not the warm-up's dummy estimate): BeforeEstimate(ctx, sink) right before, AfterEstimate(pairSeq,
		// EstimateOutcome) right after. With no hook it is `estimate()` and nothing else (no sink, no matcher
		// call, no GPU work). Either way a successful estimate is recorded for FieldOf, but only when the validity
		// epoch is still `epochAtChoice` (the epoch the caller chose the pair under) after the estimate. ahead: called
		// from the estimate ahead (EstimateContext::ahead; today that is the render thread too). sourceHz: the content
		// rate for nextPairSeconds (<= 0 unknown). syncFallback(FallbackCatalog::Decision): the renderer's fallback
		// registry, called only while a hook is registered or a hook failure is still shown.
		// A hook that throws never stops the estimate: BeforeEstimate throwing = the guidance is cleared and the plain
		// estimate runs (AfterEstimate is then skipped); AfterEstimate throwing is contained. Each failure is counted,
		// logged and raised as the "vector_engine_hook" fallback entry (released after 8 hooked estimates without one).
		// An exception from the estimate itself is rethrown as before, after AfterEstimate(pairSeq, an all-absent EstimateOutcome).
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
			ctx.fromSeq = aSequence;
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
					try { hook->AfterEstimate(ctx.pairSeq, EstimateOutcome()); }
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
				EstimateOutcome outcome;
				outcome.guidanceUsed = used;
				outcome.narrowIdle = use.narrowDroppedEngine;
				outcome.e1Idle = use.e1DroppedEngine;
				outcome.rejection = use.rejected && use.rejected[0] ? use.rejected : nullptr;
				outcome.countsTex = used && (use.narrowPass || use.e1Pass) ? matcher.CameraGuidanceCounts() : nullptr;
				try { hook->AfterEstimate(ctx.pairSeq, outcome); }
				catch (const std::exception& e) { failure = std::string("AfterEstimate: ") + e.what(); }
				catch (...) { failure = "AfterEstimate: unknown exception"; }
			}
			syncFallback(NoteHookOutcome(!failure.empty(), failure));
			NoteEstimated(done, aSequence, bSequence, epochAtChoice);
			return done;
		}
		unsigned long long PreEstimateHookFailures() const;
		bool PreEstimateHooked() const;
		std::shared_ptr<VpSkeleton::SkeletonMemo> SkeletonMemoFor(uint64_t seq) const;
		// The optical-flow engine's whole-frame motion of the pair the matcher took ahead (flow->global[0], D1 format:
		// 1/8-size texels, z ambiguity, w written); null = no producer pair. CPU record for EngineGlobal only.
		void NoteEngineGlobal(uint64_t from, uint64_t to, const float* global, int searchHalvings);
		// W1-C4 (audit MUST 1): the same record with `usable` from the pair's one sample (MotionRecord::EstimatePair; no second sample).
		void NoteEngineGlobal(uint64_t from, uint64_t to, const float* global, int searchHalvings, bool usable);

		// ---- IVectorEngine (vpv.vector_engine.v1), from today's state; render thread -----------------------------
		// The verified field = the matcher's live block field (rgba32f: vx, vy in source px, trust, match error; one
		// texel per 16x16 source block) of the last successful estimate, while the live outputs still hold that pair
		// in the current validity epoch. seq = the pair's later frame (pairTo). Absent (0 / false) otherwise.
		uint64_t LatestVerifiedFieldSeq() const override;
		uint64_t FieldEra() const override;                  // the validity epoch
		bool FieldOf(uint64_t seq, VerifiedField& out) const override;
		float FieldScale() const override { return 1.0f; }   // the field is in source px
		// F2: the seed of the live pair under FieldOf's rules (VectorEngine.cpp; HOOKS.md section 7).
		bool SeedFieldOf(uint64_t pairTo, SeedField& out) const override;
		bool GlobalFromEngine() const override;
		bool EngineGlobal(uint64_t seq, GlobalVec& out) const override;
		// True unless every adjacent pair in [from, to] is valid in the current epoch and lies at or after the first
		// estimate made in it; unknown = true (conservative).
		bool PairSpansSlip(uint64_t from, uint64_t to) const override;
		// FlowIsCpuResident stays false on purpose: the camera link's GuidanceContext only calls FlowOf when it is true, so flipping it is the
		// consumer-visible step (the camera finisher's), not part of providing FlowOf.
		bool FlowIsCpuResident() const override { return false; }
		// SK-MV: the P2 pair (seq - 1, seq) of the NVOF producer as a CPU view, per block in SOURCE px (forward vectors, S10.5 half-size
		// px / 32 x source px per engine px), block = 4 engine px. Read-only over the P2 slot; never starts the producer or the matcher.
		// False (view cleared, counted by reason in FlowReads) when: no pair (none computed, pruned, an older generation), the
		// vectors-valid state (P5a MotionValidity) does not hold the pair, no producer / low latency, or the source size is not known yet
		// (set by the statistics service). Never a stale pair. The view's pointers stay valid until the NEXT FlowOf call on this engine
		// (the engine holds the shared pair and its converted planes as a lease; the pair is copied out of the slot before any Prune)
		// or the engine's destruction; call and read on the render thread. QKP E: a view is valid until its pair leaves the lease window
		// (a frame older than current - back, or the pair pruned from the P2 slot) or an invalidation (a vectors-valid / board epoch move);
		// only a FlowOf call evicts, never another pair's FlowOf by itself. Without a board: until the next FlowOf of another pair.
		// (The earlier text said "until the NEXT FlowOf call": that was the single lease.)
		bool FlowOf(uint64_t seq, FlowView& out) const override;
		// The source raster size (and optionally the expected engine luma size from SizeFor) the producer was sized for, recorded
		// atomically under flowMutex_ so a resolution switch (e.g. 1080p -> 4K) never exposes a torn (width, height) or (source, target)
		// snapshot. Until a positive width is noted, FlowOf answers "size unknown" (unscaled) rather than guess a scale; when a positive
		// target engine size is noted, FlowOf also refuses any stored pair whose (halfW, halfH) differs from that target as unscaled.
		void NoteFlowSourceSize(int width, int height, int targetEngineW = 0, int targetEngineH = 0);
		// Compatibility wrapper for width-only callers (tests): sets height and target size to 0 (FlowOf falls back to scaleX for Y).
		void NoteFlowSourceWidth(int width);
		struct FlowReadCounters
		{
			unsigned long long requests = 0, served = 0;
			unsigned long long noProducer = 0;     // no producer (low latency, optical flow off, not started / dropped)
			unsigned long long noPair = 0;         // the producer has no such pair (not computed yet, pruned, older generation)
			unsigned long long invalidated = 0;    // the vectors-valid state does not hold the pair (resync, gap, format change)
			unsigned long long unscaled = 0;       // the source size is not known yet, or the stored pair's engine size does not match the current target size
			// W1-C3: the per-pair work (computed once per pair id, whatever the number of FlowOf calls): the lease builds (source-px
			// conversion of the pair) and the cell-trust computations (FlowView::CellTrust, once per direction and tolerance while the pair is the current lease).
			unsigned long long conversions = 0;
			unsigned long long trustComputes = 0;
			// Of those computations, the planes worked out here instead of taken from the producer's extras (forward with extras: 0 each;
			// forward without, and every backward: one each). The successor of TD's old fbErrorComputed.
			unsigned long long fbErrorRecomputes = 0;
			unsigned long long costWeightRecomputes = 0;
		};
		FlowReadCounters FlowReads() const;
		// QKP slice E: the lease window's ledger and the ONE line that names how it is bounded. DORMANT: no production caller yet (no host
		// calls SetQueueWindowBoard and nothing puts this line on Ctrl+I / Config status; only VectorLeaseTests read it; 102b QKP-E-HOST-WIRING).
		// "vector lease window not fed" (no board: the single lease), "vector lease window capped by the P2 ring: behind 3 / ahead 13"
		// (the P2 retention binds below the board window), "" when the board's window is what bounds it. Render thread.
		struct LeaseCounters
		{
			unsigned long long live = 0;                 // leases held now
			unsigned long long evictedWindow = 0;        // frame older than current - back (never by a newer FlowOf)
			unsigned long long droppedPruned = 0;        // the pair left the P2 slot
			unsigned long long droppedEpoch = 0;         // a validity / board epoch moved, or the producer stopped (all leases; GF-P4 S8)
			unsigned long long droppedSingle = 0;        // no board: the single lease replaced by another pair
			unsigned long long window = 0;               // the bound's back term in frames (0 = not fed)
		};
		LeaseCounters Leases() const;
		std::string LeaseWindowLine() const;
		// The host's window board (core/QueueWindow.h); null = not fed. Set and clear on the render thread; outlives the engine's use.
		void SetQueueWindowBoard(const VpSkeleton::QueueWindowBoard* board);
		// Contract: set AND clear on the render thread or under renderMutex (an estimate reads the pointer once and
		// calls it without a lock); the hook object may be destroyed only after the clear has returned.
		void SetPreEstimateHook(IPreEstimateHook* hook) override;
		void SetCameraFitWanted(bool wanted) override;
		// Y1b: ONE observer of the P2 slot (IVectorEngine::SetP2PublishSink). Adds the validity epoch and whole-frame usability.
		void SetP2PublishSink(std::function<void(const VpMotion::P2Published&)> sink) override;
		uint64_t P2SlotGeneration() const override;
		size_t AheadFieldSeqs(uint64_t* out, size_t cap) const override;
		// Y1b: the estimate ahead took the matcher's snapshot of the live pair whose A is `liveFrom` (SnapshotForWarp returned true):
		// the NEXT successful estimate moves that record to a snapshot slot (slot 0 on the matcher). Render thread.
		void NoteSnapshotTaken(uint64_t liveFrom);
		// Y1a / Z3: records an ahead verified field stored in matcher snapshot/ahead slot `matcherSlot` (>= 0). Render thread.
		void NoteAheadField(uint64_t from, uint64_t to, uint64_t epoch, int matcherSlot);
		// Clears all live + snapshot field records and any pending snapshot marker (resync/reset). Render thread.
		void ResetFieldRecords();
		// Z3 (N#5): declares / releases the motion ahead field ring's VRAM to VramUsers under kUserMotionMatcher
		// using the derived LaneAhead::RingVramBytes(width, height, depth) formula.
		void DeclareAheadRingVram(uint32_t width, uint32_t height, int depth);
		void ReleaseAheadRingVram();
		uint64_t AheadRingVramBytes() const;
		// Rule 1 derivation: 1 live slot (slot 0) + D snapshot/ahead slots (slots 1..D), where D = VpSkeleton::kAheadCeiling
		// (== LaneAhead::kAheadCeilingLead == 6, verified by static_assert in VectorEngine.cpp). Total = 1 + 6 = 7 slots.
		static constexpr size_t kFieldSlots = 1u + static_cast<size_t>(VpSkeleton::kAheadCeiling);
		bool CameraFitWanted() const;
		// W2-C1 (C1-4) TEST SEAM (rule 25; README "One motion-engine state"): every producer start fails with this reason, as NVOF
		// does on an adapter or size it cannot serve, without a device (PlanProducer then treats the device as present). Empty (the
		// default; nothing in the product sets it) = the real start. CPU tests only: EngineAbsentPipelineTests scenario 2.
		void FailProducerStartsForTesting(const std::string& reason);
		// Reservations (RefinedFieldOf, SubmitLongPair ...): IVectorEngine's "absent" defaults.

		// ---- W2-C5b: the ON-DEMAND estimate entry (OnDemandEstimate.h; refs\93-prep-C5b.md); render thread --------------------
		// A render-time consumer with no pair computed ahead asks the look-ahead producer's ONE session for (from, to) now. Never starts
		// the producer and never opens a second session: no producer (low latency without it, optical flow off, stopped) answers
		// Unavailable at once; budgetMs <= 0 answers NoBudget; a pair already in the P2 slot answers FromSlot (zero work). The C1 state
		// gate (OnDemandStateOn): while the engine's one state (State(), W2-C1) is Off, a request answers Unavailable with zero work
		// (counted `gated`; doc 91 D: the global engine state is honoured by every consumer).
		OnDemand::PairFuture RequestPair(OnDemand::PairRequest request);
		// Waits up to waitMs (<= 0: no wait) for the answer. The pair (cause None), or null with the cause the miss counts as
		// (OnDemandLate: not answered in time - the request is abandoned, so the worker skips it before any GPU work;
		// OnDemandNoBudget; OnDemandUnavailable: no producer, stopping, refused or failed).
		MotionAhead::FlowPtr AwaitPair(const OnDemand::PairFuture& future, double waitMs, FallbackCatalog::NoPairCause& cause);
		struct OnDemandCounters
		{
			unsigned long long requests = 0, served = 0, fromSlot = 0, late = 0, unavailable = 0, noBudget = 0, gated = 0;
			// requests counted by RequestPair; each awaited one lands once in served / fromSlot / late / unavailable / noBudget
			// (AwaitPair). gated is a breakdown of unavailable (the C1 state gate said no).
			double lastWaitMs = 0.0, waitMsSum = 0.0;   // AwaitPair's wait (served ones in the sum)
			// Commit 4 (the dequeue hook + the estimate's fill): issued by the hook; unclaimed = issued, never awaited (a newer
			// issue or a different pair replaced it: abandoned, no GPU work if not started); aheadCovered = the pair was found
			// computed ahead when the estimate came (the request abandoned); gapServed = a hiccup gap pair served on demand (no
			// gap seed needed); idleSkips = not issued because no estimate read a pair within kConsumerIdleCycles hook calls.
			unsigned long long issued = 0, unclaimed = 0, aheadCovered = 0, gapServed = 0, idleSkips = 0;
			// uncached = not issued because a frame's luma analysis was not cached yet at the dequeue (the hook never decodes under
			// the queue mutex; at depth 0 nothing walks ahead, so this is the share the path cannot serve there).
			unsigned long long uncached = 0;
			double budgetMs = 0.0, renderMs = 0.0;   // the last budget issued with; the measured render time (EWMA)
		};
		OnDemandCounters OnDemandReads() const;
		// The C1 state gate (C5b opus gate MUST 2): no request work while the engine's one state is Off (vector_demand off, no want,
		// interlaced, smooth motion, VRAM release, ...). Render thread (State() is derived there, with its one-frame lag).
		bool OnDemandStateOn() const;
		// ---- W2-C5b commit 4: the dequeue issues, the estimate fills (OnDemandIssue.h; VectorEngineEstimate.inl) -----------
		enum : unsigned { kConsumerIdleCycles = 4 };   // enum constant: ODR-safe in C++14
		// The post (OnDemandIssue.h), once per staged pair (once per new A): the render-time sample (OnDemand::RenderSample) and the consumer clock.
		void NoteOnDemandCycle(double sourceHz, double dequeueWaitMs);
		// The deadline for a request issued now: OnDemand::RenderBudget at the pair's source rate.
		double OnDemandBudgetMs(double sourceHz) const;
		// True while an estimate read (or asked for) a pair within the last kConsumerIdleCycles hook calls, or before the first
		// kConsumerIdleCycles calls: no consumer = no request (no GPU work for a pair nobody reads).
		bool OnDemandConsumerActive();
		// Rule 32: the renderer notes optical_flow_on_demand every frame from Impl::ResolveMotionEngine (PhaseTimeline.inl, render
		// thread, before RenderLocked); once noted it replaces ProducerInputs::onDemand in PlanProducer. Render thread only.
		void NoteHostOnDemandSetting(OnDemand::Setting s);
		// The setting PlanProducer published (the host's noted value, else ProducerInputs::onDemand); the walk's stage reads it.
		void SetOnDemandSetting(OnDemand::Setting s);
		OnDemand::Setting OnDemandSettingNow() const { return static_cast<OnDemand::Setting>(onDemandSetting_.load(std::memory_order_acquire)); }
		// Stall fix (W2-C5b re-check, MUST 3): the walk's stage only STAGES the predicted pair under the renderer's queue mutex
		// (plain copies: the two frames' analysis caches as a peek closure, sequences, rate, wait); PostStagedOnDemand
		// (OnDemandIssue.h), after the queue block, takes it and does every lock and the issue (cache peeks, P2 lookup, the
		// producer's mutex and wake) outside the queue mutex. Render thread; the small mutex only makes a stray caller safe.
		struct OnDemandStaged
		{
			bool active = false;
			uint64_t from = 0, to = 0;
			double pairRateHz = 0.0, dequeueWaitMs = 0.0;
			std::function<bool(AnalysisLumaSource& lumaFrom, AnalysisLumaSource& lumaTo)> peek;   // false: a frame not cached yet
		};
		void StageOnDemand(OnDemandStaged staged);       // replaces one not posted yet
		// Rule 32 (no new hook lines): the walk stages (StageOnDemandAtWalk, OnDemandIssue.h). True once per new A (the frame
		// the dequeue takes next); false when the walk sees the same A again (the dequeue held it): no second request and no
		// second cycle sample for it. Render thread only.
		bool NoteOnDemandWalkFrame(uint64_t from);
		bool TakeStagedOnDemand(OnDemandStaged& out);    // false: nothing staged since the last take
		// Issues ONE request for the dequeue's selected pair (through RequestPair); an older one not yet claimed is abandoned.
		void IssueOnDemand(OnDemand::PairRequest request);
		// The estimate's fill: `ahead` (the pair found computed ahead) is returned as is; else the pending request for exactly
		// (from, to) is awaited within what is left of its budget. `allowed` false (the vectors-valid state refuses this
		// adjacent pair) abandons it unread. cause: None (served, or nothing was asked) or the OnDemand* miss; a late answer is
		// logged (the first, then every 10th). Marks the consumer active.
		MotionAhead::FlowPtr OnDemandFill(MotionAhead::FlowPtr ahead, uint64_t from, uint64_t to, bool allowed,
			FallbackCatalog::NoPairCause& cause);
		// The last fill: did it serve exactly (from, to)? Its cause (None when it served or asked nothing).
		bool OnDemandServed(uint64_t from, uint64_t to) const;
		FallbackCatalog::NoPairCause OnDemandLastCause() const;
		void NoteOnDemandGapServed();
		// Counts the pair and remembers it, so the fill of that pair reports NoPairCause::OnDemandUncached (not plain Missing).
		void NoteOnDemandUncached(uint64_t from, uint64_t to);
		// The Ctrl+I segment (the de-judder's stats line): "on demand: issued N, served N, late N, no budget N, uncached N";
		// empty while nothing was issued or skipped (off / auto: nothing shown, nothing changed).
		std::string OnDemandStatusSegment() const;
		// The Ctrl+I segment of the look-ahead producer's ring misses (register 115 #13 / #35: never log-only):
		// "engine-luma ring misses N (last: <words>)"; empty with no producer or no miss. Then (BBP-A-COUNTS-CTRLALTD, with or
		// without a producer) "; " + P2CpuFormStatusSegment(): "P2 CPU form served N, declined N (last: <why>)", empty with no decline;
		// then zero-gated NVOF ahead P1 deferrals and "ForwardShallow backward pairs served N" when vectorShallowBackwardPairs > 0.
		std::string ProducerStatusSegment() const;
		std::string P2CpuFormStatusSegment() const;
		// L-B4 (BBP-B B3): the matcher's read seam for the shared luma pyramid (kP1HalfLuma, Gpu, MatcherPyramidKey(level)) of the
		// renderer registered under this engine's host. NotConfigured without a skeleton memo for the frame (nothing to fall back
		// from); Unavailable with the cause when the product is asked and not served (the matcher then shows the catalog row 13 id
		// product.motion_warp). `namedException` (non-null: RGB structure luma, DV unpack output) = every level is Unavailable with
		// "named exception: <it>" (plan (b): those lumas are not the shared source plane).
		MatcherPyramid::Source MatcherPyramidSource(const char* namedException = nullptr) const;
		// L-B4 (BBP-B HIGH-1): the matcher's B upload (and the ahead pair's B / C) ARE the unpacked planes of those queued frames on
		// the render device: offered as P13 views (SourcePlanes::Offer, VP-V code, no host line) for this render frame BEFORE the
		// estimate, so the P1 owner fill finds its source and the read seam its identity. The render frame's own seq (the pair's A)
		// is NEVER offered (its P13 stays the Entrance image, as at base): A's levels are the product made when it was the last
		// pair's B (keepFrames 1), recognised through the log of these offers. Render thread. `luma` / `chroma` are pl_tex (chroma
		// may be null). The generation is below every render frame's. False = not offered (Original pathway, no host, table full).
		bool OfferMatcherPlanes(uint64_t seq, const void* luma, const void* chroma, bool fullRange) const;
		static constexpr int64_t kMatcherOfferGeneration = -1;
		// The last matcher offers (seq, range), shared with the read seam.
		struct MatcherOfferLog
		{
			static constexpr size_t kEntries = 4;   // B, the ahead B / C, and the previous frame's B (the next A)
			struct Entry { uint64_t seq = 0; bool fullRange = false; };
			std::mutex m;
			Entry entries[kEntries];
			size_t next = 0;
			uint64_t epoch = 0;
			bool epochKnown = false;
		};
		// Render thread, before the offers of an estimate: a motion-validity epoch change (resync, seek, discontinuity) forgets every
		// earlier offer, so a frame of an older epoch is never recognised as A (its kept product is not reused across the change).
		void NoteMatcherEpoch(uint64_t epoch) const;

	private:
		std::shared_ptr<MatcherOfferLog> matcherOffers_ = std::make_shared<MatcherOfferLog>();
		void NoteEstimated(bool done, uint64_t aSequence, uint64_t bSequence, uint64_t epochAtChoice);
		FallbackCatalog::Decision NoteHookOutcome(bool failed, const std::string& what);   // counts and logs a failure
		FallbackCatalog::Decision ReleaseHookFailures();

		const void* host_ = nullptr;
		void SyncP1AheadDemand(uint8_t aheadDepth);
		void ClearP1AheadDemand();
		bool p1AheadDeclared_ = false;
		uint8_t p1AheadDepth_ = 0;
		uint64_t p1AheadErasures_ = 0;
		int p1AheadDemandOwner_ = 0;
		// QKP slice E (design s9.3): FlowOf's leases are a MAP keyed (from, to, scaleX, scaleY), bounded by the SAME derived queue window as the
		// frame memo (core/QueueWindow.h; the board is the host's one snapshot, fed by SetQueueWindowBoard). A lease is dropped when its
		// frame is older than current - back, when its pair is no longer in the P2 slot (a lease never extends P2 retention), or when
		// the vectors-valid epoch / the board's invalidation epoch moved. Without a board the behaviour is today's single lease. In production
		// there is no board yet (slice E is dormant: no host wiring, LeaseWindowLine not displayed; 102b QKP-E-HOST-WIRING), so it is not shown.
		// FlowOf's lease (one per pair and per-axis scale): the shared pair and the converted planes the last returned view points into (render thread; the mutex only
		// makes a stray second reader safe, it does not extend a view's validity).
		struct FlowLease : IFlowTrustMemo
		{
			MotionAhead::FlowPtr flow;
			uint64_t from = 0, to = 0;
			double scaleX = 0, scaleY = 0;
			uint64_t validityEpoch = 0, boardEpoch = 0;   // QKP E: the epochs the lease was built in (a moved epoch drops it)
			std::vector<float> x, y;
			// W1-C3: the cell trust memo, one entry per tolerance asked for this pair (each computed once and counted). A std::list so a
			// pointer handed out stays valid when another tolerance is added (a vector of entries would move them).
			struct TrustEntry
			{
				float tolerance = 0.0f;
				bool backward = false;   // L-B6: the backward direction's plane (fields swapped)
				bool ok = false;
				std::vector<float> trust;
			};
			std::atomic<unsigned long long>* computes = nullptr;
			std::atomic<unsigned long long>* fbErrorRecomputes = nullptr;
			std::atomic<unsigned long long>* costWeightRecomputes = nullptr;
			mutable std::mutex trustMutex;
			mutable std::list<TrustEntry> trusts;
			const float* CellTrust(float toleranceEnginePx) const override { return Trust(toleranceEnginePx, false); }
			const float* CellTrustBackward(float toleranceEnginePx) const override { return Trust(toleranceEnginePx, true); }
			const float* Trust(float toleranceEnginePx, bool backward) const;
		};
		mutable std::mutex flowMutex_;
		struct FlowSourceSize
		{
			int sourceW = 0, sourceH = 0;
			int targetEngineW = 0, targetEngineH = 0;
		};
		FlowSourceSize flowSize_;   // under flowMutex_: atomic (sourceW, sourceH, targetEngineW, targetEngineH) snapshot (ServeStatistics / RunDejudderEstimate)
		using LeaseKey = std::tuple<uint64_t, uint64_t, double, double>;   // (from, to, scaleX, scaleY)
		mutable std::map<LeaseKey, std::shared_ptr<FlowLease>> flowLeases_;
		std::atomic<const VpSkeleton::QueueWindowBoard*> queueBoard_{ nullptr };
		mutable LeaseCounters leaseCounters_;
		mutable FlowReadCounters flowReads_;
		mutable std::atomic<unsigned long long> flowFbErrorRecomputes_{ 0 };
		mutable std::atomic<unsigned long long> flowCostWeightRecomputes_{ 0 };
		mutable std::atomic<unsigned long long> flowTrustComputes_{ 0 };   // W1-C3: FlowLease::CellTrust computations (FlowReads().trustComputes)
		mutable std::atomic<unsigned long long> p2CpuFormServed_{ 0 }, p2CpuFormDeclined_{ 0 };   // B-prereq (a): the P2 CPU form's reads
		mutable std::mutex p2CpuFormWhyMutex_;
		mutable std::string p2CpuFormLastWhy_;       // the last decline's reason (under p2CpuFormWhyMutex_)
		unsigned long long p2CpuFormRecorded_ = 0;   // declines at the last DiagnosticHistory record (render thread)
		bool p2CpuFormRecordedAny_ = false;          // the first read has been recorded (render thread)
		unsigned long long motionAheadP1DeferredRecorded_ = 0;   // P1 deferrals at the last DiagnosticHistory record (render thread)
		unsigned long long vectorShallowBackwardRecorded_ = 0;   // ForwardShallow backward pairs at the last DiagnosticHistory record (render thread)
		InvalidateSink invalidateSink_ = nullptr;
		const IMatcherView* matcher_ = nullptr;
		std::atomic<IPreEstimateHook*> preEstimateHook_{ nullptr };
		std::atomic<bool> cameraFitWanted_{ false };   // SK-W stage D: the external CameraFit want, merged into the next decision
		std::string producerStartFailureForTesting_;   // C1-4 test seam (FailProducerStartsForTesting); empty in the product
		struct FieldRecord
		{
			uint64_t from = 0, to = 0, epoch = 0;
			int matcherSlot = -1;   // -1 = live slot (fields_[0]), >= 0 = matcher snapshot/ahead slot (SnapFieldAt / SnapFieldFromAt)
		};
		// Y1a / Y1b: slot 0 = LIVE record (last successful estimate), slots 1..D = SNAPSHOT/AHEAD records (newest first).
		FieldRecord fields_[kFieldSlots];   // render thread
		uint64_t snapshotOfFrom_ = 0;       // the A of the live pair the last ahead snapshot captured (0 = none pending)
		uint64_t aheadRingVramBytes_ = 0;   // Z3 (N#5): declared kUserMotionMatcher ahead field ring VRAM (0 = none)
		FieldRecord& RecordLive();
		const FieldRecord& RecordLive() const;
		const FieldRecord& RecordSnapshot(size_t i) const;
		size_t FindByTo(uint64_t to) const;
		uint64_t eraFirstFrom_ = 0;         // the A of the first successful estimate in RecordLive().epoch
		struct GlobalRecord { uint64_t from = 0, to = 0, epoch = 0; bool valid = false; double x = 0, y = 0; };
		static constexpr size_t kGlobalRing = 16;   // the link may ask about a pair a frame or so after it was taken
		GlobalRecord global_[kGlobalRing];          // slot = to % kGlobalRing
		FallbackCatalog::LookAheadFailures hookFailures_ = FallbackCatalog::PreEstimateHookFailures();
		mutable std::mutex onDemandMutex_;          // W2-C5b
		OnDemandCounters onDemand_;                  // under onDemandMutex_
		// Commit 4, all under onDemandMutex_: the one pending request the dequeue issued, the render-time budget, the cycle clock.
		struct OnDemandPendingRequest
		{
			bool active = false;
			uint64_t from = 0, to = 0;
			OnDemand::PairFuture future;
			OnDemand::Clock::time_point issued;
			double budgetMs = 0.0;
		};
		OnDemandPendingRequest onDemandPending_;
		std::mutex onDemandStagedMutex_;            // the stall fix's staged pair (never held with onDemandMutex_)
		OnDemandStaged onDemandStaged_;
		uint64_t onDemandWalkFrom_ = 0;             // the last A the walk staged (render thread only)
		bool onDemandWalkFromKnown_ = false;
		OnDemand::Setting hostOnDemand_ = OnDemand::Setting::Auto;   // NoteHostOnDemandSetting (render thread)
		bool hostOnDemandKnown_ = false;
		std::atomic<unsigned char> onDemandSetting_{ static_cast<unsigned char>(OnDemand::Setting::Auto) };   // not under the mutex
		OnDemand::RenderBudget onDemandBudget_;
		OnDemand::Clock::time_point onDemandLastCycle_;
		bool onDemandCycleKnown_ = false;
		double onDemandCycleWaitMs_ = 0.0;           // this cycle's on-demand wait (excluded from the render sample)
		unsigned long long onDemandCycles_ = 0, onDemandConsumerCycle_ = 0;
		struct { uint64_t from = 0, to = 0; bool served = false; FallbackCatalog::NoPairCause cause = FallbackCatalog::NoPairCause::None; } onDemandLast_;
		struct { uint64_t from = 0, to = 0; bool active = false; } onDemandUncachedPair_;   // the last pair not asked for: uncached (onDemandMutex_)
	};
}
