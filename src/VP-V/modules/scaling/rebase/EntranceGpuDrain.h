#pragma once

// entrance_gpu_drain (slot scaling.rebase; expert, default off; bug hunt 2026-10-25, root cause open).
//
// With the entrance stages' GPU work still in flight when libplacebo's next render starts, its
// scaling passes returned +-1 of 16 bits on scattered textured pixels (the polar re-base and the
// render's scalers), so pictures between frames differed from the plain draw of the same source
// frame. Draining the GPU after the entrance cured it on the laptop (24/24 captures; a flush 22/24).
//
// The host notes every entrance run (NoteEntranceRun) and calls BeforeRender immediately before each
// libplacebo render that reads the entrance's output (the main present, its crossfade, each extra).
// One drain follows the last entrance run: BeforeRender consumes the note, so a second call before
// the next entrance run does nothing. Render thread only (no locking).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace EntranceGpuDrain
{
	enum class Mode { Off, Flush, Finish };

	// "off" | "flush" | "finish" (normalised, lower case); false for anything else.
	inline bool Parse(const std::string& value, Mode& mode)
	{
		if (value == "off") { mode = Mode::Off; return true; }
		if (value == "flush") { mode = Mode::Flush; return true; }
		if (value == "finish") { mode = Mode::Finish; return true; }
		return false;
	}

	inline const char* Name(Mode mode)
	{
		return mode == Mode::Finish ? "finish" : mode == Mode::Flush ? "flush" : "off";
	}

	// The visible status text while the drain is on (the host lists it under "engaged").
	inline std::string StatusReason(Mode mode)
	{
		return mode == Mode::Finish ?
			"finish: the GPU is drained after the entrance stages (expert workaround: pictures between frames exact)" :
			"flush: the GPU queue is flushed after the entrance stages (expert workaround)";
	}

	class Drain
	{
	public:
		using Clock = std::chrono::steady_clock;

		void NoteEntranceRun() { ran = true; }

		// Before a render that reads the entrance's output. `mode` is the current setting, `slotRuns`
		// whether scaling.rebase runs (the slot registry; false = nothing). flush / finish issue the
		// GPU calls. A mode change starts the statistics over. Every ~10 s with >= 20 samples,
		// `summary` receives the CPU wait p50 / p99 / max (empty otherwise). True when it drained.
		bool BeforeRender(Mode mode, bool slotRuns, const std::function<void()>& flush,
			const std::function<void()>& finish, std::string* summary = nullptr)
		{
			if (summary) summary->clear();
			if (mode != lastMode)
			{
				lastMode = mode;
				samples.clear();
				summaryAt = Clock::now();
			}
			if (!ran)
				return false;
			ran = false;
			if (mode == Mode::Off || !slotRuns)
				return false;
			const Clock::time_point start = Clock::now();
			if (mode == Mode::Finish) { if (finish) finish(); }
			else if (flush) flush();
			const Clock::time_point now = Clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(now - start).count());
			if (summary && now - summaryAt >= std::chrono::seconds(10) && samples.size() >= 20)
			{
				*summary = Summarise(mode);
				samples.clear();
				summaryAt = now;
			}
			return true;
		}

		size_t Samples() const { return samples.size(); }

	private:
		std::string Summarise(Mode mode) const
		{
			std::vector<double> sorted = samples;
			std::sort(sorted.begin(), sorted.end());
			const auto at = [&](double q) { return sorted[static_cast<size_t>(q * static_cast<double>(sorted.size() - 1))]; };
			char text[200];
			std::snprintf(text, sizeof(text), "Entrance GPU drain (%s): CPU wait p50 %.3f ms, p99 %.3f ms, max %.3f ms over %zu renders",
				Name(mode), at(0.5), at(0.99), sorted.back(), sorted.size());
			return text;
		}

		bool ran = false;
		Mode lastMode = Mode::Off;
		std::vector<double> samples;   // ms per drain since the last summary (or mode change)
		Clock::time_point summaryAt = Clock::now();
	};
}
