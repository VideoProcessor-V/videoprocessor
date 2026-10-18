#pragma once

// VP Renderer (file sources): graph reference clock -> QueryPerformanceCounter.
// Pure, header-only, graph-free; C++14.
//
// The filter feeds correlation pairs (reference clock time, QPC tick, QPC
// frequency) taken periodically from IReferenceClock::GetTime and
// QueryPerformanceCounter. The mapper keeps a smoothed linear fit (least
// squares over the most recent pairs, slope limited to a drift bound) so the
// reference clock's drift against QPC is followed. Mapping:
//   presentation reference time = tStart + rtStart / rate   (rate > 0)
//   presentationQpc = fit(presentation reference time + av offset)
// tStart is passed with every call: pause freezes stream time and Run()
// supplies a new tStart, so the mapper itself holds no graph state.

#include <dshow.h>

#include <cstdint>
#include <cmath>
#include <deque>
#include <limits>

namespace filesource
{
	struct ClockCorrelation
	{
		int64_t referenceTime100ns = 0; // IReferenceClock::GetTime
		int64_t qpcTicks = 0;           // QueryPerformanceCounter
		int64_t qpcFrequency = 0;       // QueryPerformanceFrequency (ticks per second)

		bool IsValid() const { return qpcFrequency > 0 && qpcTicks > 0; }
	};

	struct ClockMapRequest
	{
		int64_t streamStartTime100ns = 0; // tStart from IMediaFilter::Run
		int64_t sampleStart100ns = 0;     // rtStart (segment relative)
		double playbackRate = 1.0;        // NewSegment rate; must be > 0
		int32_t avOffsetMs = 0;           // file_av_offset_ms (signed; + = later)
		// DirectShow sources normally rate-adjust their timestamps themselves; set
		// true when rtStart is already in stream time so it is not divided again.
		bool sampleTimesAlreadyRateScaled = false;
	};

	struct ClockMapResult
	{
		bool ok = false;
		int64_t referenceTime100ns = 0; // tStart + rtStart / rate + offset
		int64_t presentationQpc = 0;    // 0 when !ok (FileFrameTiming: unknown)
	};

	namespace detail
	{
		// Saturating conversion of a double to int64.
		inline bool DoubleToInt64(double v, int64_t& out)
		{
			if (!(v == v)) // NaN
				return false;
			// 2^63 as double; anything >= cannot be represented.
			const double limit = 9223372036854775808.0;
			if (v >= limit || v < -limit)
				return false;
			out = (int64_t)std::llround(v);
			return true;
		}
	}

	class FileClockMapper
	{
	public:
		// window: correlations kept for the fit. maxDriftPpm bounds the fitted
		// slope around the nominal QPC/100 ns ratio. jumpToleranceMs: a new pair
		// further than this from the current fit restarts the fit (clock change
		// or discontinuity).
		explicit FileClockMapper(size_t window = 16, double maxDriftPpm = 1000.0,
			double jumpToleranceMs = 5.0) :
			m_window(window < 1 ? 1 : window),
			m_maxDriftPpm(maxDriftPpm < 0.0 ? 0.0 : maxDriftPpm),
			m_jumpToleranceMs(jumpToleranceMs <= 0.0 ? 5.0 : jumpToleranceMs)
		{
		}

		void Reset()
		{
			m_pairs.clear();
			m_fitValid = false;
		}

		size_t CorrelationCount() const { return m_pairs.size(); }
		bool HasFit() const { return m_fitValid; }

		// Fitted QPC ticks per 100 ns reference unit (nominal = frequency / 1e7).
		double SlopeQpcPer100ns() const { return m_slope; }
		// Fitted drift in parts per million relative to nominal.
		double DriftPpm() const
		{
			if (!m_fitValid || m_frequency <= 0)
				return 0.0;
			const double nominal = (double)m_frequency / 1.0e7;
			return (m_slope / nominal - 1.0) * 1.0e6;
		}

		// Add a correlation. Returns false (and ignores it) when invalid.
		bool AddCorrelation(const ClockCorrelation& c)
		{
			if (!c.IsValid())
				return false;
			if (!m_pairs.empty())
			{
				const ClockCorrelation& last = m_pairs.back();
				const bool frequencyChanged = c.qpcFrequency != last.qpcFrequency;
				const bool backwards = c.referenceTime100ns <= last.referenceTime100ns ||
					c.qpcTicks <= last.qpcTicks;
				bool jumped = false;
				if (!frequencyChanged && !backwards && m_fitValid)
				{
					int64_t predicted = 0;
					if (MapReferenceToQpc(c.referenceTime100ns, predicted))
					{
						const double errorMs = std::fabs((double)(c.qpcTicks - predicted)) * 1000.0 /
							(double)c.qpcFrequency;
						jumped = errorMs > m_jumpToleranceMs;
					}
				}
				if (frequencyChanged || backwards || jumped)
					m_pairs.clear();
			}
			m_pairs.push_back(c);
			while (m_pairs.size() > m_window)
				m_pairs.pop_front();
			Refit();
			return true;
		}

		// Reference time (100 ns) -> QPC tick using the fit.
		bool MapReferenceToQpc(int64_t referenceTime100ns, int64_t& qpcOut) const
		{
			if (!m_fitValid)
				return false;
			const double dx = (double)referenceTime100ns - m_meanRef;
			const double q = m_meanQpc + m_slope * dx;
			if (!detail::DoubleToInt64(q, qpcOut))
				return false;
			return qpcOut > 0;
		}

		// presentationQpc = fit(tStart + rtStart / rate + avOffset).
		ClockMapResult Map(const ClockMapRequest& r) const
		{
			ClockMapResult result;
			if (!m_fitValid)
				return result;
			if (!(r.playbackRate > 0.0) || !std::isfinite(r.playbackRate))
				return result;
			const double streamOffset = r.sampleTimesAlreadyRateScaled
				? (double)r.sampleStart100ns
				: (double)r.sampleStart100ns / r.playbackRate;
			const double refTime = (double)r.streamStartTime100ns + streamOffset +
				(double)r.avOffsetMs * 10000.0;
			int64_t ref = 0;
			if (!detail::DoubleToInt64(refTime, ref))
				return result;
			int64_t qpc = 0;
			if (!MapReferenceToQpc(ref, qpc))
				return result;
			result.ok = true;
			result.referenceTime100ns = ref;
			result.presentationQpc = qpc;
			return result;
		}

	private:
		void Refit()
		{
			m_fitValid = false;
			if (m_pairs.empty())
				return;
			m_frequency = m_pairs.back().qpcFrequency;
			const double nominal = (double)m_frequency / 1.0e7;
			// Work relative to the newest pair to keep doubles exact.
			const ClockCorrelation& base = m_pairs.back();
			const size_t n = m_pairs.size();
			double sx = 0.0, sy = 0.0;
			for (const ClockCorrelation& c : m_pairs)
			{
				sx += (double)(c.referenceTime100ns - base.referenceTime100ns);
				sy += (double)(c.qpcTicks - base.qpcTicks);
			}
			const double mx = sx / (double)n;
			const double my = sy / (double)n;
			double sxx = 0.0, sxy = 0.0;
			for (const ClockCorrelation& c : m_pairs)
			{
				const double dx = (double)(c.referenceTime100ns - base.referenceTime100ns) - mx;
				const double dy = (double)(c.qpcTicks - base.qpcTicks) - my;
				sxx += dx * dx;
				sxy += dx * dy;
			}
			double slope = nominal;
			if (n >= 2 && sxx > 0.0)
				slope = sxy / sxx;
			const double bound = nominal * m_maxDriftPpm * 1.0e-6;
			if (slope > nominal + bound) slope = nominal + bound;
			if (slope < nominal - bound) slope = nominal - bound;
			m_slope = slope;
			m_meanRef = (double)base.referenceTime100ns + mx;
			m_meanQpc = (double)base.qpcTicks + my;
			m_fitValid = true;
		}

		size_t m_window;
		double m_maxDriftPpm;
		double m_jumpToleranceMs;
		std::deque<ClockCorrelation> m_pairs;
		bool m_fitValid = false;
		int64_t m_frequency = 0;
		double m_slope = 0.0;
		double m_meanRef = 0.0;
		double m_meanQpc = 0.0;
	};

	// Adapter for the filter: one correlation from a clock exposing
	// HRESULT GetTime(REFERENCE_TIME*) (IReferenceClock) and QPC readers with
	// the signature bool(int64_t*). QPC is read before and after GetTime and the
	// midpoint used, which halves the uncertainty from the call latency. A
	// template so tests can pass a fake clock and fake counters.
	template <class ClockT, class QpcFnT, class QpcFreqFnT>
	inline bool SampleClockCorrelation(ClockT* clock, QpcFnT readQpc, QpcFreqFnT readQpcFrequency,
		ClockCorrelation& out)
	{
		if (clock == nullptr)
			return false;
		int64_t frequency = 0;
		if (!readQpcFrequency(&frequency) || frequency <= 0)
			return false;
		int64_t before = 0, after = 0;
		if (!readQpc(&before))
			return false;
		REFERENCE_TIME referenceTime = 0;
		const HRESULT hr = clock->GetTime(&referenceTime);
		if (!readQpc(&after))
			return false;
		if (FAILED(hr) || after < before)
			return false;
		out.referenceTime100ns = (int64_t)referenceTime;
		out.qpcTicks = before + (after - before) / 2;
		out.qpcFrequency = frequency;
		return out.IsValid();
	}

	inline bool ReadQpcTicks(int64_t* ticks)
	{
		LARGE_INTEGER value;
		if (!QueryPerformanceCounter(&value))
			return false;
		*ticks = (int64_t)value.QuadPart;
		return true;
	}

	inline bool ReadQpcFrequency(int64_t* frequency)
	{
		LARGE_INTEGER value;
		if (!QueryPerformanceFrequency(&value))
			return false;
		*frequency = (int64_t)value.QuadPart;
		return true;
	}

	// What the filter calls: IReferenceClock::GetTime + QueryPerformanceCounter.
	inline bool SampleReferenceClockCorrelation(IReferenceClock* clock, ClockCorrelation& out)
	{
		return SampleClockCorrelation(clock, &ReadQpcTicks, &ReadQpcFrequency, out);
	}
}
