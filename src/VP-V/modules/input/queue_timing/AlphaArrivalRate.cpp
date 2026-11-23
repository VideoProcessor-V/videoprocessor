// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <pch.h>
#include <DisplayRefreshRatePolicy.h>

#include <vprenderer/AlphaCadenceCorrectionPolicy.h>
#include <VP-V/core/OnePole.h>
#include <GeneralRateLog.h>

#include <algorithm>
#include <cmath>

void AlphaArrivalRateEstimator::Reset()
{
	*this = AlphaArrivalRateEstimator();
}

void AlphaArrivalRateEstimator::Add(double arrivalSeconds, double rateHintHz)
{
	if (!(arrivalSeconds > 0.0) || !std::isfinite(arrivalSeconds))
		return;
	static GeneralRate::FallbackNotice fallback;   // fixed-constant batch B7 2026-10-18: logged
	const bool hinted = GeneralRate::NoteFallback(fallback, CaptureRateBounds::Accepts(rateHintHz),
		"arrival-rate estimator (no capture rate hint)", 60.0);
	const double rate = hinted ? rateHintHz : 60.0;
	if (m_started && arrivalSeconds == m_last)
		return;    // the same frame again (a film frame's update): no new period
	if (m_started && arrivalSeconds < m_last)
		Reset();   // a clock step back: start over
	if (m_started && hinted && m_hintHz > 0.0 &&
		std::abs(rateHintHz - m_hintHz) > kHintChangeShare * m_hintHz)
		Reset();   // Bug hunt 3 F3: another rate (a pulldown lock change): start over
	if (hinted)
		m_hintHz = rateHintHz;
	double periods = 1.0;
	if (m_started && hinted)
	{
		// The periods since the last frame, counted with the hint (the card's rate is
		// within ppm of the truth, so rounding finds them): a missed or dropped frame
		// advances the index by its periods instead of bending the line. The slope
		// itself stays measured on QPC.
		const double elapsed = (arrivalSeconds - m_last) * rateHintHz;
		if (elapsed > kMaximumGapSeconds * rateHintHz)
			Reset();   // a break, not missed frames
		else
			periods = (std::max)(1.0, std::floor(elapsed + 0.5));
	}
	if (!m_started)
	{
		m_started = true;
		m_t0 = arrivalSeconds;
		m_index = 0.0;
	}
	else
	{
		m_index += periods;
	}
	m_last = arrivalSeconds;
	const double keep = std::exp(-1.0 / (rate * kWindowSeconds));
	const double x = m_index;
	const double y = arrivalSeconds - m_t0;
	m_w = m_w * keep + 1.0;
	m_x = m_x * keep + x;
	m_y = m_y * keep + y;
	m_xx = m_xx * keep + x * x;
	m_xy = m_xy * keep + x * y;
}

double AlphaArrivalRateEstimator::RateHz() const
{
	if (!m_started || m_last - m_t0 < kMinimumSpanSeconds || m_w < 2.0)
		return 0.0;
	const double meanX = m_x / m_w;
	const double meanY = m_y / m_w;
	const double sxx = m_xx / m_w - meanX * meanX;
	const double sxy = m_xy / m_w - meanX * meanY;
	if (!(sxx > 0.0) || !(sxy > 0.0))
		return 0.0;
	const double hz = sxx / sxy;   // 1 / (seconds per frame)
	return CaptureRateBounds::Accepts(hz) ? hz : 0.0;
}

uint32_t AlphaCadenceCorrectionPolicy::FramesFor(double seconds, double rateHz,
	uint32_t legacy, bool timingFixes)
{
	if (!timingFixes || !CaptureRateBounds::Accepts(rateHz))
		return legacy;
	return static_cast<uint32_t>((std::max)(1.0, std::ceil(seconds * rateHz)));
}

double AlphaCadenceCorrectionPolicy::FilterAlpha(double rateHz, bool timingFixes)
{
	if (!timingFixes || !CaptureRateBounds::Accepts(rateHz))
		return RATE_FILTER_ALPHA;
	return OnePole::AlphaPerFrame(RATE_FILTER_SECONDS, rateHz);
}
