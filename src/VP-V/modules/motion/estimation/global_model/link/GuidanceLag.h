// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-1): where the guide lag d comes from. d = max(2, 1 + ceil((readback + p95 fit) / source period)) is fixed per
// graph (recomputed on a rebuild only) from MEASURED values: the p95 of the fit cost (CameraMotion::costMicros of the published
// records, i.e. the camera worker's own clock) and the readback latency of the field copy (request -> ready, render thread
// clock). The source period is the measured one the graph was built for. Nothing here is a rate or a frame count; with no
// measurement yet the lag is the structural floor (2).
// A deterministic run (N1) does not use the measured figures: it keeps the floor, so the same input gives the same d.
// Thread-safe (one mutex): the worker adds fit costs, the render thread adds readbacks and reads the lag. C++14, header-only.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace CameraLink
{
	constexpr uint8_t kMinGuideLag = 2;
	constexpr uint8_t kMaxGuideLag = 16;   // was kMaxInputs * 4 (= 16 when kMaxInputs was 4): pinned, not tied to the input limit

	// d = max(2, 1 + ceil((readback + p95 fit) / source period)). All three in seconds, measured; unknown (<= 0) period -> 2.
	inline uint8_t GuideLagFor(double readbackSeconds, double p95FitSeconds, double sourcePeriodSeconds)
	{
		if (!(sourcePeriodSeconds > 0))
			return kMinGuideLag;
		const double need = (std::max)(0.0, readbackSeconds) + (std::max)(0.0, p95FitSeconds);
		const double d = 1.0 + std::ceil(need / sourcePeriodSeconds - 1e-9);
		const double clamped = (std::min)(static_cast<double>(kMaxGuideLag), (std::max)(static_cast<double>(kMinGuideLag), d));
		return static_cast<uint8_t>(clamped);
	}

	// A small sliding window of positive samples (seconds) with a nearest-rank percentile.
	class SampleWindow
	{
	public:
		explicit SampleWindow(size_t capacity = 128) : m_cap(capacity < 4 ? 4 : capacity) {}

		void Add(double seconds)
		{
			if (!(seconds > 0))
				return;   // 0 = not measured (a record without a clock): never a sample
			if (m_samples.size() < m_cap)
				m_samples.push_back(seconds);
			else
				m_samples[m_next] = seconds;
			m_next = (m_next + 1) % m_cap;
		}
		size_t Count() const { return m_samples.size(); }
		void Clear() { m_samples.clear(); m_next = 0; }

		// Nearest rank: the smallest sample with at least q of the samples at or below it. 0 when empty.
		double Quantile(double q) const
		{
			if (m_samples.empty())
				return 0;
			std::vector<double> s = m_samples;
			std::sort(s.begin(), s.end());
			const double qq = q < 0 ? 0 : q > 1 ? 1 : q;
			size_t rank = static_cast<size_t>(std::ceil(qq * static_cast<double>(s.size())));
			if (rank < 1)
				rank = 1;
			return s[rank - 1];
		}
		double P95() const { return Quantile(0.95); }

	private:
		size_t m_cap;
		size_t m_next = 0;
		std::vector<double> m_samples;
	};

	class GuideLagMeter
	{
	public:
		// The camera worker: the fit time of one record (seconds, from costMicros).
		void AddFitSeconds(double seconds)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_fit.Add(seconds);
		}
		// The render thread: the readback latency of one field copy (request -> ready), seconds.
		void AddReadbackSeconds(double seconds)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_readback.Add(seconds);
		}
		double P95FitSeconds() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_fit.P95();
		}
		double P95ReadbackSeconds() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_readback.P95();
		}
		size_t FitSamples() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_fit.Count();
		}
		// d for the graph being (re)built. sourcePeriodSeconds <= 0 (unknown) or deterministic -> the floor.
		uint8_t Lag(double sourcePeriodSeconds, bool deterministic) const
		{
			if (deterministic)
				return kMinGuideLag;
			std::lock_guard<std::mutex> lock(m_mutex);
			return GuideLagFor(m_readback.P95(), m_fit.P95(), sourcePeriodSeconds);
		}
		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_fit.Clear();
			m_readback.Clear();
		}

	private:
		mutable std::mutex m_mutex;
		SampleWindow m_fit, m_readback;
	};
}
