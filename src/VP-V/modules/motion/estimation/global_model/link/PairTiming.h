// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-1, doc 56 section 1.4): the seconds between the two source frames of a pair. No rate is hard coded:
// the period is 1 / film rate while the cadence is locked, otherwise 1 / the measured source rate, otherwise (visibly)
// 1 / the nominal rate, otherwise unknown (0: the fitter answers NoInput, status "no model (timing unknown)").
// Pure, header-only, C++14, deterministic.
#pragma once

#include <cstdint>

namespace CameraLink
{
	enum class TimingBasis : uint8_t { Unknown, FilmLock, Measured, Nominal };

	inline const char* Text(TimingBasis b)
	{
		switch (b)
		{
		case TimingBasis::FilmLock: return "film cadence lock";
		case TimingBasis::Measured: return "measured source rate";
		case TimingBasis::Nominal: return "nominal rate";
		default: return "unknown";
		}
	}

	// What the host knows at a kTiming relatch.
	struct TimingFacts
	{
		double measuredSourceHz = 0;   // measured rate of the SOURCE frames (unique frames per second); 0 = unknown
		double filmHz = 0;             // the locked film rate (e.g. 23.976); only meaningful with cadenceLocked
		bool cadenceLocked = false;
		double nominalHz = 0;          // the container / signal nominal rate; 0 = unknown
	};

	class PairTiming
	{
	public:
		// kTiming relatch: re-derive the period from the facts.
		void Latch(const TimingFacts& f)
		{
			m_basis = TimingBasis::Unknown;
			m_period = 0;
			if (f.cadenceLocked && f.filmHz > 0)
			{
				m_period = 1.0 / f.filmHz;
				m_basis = TimingBasis::FilmLock;
			}
			else if (f.measuredSourceHz > 0)
			{
				m_period = 1.0 / f.measuredSourceHz;
				m_basis = TimingBasis::Measured;
			}
			else if (f.nominalHz > 0)
			{
				m_period = 1.0 / f.nominalHz;
				m_basis = TimingBasis::Nominal;
			}
			++m_latches;
		}

		double Period() const { return m_period; }
		TimingBasis Basis() const { return m_basis; }
		uint32_t Latches() const { return m_latches; }

		// Seconds between pairFrom and pairTo. uniqueFramesBetween = number of UNIQUE source frame steps (cadence repeats
		// skipped, as the matcher does; a plain consecutive pair is 1). For a VFR file with source timestamps on both frames
		// (seconds, to > from >= 0) the timestamps win. 0 = unknown.
		double Seconds(int uniqueFramesBetween, bool haveTimestamps = false, double tsFrom = 0, double tsTo = 0) const
		{
			if (haveTimestamps && tsTo > tsFrom && tsFrom >= 0)
				return tsTo - tsFrom;
			if (m_period <= 0 || uniqueFramesBetween <= 0)
				return 0;
			return m_period * static_cast<double>(uniqueFramesBetween);
		}

	private:
		double m_period = 0;
		TimingBasis m_basis = TimingBasis::Unknown;
		uint32_t m_latches = 0;
	};
}
