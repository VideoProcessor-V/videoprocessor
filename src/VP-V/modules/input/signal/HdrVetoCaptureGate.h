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

	void Arm(uint64_t generation, const VideoStateComPtr& sdrState);
	void Disarm();
	bool Armed(uint64_t generation) const;
	bool WantsSample(uint64_t generation, uint64_t nowMs);

	struct Observation
	{
		bool cut = false;
		VideoStateComPtr switchState;
		float gridDifference = 0.0f;
		bool usedFallback = false;
		const char* fallbackReason = nullptr;
	};

	Observation Observe(uint64_t generation, const HdrVetoFrameStats& stats, uint64_t sequence = 0);
	void SetSceneReader(HdrVetoSceneReaderFn reader);
	float LastGridDifference() const;
	float LastHistogramDistance() const { return m_cutDetector.LastHistogramDistance(); }
	uint64_t SharedCutHits() const;
	uint64_t FallbackCutCount() const;
	const char* LastFallbackReason() const;
	bool UsedFallbackOnLastObserve() const;

private:
	void DetReset() { m_cutDetector.Reset(); }
	bool DetObserve(const HdrVetoFrameStats& s, uint64_t q, HdrVetoSceneReaderFn r, HdrVetoCutDiag* d, const char* w) { return m_cutDetector.Observe(s, q, r, d, w); }
	float DetGrid() const { return m_cutDetector.LastGridDifference(); }
	mutable std::mutex m_mutex;
	bool m_armed = false;
	uint64_t m_generation = 0;
	VideoStateComPtr m_sdrState;

	// Synchronized under m_mutex.
	bool m_captureGenerationKnown = false;
	uint64_t m_captureGeneration = 0;
	uint64_t m_lastSampleMs = 0;
	bool m_haveSample = false;
	HdrVetoSceneReaderFn m_sceneReader = nullptr;
	HdrVetoCutDiag m_diag;
	HdrVetoCutDetector m_cutDetector;
};
