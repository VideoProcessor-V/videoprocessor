#pragma once

// Bug hunt 3 (C2): the newlldv SDR veto returned to SDR 1-5 frames after the
// cut it waited for. Its picture statistics were taken after the frame had
// gone to the renderer, at most every 50 ms, and the switch was made on the UI
// thread after a posted message, so the cut frame and the few after it were
// queued with the PQ state.
//
// The gate moves the switch onto the capture thread and onto the cut frame:
//   - the UI thread arms it (with the effective SDR state the veto returns to)
//     once the veto is armed, and disarms it when the epoch, the settings or
//     the measurement change;
//   - while armed, every frame is measured before it goes to the renderer (no
//     50 ms sub-sampling), and the frame on which the cut detector fires is
//     the first one queued with the SDR state.
// The UI thread still commits the veto (HdrInferenceTracker::OnPicture sees
// the same cut flag) and pushes the same state again, which the renderer
// takes as unchanged. Pure apart from the state pointer: unit tested.

#include "HdrInference.h"
#include "VideoState.h"

#include <cstdint>
#include <mutex>

class HdrVetoCaptureGate
{
public:
	// The sampling interval while the veto is not armed (statistics for the
	// shot evidence only; 20 Hz is plenty).
	static constexpr uint64_t UNARMED_INTERVAL_MS = 50;

	// UI thread. sdrState: the effective state the veto returns this epoch to;
	// null disarms.
	void Arm(uint64_t generation, const VideoStateComPtr& sdrState)
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		m_armed = sdrState != nullptr;
		m_generation = generation;
		m_sdrState = sdrState;
	}

	void Disarm()
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		m_armed = false;
		m_sdrState = nullptr;
	}

	bool Armed(uint64_t generation) const
	{
		std::lock_guard<std::mutex> guard(m_mutex);
		return m_armed && m_generation == generation;
	}

	// Capture thread: whether to measure this frame. A new measurement
	// generation starts the cut detector afresh. Armed: every frame, so the cut
	// is seen on its own frame.
	bool WantsSample(uint64_t generation, uint64_t nowMs)
	{
		if (generation != m_captureGeneration || !m_captureGenerationKnown)
		{
			m_captureGeneration = generation;
			m_captureGenerationKnown = true;
			m_cutDetector.Reset();
			m_lastSampleMs = 0;
			m_haveSample = false;
		}
		if (!Armed(generation) && m_haveSample && nowMs - m_lastSampleMs < UNARMED_INTERVAL_MS)
			return false;
		m_lastSampleMs = nowMs;
		m_haveSample = true;
		return true;
	}

	struct Observation
	{
		bool cut = false;
		// Set (once) on the cut frame while armed: queue this frame and the
		// rest of the epoch with this state.
		VideoStateComPtr switchState;
	};

	// Capture thread: one measured picture (before it goes to the renderer).
	Observation Observe(uint64_t generation, const HdrVetoFrameStats& stats)
	{
		Observation result;
		result.cut = m_cutDetector.Observe(stats);
		if (result.cut)
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			if (m_armed && m_generation == generation && m_sdrState)
			{
				result.switchState = m_sdrState;
				m_armed = false;   // one switch per arming
				m_sdrState = nullptr;
			}
		}
		return result;
	}

	float LastGridDifference() const { return m_cutDetector.LastGridDifference(); }
	float LastHistogramDistance() const { return m_cutDetector.LastHistogramDistance(); }

private:
	mutable std::mutex m_mutex;
	bool m_armed = false;
	uint64_t m_generation = 0;
	VideoStateComPtr m_sdrState;

	// Capture thread only.
	bool m_captureGenerationKnown = false;
	uint64_t m_captureGeneration = 0;
	uint64_t m_lastSampleMs = 0;
	bool m_haveSample = false;
	HdrVetoCutDetector m_cutDetector;
};
