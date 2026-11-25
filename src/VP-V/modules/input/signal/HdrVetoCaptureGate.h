#pragma once

// Bug hunt 3 (C2): moves the armed newlldv SDR veto switch onto the capture thread and cut frame.
// UI thread arms/disarms with the effective SDR state; while armed every frame is measured before
// reaching the renderer so the cut frame itself queues with SDR. UI thread still commits the veto.
// Pure apart from the state pointer: unit tested.

#include <VP-V/modules/input/signal/HdrInference.h>
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

	// Capture thread: whether to measure this frame (armed = every frame; new generation resets detector).
	bool WantsSample(uint64_t generation, uint64_t nowMs)
	{
		if (generation != m_captureGeneration || !m_captureGenerationKnown)
		{
			m_captureGeneration = generation;
			m_captureGenerationKnown = true;
			m_cutDetector.Reset();
			m_diag = HdrVetoCutDiag();
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
		VideoStateComPtr switchState;   // set once on the cut frame while armed
	};

	// Capture thread: one measured picture (before it goes to the renderer).
	Observation Observe(uint64_t generation, const HdrVetoFrameStats& stats, uint64_t sequence = 0)
	{
		Observation result;
		result.cut = m_cutDetector.Observe(stats, sequence, m_sceneReader, &m_diag);
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

	void SetSceneReader(HdrVetoSceneReaderFn reader) { m_sceneReader = reader; }
	float LastGridDifference() const { return m_cutDetector.LastGridDifference(); }
	float LastHistogramDistance() const { return m_cutDetector.LastHistogramDistance(); }
	uint64_t SharedCutHits() const { return m_diag.sharedHits; }
	uint64_t FallbackCutCount() const { return m_diag.fallbackCount; }
	const char* LastFallbackReason() const { return m_diag.lastFallbackReason; }
	bool UsedFallbackOnLastObserve() const { return m_diag.usedFallback; }

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
	HdrVetoSceneReaderFn m_sceneReader = nullptr;
	HdrVetoCutDiag m_diag;
	HdrVetoCutDetector m_cutDetector;
};
