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
#include <VectorStore.h>
#include <vprenderer/NvOpticalFlow.h>

#include <d3d11.h>
#include <atlbase.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
	};
	using FlowPtr = std::shared_ptr<const Flow>;

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
		// continuity generation (a new one clears the store and the chain).
		void Submit(uint64_t sequence, uint64_t generation, const AnalysisLumaSource& luma);
		// The engine input size the de-judder searches at (its half size, or a
		// quarter with the half-size inner matcher); 0 = the frame's half size.
		void SetTargetSize(int w, int h) { targetW.store(w, std::memory_order_release); targetH.store(h, std::memory_order_release); }
		uint64_t LastSubmitted() const { return lastSubmitted.load(std::memory_order_acquire); }

		// The consumer: the pair's flow if it is ready (a hit), else null (a miss).
		FlowPtr Take(uint64_t from, uint64_t to);
		void Prune(uint64_t oldestNeeded) { store.Prune(oldestNeeded); }
		VectorStore<FlowPtr>::Stats StoreStats() const { return store.GetStats(); }

		struct Stats { unsigned long long frames = 0, pairs = 0, dropped = 0, failures = 0; double lastPairMs = 0.0, lumaMs = 0.0; };
		Stats GetStats() const;

	private:
		struct Job { uint64_t sequence = 0, generation = 0; AnalysisLumaSource luma; };
		void Worker();
		bool Process(const Job& job);
		bool EnsureSize(int halfW, int halfH);

		mutable std::mutex mutex;
		std::condition_variable wake;
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
		std::vector<uint8_t> lumaBuffer;
		VectorStore<FlowPtr> store{ 16 };
	};
}
