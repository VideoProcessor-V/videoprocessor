/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#include <pch.h>

#include "DisplayMode.h"

#include <atomic>
#include <cmath>
#include <DebugLog.h>


DisplayMode::DisplayMode(
	unsigned int frameWidth,
	unsigned int frameHeight,
	bool interlaced,
	unsigned int timeScale,
	unsigned int frameDuration, FieldOrder fieldOrder):
	m_frameWidth(frameWidth),
	m_frameHeight(frameHeight),
	m_interlaced(interlaced),
    m_fieldOrder(fieldOrder),
	m_timeScale(timeScale),
	m_frameDuration(frameDuration)
{
	if (frameWidth < 100 || frameWidth > 10000)
		throw std::runtime_error("resolutionX not valid");

	if (frameHeight < 100 || frameHeight > 10000)
		throw std::runtime_error("resolutionY not valid");

	if (timeScale == 0)
		throw std::runtime_error("timeScale cannot be zero");

	if (frameDuration == 0)
		throw std::runtime_error("frameDuration cannot be zero");

	if (frameDuration >= timeScale)
		throw std::runtime_error("frameDuration cannot be >= timeScale");

	// audit 2026-10-16: the refresh limit is a policy (about 10-240 Hz), not a
	// fixed 23-120 Hz reject; unusual rates are logged once per distinct rate.
	const double refreshRateHz = RefreshRateHz();
	if (!IsRefreshRateSupported(refreshRateHz))
		throw std::runtime_error("frameDuration and timeScale do not lead to a supported refresh rate");
	if (IsRefreshRateUnusual(refreshRateHz))
	{
		static std::atomic<long long> lastLoggedMilliHz{ -1 };
		const long long milliHz = (long long)std::llround(refreshRateHz * 1000.0);
		if (lastLoggedMilliHz.exchange(milliHz) != milliHz)
			DebugLog::Log("DisplayMode: unusual refresh rate %.3f Hz (%ux%u%s) accepted by the %.0f-%.0f Hz policy",
				refreshRateHz, frameWidth, frameHeight, interlaced ? "i" : "p",
				MinimumRefreshRateHz(), MaximumRefreshRateHz());
	}
}


bool DisplayMode::IsRefreshRateSupported(double refreshRateHz)
{
	// audit 2026-10-16
	return refreshRateHz >= MinimumRefreshRateHz() &&
		refreshRateHz <= MaximumRefreshRateHz();
}


bool DisplayMode::IsRefreshRateUnusual(double refreshRateHz)
{
	// audit 2026-10-16: 23.976 is the lowest common video rate, 120 the highest
	// common HDMI video rate; a small tolerance keeps 23.976/119.88 ordinary.
	return refreshRateHz < 23.0 || refreshRateHz > 120.5;
}


uint64_t DisplayMode::NanosecondsPerFrame() const
{
	// audit 2026-10-16
	return ((uint64_t)m_frameDuration * 1000000000ULL + m_timeScale / 2) / m_timeScale;
}


double DisplayMode::RefreshRateHz() const
{
	return (double)m_timeScale / (double)m_frameDuration;
}


CString DisplayMode::ToString() const
{
	CString s;

	// Resolutiion
	s.AppendFormat(_T("%ix%i"), m_frameWidth, m_frameHeight);

	if (m_interlaced)
		s += TEXT("I ");  // Interlaced
	else
		s += TEXT("P ");  // Progressive

	// Mode name if well-known
	// https://en.wikipedia.org/wiki/List_of_common_resolutions
	if (m_frameWidth == 1280 && m_frameHeight == 720)
		s += TEXT(" (HD)");
	else if (m_frameWidth == 1920 && m_frameHeight == 1080)
		s += TEXT(" (Full HD)");
	else if (m_frameWidth == 2048 && m_frameHeight == 1556)
		s += TEXT(" (2K FullFrame)");
	else if (m_frameWidth == 2048 && m_frameHeight == 1080)
		s += TEXT(" (2K DCI)");
	else if (m_frameWidth == 3840 && m_frameHeight == 2160)
		s += TEXT(" (4K UHDTV)");
	else if (m_frameWidth == 4096 && m_frameHeight == 2160)
		s += TEXT(" (4K DCI)");

	s += TEXT(" @ ");

	// Refresh
	s.AppendFormat(_T("%.3f"), RefreshRateHz());

	return s;
}


bool DisplayMode::operator == (const DisplayMode& other) const
{
	return
		FrameWidth() == other.FrameWidth() &&
		FrameHeight() == other.FrameHeight() &&
		TimeScale() == other.TimeScale() &&
		FrameDuration() == other.FrameDuration() &&
		IsInterlaced() == other.IsInterlaced() && FirstFieldOrder() == other.FirstFieldOrder();
}


bool DisplayMode::operator != (const DisplayMode& other) const
{
	return !(*this == other);
}
