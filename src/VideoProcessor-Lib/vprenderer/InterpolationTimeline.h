#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// VP's per-vsync interpolation timeline (docs/VP-interpolator-50-60-plan.md, Stage B):
// the display refreshes that fall between a source frame A and the next frame B,
// each with its true phase. Pure, so the schedule is unit-tested without a GPU.
//
// The render loop calls Plan once per dequeued source frame. The outputs are the
// refresh times (on the source clock) inside [A, B): none (a source faster than the
// display skips that frame: 60p on 50 Hz), one, or several (50p on 60 Hz), spaced by
// the display period, so the presents consume source frames at the source rate and
// refreshes at the display rate - any ratio, nothing fixed (owner [[vp-general-not-fixed]]).
// The first output of a frame sits at phase 0 .. 1/ratio: A's own refresh is
// interpolated too, not shown at phase 0 by default.
//
// - The source clock is smoothed: a frame's time is the previous one plus whole
//   source periods, pulled 5% towards the measured arrival, re-anchored when it
//   is off by more than a quarter period (capture jitter does not move phases).
// - B's time when known: the span (B - A) in whole source periods, up to
//   kMaxSpanFrames (gap fill: a pair across missed frames spans k + 1 periods);
//   otherwise one period.
// - A discontinuity (a new queue generation, a sequence going back or jumping
//   beyond the span, a rate change, time running backwards or a jump beyond the
//   span) restarts the schedule at A's own time (phase 0); a gap within the span
//   (missed or dropped frames) continues the display's cadence.
// - A pulldown film update (the same film time) gets no refresh of its own.
// - SetRateTrim closes the loop on the queue depth (the display period trimmed).
// - A cadence repeat (the same sequence again) re-presents the last output
//   without advancing.
// - Never waits: the phases are a schedule; a missing B is the renderer's
//   fallback (hold A).
namespace InterpolationTimeline
{
	constexpr int kMaxSpanFrames = 4;       // a pair spans at most this many source periods
	constexpr size_t kMaxOutputs = 16;      // per source frame (as TemporalRenderer's bound)

	// Hiccup repair (interpolation_fill_gaps, owner 2026-10-17): decided independently
	// of motion smoothing. B is offered across a short loss of whole frames (1 to
	// kMaxSpanFrames - 1 missed) when its time is within the span; never across a
	// flush (gapFrames 0) and never on the low-latency path (as before: it never
	// interpolates there).
	inline bool GapFillPair(bool gapBefore, bool fillGaps, uint32_t gapFrames, bool spanOk, bool lowLatency)
	{
		return gapBefore && fillGaps && !lowLatency && gapFrames >= 1 &&
			gapFrames < static_cast<uint32_t>(kMaxSpanFrames) && spanOk;
	}

	// The timeline presents a gap pair by itself (its vectors and phases for that pair
	// only) when interpolation is not presenting this source already.
	inline bool GapFillStandalone(bool gapFillPair, bool interpolating)
	{
		return gapFillPair && !interpolating;
	}

	struct Output
	{
		double pts = 0.0;     // the refresh time on the source clock (seconds)
		double phase = 0.0;   // 0 = A, 1 = B
	};

	class Timeline
	{
	public:
		void Reset()
		{
			initialized = false;
			outputs.clear();
		}

		// generation: the queue generation; sequence: the source sequence; pts: A's time
		// (seconds); nextPts: B's time when B is known (0 = not known); rates in Hz.
		const std::vector<Output>& Plan(uint64_t generation, uint64_t sequence, double pts, double nextPts,
			double sourceHz, double displayHz, bool discontinuity)
		{
			outputs.clear();
			if (!(sourceHz > 0.0) || !(displayHz > 0.0) || !std::isfinite(pts) || !std::isfinite(sourceHz) ||
				!std::isfinite(displayHz))
			{
				// Rates unknown: A at its own time, today's single present.
				Reset();
				outputs.push_back({ pts, 0.0 });
				return outputs;
			}
			const double period = 1.0 / sourceHz, displayPeriod = trim / displayHz;
			if (initialized && !discontinuity && generation == lastGeneration && sequence == lastSequence &&
				sequence != 0)
			{
				// A cadence repeat: the last output again (the refresh it was given to fill).
				++repeats;
				outputs.push_back(lastOutput);
				return outputs;
			}
			if (initialized && !discontinuity && generation == lastGeneration && sequence == lastSequence + 1 &&
				std::abs(pts - lastPts) <= 1e-6 * period)
			{
				// A pulldown film update (option b: the same film frame, updated, at its time):
				// no new time - no refresh of its own; the schedule goes on with the next frame.
				++updates;
				lastSequence = sequence;
				return outputs;
			}
			const bool rateChanged = initialized &&
				(std::abs(sourceHz - lastSourceHz) > (std::max)(0.05, lastSourceHz * 0.001) ||
				 std::abs(displayHz - lastDisplayHz) > (std::max)(0.1, lastDisplayHz * 0.001));
			const bool restart = !initialized || discontinuity || generation != lastGeneration ||
				// A sequence hole within the span (a dropped queued frame) continues the schedule.
				sequence <= lastSequence || sequence - lastSequence > static_cast<uint64_t>(kMaxSpanFrames) ||
				rateChanged || !(pts > lastPts) ||
				pts - lastPts > (kMaxSpanFrames + 0.5) * period;
			if (restart)
			{
				if (initialized) ++restarts;
				clock = pts;
				nextOutput = pts;
				initialized = true;
			}
			else
			{
				const double frames = (std::max)(1.0, std::round((pts - lastPts) / period));
				const double predicted = clock + frames * period;
				const double error = pts - predicted;
				if (std::abs(error) > 0.25 * period)
				{
					clock = pts;
					++reanchors;
				}
				else
					clock = predicted + 0.05 * error;
				// Time the display spent on A while no frame came (a gap without a pair): the
				// schedule keeps the display's cadence and resumes at the first refresh at or
				// after this frame's time.
				if (nextOutput < clock - 1e-9)
					nextOutput += std::ceil((clock - nextOutput) / displayPeriod - 1e-9) * displayPeriod;
			}
			double span = period;
			if (nextPts > pts && std::isfinite(nextPts))
			{
				const double k = std::round((nextPts - pts) / period);
				if (k >= 1.0 && k <= kMaxSpanFrames)
					span = k * period;
			}
			lastSpanFrames = static_cast<int>(std::lround(span / period));
			const double end = clock + span;
			while (nextOutput < end - 1e-9 && outputs.size() < kMaxOutputs)
			{
				const double phase = (std::min)((std::max)((nextOutput - clock) / span, 0.0), std::nextafter(1.0, 0.0));
				outputs.push_back({ nextOutput, phase });
				nextOutput += displayPeriod;
			}
			if (outputs.size() == kMaxOutputs && nextOutput < end - 1e-9)
			{
				// More refreshes per frame than a live budget allows: today's single present.
				Reset();
				outputs.push_back({ pts, 0.0 });
				return outputs;
			}
			if (outputs.empty())
			{
				++skipped;
				lastOutput = { clock, 0.0 };   // a repeat of a skipped frame shows it (phase 0), nothing stale
			}
			else
				lastOutput = outputs.back();
			lastGeneration = generation;
			lastSequence = sequence;
			lastPts = pts;
			lastSourceHz = sourceHz;
			lastDisplayHz = displayHz;
			return outputs;
		}

		// The display-period trim (closed-loop steering from the queue depth): > 1 spaces the
		// refreshes wider, so fewer per source frame and the queue drains; clamped to +-1%.
		void SetRateTrim(double value) { trim = (std::min)(1.01, (std::max)(0.99, value)); }
		double RateTrim() const { return trim; }
		unsigned long long Updates() const { return updates; }
		const std::vector<Output>& Outputs() const { return outputs; }
		bool Initialized() const { return initialized; }
		double Clock() const { return clock; }
		int LastSpanFrames() const { return lastSpanFrames; }
		unsigned long long Restarts() const { return restarts; }
		unsigned long long Reanchors() const { return reanchors; }
		unsigned long long Skipped() const { return skipped; }
		unsigned long long Repeats() const { return repeats; }

	private:
		std::vector<Output> outputs;
		Output lastOutput;
		bool initialized = false;
		uint64_t lastGeneration = 0, lastSequence = 0;
		double lastPts = 0.0, lastSourceHz = 0.0, lastDisplayHz = 0.0;
		double clock = 0.0, nextOutput = 0.0;
		int lastSpanFrames = 1;
		unsigned long long restarts = 0, reanchors = 0, skipped = 0, repeats = 0, updates = 0;
		double trim = 1.0;
	};
}
