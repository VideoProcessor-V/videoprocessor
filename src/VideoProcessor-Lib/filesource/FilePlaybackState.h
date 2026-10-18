#pragma once

// VP Renderer (file sources): stop/pause/run/flush/seek/step/rate state
// machine that decides FileFrameTiming.discontinuity, .paused and
// .playbackRate, and fills a FileFrameTiming. Pure, header-only, graph-free;
// C++14. The filter forwards its DirectShow callbacks here.
//
// Discontinuity rules: the first sample delivered after any of
//   - leaving Stopped (a fresh start),
//   - EndFlush (flush / seek),
//   - NewSegment (seek or rate change; any new segment),
//   - a playback rate change,
//   - a frame step,
//   - a sample carrying the DirectShow discontinuity flag
// is marked discontinuous; the flag is consumed by that sample.

#include <FileFrameTiming.h>
#include "FileClockMapper.h"
#include "RationalRate.h"

#include <cstdint>
#include <cmath>

namespace filesource
{
	enum class FileGraphState { Stopped, Paused, Running };

	// Motion (VP motion engine / smooth motion) only at normal speed.
	inline bool MotionAllowedAtRate(double playbackRate)
	{
		return std::fabs(playbackRate - 1.0) < 1e-9;
	}

	struct FileSampleTimes
	{
		bool hasStart = false;          // IMediaSample::GetTime returned start
		bool hasStop = false;           // ... and stop (VFW_S_NO_STOP_TIME -> false)
		int64_t start100ns = 0;
		int64_t stop100ns = 0;
		bool discontinuityFlag = false; // IMediaSample::IsDiscontinuity() == S_OK
	};

	class FilePlaybackState
	{
	public:
		FileGraphState State() const { return m_state; }
		bool IsFlushing() const { return m_flushing; }
		double PlaybackRate() const { return m_rate; }
		int64_t StreamStartTime() const { return m_tStart; }
		bool DiscontinuityPending() const { return m_discontinuityPending; }
		bool StepPending() const { return m_stepPending; }

		// Samples arriving while flushing (or stopped) are dropped by the filter.
		bool AcceptsSamples() const { return !m_flushing && m_state != FileGraphState::Stopped; }

		// Motion only while running at rate 1 (not paused, not stepping).
		bool MotionAllowed() const
		{
			return m_state == FileGraphState::Running && !m_stepPending && MotionAllowedAtRate(m_rate);
		}

		// IMediaFilter::Stop
		void Stop()
		{
			m_state = FileGraphState::Stopped;
			m_flushing = false;
			m_stepPending = false;
			m_discontinuityPending = true;
		}

		// IMediaFilter::Pause (from Stopped: pre-roll; from Running: hold).
		void Pause()
		{
			if (m_state == FileGraphState::Stopped)
				m_discontinuityPending = true;
			m_state = FileGraphState::Paused;
		}

		// IMediaFilter::Run(tStart). A resume after pause supplies a new tStart;
		// stream time is continuous so no discontinuity unless coming from Stop.
		void Run(int64_t tStart)
		{
			if (m_state == FileGraphState::Stopped)
				m_discontinuityPending = true;
			m_state = FileGraphState::Running;
			m_tStart = tStart;
			m_stepPending = false;
		}

		void BeginFlush() { m_flushing = true; }

		void EndFlush()
		{
			m_flushing = false;
			m_discontinuityPending = true;
		}

		// IPin::NewSegment. Returns true when the rate changed.
		bool NewSegment(int64_t segmentStart, int64_t segmentStop, double rate)
		{
			m_segmentStart = segmentStart;
			m_segmentStop = segmentStop;
			bool rateChanged = false;
			if (rate > 0.0 && std::isfinite(rate))
			{
				rateChanged = std::fabs(rate - m_rate) > 1e-12;
				m_rate = rate;
			}
			m_discontinuityPending = true;
			return rateChanged;
		}

		// Rate change outside NewSegment (e.g. a quality / rate property).
		void SetRate(double rate)
		{
			if (rate > 0.0 && std::isfinite(rate) && std::fabs(rate - m_rate) > 1e-12)
			{
				m_rate = rate;
				m_discontinuityPending = true;
			}
		}

		// IAMFrameStep::Step: the next frame is shown and held (paused).
		void FrameStep()
		{
			m_stepPending = true;
			m_discontinuityPending = true;
		}

		int64_t SegmentStart() const { return m_segmentStart; }
		int64_t SegmentStop() const { return m_segmentStop; }

		// Build the timing for the next accepted sample and consume the pending
		// discontinuity. presentationQpc is filled only while running (not for a
		// held / pre-roll / stepped picture) and only when the mapper has a fit.
		FileFrameTiming BuildTiming(const FileSampleTimes& sample, const RationalRate& sourceRate,
			double measuredRateHz, const FileClockMapper& mapper, int32_t avOffsetMs,
			bool sampleTimesAlreadyRateScaled = false)
		{
			FileFrameTiming t;
			t.structSize = sizeof(FileFrameTiming);
			t.isFileSource = true;
			t.playbackRate = m_rate;
			t.sampleStart100ns = sample.hasStart ? sample.start100ns : 0;
			t.sampleStop100ns = sample.hasStop ? sample.stop100ns : 0;
			if (sourceRate.IsValid())
			{
				t.sourceRateNumerator = sourceRate.numerator;
				t.sourceRateDenominator = sourceRate.denominator;
				t.sourceRateHz = sourceRate.Hz();
			}
			else
			{
				t.sourceRateNumerator = 0;
				t.sourceRateDenominator = 0;
				t.sourceRateHz = measuredRateHz > 0.0 ? measuredRateHz : 0.0;
			}

			t.discontinuity = m_discontinuityPending || sample.discontinuityFlag;
			m_discontinuityPending = false;

			const bool held = m_state != FileGraphState::Running || m_stepPending;
			t.paused = held;
			if (m_stepPending)
				m_stepPending = false; // the stepped frame is this one

			t.presentationQpc = 0;
			if (!held && sample.hasStart)
			{
				ClockMapRequest request;
				request.streamStartTime100ns = m_tStart;
				request.sampleStart100ns = sample.start100ns;
				request.playbackRate = m_rate;
				request.avOffsetMs = avOffsetMs;
				request.sampleTimesAlreadyRateScaled = sampleTimesAlreadyRateScaled;
				const ClockMapResult mapped = mapper.Map(request);
				if (mapped.ok)
					t.presentationQpc = mapped.presentationQpc;
			}
			return t;
		}

	private:
		FileGraphState m_state = FileGraphState::Stopped;
		bool m_flushing = false;
		bool m_discontinuityPending = true;
		bool m_stepPending = false;
		double m_rate = 1.0;
		int64_t m_tStart = 0;
		int64_t m_segmentStart = 0;
		int64_t m_segmentStop = 0;
	};
}
