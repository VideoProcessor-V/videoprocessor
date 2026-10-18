#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Fixed-constant batches B3-B7 (2026-10-18; owner rule [[vp-general-not-fixed]]: never hard-code
// refresh rates, frame rates, cadences or resolutions - derive them from the measured rates).
// docs/VP-fixed-constants-inventory-2026-10-18.md, docs/agent-reports/no-fixed-constants-2.md.
//
// The shared rules every converted site uses, so the conversions agree:
//
//  - A count that was tuned in FRAMES keeps its value at and below 60 Hz and holds the same TIME
//    above it (count / 60 s): FramesWithFloor. The frame count is the floor, the time it lasts at
//    60 Hz the target. So 23.976 / 24 / 25 / 29.97 / 30 / 50 / 59.94 / 60 are unchanged (the
//    owner's film and video setups), and 100 / 120 / 144 / 240 Hz keep the time instead of a
//    window a quarter as long. 60 Hz is the highest standard rate the counts were tuned at, not
//    a rate the code relies on: no list, no band.
//  - A per-frame one-pole weight follows the same rule (PerFrameWeight).
//  - A distance in pixels that was tuned on pictures of up to 2160 lines keeps its value at and
//    below 2160 lines and grows with the height above it (GeneralScale::AboveReference) - the rule
//    GrainPreserve::GrainScale already used. 1080 and 2160 are unchanged.
//  - An unmeasured-rate fallback is logged once each time it starts being used (FallbackNotice).
//
// Behaviour changes are switched by general_rate_fixes (renderer profile, default on). Code with
// no renderer profile (the DirectShow pin, the GUI queue) reads the process-wide copy the GUI
// sets from the resolved profile (ProcessEnabled).
namespace GeneralRate
{
	// The rate up to which a frame-count tuning is used as it stands (see above).
	constexpr double kFrameCountReferenceHz = 60.0;
	// Relative measurement tolerance: a period quantised to whole microseconds (16666 us reads
	// 60.002 Hz) or a measured rate a hair above a standard one does not add a frame.
	constexpr double kRateTolerance = 1e-3;

	inline bool Measured(double rateHz) { return std::isfinite(rateHz) && rateHz > 0.0; }

	// The legacy frame count at and below kFrameCountReferenceHz; above it, whole frames covering
	// the same time (rounded up, so the window is never shorter). Off, an unknown rate or a count
	// of 0: the count as given.
	inline long long FramesWithFloor(long long legacyFrames, double rateHz, bool general = true)
	{
		if (!general || legacyFrames <= 0 || !Measured(rateHz) || rateHz <= kFrameCountReferenceHz * (1.0 + kRateTolerance))
			return legacyFrames;
		// The tolerance: 4 x 120.005 / 60 (an 8333 us period) is 8.0002 -> 8, not 9.
		const double exact = static_cast<double>(legacyFrames) * rateHz / kFrameCountReferenceHz;
		const double timed = std::ceil(exact * (1.0 - kRateTolerance));
		return (std::max)(legacyFrames, static_cast<long long>((std::min)(timed, 1.0e9)));
	}
	inline int FramesWithFloor(int legacyFrames, double rateHz, bool general = true)
	{
		return static_cast<int>(FramesWithFloor(static_cast<long long>(legacyFrames), rateHz, general));
	}
	inline size_t FramesWithFloor(size_t legacyFrames, double rateHz, bool general = true)
	{
		return static_cast<size_t>(FramesWithFloor(static_cast<long long>(legacyFrames), rateHz, general));
	}
	inline uint32_t FramesWithFloor(uint32_t legacyFrames, double rateHz, bool general = true)
	{
		return static_cast<uint32_t>((std::min)(FramesWithFloor(static_cast<long long>(legacyFrames), rateHz, general),
			static_cast<long long>(UINT32_MAX)));
	}

	// The time a frame count stands for (its length at kFrameCountReferenceHz).
	inline double SecondsOf(double legacyFrames) { return legacyFrames / kFrameCountReferenceHz; }

	// A one-pole weight given per frame (this frame's share): unchanged at and below
	// kFrameCountReferenceHz; above it the weight per frame that gives the same time constant.
	inline double PerFrameWeight(double alpha, double rateHz, bool general = true)
	{
		if (!general || !(alpha > 0.0 && alpha < 1.0) || !Measured(rateHz) || rateHz <= kFrameCountReferenceHz * (1.0 + kRateTolerance))
			return alpha;
		return 1.0 - std::pow(1.0 - alpha, kFrameCountReferenceHz / rateHz);
	}

	// The process-wide copy of general_rate_fixes for code with no renderer profile (the
	// DirectShow pin, the GUI's queue target). The GUI sets it from each resolved profile
	// snapshot; on until then (the default). Per module: the VP renderer DLL reads its own
	// profile setting instead.
	inline std::atomic<bool>& ProcessFlag()
	{
		static std::atomic<bool> flag{ true };
		return flag;
	}
	inline void SetProcessEnabled(bool on) { ProcessFlag().store(on, std::memory_order_release); }
	inline bool ProcessEnabled() { return ProcessFlag().load(std::memory_order_acquire); }

	// A log throttle in time: frame-count throttles (`count % 120 == 0`) converted to the time the
	// count lasted at 60 Hz (SecondsOf). The first call is due.
	class LogInterval
	{
	public:
		using Clock = std::chrono::steady_clock;
		explicit LogInterval(double seconds) : interval(std::chrono::duration_cast<Clock::duration>(
			std::chrono::duration<double>((std::max)(0.0, seconds)))) {}
		bool Due(Clock::time_point now = Clock::now())
		{
			if (started && now - last < interval)
				return false;
			started = true;
			last = now;
			return true;
		}
		// `force` (the first event, a change): due now, and the interval restarts from here.
		bool DueOr(bool force, Clock::time_point now = Clock::now())
		{
			if (!force)
				return Due(now);
			started = true;
			last = now;
			return true;
		}
		static LogInterval ForFrames(double legacyFrames) { return LogInterval(SecondsOf(legacyFrames)); }

	private:
		Clock::duration interval;
		Clock::time_point last{};
		bool started = false;
	};

	// An unmeasured-rate fallback: Use(measured) is true when a log line is due - the first use of
	// the fallback, and again after the rate was measured in between. Thread-safe.
	class FallbackNotice
	{
	public:
		bool Use(bool measured)
		{
			if (measured)
			{
				inFallback.store(false, std::memory_order_relaxed);
				return false;
			}
			if (inFallback.exchange(true, std::memory_order_relaxed))
				return false;
			uses.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		uint64_t Uses() const { return uses.load(std::memory_order_relaxed); }

	private:
		std::atomic<bool> inFallback{ false };
		std::atomic<uint64_t> uses{ 0 };
	};
}

namespace GeneralScale
{
	// The tallest picture the pixel-valued tunings were made on (the owner's 4K setup; owner
	// 2026-10-14: distances as a share of height, reference 2160).
	constexpr double kReferenceLines = 2160.0;

	// 1 at and below kReferenceLines (1080 and 2160 unchanged: a 1080-line picture's grain or
	// vector tolerance is not half a pixel), lines / kReferenceLines above it. Off or unknown: 1.
	inline double AboveReference(double lines, bool general = true)
	{
		return general && std::isfinite(lines) && lines > kReferenceLines ? lines / kReferenceLines : 1.0;
	}
	// A whole-pixel size scaled the same way (rounded, never below the size given).
	inline int AboveReferencePixels(int pixels, double lines, bool general = true)
	{
		const double scaled = pixels * AboveReference(lines, general);
		return (std::max)(pixels, static_cast<int>(std::lround(scaled)));
	}
}
