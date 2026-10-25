#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Bug hunt 3 F4 (owner decision 2026-10-18): while hiccup repair
// (interpolation_fill_gaps, default on) is enabled, the motion engine (the
// de-judder's matcher, its textures and the optical-flow engine inside, and the
// producer) stays loaded and idle on a source whose de-judder is not active,
// whatever the reason the hiccup repair can still present a gap pair for (off,
// a video-rate cadence, a mask-only profile on video): no create at a hiccup and
// no release after it. Zero per-frame cost: an idle engine is not run.
//
// Low video memory: the engine is released while a video memory allocation failed
// recently or Windows reports budget pressure (DXGI QueryVideoMemoryInfo: usage at
// kPressureShare of the budget or above), and kept again (recreated at the next gap
// pair) only after kHoldSeconds without either, with room for it under
// kRecoverShare of the budget. Pure, CPU-tested.
namespace MotionEngineIdle
{
	constexpr double kPressureShare = 0.95;
	constexpr double kRecoverShare = 0.85;
	constexpr double kHoldSeconds = 30.0;
	constexpr double kBudgetPollSeconds = 1.0;
	// The idle engine's video memory per source pixel (P010 4:2:0), from the texture sizes:
	// the warped planes, B's planes and the interpolator's picture (3 x 3 bytes), the
	// interpolator's grain bands (interpolation_grain preserve: 4 half-size rgba32f-class
	// textures per plane, 6 bytes), the half-size r16f pyramids (3 x 0.5 bytes, plus the
	// inner instance's at 4K) and the block fields: about 17 bytes. 1080p about 35 MB, 4K
	// about 141 MB, plus the optical-flow engine's own buffers (docs/agent-reports/h3-timing.md).
	constexpr double kIdleBytesPerSourcePixel = 17.0;

	inline uint64_t IdleBytes(int width, int height)
	{
		if (width <= 0 || height <= 0)
			return 0;
		return static_cast<uint64_t>(kIdleBytesPerSourcePixel * static_cast<double>(width) * static_cast<double>(height));
	}

	class Keeper
	{
	public:
		// A video memory allocation for the engine failed (texture creation, B's upload).
		void NoteAllocationFailure(double nowSeconds)
		{
			if (!pressured) ++pressures;
			pressured = true;
			since = nowSeconds;
		}
		// One budget reading. engineBytesIfAbsent: what recreating the engine would add (0
		// while it is held: its bytes are already in the usage).
		void NoteBudget(uint64_t usageBytes, uint64_t budgetBytes, uint64_t engineBytesIfAbsent, double nowSeconds)
		{
			lastPoll = nowSeconds;
			if (budgetBytes == 0)
				return;   // not reported: allocation failures alone decide
			const double budget = static_cast<double>(budgetBytes);
			if (static_cast<double>(usageBytes) >= kPressureShare * budget)
			{
				if (!pressured) ++pressures;
				pressured = true;
				since = nowSeconds;
				return;
			}
			if (pressured && nowSeconds - since >= kHoldSeconds &&
				static_cast<double>(usageBytes) + static_cast<double>(engineBytesIfAbsent) <= kRecoverShare * budget)
				pressured = false;
		}
		// Without budget readings (none reported) a failure's hold still ends.
		void NoteTime(double nowSeconds)
		{
			if (pressured && !budgetSeen && nowSeconds - since >= kHoldSeconds)
				pressured = false;
		}
		void NoteBudgetReported(bool reported) { budgetSeen = reported; }
		bool BudgetDue(double nowSeconds) const { return lastPoll < 0.0 || nowSeconds - lastPoll >= kBudgetPollSeconds; }
		// Keep the idle engine: hiccup repair on, a reason it can repair under, no pressure.
		bool Keep(bool fillGaps, bool repairPossible) const { return fillGaps && repairPossible && !pressured; }
		bool Pressured() const { return pressured; }
		uint64_t Pressures() const { return pressures; }
		void Reset() { *this = Keeper(); }

	private:
		bool pressured = false;
		bool budgetSeen = false;
		double since = 0.0;
		double lastPoll = -1.0;
		uint64_t pressures = 0;
	};
}
