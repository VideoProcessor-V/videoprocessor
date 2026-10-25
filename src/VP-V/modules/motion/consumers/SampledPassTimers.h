#pragma once

// Motion GPU timers, SAMPLED and NEVER DRAINED (VP-only, 2026-10-24 draft; efficiency audit
// section 4 item 3).
//
// MotionWarp's pass timing (DejudderWarp.cpp TimedDispatch/FlushPassTimers) is tests-only because
// it drains the GPU: pl_gpu_finish before and after every pass, and FlushPassTimers finishes once
// more. That turns a pipelined GPU into a serial one for the frame (a full CPU<->GPU round trip per
// pass, ~40 passes per estimate) - so production has NO motion timing at all.
//
// This keeps libplacebo's asynchronous pl_timer per pass description, arms the timers on one frame
// in `every` (the others pay nothing), and only POLLS results (pl_timer_query returns 0 until the
// GPU has finished that pass, so results arrive a few frames later). It never calls pl_gpu_finish.
//
// Generic over the timer handle and its operations so the CPU tests drive it with a fake GPU:
//   Ops::Create() -> Timer, Ops::Query(Timer) -> uint64_t ns (0 = not ready; may be called again),
//   Ops::Destroy(Timer&).

#include <cstdint>
#include <map>
#include <string>

namespace VpMotion
{
	template <class Timer, class Ops>
	class SampledPassTimers
	{
	public:
		explicit SampledPassTimers(Ops ops = Ops(), unsigned every = 30) : m_ops(ops), m_every(every ? every : 1) {}
		~SampledPassTimers() { Release(); }
		SampledPassTimers(const SampledPassTimers&) = delete;
		SampledPassTimers& operator=(const SampledPassTimers&) = delete;

		void SetEvery(unsigned every) { m_every = every ? every : 1; }

		// Once per estimate/frame: decides whether this frame's passes are timed.
		bool BeginFrame()
		{
			m_armed = (m_frame++ % m_every) == 0;
			if (m_armed)
				++m_sampledFrames;
			return m_armed;
		}
		bool Armed() const { return m_armed; }

		// The timer to attach to this pass (compute.timer), or a null Timer when not armed.
		Timer TimerFor(const std::string& description)
		{
			if (!m_armed)
				return Timer{};
			Entry& e = m_entries[description];
			if (!e.timer)
				e.timer = m_ops.Create();
			++e.issued;
			return e.timer;
		}

		// Non-blocking: collect whatever results are ready (call once per frame, any time).
		void Poll()
		{
			for (auto& kv : m_entries)
			{
				Entry& e = kv.second;
				if (!e.timer)
					continue;
				for (uint64_t ns = m_ops.Query(e.timer); ns; ns = m_ops.Query(e.timer))
				{
					e.totalNs += ns;
					++e.samples;
				}
			}
		}

		// Mean GPU ms of a pass over the samples so far (0 when none).
		double MeanMs(const std::string& description) const
		{
			const auto it = m_entries.find(description);
			return it == m_entries.end() || it->second.samples == 0 ? 0.0 :
				static_cast<double>(it->second.totalNs) / 1e6 / static_cast<double>(it->second.samples);
		}
		uint64_t Samples(const std::string& description) const
		{
			const auto it = m_entries.find(description);
			return it == m_entries.end() ? 0 : it->second.samples;
		}
		uint64_t SampledFrames() const { return m_sampledFrames; }

		void Release()
		{
			for (auto& kv : m_entries)
				if (kv.second.timer)
					m_ops.Destroy(kv.second.timer);
			m_entries.clear();
		}

	private:
		struct Entry
		{
			Timer timer{};
			uint64_t issued = 0, samples = 0, totalNs = 0;
		};
		Ops m_ops;
		unsigned m_every;
		uint64_t m_frame = 0, m_sampledFrames = 0;
		bool m_armed = false;
		std::map<std::string, Entry> m_entries;
	};
}
