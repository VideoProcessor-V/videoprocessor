// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>

// colour.display_mapping: the ONE home of the DV mapping maths shared by DolbyVisionTrims,
// DvTrimSelection and DvDolbyStyle (CF-1 P2): the SMPTE ST 2084 PQ transfer (double
// precision, no sub-12-bit intermediate) and the ITU-R BT.2100 ICtCp matrices with the
// 3x3 helpers. The old call names (DolbyVisionTrims::PqFromNits, DvDolbyStyle::kLmsToIctcp,
// ...) forward here with identical arithmetic (bit for bit, DvMathTests).
namespace DvMath
{
	// SMPTE ST 2084: nits <-> PQ signal (0..1).
	inline double PqFromNits(double nits)
	{
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
		const double y = (nits < 0.0 ? 0.0 : nits) / 10000.0;
		const double p = std::pow(y, m1);
		return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
	}

	inline double NitsFromPq(double pq)
	{
		const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
		const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
		const double e = std::pow(pq < 0.0 ? 0.0 : pq, 1.0 / m2);
		const double num = e - c1 > 0.0 ? e - c1 : 0.0;
		return 10000.0 * std::pow(num / (c2 - c3 * e), 1.0 / m1);
	}

	// Linear BT.2020 RGB (1 = 10000 nits) <-> LMS, and PQ LMS <-> ICtCp (ITU-R BT.2100 Table 7).
	constexpr double kRgb2020ToLms[3][3] = { { 1688.0 / 4096, 2146.0 / 4096, 262.0 / 4096 },
		{ 683.0 / 4096, 2951.0 / 4096, 462.0 / 4096 }, { 99.0 / 4096, 309.0 / 4096, 3688.0 / 4096 } };
	constexpr double kLmsToIctcp[3][3] = { { 0.5, 0.5, 0.0 }, { 6610.0 / 4096, -13613.0 / 4096, 7003.0 / 4096 },
		{ 17933.0 / 4096, -17390.0 / 4096, -543.0 / 4096 } };

	inline void Mul(const double m[3][3], const double v[3], double out[3])
	{
		for (int r = 0; r < 3; ++r)
			out[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
	}

	inline bool Invert(const double m[3][3], double inv[3][3])
	{
		const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
			m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
		if (!(std::fabs(d) > 1e-15))
			return false;
		inv[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
		inv[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
		inv[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
		inv[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
		inv[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
		inv[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
		inv[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
		inv[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
		inv[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
		return true;
	}
}
