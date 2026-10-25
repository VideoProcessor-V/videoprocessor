#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// Efficiency timer plan (audit 2026-10-24, section 4). Pure and header-only (no Windows, no
// GPU): the renderer feeds measurements, this keeps the statistics and formats the readout for
// Ctrl+Alt+D and the diagnostic bundle. Nothing here waits on the GPU: every GPU number fed in
// is an already-completed timer result (libplacebo `pass->last`, a polled pl_timer, an
// AlphaD3D11FrameGpuTimer range read back frames later).
//
//   1. Per-pass GPU time by libplacebo pass description, sampled 1 frame in N (Sampler).
//   2. Named GPU groups the per-pass callback cannot see (entrance S1 per stage, DejudderWarp
//      matcher / NVOF consume / warp, upload unpack) - polled timers, never drained.
//   3. CPU splits (QPC): upload CPU copy / early-slot wait; RenderLocked analysis / hook assembly /
//      submit, with WAITS (vblank, present, fences) kept in their own lines so they are never
//      counted as work.
namespace PassTimers
{
	// 1 frame in N is timed (N >= 1). Deterministic: frame indices divisible by N.
	class Sampler
	{
	public:
		explicit Sampler(uint32_t everyN = 8) : n(everyN ? everyN : 1) {}
		bool Take(uint64_t frameIndex) const { return frameIndex % n == 0; }
		uint32_t EveryN() const { return n; }
		void SetEveryN(uint32_t everyN) { n = everyN ? everyN : 1; }

	private:
		uint32_t n;
	};

	enum class Kind { GpuPass, GpuGroup, CpuWork, CpuWait };

	struct Stat
	{
		Kind kind = Kind::GpuPass;
		uint64_t samples = 0;
		double sumMs = 0.0;
		double maxMs = 0.0;
		double lastMs = 0.0;
		double MeanMs() const { return samples ? sumMs / static_cast<double>(samples) : 0.0; }
	};

	// Keyed by name; a libplacebo pass description is the key for Kind::GpuPass.
	class Accumulator
	{
	public:
		// A zero or negative GPU result means "no completed measurement yet" (libplacebo reports
		// 0 until a pass's timer has been read back): not a sample.
		void Add(Kind kind, const std::string& name, double ms)
		{
			if (ms <= 0.0 && (kind == Kind::GpuPass || kind == Kind::GpuGroup))
				return;
			if (ms < 0.0)
				ms = 0.0;
			Stat& s = stats[name];
			s.kind = kind;
			++s.samples;
			s.sumMs += ms;
			s.lastMs = ms;
			if (ms > s.maxMs)
				s.maxMs = ms;
		}
		void AddGpuPassNs(const char* description, uint64_t ns)
		{
			Add(Kind::GpuPass, description && *description ? description : "(unnamed pass)", static_cast<double>(ns) * 1e-6);
		}
		const Stat* Find(const std::string& name) const
		{
			const auto it = stats.find(name);
			return it == stats.end() ? nullptr : &it->second;
		}
		void Clear() { stats.clear(); }
		bool Empty() const { return stats.empty(); }

		// Mean work per timed frame of a kind (sum of the means): the GPU or CPU cost line.
		double TotalMeanMs(Kind kind) const
		{
			double total = 0.0;
			for (const auto& kv : stats)
				if (kv.second.kind == kind)
					total += kv.second.MeanMs();
			return total;
		}

		// The readout: one heading, then up to `maxLines` lines per kind, largest mean first.
		//   Timers (GPU passes sampled 1 in 8; waits excluded from work):
		//   GPU pass  1.234 ms avg  2.000 max  (120)  VP extended range: clipped picture + remainder
		std::string Readout(uint32_t everyN, size_t maxLines = 12) const
		{
			char line[256];
			std::snprintf(line, sizeof(line), "Timers (GPU passes sampled 1 in %u; waits excluded from work):\n", everyN ? everyN : 1);
			std::string text = line;
			if (stats.empty())
				return text + "  no samples yet\n";
			static const Kind kinds[] = { Kind::GpuPass, Kind::GpuGroup, Kind::CpuWork, Kind::CpuWait };
			static const char* const labels[] = { "GPU pass", "GPU group", "CPU work", "CPU wait" };
			for (size_t k = 0; k < 4; ++k)
			{
				std::vector<std::pair<std::string, Stat>> rows;
				for (const auto& kv : stats)
					if (kv.second.kind == kinds[k])
						rows.push_back(kv);
				if (rows.empty())
					continue;
				std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.MeanMs() > b.second.MeanMs(); });
				std::snprintf(line, sizeof(line), "  %s total %.3f ms avg per timed frame\n", labels[k], TotalMeanMs(kinds[k]));
				text += line;
				for (size_t i = 0; i < rows.size() && i < maxLines; ++i)
				{
					std::snprintf(line, sizeof(line), "    %7.3f ms avg %7.3f max (%llu)  %s\n", rows[i].second.MeanMs(),
						rows[i].second.maxMs, static_cast<unsigned long long>(rows[i].second.samples), rows[i].first.c_str());
					text += line;
				}
				if (rows.size() > maxLines)
				{
					std::snprintf(line, sizeof(line), "    ... %zu more\n", rows.size() - maxLines);
					text += line;
				}
			}
			return text;
		}

	private:
		std::map<std::string, Stat> stats;
	};

	// A CPU split of one function: Mark(name, kind) closes the interval since the previous mark.
	// The clock is injected (QPC ticks -> ms in the renderer; any monotonic value in tests).
	class CpuSplit
	{
	public:
		void Begin(double nowMs) { last = nowMs; open = true; }
		void Mark(Accumulator& acc, const char* name, Kind kind, double nowMs)
		{
			if (!open)
				return;
			acc.Add(kind, name, nowMs - last);
			last = nowMs;
		}
		void End() { open = false; }

	private:
		double last = 0.0;
		bool open = false;
	};
}
