#pragma once

// Optical flow ahead of display (docs/VP-nvof-ahead.md; owner 2026-10-16: "start
// working on the frame 4 or so ahead and save the vector sets ... always have to
// default back to the original gpu path").
//
// The render loop hands this producer each queued source frame once, as soon as
// it is in the look-ahead window (its luma through the analysis source - the
// queued frame's own pixels, kept alive by the source's owner). A worker thread
// makes the half-size 8-bit luma (the matcher's half size), uploads it on its own
// D3D11 device on the render GPU, runs the NVIDIA optical-flow engine for each
// consecutive pair (both directions, costs), reads the small flow fields back and
// PUBLISHES them per pair into the P2 slot (VectorProducts.h: P2Slot, owned by the vector engine and
// injected with SetSlot; this class owns no store of its own) under the continuity generation. The
// de-judder reads its pair through the engine's P2View (no engine work at render time); a miss means
// it takes the matcher path (P2m, shown as a fallback): this producer is the ONE NvOpticalFlow session
// per device (SK-MV, doc 83: the de-judder opens none). Never waits: a
// full queue drops the oldest submissions; three failures in a row stop the producer
// and everything falls back - and (bug hunt 35 #1) it restarts itself with a back-off
// (ProducerRecovery.h) on a fresh device while it is still wanted (not Stop()ped).
// GetHealth() and FallbackCatalog::VectorProducer show the state.

#include <AnalysisLumaSource.h>
#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/modules/motion/estimation/ProducerRecovery.h>
#include <VP-V/modules/motion/estimation/OnDemandEstimate.h>

#include <d3d11.h>
#include <atlbase.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace MotionAhead
{
	namespace OnDemand = ::VpMotion::OnDemand;   // W2-C5b: the on-demand request types (OnDemandEstimate.h)

	// One pair's engine output: S10.5 vectors per 4x4 half-size pixels (the
	// engine's R16G16_SINT layout, x then y), forward (from -> to) and backward,
	// and the costs (higher = less sure).
	struct Flow
	{
		int halfW = 0, halfH = 0;   // the half-size luma the flow is for
		int gridW = 0, gridH = 0;   // (halfW + 3) / 4 x (halfH + 3) / 4
		std::vector<int16_t> forward, backward;
		std::vector<uint8_t> forwardCost, backwardCost;
		// Planner Q1 (2026-10-16): the whole frame's motion from the engine's field, in the
		// de-judder's D1 format - (x, y) in 1/8-size texels (one engine cell), z the
		// ambiguity (< 0.9 = usable, as D1's best / second ratio), w = 1 - and the second
		// motion layer (x, y, -, w = 1 when there is one), forward then backward. The
		// de-judder takes them instead of its own global search when the pair came ahead.
		float global[2][4] = {}, second[2][4] = {};
		// W1-C3: global / second were worked out (FlowGlobals) and with which general_rate_fixes flag; a Flow built without them (a
		// test, a replica) leaves globalSet false and every reader computes the vote itself, as before.
		bool globalSet = false;
		bool globalGeneral = true;
		// The engine's extras as trust inputs (OpticalFlowTrust.h), worked out on the
		// worker: the forward cost per de-judder block (blocks of kBlockPx half-size px,
		// blocksX x blocksY) as a weight in [0, 1], and the engine's own forward/backward
		// consistency error per cell (half-size px). Empty when the costs are missing:
		// then the de-judder runs exactly as without them.
		static constexpr int kBlockPx = 8;
		int blocksX = 0, blocksY = 0;
		std::vector<float> costWeight;
		std::vector<float> fbError;
	};
	// FlowPtr (std::shared_ptr<const Flow>) is declared in VectorProducts.h.

	// The whole frame's motion from one engine field (S10.5 half-size pixels per 4 x 4
	// cell; costs higher = less sure): the most common vector (a 3 x 3 vote on a
	// half-cell grid, refined by the mean of the cells within one cell of it), its share
	// of the confident cells as the ambiguity, and the most common vector away from it as
	// the second layer (when at least 10 % of the cells hold it).
	// Fixed-constant batch B4 (2026-10-18, sweep #11, general_rate_fixes): the vote range (+-40
	// texels: D1 searches +-32) as set up for a working picture of up to 2160 lines, by height
	// above it (GeneralScale::AboveReference of the field's lines: 8 full-size lines per cell row).
	constexpr int kFlowVoteRange = 40;
	inline int FlowVoteRange(int gridH, bool general = true)
	{
		return GeneralScale::AboveReferencePixels(kFlowVoteRange, 8.0 * (std::max)(0, gridH), general);
	}

	inline void FlowGlobal(const std::vector<int16_t>& vectors, const std::vector<uint8_t>& costs, int gridW, int gridH,
		float global[4], float second[4], bool general = true)
	{
		// Units: one engine cell (4 half-size px) = one 1/8-size texel; votes in half texels.
		const int kRange = FlowVoteRange(gridH, general);   // +-40 texels up to 2160 lines (D1 searches +-32)
		const int kBins = 4 * kRange + 1;                   // half-texel bins
		global[0] = global[1] = 0.0f; global[2] = 1.0f; global[3] = 1.0f;
		second[0] = second[1] = second[2] = second[3] = 0.0f;
		const size_t cells = static_cast<size_t>(gridW) * gridH;
		if (gridW <= 0 || gridH <= 0 || vectors.size() < cells * 2 || costs.size() < cells) return;
		// Confident cells: cost at or below the field's median (the engine's own ranking).
		std::vector<uint8_t> sorted(costs.begin(), costs.begin() + cells);
		std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
		const uint8_t costLimit = sorted[sorted.size() / 2];
		std::vector<int> votes(static_cast<size_t>(kBins) * kBins, 0);
		long confident = 0;
		auto binOf = [kRange](int16_t v) { return static_cast<int>(std::lround(v / 32.0 / 4.0 * 2.0)) + 2 * kRange; };
		for (size_t i = 0; i < cells; ++i)
		{
			if (costs[i] > costLimit) continue;
			const int bx = binOf(vectors[i * 2]), by = binOf(vectors[i * 2 + 1]);
			if (bx < 0 || by < 0 || bx >= kBins || by >= kBins) continue;
			++votes[static_cast<size_t>(by) * kBins + bx];
			++confident;
		}
		if (confident == 0 || confident < static_cast<long>(cells / 10)) return;   // too few to judge: ambiguous (none: no 0/0, audit 2026-10-16)
		// The 3 x 3 summed vote (a texel-and-a-half window): its peak, then away from it.
		auto window = [&](int bx, int by) {
			int s = 0;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int x = bx + dx, y = by + dy;
					if (x >= 0 && y >= 0 && x < kBins && y < kBins) s += votes[static_cast<size_t>(y) * kBins + x];
				}
			return s;
		};
		int best = -1, bestX = 0, bestY = 0;
		for (int y = 0; y < kBins; ++y)
			for (int x = 0; x < kBins; ++x)
			{
				if (!votes[static_cast<size_t>(y) * kBins + x]) continue;
				const int s = window(x, y);
				if (s > best) { best = s; bestX = x; bestY = y; }
			}
		// Refine: the mean of the confident cells within one texel of the peak.
		const double px = (bestX - 2 * kRange) / 2.0, py = (bestY - 2 * kRange) / 2.0;
		double sx = 0.0, sy = 0.0;
		long close = 0;
		for (size_t i = 0; i < cells; ++i)
		{
			if (costs[i] > costLimit) continue;
			const double vx = vectors[i * 2] / 32.0 / 4.0, vy = vectors[i * 2 + 1] / 32.0 / 4.0;
			if (std::fabs(vx - px) <= 1.0 && std::fabs(vy - py) <= 1.0) { sx += vx; sy += vy; ++close; }
		}
		const double share = static_cast<double>(close) / confident;
		// Half-texel steps, as D1's.
		global[0] = static_cast<float>(std::round((close ? sx / close : px) * 2.0) / 2.0);
		global[1] = static_cast<float>(std::round((close ? sy / close : py) * 2.0) / 2.0);
		// Ambiguity on D1's scale (consumers use < 0.9): 1.05 - 1.5 x share, so a tenth of the
		// confident cells agreeing = 0.9 (usable above that) and 70 % or more = 0.05.
		global[2] = static_cast<float>((std::min)(1.0, (std::max)(0.05, 1.05 - share * 0.45 / 0.3)));
		// The second layer: the best window more than a texel from the first.
		int secondBest = 0, sxBin = 0, syBin = 0;
		for (int y = 0; y < kBins; ++y)
			for (int x = 0; x < kBins; ++x)
			{
				if (!votes[static_cast<size_t>(y) * kBins + x]) continue;
				if (std::abs(x - bestX) <= 2 && std::abs(y - bestY) <= 2) continue;
				const int s = window(x, y);
				if (s > secondBest) { secondBest = s; sxBin = x; syBin = y; }
			}
		if (secondBest > 0 && secondBest >= confident / 10)
		{
			second[0] = static_cast<float>((sxBin - 2 * kRange) / 2.0);
			second[1] = static_cast<float>((syBin - 2 * kRange) / 2.0);
			second[3] = 1.0f;
		}
	}

	// W1-C3: the pair's whole-frame motion both ways (Flow::global / second), worked out once on the producer's worker and flagged so
	// readers (the FlowOf view: the camera ahead fit) take it instead of voting again.
	inline void FlowGlobals(Flow& flow, bool general)
	{
		FlowGlobal(flow.forward, flow.forwardCost, flow.gridW, flow.gridH, flow.global[0], flow.second[0], general);
		FlowGlobal(flow.backward, flow.backwardCost, flow.gridW, flow.gridH, flow.global[1], flow.second[1], general);
		flow.globalSet = true;
		flow.globalGeneral = general;
	}

	// The trust extras of one pair (Flow::costWeight, Flow::fbError) from its fields: the
	// de-judder's blocks over the flow's half size, the forward cost normalised per block,
	// the engine's own forward/backward error per cell. Either stays empty when its inputs
	// are missing (no costs: an older driver, a field without them).
	inline void FlowExtras(Flow& flow)
	{
		flow.blocksX = (flow.halfW + Flow::kBlockPx - 1) / Flow::kBlockPx;
		flow.blocksY = (flow.halfH + Flow::kBlockPx - 1) / Flow::kBlockPx;
		if (!OpticalFlowTrust::CostWeights(flow.forwardCost, flow.gridW, flow.gridH, NvOpticalFlow::Engine::Grid,
				Flow::kBlockPx, flow.blocksX, flow.blocksY, flow.costWeight))
			flow.costWeight.clear();
		if (!OpticalFlowTrust::FbError(flow.forward, flow.backward, flow.gridW, flow.gridH, NvOpticalFlow::Engine::Grid, flow.fbError))
			flow.fbError.clear();
	}

	// The half-size 8-bit luma of a frame ((w + 1) / 2 x (h + 1) / 2, 2 x 2
	// averaged; 10-bit codes / 4): P010 / P210 planes read directly, other
	// formats through the source's sampler. False when the source is not valid.
	bool HalfLuma8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& halfW, int& halfH);

	class Producer
	{
	public:
		Producer();
		~Producer();
		Producer(const Producer&) = delete;
		Producer& operator=(const Producer&) = delete;

		// A device of its own on the render device's adapter, and the worker.
		// False (Error()) when D3D11 or the adapter is not available; the engine
		// itself is opened on the first frame (size known).
		bool Start(ID3D11Device* renderDevice, NvOpticalFlow::Preset preset);
		// SK-MV: the P2 slot this producer publishes its pairs into (the vector engine's; call before Start). The slot is
		// cleared by Start, so the pairs of an older producer are never read as this one's. A producer without a slot makes a
		// slot of its own at construction (a standalone producer in a test); the engine always injects its own.
		void SetSlot(std::shared_ptr<VpMotion::P2Slot> target) { if (target) slot = std::move(target); }
		std::shared_ptr<VpMotion::P2Slot> Slot() const { return slot; }
		void Stop();
		bool Running() const { return running.load(std::memory_order_acquire); }
		// The engine opened and ran at least one pair (the depth decision's "engine").
		bool EngineReady() const { return engineReady.load(std::memory_order_acquire); }
		std::string Error() const;

		// From the render loop, each frame once, oldest first. `generation` is the
		// continuity generation (a new one clears the store and the chain). breakBefore:
		// this frame follows a break the sequence does not show (a missed-capture gap,
		// a film update, a pulldown lock change): no pair with the previous submission
		// (MotionValidity step 1 F), so the store holds only valid pairs.
		void Submit(uint64_t sequence, uint64_t generation, const AnalysisLumaSource& luma, bool breakBefore = false);
		// The engine input size the de-judder searches at (its half size, or a
		// quarter with the half-size inner matcher); 0 = the frame's half size.
		void SetTargetSize(int w, int h) { targetW.store(w, std::memory_order_release); targetH.store(h, std::memory_order_release); }
		// L-B4 (a): the frame's P1 (the skeleton's half luma) is the source of the 8-bit engine luma, read through this reader
		// (Acquire(kP1HalfLuma, seq, Cpu) from this thread); null / a miss / a format whose engine codes differ from P1's
		// (RGB, full-range 4:2:x: FIX-B4-RGB) = the producer's own HalfLuma8 decode. Any thread.
		void SetP1Source(std::shared_ptr<VpSkeleton::IP1Source> source)
		{ std::lock_guard<std::mutex> lock(p1Mutex); p1Source = std::move(source); }
		uint64_t LastSubmitted() const { return lastSubmitted.load(std::memory_order_acquire); }
		// Fixed-constant batch B3 (2026-10-18): the pending submissions follow the depth in use
		// (MotionAhead::PendingJobsFor: 6 at the default depth, as before).
		// B1 (2026-11-10): the walk hands over the LEAD (MotionAhead::LeadFor: every frame queued ahead, at least the depth) each pass;
		// the capacity is sized for the larger of the two. Call chain, one thread (the render loop): SetDepth from
		// VectorEngine::PlanProducer (VectorEngine.cpp, `motionProducer->SetDepth(ahead.configured)`; LibplaceboVideoRenderer.cpp
		// :23172), SetLead from MotionAhead::CollectQueueAhead (MotionAheadWalk.h; renderer :31169, under m_queueMutex) every pass,
		// before RunQueueWalk (renderer :31352) submits that pass's frames: the capacity always covers the frames handed over.
		void SetDepth(int depth) { pendingDepth.store(depth, std::memory_order_release); UpdatePendingCapacity(); }
		void SetLead(int lead) { pendingLead.store(lead, std::memory_order_release); UpdatePendingCapacity(); }
		size_t PendingCapacity() const { return maxPendingJobs.load(std::memory_order_acquire); }
		// general_rate_fixes for the engine's global vote range (FlowVoteRange).
		void SetGeneralRates(bool on) { generalRates.store(on, std::memory_order_release); }

		// W2-C5b: an ON-DEMAND pair for a render-time consumer (OnDemandEstimate.h). Any thread; never blocks on the worker:
		// a pair already in the slot answers FromSlot at once (zero work); not running / stopping / restarting answers
		// Unavailable; budgetMs <= 0 answers NoBudget; else the request is queued AHEAD of the look-ahead jobs (at most
		// OnDemand::Queue::kMaxPending; the oldest is answered Refused) and the worker answers it. Adjacent pairs are also
		// published into the P2 slot (compute once); gap pairs are returned only (P2 stays adjacency-only).
		OnDemand::PairFuture Request(OnDemand::PairRequest request);
		struct RequestStats
		{
			unsigned long long requests = 0, fromSlot = 0, ready = 0, late = 0, refused = 0, failed = 0, unavailable = 0, noBudget = 0;
			unsigned long long chainRuns = 0, adoptRuns = 0, offChainRuns = 0;   // engine runs by OnDemand::Mode
			// Audit fixes (W2-C5b stage 1): staleGeneration - answered Late, the stream moved to a newer generation after the
			// request was issued (in `late` too); forcedOffChain - a request of a NEWER generation than the walk has reached ran
			// off the chain (the walk owns the generation change); fairnessJobs - look-ahead jobs run ahead of a waiting request
			// by the streak bound (kMaxRequestStreak); droppedBehindRequests - look-ahead jobs dropped (Stats::dropped) while
			// requests were being served; adoptLostPairs - walk pairs lost because an Adopt moved the chain past the walk's next
			// frame (it re-seeds); hintsLost - walk pairs run without the temporal hints because a request run reset them.
			unsigned long long staleGeneration = 0, forcedOffChain = 0, fairnessJobs = 0, droppedBehindRequests = 0;
			// staleJobs: look-ahead jobs of a generation older than one a no-walk request already started - skipped (re-check S1).
			unsigned long long adoptLostPairs = 0, hintsLost = 0, staleJobs = 0;
			double lastQueuedMs = 0.0, lastRunMs = 0.0, runMsSum = 0.0;
		};
		// At most this many requests in a row while a look-ahead job waits; then one job runs (the walk is never starved).
		enum : int { kMaxRequestStreak = 2 };          // enum constants: ODR-safe in C++14 when bound to a const&
		// The walk counts as present while a job waits or one ran within this long (ms); a request then never moves the
		// slot's generation (the walk owns it) - a newer-generation request runs off the chain instead.
		enum : uint64_t { kWalkIdleMs = 1000 };
		RequestStats GetRequestStats() const;

		// W2-C5b test seam (OnDemandEstimateTests, CPU): a producer with no D3D11 device and no engine. The worker runs as
		// usual (the same queue, priority and chain rules); a look-ahead job calls `job` and a request's engine run calls
		// `request` (fill the Flow; false = the run failed). Start() clears the seam.
		struct TestEngine
		{
			std::function<bool(uint64_t sequence, uint64_t generation)> job;
			std::function<bool(const OnDemand::PairRequest& request, OnDemand::Mode mode, Flow& flow)> request;
		};
		bool StartForTest(std::shared_ptr<TestEngine> engine);

		// Reads of the slot this producer publishes into (for a standalone producer; the vector engine's consumers read
		// through its P2View instead): the pair's flow if it is ready (a hit), else null (a miss).
		FlowPtr Take(uint64_t from, uint64_t to) { return slot->Find(from, to); }
		void Prune(uint64_t oldestNeeded) { slot->Prune(oldestNeeded); }
		VpMotion::P2Slot::Stats StoreStats() const { return slot->GetStats(); }

		// extrasMs: the trust extras of the last pair (global, cost, consistency) on the worker.
		struct Stats { unsigned long long frames = 0, pairs = 0, dropped = 0, failures = 0; double lastPairMs = 0.0, lumaMs = 0.0, extrasMs = 0.0; double pairMsSum = 0.0; /* SK-MV: the sum of all pair times, for the standing-cost line */ };
		Stats GetStats() const;

		// Bug hunt 35 #1: the stop/restart state (FallbackCatalog::VectorProducer builds the entry).
		struct Health
		{
			bool recovering = false;          // stopped, a restart is pending
			unsigned stops = 0, restarts = 0; // this session
			uint64_t retryInMs = 0;           // while recovering
			std::string error;                // the failure that stopped it last
		};
		Health GetHealth() const;
		// Test seam: the back-off waits (ms) are scaled by this divisor (1 = real time).
		void SetRecoveryTimeDivisorForTest(unsigned divisor) { recoveryDivisor.store(divisor < 1 ? 1 : divisor, std::memory_order_release); }

	private:
		struct Job { uint64_t sequence = 0, generation = 0; AnalysisLumaSource luma; bool breakBefore = false; };
		void Worker();
		bool Process(const Job& job);
		bool ProcessJobForTest(const Job& job);
		// W2-C5b; false = an engine failure (counted like a job's). engineRan = the engine did work (only that resets the
		// consecutive-failure count; a Late / FromSlot / nothing-to-run answer does not).
		bool ProcessRequest(OnDemand::Pending& pending, bool& engineRan);
		void NoteWalkJob(const Job& job);                 // W2-C5b: the walk's bookkeeping (adoptLostPairs, hintsLost, lastJobMs)
		bool WalkActiveLocked() const;                    // W2-C5b, under mutex
		void ReleaseStaleRequestTextures(int w, int h);   // W2-C5b: the off-chain textures go on an engine size change
		void Answer(OnDemand::Pending& pending, OnDemand::PairResult result);
		// The frame's 8-bit luma at the engine size (HalfLuma8, then the quarter step when the target asks for it).
		// False = nothing to do: not a readable frame, or a size this producer does not make.
		bool LumaAtTarget(uint64_t sequence, const AnalysisLumaSource& luma, std::vector<uint8_t>& buffer, int& w, int& h);
		// One engine run on (a, b) at w x h, read back into a new Flow with its global motion and trust extras. ran = the
		// engine itself succeeded (the readback may still fail). False = failed (error set).
		bool RunPair(ID3D11Texture2D* a, ID3D11Texture2D* b, int w, int h, bool continuous, uint64_t timedSequence, bool& ran,
			std::shared_ptr<Flow>& flow, double& extrasMs);
		bool EnsureRequestTextures(int w, int h);   // W2-C5b: the off-chain pair (made on first use; its own VRAM declaration)
		void ReleaseRequestTextures();
		bool EnsureSize(int halfW, int halfH);
		void ReleaseEngine();
		bool CreateDevice(std::string& why);
		static uint64_t NowMs();

		mutable std::mutex mutex;
		std::condition_variable wake;
		unsigned consecutiveFailures = 0;   // under mutex
		std::deque<Job> jobs;
		OnDemand::Queue requests;           // W2-C5b, under mutex: taken before `jobs`
		RequestStats requestStats;          // under mutex
		int requestStreak = 0;              // W2-C5b, under mutex: requests run in a row while a job waited
		bool requestRunning = false;        // W2-C5b, under mutex: the worker is serving a request now
		bool requestOwnsGeneration = false; // W2-C5b, worker only: a no-walk request set the slot's generation (no job since)
		uint64_t lastJobMs = 0;             // W2-C5b, under mutex: NowMs() of the last look-ahead job (0 = none yet)
		bool adoptOverWalk = false;         // W2-C5b, worker only: an Adopt moved the chain while the walk was present
		bool requestResetHints = false;     // W2-C5b, worker only: a request run reset the temporal hints
		std::shared_ptr<TestEngine> testEngine;   // W2-C5b test seam (set before the worker starts)
		bool stopping = false;
		std::thread worker;
		std::atomic<bool> running{ false }, engineReady{ false };
		std::atomic<uint64_t> lastSubmitted{ 0 };
		std::atomic<size_t> maxPendingJobs{ 6 };
		std::atomic<int> pendingDepth{ 0 }, pendingLead{ 0 };   // B1: the inputs of maxPendingJobs
		void UpdatePendingCapacity()
		{
			const int d = pendingDepth.load(std::memory_order_acquire), l = pendingLead.load(std::memory_order_acquire);
			maxPendingJobs.store(PendingJobsFor(d > l ? d : l), std::memory_order_release);
		}
		std::atomic<bool> generalRates{ true };
		std::atomic<int> targetW{ 0 }, targetH{ 0 };
		std::mutex p1Mutex;                                 // guards p1Source (set from the render thread, read by the worker)
		std::shared_ptr<VpSkeleton::IP1Source> p1Source;
		std::vector<uint8_t> quarterBuffer;
		std::string error;
		Stats stats;
		ProducerRecovery recovery;          // under mutex
		std::string stopError;              // under mutex: the error that caused the last stop
		std::atomic<unsigned> recoveryDivisor{ 1 };
		CComPtr<IDXGIAdapter> renderAdapter;   // the render GPU (set in Start, read by the worker)

		// Worker side.
		CComPtr<ID3D11Device> device;
		CComPtr<ID3D11DeviceContext> context;
		NvOpticalFlow::Preset preset = NvOpticalFlow::Preset::Medium;
		std::unique_ptr<NvOpticalFlow::Engine> engine;
		int sizeW = 0, sizeH = 0;
		CComPtr<ID3D11Texture2D> lumaTex[2];   // previous / current (ping-pong)
		CComPtr<ID3D11Texture2D> flowTex[2], costTex[2], flowRead[2], costRead[2];
		int current = 0;
		bool havePrevious = false;
		uint64_t previousSequence = 0, previousGeneration = 0;
		uint64_t hintSequence = UINT64_MAX, hintGeneration = 0;   // the "to" frame of the engine's last pair
		std::vector<uint8_t> lumaBuffer;
		std::vector<uint8_t> requestLumaBuffer;   // W2-C5b: a request's `from` frame (Adopt / OffChain)
		CComPtr<ID3D11Texture2D> requestTex[2];   // W2-C5b: the off-chain pair (from, to); empty until an OffChain request
		int requestTexW = 0, requestTexH = 0;
		std::shared_ptr<VpMotion::P2Slot> slot{ std::make_shared<VpMotion::P2Slot>(MotionAhead::kP2RingPairs) };   // published into (SetSlot injects the engine's)
	};
}
