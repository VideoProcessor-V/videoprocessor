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
#include <VP-V/core/LaneInterfaces.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace VpFrameAnalysis
{
	// ---- Range model (WP-23 decision; docs/design/wp23-frame-analysis-store.md) ----
	// VP's analysis domain is 10-bit studio codes. With AnalysisLumaSource::limitedDomain (VP-V
	// pathway, source_coding_fixes) the sampler already emits them; Bill's v1.4.02 sampler
	// (Original pathway) emits the delivered codes with an AnalysisLumaRange tag. The ONE range
	// function is the decode owner's AnalysisLumaSource::CodesFullRange (merge 2026-10-31; 15c C6
	// deleted this producer's wrapper of it). Sampler luma code (0..1023) -> studio code, the single
	// conversion for every VP consumer: the decode owner's StudioLumaMap (AnalysisLumaDomain.h).
	using Vpv::AnalysisDomain::StudioLumaMap;

	// ---- The single luma decode (the decode owner's, 15c C6; owner answer 3, 2026-11-15) ----
	// Gives the source an empty pooled shared plane when the owner says it needs one
	// (Vpv::AnalysisDomain::NeedsLumaPlane), before the source is copied out to readers, so every copy
	// shares it. No-op otherwise or when it has one.
	void AttachSharedLuma(AnalysisLumaSource& source);
	// Fills and publishes the shared plane once (the first caller that claims it): the decode owner's
	// Vpv::AnalysisDomain::DecodeSharedLuma. False when there is nothing to do (no plane, already
	// claimed, a format that cannot be read).
	using Vpv::AnalysisDomain::DecodeSharedLuma;
	// Planes are recycled from a small pool once nothing holds them; the store's switch-off empties it.
	void ReleasePlanePool();
	size_t PlanePoolSize();

	// ---- C9 commit A: the frame's luma view and the P1 engine reader (doc 93 C9P s2b) ----
	// The frame's decoded luma as the lane reads it (10-bit sampler codes, msbShift 6, stride = width; keepAlive holds the
	// plane): the shared plane, or - for a format read in place (P010 / P210 / decoded DV) - a pooled plane decoded here,
	// never a private decoder. view.seq is left 0 (the caller's key). False when there is no ready plane (the shared plane
	// not decoded yet or claimed by another thread, an unreadable frame). The source frame tap delivers exactly this view.
	bool SourceViewOf(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view);
	// P10 N fix (refs/116b, owner 2026-11-12: 12-bit end to end): the SAME frame's luma at TWELVE bits (msbShift 4: the
	// DvTunnelTapLaw 12-bit tap code, limited domain), for a packed DV tunnel source only (false for every other format: those
	// carry no more than their own ten-bit codes). A VP-V side product of Bill's untouched AnalysisLumaSource (Sample().luma stays
	// the ten-bit code); the lane's other readers keep SourceViewOf (msbShift 6). The tap is the Q6 analysis axis (AnalysisAxis10.h): a
	// 12-bit sample is code12 << 4 = Q6 (msbShift 4), so a consumer keeps the axis law (GrainSplit sourceBits 10, codeUnit 64 / 65535,
	// the same for every tap; W21-3 proves it equal to the native 12-bit law sourceBits 12, codeUnit 16 / 65535).
	// THE 12-bit decode (rule 1): every call that decodes is counted here, as ComputeOnce unit "analysis.tap12" (variant 12) on the
	// calling thread's frame scope (Process sets it to the job's seq; a call outside any scope is a counted coverage gap).
	// False, with nothing counted, for a non-DV or an invalid source (nothing to tap). For a VALID packed DV source IsValid() implies the
	// 12-bit law (DvTunnelTapLaw::Supports invariant, asserted), so the one reachable refusal is a 12-bit plane that cannot be allocated
	// (the pool is full and an unpooled plane cannot be had either): false, counted in DvTapRefusals.
	bool SourceViewOf12(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view);

	// W21-1: the view the source frame tap delivers (12-bit end to end), the same law Process applies to its shared decode. A valid
	// packed DV tunnel source delivers SourceViewOf12 (the Q6 axis at 12 bits, msbShift 4, pooled plane); every other source, and an
	// invalid DV source (not a refusal: nothing to tap), delivers exactly SourceViewOf. When SourceViewOf12 refuses a valid DV frame
	// (its 12-bit plane could not be allocated; counted there and shown on Ctrl+I, skeleton_lane line "analysis tap: 10-bit (DV 12-bit
	// tap refused: N frame(s), no memory for the 12-bit plane)") the 10-bit SourceViewOf is delivered instead: never silent.
	// `refused` (optional) is set when this call fell back. False when no view could be made at all.
	bool TapViewOf(const AnalysisLumaSource& source, VpSkeleton::SourceFrameView& view, bool* refused = nullptr);
	// Valid DV frames whose 12-bit tap was refused (see SourceViewOf12) and whose readers fell back to the 10-bit view (process total;
	// one count per refused decode, i.e. per frame: Process decodes each frame at most once; shown, never log-only).
	uint64_t DvTapRefusals();
	void NoteDvTapRefusal();   // counts one refused frame; called by SourceViewOf12 only (tests may call it to drive the Ctrl+I line)
	void ResetDvTapRefusals();

	// The P1 engine half of a frame through its shared decode (the live fill): DecodeSharedLuma when nobody has (a no-op when
	// published or claimed), SourceViewOf, the P1 half law (HalfLumaRows), then Engine8Half - the lane's law, one recipe.
	// out = HalfDim(w) x HalfDim(h) bytes. False when there is no view.
	bool LiveEngine8(const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh);

	// The named exception (doc 105 phase 4b, doc 91 D C9): formats whose producer half (MotionAhead::HalfLuma8) decodes its
	// own codes, which are not the shared decode's - RGB structure luma (R + 2G + B) / 4 (R12B, R12L, r210, R10b, R10l, 8-bit
	// BGRA / ARGB) and raw full-range 4:2:x words (P010 / P210 / v210 / UYVY at full range). For these the engine view of P1
	// is NOT byte-identical to the producer's; commit B keeps the producer's own decode for them unless EPE evidence shows
	// the shared-decode view is as good. Every other format: the reader's bytes are the producer's.
	bool P1EngineViewDiffersFromProducer(const AnalysisLumaSource& source);

	// IP1Source: the lane's published P1 under the slot seqlock when `published` has it, else the live fill above (counted:
	// p1FilledByMotion, target 0 in steady state once the lane covers the full queue depth). A null hook = lane off: live fill
	// only. Any thread (one reader object per calling worker: the scratch row buffer is the reader's).
	class P1Reader : public VpSkeleton::IP1Source
	{
	public:
		explicit P1Reader(VpSkeleton::PublishedEngine8Fn published = VpSkeleton::PublishedEngine8Fn()) : m_published(std::move(published)) {}
		P1Reader(const P1Reader&) = delete;
		P1Reader& operator=(const P1Reader&) = delete;

		bool ReadEngine8(uint64_t seq, const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh,
			VpSkeleton::P1ReadKind& how) override;

		uint64_t LanePublished() const { return m_lanePublished.load(std::memory_order_relaxed); }
		uint64_t FilledByMotion() const { return m_filledByMotion.load(std::memory_order_relaxed); }   // p1FilledByMotion
		uint64_t SeqlockRetries() const { return m_seqlockRetries.load(std::memory_order_relaxed); }   // Recycled copies (then live)
		uint64_t Unread() const { return m_unread.load(std::memory_order_relaxed); }

	private:
		VpSkeleton::PublishedEngine8Fn m_published;
		std::atomic<uint64_t> m_lanePublished{ 0 };
		std::atomic<uint64_t> m_filledByMotion{ 0 };
		std::atomic<uint64_t> m_seqlockRetries{ 0 };
		std::atomic<uint64_t> m_unread{ 0 };
	};

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
	};

	class Reducer
	{
	public:
		// `studio` maps the sampler's codes to studio codes (StudioLumaMap).
		Reducer(const FrameAnalysis::Rect& picture, const std::array<uint16_t, 1024>& studio);
		// Row y's sampler codes for [picture.left, picture.right). Rows outside are ignored.
		void AddRow(int y, const uint16_t* codes);
		Measured Finish();

	private:
		FrameAnalysis::Rect m_picture;
		std::array<uint16_t, 1024> m_studio;
		std::array<uint32_t, FrameAnalysis::kHistogramBins> m_histogram{};
		uint64_t m_samples = 0;
		uint64_t m_sum = 0;
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
		const void* owner = nullptr;        // set when a SourceFrameTap sink of this renderer wants the decoded plane
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
