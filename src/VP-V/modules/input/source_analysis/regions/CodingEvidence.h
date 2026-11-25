// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// input.source_analysis/regions: P16 kCodingEvidence, the CODING EVIDENCE of the source picture (the one measure of block
// coding, banding and effective bit depth; lead8/iface-P16.md). MOVED here from SourceResolutionDetector (U13: one owner, no
// private copy): the per-tile measure (MeasureTile: code histogram, 8/16 px period blocking, the 8x8 band-edge blocks, the robust
// grid / band readings), the grid finder (GridBlockiness), BandEdgeBlock, EffectiveBitsOf and the frame reduction (ReduceFrame).
//
// Product form: CPU, the default variant, per monitor frame seq. Bytes = Header + one record per tile (Serialise / Deserialise).
// The owner fill (MakeOwnerFill) measures the tile pack the monitor staged for that seq (Hub::Stage); consumers get it through
// SkeletonMemo::Acquire (Hub::AcquireTiles): no private measurement. Not acquired (no skeleton memo / declined) = coding evidence
// UNAVAILABLE, shown in the consumer's fallbacks. The one fallback (rules 1/5, rule 25 addendum): when NO renderer store exists at all
// (a standalone monitor: tests, tools; or a renderer torn down while jobs are still queued) the consumer calls this unit's
// Hub::MeasureWithoutMemo - the same MeasurePack the owner fill runs, counted here and shown by the consumer. Never measured privately.
//
// C++14, no Windows or libplacebo headers.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <VP-V/core/SkeletonAcquire.h>

namespace VpUnit
{
namespace CodingEvidence
{
	constexpr uint32_t kUnitVersion = 1;   // the core's kExpectedCodingEvidenceUnit
	constexpr const char* kUnitPath = "input/source_analysis/regions";

	// One tile's coding measure (spec 1b / 2d; hunt 2 M2/M3).
	// histogram: the code histogram; blockiness: boundary step against the others at the 8 / 16 px periods (1 = none);
	// gradient: the mean step; blocks / bandEdges / flatBlocks: the 8x8 blocks, those holding a band edge, flat ones.
	// flatHistogram: codes of the low-activity 8x8 blocks only; blockinessRobust: the grid found at any period 8 px .. tile / 8, any
	// phase, a boundary up to 3 px wide, both axes (the stronger; a staircase is no grid); blockPeriod: that period (0 = none);
	// bandEdgesRobust: 8x8 blocks with a band edge of two dominant levels.
	struct TileMeasure
	{
		std::vector<unsigned> histogram;
		double blockiness = 1.0, gradient = 0.0;
		int blocks = 0, bandEdges = 0, flatBlocks = 0;
		std::vector<unsigned> flatHistogram;
		double blockinessRobust = 1.0, blockPeriod = 0.0;
		int bandEdgesRobust = 0;
	};

	// The frame's reading (spec 1b: 1 = clean .. 0 = badly blocked or banded; spec 2d: the bits the picture really uses, 0 = unknown).
	struct FrameCoding
	{
		double bitrateQuality = 1.0, effectiveBits = 0.0;
		double blockiness = 1.0, bandShare = 0.0;
	};

	// `luma` row-major, `stride` in elements, values 0..1; `n` x `n` pixels on the frame's 16 px grid; `codeMaximum` = 1023 for 10 bit.
	TileMeasure MeasureTile(const float* luma, int stride, int n, int codeMaximum);

	// The frame from its tiles. `eligible[i]` (the caller's judgement: 1 = tile i may count as grid evidence; an exactly periodic
	// tile's own repeat looks like a grid) applies with codingFixes; a short or empty vector means not eligible.
	FrameCoding ReduceFrame(const std::vector<const TileMeasure*>& tiles, const std::vector<char>& eligible, bool codingFixes);

	// Hunt 2 M2: the effective bits of a code histogram: 16-code windows with at least 256 pixels whose neighbouring windows hold within
	// 3x as many, each window's occupancy as a Hill number of order 4; the median window. 0 when no window can be judged.
	double EffectiveBitsOf(const std::vector<unsigned long long>& histogram);
	// Hunt 2 M3: the coding-grid blockiness of one axis's mean absolute step per position (`steps[x]` between x and x + 1). 1 when no
	// grid stands out; `period` gets the grid's period in pixels (0 = none). `axisScale` is native codes per 10-bit axis code (1 at
	// <= 10-bit, 4 at 12-bit; clamped to >= 1) so the quarter-axis-code noise floor matches MeasureTile at all bit depths.
	double GridBlockiness(const std::vector<double>& steps, double& period, int axisScale = 1);
	// Hunt 2 M3: an 8x8 block (row-major codes) holds a band edge (two dominant levels a minimum to maximum step apart, split along a line).
	bool BandEdgeBlock(const int* block, int minimumStep, int maximumStep);

	// ---- the product's bytes ----
	std::vector<uint8_t> Serialise(const std::vector<TileMeasure>& tiles, int tileSize, int codeMaximum);
	bool Deserialise(const uint8_t* bytes, size_t size, std::vector<TileMeasure>& tiles);

	// ---- the fill's input and the consumer's access ----
	// The tiles of one monitor frame as the monitor quantised them (tile-major, tileSize^2 floats each, values 0..1).
	struct TilePack
	{
		int tiles = 0, tileSize = 0, codeMaximum = 1023;
		std::vector<float> luma;
	};

	// The one per-pack measure (MeasureTile per tile, tile order): what the owner fill serialises and what the no-store fallback
	// returns. False (nothing measured) when the pack is malformed; zero tiles = true with no records (the fill declines that case).
	bool MeasurePack(const TilePack& pack, std::vector<TileMeasure>& tiles);

	// Process-wide seam (module-level; no Bill file): the skeleton module gives the memo provider, the monitor stages its pack and
	// Acquires. A later MemoFor of the core replaces the provider only.
	// Lifetime: Instance() is never destroyed (a leaked heap object), so a module destructor running during static destruction
	// still finds it. The provider is called under a shared lock that SetMemoProvider / ClearMemoProvider take exclusively: once a
	// clear returns, no call into the old provider (and the module its lambda captured) is running or can start.
	class Hub
	{
	public:
		using MemoProvider = std::function<std::shared_ptr<VpSkeleton::SkeletonMemo>(const void* host)>;
		static Hub& Instance();
		// Installs `provider` (nullptr = none) and returns its token (0 for nullptr). Waits for in-flight provider calls.
		uint64_t SetMemoProvider(MemoProvider provider);
		// Clears the provider only if `token` is still the installed one (a newer module's provider is kept). Waits as above.
		void ClearMemoProvider(uint64_t token);
		// The visible no-store fallback (no renderer store exists): MeasurePack, counted (MeasuredWithoutMemo). The CALLER shows it.
		bool MeasureWithoutMemo(const TilePack& pack, std::vector<TileMeasure>& tiles);
		unsigned long long MeasuredWithoutMemo() const { return withoutMemo_.load(std::memory_order_relaxed); }
		// Stage the pack for `seq`; the fill takes it. Bounded (the oldest are dropped).
		void Stage(uint64_t seq, std::shared_ptr<const TilePack> pack);
		std::shared_ptr<const TilePack> Staged(uint64_t seq) const;
		void Unstage(uint64_t seq);
		// Stage + Acquire + Deserialise + Unstage. False (with `why` = the Acquire token or "no skeleton memo") = unavailable.
		bool AcquireTiles(const void* host, uint64_t seq, std::shared_ptr<const TilePack> pack, std::vector<TileMeasure>& tiles, std::string& why);
	private:
		mutable std::mutex mutex_;   // staged_
		std::shared_timed_mutex providerMutex_;   // provider_ + providerToken_; shared while the provider is called
		MemoProvider provider_;
		uint64_t providerToken_ = 0, nextToken_ = 0;
		std::atomic<unsigned long long> withoutMemo_{ 0 };
		std::map<uint64_t, std::shared_ptr<const TilePack>> staged_;
	};

	// The owner fill of (kCodingEvidence, Cpu): measures Hub::Staged(request.seq). keepFrames = how many frames after its own the
	// product stays readable: the asking monitor's worker lags the renderer by up to its latency, so the registrar passes
	// SourceResolutionMonitor::kLatencyFrames (derived from the monitor's job-queue depth, not a fixed number). A frame older than
	// the window is released and read as visible unavailability (FrameReleased).
	VpSkeleton::OwnerFill MakeOwnerFill(Hub& hub, uint8_t keepFrames);
}
}
