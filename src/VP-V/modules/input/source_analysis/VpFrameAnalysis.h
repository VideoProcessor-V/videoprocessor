#pragma once

// VP frame analysis producer (WP-23 + addendum "one luma decode for everyone"). VP-ONLY.
//
// Two jobs, both done ONCE per source frame on one worker thread (laptop thread budget):
//
// 1. The single luma decode. AnalysisLumaSource::sharedLuma (Bill's analysis view, WP-23 hook,
//    offered upstream: docs/design/wp23-bill-analysers.md) is filled with Sample(x, y).luma of
//    every pixel. Every analyser then reads luma from it - Bill's (SceneDetector, active picture,
//    subtitles: SampleLuma) and ours (LED look-ahead, content intent, intent interpreter, HDR veto
//    grid, NVOF producer, source-resolution look-ahead). We feed his code its INPUT once; his
//    decisions stay his. Readers never wait: before the plane is published they decode as before,
//    so every value and decision is identical with or without it (tests: SharedLumaEquivalence).
//    Formats whose luma is already a plane (P010 / P210 / decoded DV, limited range) need no
//    decode: their readers read the plane in place and no copy is made.
//
// 2. The FrameAnalysis::Store record (core/FrameAnalysisStore.h): histogram, APL, percentiles,
//    BTB/WTW/near-black/clipped occupancy and the grain estimate over the active picture (full
//    coverage), from the same row pass, in the studio code domain (StudioLumaMap: the one range
//    conversion for VP).
//
// The producer never blocks the caller: Submit() opens the record (late until published) and
// queues the job; a full queue drops its OLDEST job (that record stays late: consumers keep their
// own computation and show "Fallbacks engaged: frame analysis (...)"). GPU reductions on the
// early-upload planes are the planned faster path (plan WP-23 "Producer placement"); this is the
// CPU path, used where no GPU path exists and by the tests as the reference.

#include <AnalysisLumaSource.h>
#include <VP-V/core/FrameAnalysisStore.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace VpFrameAnalysis
{
	// ---- Range model (WP-23 decision; docs/design/wp23-frame-analysis-store.md) ----
	// VP's analysis domain is 10-bit studio codes. Our tree's sampler already emits them
	// (kAnalysisLumaSampleLimitedDomain); Bill's v1.4.02 sampler emits the delivered codes with
	// an AnalysisLumaRange tag. At the WP-21 merge this ONE function becomes
	// `return source.range == AnalysisLumaRange::Full;` and our full->limited edit leaves his file.
	bool SamplerCodesAreFullRange(const AnalysisLumaSource& source);
	// Sampler luma code (0..1023) -> studio code, the single conversion for every VP consumer.
	std::array<uint16_t, 1024> StudioLumaMap(const AnalysisLumaSource& source);

	// ---- The single luma decode ----
	// True when the format's luma must be decoded (not already a readable 10-bit plane).
	bool NeedsDecode(const AnalysisLumaSource& source);
	// Gives the source an empty shared plane when it needs a decode (before the source is copied
	// out to readers, so every copy shares it). No-op otherwise or when it has one.
	void AttachSharedLuma(AnalysisLumaSource& source);
	// Fills and publishes the shared plane once (the first caller that claims it). False when there
	// is nothing to do (no plane, already claimed, a format that cannot be read).
	bool DecodeSharedLuma(const AnalysisLumaSource& source);
	// Planes are recycled from a small pool once nothing holds them; the store's switch-off empties it.
	void ReleasePlanePool();
	size_t PlanePoolSize();

	// Sample().luma for [x0, x0 + count) of row y, in the sampler's domain: the shared plane, the
	// 16-bit plane read in place, the row decoder, or the per-pixel sampler (in that order).
	class LumaRowReader
	{
	public:
		explicit LumaRowReader(const AnalysisLumaSource& source) : m_source(source) {}
		const uint16_t* Row(int y, int x0, int count);

	private:
		const AnalysisLumaSource& m_source;
		std::vector<uint16_t> m_buffer;
	};

	// ---- Reductions (CPU path and the tests' reference) ----
	struct Measured
	{
		FrameAnalysis::LumaStats luma;
		FrameAnalysis::RangeOccupancy range;
		FrameAnalysis::GrainEstimate grain;
	};

	// Grain / noise: 8x8 blocks over the picture; RMS of the flattest decile (blocks near black or
	// clipped excluded: crushed noise is not grain); confidence = share of equally flat blocks.
	constexpr int kGrainBlock = 8;

	class Reducer
	{
	public:
		// `studio` maps the sampler's codes to studio codes (StudioLumaMap).
		Reducer(const FrameAnalysis::Rect& picture, const std::array<uint16_t, 1024>& studio);
		// Row y's sampler codes for [picture.left, picture.right). Rows outside are ignored.
		void AddRow(int y, const uint16_t* codes);
		Measured Finish();

	private:
		void FlushBand();
		FrameAnalysis::Rect m_picture;
		std::array<uint16_t, 1024> m_studio;
		std::array<uint32_t, FrameAnalysis::kHistogramBins> m_histogram{};
		uint64_t m_samples = 0;
		uint64_t m_sum = 0;
		std::vector<uint64_t> m_blockSum, m_blockSumSq;
		std::vector<uint8_t> m_blockExcluded;
		int m_bandRows = 0;
		std::vector<float> m_blockVariance;
		uint32_t m_blocks = 0;
	};

	// The whole picture in one call (tests, CPU fallback). False when the source cannot be read.
	bool Reduce(const AnalysisLumaSource& source, const FrameAnalysis::Rect& picture, Measured& out);

	// ---- The producer ----
	struct Job
	{
		FrameAnalysis::Identity identity;   // identity.seq is the ring key; picture = Bill's crop or full raster
		AnalysisLumaSource source;          // keeps the frame's buffer (and shared plane) alive
		bool decodePlane = true;            // fill the shared luma plane (some reader wants full luma)
		bool measure = true;                // compute the measured groups
	};

	// Decode + reduce + publish, on the calling thread (the worker; tests call it directly).
	void Process(FrameAnalysis::Store& store, const Job& job);

	class Producer
	{
	public:
		explicit Producer(FrameAnalysis::Store& store) : m_store(store) {}
		~Producer() { Stop(); }
		Producer(const Producer&) = delete;
		Producer& operator=(const Producer&) = delete;

		void Start();
		void Stop();
		bool Running() const { return m_running.load(std::memory_order_acquire); }

		// Opens the record and queues the job; never waits for the worker. Capacity = the store's
		// ring depth (a job older than that would be evicted anyway).
		void Submit(Job job);
		// Processes everything queued on the calling thread (tests; Stop()).
		void Drain();

		uint64_t Processed() const { return m_processed.load(std::memory_order_relaxed); }
		uint64_t Dropped() const { return m_dropped.load(std::memory_order_relaxed); }
		double LastMs() const { return m_lastMs.load(std::memory_order_relaxed); }
		// A visible fallback reason while jobs are being dropped (the worker is behind), else false.
		bool Behind(std::string& reason) const;

	private:
		void Run();
		FrameAnalysis::Store& m_store;
		mutable std::mutex m_mutex;
		std::condition_variable m_wake;
		std::deque<Job> m_jobs;
		std::thread m_thread;
		bool m_stop = false;
		std::atomic<bool> m_running{ false };
		std::atomic<uint64_t> m_processed{ 0 };
		std::atomic<uint64_t> m_dropped{ 0 };
		uint64_t m_cleanSinceDrop = 0;   // submissions without a drop since the last one
		std::atomic<double> m_lastMs{ 0.0 };
	};
}
