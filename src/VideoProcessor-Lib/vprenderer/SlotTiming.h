#pragma once

#include "PipelineSlots.h"

#include <array>
#include <cstdint>

// Per-slot GPU timing and the projected 3090 result (owner 2026-10-24, barrier B4).
//
// The 3090 trains unattended this week, so the real-time proof uses the LAPTOP (RTX 3050 Ti):
// the pipeline runs every frame (an every-frame analysis mode is fine even at ~1 s/frame), each
// slot's GPU time is measured with timestamp queries around its passes/hooks (the renderer feeds
// Record()), and the 3090 result is PROJECTED from per-stage measured ratios (3090 ms / laptop ms
// for the same stage, from BenchKit or earlier paired runs). A stage without a measured ratio
// uses the fallback ratio and is flagged, so a projection never looks more certain than it is.
//
// Pure and header-only; the GPU query plumbing lives in the renderer.
namespace SlotTiming
{
	using PipelineSlots::Id;
	constexpr size_t kSlots = static_cast<size_t>(Id::Count);

	class Accumulator
	{
	public:
		void Record(Id id, double gpuMs)
		{
			const size_t i = static_cast<size_t>(id);
			sumMs[i] += gpuMs;
			++count[i];
			if (gpuMs > maxMs[i]) maxMs[i] = gpuMs;
		}
		double MeanMs(Id id) const
		{
			const size_t i = static_cast<size_t>(id);
			return count[i] ? sumMs[i] / static_cast<double>(count[i]) : 0.0;
		}
		double MaxMs(Id id) const { return maxMs[static_cast<size_t>(id)]; }
		uint64_t Samples(Id id) const { return count[static_cast<size_t>(id)]; }

	private:
		std::array<double, kSlots> sumMs{};
		std::array<double, kSlots> maxMs{};
		std::array<uint64_t, kSlots> count{};
	};

	struct Projection
	{
		double laptopMs = 0.0;        // sum of slot means on the laptop
		double projectedMs = 0.0;     // sum of slot means x ratio
		int unmeasuredRatios = 0;     // slots that used the fallback ratio
		bool fitsFrame = false;       // projectedMs <= frame budget
	};

	// ratio[i] > 0 = measured 3090/laptop ratio for slot i; 0 = unknown (use fallbackRatio).
	inline Projection Project(const Accumulator& acc, const std::array<double, kSlots>& ratio,
		double fallbackRatio, double frameBudgetMs)
	{
		Projection p;
		for (size_t i = 0; i < kSlots; ++i)
		{
			const Id id = static_cast<Id>(i);
			if (!acc.Samples(id))
				continue;
			const double mean = acc.MeanMs(id);
			const bool measured = ratio[i] > 0.0;
			p.laptopMs += mean;
			p.projectedMs += mean * (measured ? ratio[i] : fallbackRatio);
			if (!measured) ++p.unmeasuredRatios;
		}
		p.fitsFrame = frameBudgetMs > 0.0 && p.projectedMs <= frameBudgetMs;
		return p;
	}
}
