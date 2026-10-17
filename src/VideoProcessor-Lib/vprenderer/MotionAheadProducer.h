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
// stores them per pair in a VectorStore under the continuity generation. The
// de-judder takes its pair from there (no engine work at render time); a miss
// means it runs the engine itself, or the searches, as before. Never waits: a
// full queue drops the oldest submissions; an engine failure stops the producer
// for the session (logged) and everything falls back.

#include <AnalysisLumaSource.h>
#include <OpticalFlowTrust.h>
#include <VectorStore.h>
#include <vprenderer/NvOpticalFlow.h>

#include <d3d11.h>
#include <atlbase.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
	using FlowPtr = std::shared_ptr<const Flow>;

	// The whole frame's motion from one engine field (S10.5 half-size pixels per 4 x 4
	// cell; costs higher = less sure): the most common vector (a 3 x 3 vote on a
	// half-cell grid, refined by the mean of the cells within one cell of it), its share
	// of the confident cells as the ambiguity, and the most common vector away from it as
	// the second layer (when at least 10 % of the cells hold it).
	inline void FlowGlobal(const std::vector<int16_t>& vectors, const std::vector<uint8_t>& costs, int gridW, int gridH,
		float global[4], float second[4])
	{
		// Units: one engine cell (4 half-size px) = one 1/8-size texel; votes in half texels.
		static constexpr int kRange = 40;              // +-40 texels (D1 searches +-32)
		static constexpr int kBins = 4 * kRange + 1;   // half-texel bins
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
		auto binOf = [](int16_t v) { return static_cast<int>(std::lround(v / 32.0 / 4.0 * 2.0)) + 2 * kRange; };
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
		// Ambiguity on D1's scale (consumers use < 0.9): a third of the cells agreeing = 0.9.
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
		uint64_t LastSubmitted() const { return lastSubmitted.load(std::memory_order_acquire); }

		// The consumer: the pair's flow if it is ready (a hit), else null (a miss).
		FlowPtr Take(uint64_t from, uint64_t to);
		void Prune(uint64_t oldestNeeded) { store.Prune(oldestNeeded); }
		VectorStore<FlowPtr>::Stats StoreStats() const { return store.GetStats(); }

		// extrasMs: the trust extras of the last pair (global, cost, consistency) on the worker.
		struct Stats { unsigned long long frames = 0, pairs = 0, dropped = 0, failures = 0; double lastPairMs = 0.0, lumaMs = 0.0, extrasMs = 0.0; };
		Stats GetStats() const;

	private:
		struct Job { uint64_t sequence = 0, generation = 0; AnalysisLumaSource luma; bool breakBefore = false; };
		void Worker();
		bool Process(const Job& job);
		bool EnsureSize(int halfW, int halfH);

		mutable std::mutex mutex;
		std::condition_variable wake;
		unsigned consecutiveFailures = 0;   // under mutex
		std::deque<Job> jobs;
		bool stopping = false;
		std::thread worker;
		std::atomic<bool> running{ false }, engineReady{ false };
		std::atomic<uint64_t> lastSubmitted{ 0 };
		std::atomic<int> targetW{ 0 }, targetH{ 0 };
		std::vector<uint8_t> quarterBuffer;
		std::string error;
		Stats stats;

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
		VectorStore<FlowPtr> store{ 16 };
	};
}
