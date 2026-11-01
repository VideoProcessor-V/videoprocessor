// VPV-HOOK-FILE
/*
 * Copyright(C) 2025 Bill Slack <bslack@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

// Generic, inert observer points for captured-frame delivery. With nothing registered
// every helper returns immediately. Register() is called only during start-up, before
// any capture thread runs; afterwards the registry is read-only and dispatch takes no
// lock and allocates nothing. Registration order is dispatch order. Observers run on the
// capture thread: they must not block or lock. A throw from an observer is swallowed
// here and never reaches the capture path. ResetForTesting() is for unit tests only.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

struct CaptureFrameFacts
{
	int64_t sequence = -1;
	int64_t arrivalTicks = 0;
	int width = 0;
	int height = 0;
	int encoding = 0;
	double nominalRateHz = 0;
	bool formatChanged = false;
};

class ICaptureObserver
{
public:
	virtual ~ICaptureObserver() = default;
	virtual void OnCaptureFrame(const CaptureFrameFacts&) {}
	virtual void OnSourceFormatChange(const CaptureFrameFacts&) {}
};

namespace CaptureDeliveryHook
{
	using Registry = std::vector<std::unique_ptr<ICaptureObserver>>;

	inline Registry& Observers() { static Registry registry; return registry; }
	inline void Register(std::unique_ptr<ICaptureObserver> observer)
	{
		if (observer) Observers().push_back(std::move(observer));
	}
	inline bool Any() { return !Observers().empty(); }
	inline void ResetForTesting() { Observers().clear(); }

	inline void Frame(const CaptureFrameFacts& facts) noexcept
	{
		if (!Any()) return;
		for (auto& o : Observers())
		{
			try { o->OnCaptureFrame(facts); }
			catch (...) {}
		}
	}

	inline void FormatChange(const CaptureFrameFacts& facts) noexcept
	{
		if (!Any()) return;
		for (auto& o : Observers())
		{
			try { o->OnSourceFormatChange(facts); }
			catch (...) {}
		}
	}
}
