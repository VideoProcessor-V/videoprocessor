// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-6): the visible state of the fit runner. Atomic counters (any thread writes), the FallbackCatalog
// ids camera.no_model.{late,ambiguous,timing,readback,slip,depth0}, and the status lines. Never log-only: CollectStatus
// of the link returns the lines; the host shows them with the other fallbacks.
// "Ambiguous" engages after 0.5 s of consecutive Ambiguous records, measured in the records' own pairSeconds (never a
// frame count), and releases automatically on the first record that is not Ambiguous.
// Pure, header-only, C++14, deterministic.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace CameraLink
{
	constexpr const char* kIdNoModelLate = "camera.no_model.late";
	constexpr const char* kIdNoModelAmbiguous = "camera.no_model.ambiguous";
	constexpr const char* kIdNoModelTiming = "camera.no_model.timing";
	constexpr const char* kIdNoModelReadback = "camera.no_model.readback";
	constexpr const char* kIdNoModelSlip = "camera.no_model.slip";
	constexpr const char* kIdNoModelDepth0 = "camera.no_model.depth0";

	constexpr double kAmbiguousEngageSeconds = 0.5;

	struct FitCounters
	{
		std::atomic<uint64_t> submitted{ 0 };        // jobs accepted by the worker queue
		std::atomic<uint64_t> queueFull{ 0 };        // jobs dropped: the bounded queue was full (visible)
		std::atomic<uint64_t> fitted{ 0 };
		std::atomic<uint64_t> published{ 0 };
		std::atomic<uint64_t> late{ 0 };             // fitted, not published (seal first / claim stale / not writable / no inputs)
		std::atomic<uint64_t> notFittedSlip{ 0 };
		std::atomic<uint64_t> timingUnknown{ 0 };    // pairSeconds 0 -> NoInput
		std::atomic<uint64_t> readbackDropped{ 0 };  // ring full or copy failed (no field for the pair)
		std::atomic<uint64_t> depth0{ 0 };           // seals with no lane depth (no look-ahead): P7 had no chance
		std::atomic<uint64_t> sealedOk{ 0 };
		std::atomic<uint64_t> sealedLate{ 0 };       // frame sealed without a published P7
		std::atomic<uint64_t> unusable{ 0 };         // published records outside the usable set
		std::atomic<uint64_t> resets{ 0 };           // fitter resets consumed by the worker
		std::atomic<uint64_t> relatches{ 0 };
	};

	// Counts consecutive Ambiguous measured seconds.
	class AmbiguousTracker
	{
	public:
		// Returns true while engaged.
		bool Observe(CameraModel::Reason reason, double pairSeconds)
		{
			if (reason == CameraModel::Reason::Ambiguous)
			{
				m_seconds += pairSeconds > 0 ? pairSeconds : 0;
				if (m_seconds >= kAmbiguousEngageSeconds)
					m_engaged = true;
			}
			else
			{
				m_seconds = 0;
				m_engaged = false;
			}
			return m_engaged;
		}
		bool Engaged() const { return m_engaged; }
		double Seconds() const { return m_seconds; }
		void Reset() { m_seconds = 0; m_engaged = false; }

	private:
		double m_seconds = 0;
		bool m_engaged = false;
	};

	// The facts the decisions are built from (a snapshot).
	struct StatusFacts
	{
		bool graphHasP7 = false;
		bool laneDepthZero = false;
		bool lateNow = false;           // the last sealed frame had no published P7
		bool timingUnknownNow = false;
		bool readbackFailedNow = false;
		bool slipNow = false;
		bool ambiguousNow = false;
		double ambiguousShare = 0;      // share of recent records Ambiguous (for the line)
	};

	inline std::vector<FallbackCatalog::Decision> BuildDecisions(const StatusFacts& f)
	{
		using namespace FallbackCatalog;
		std::vector<Decision> d;
		auto add = [&](const char* id, bool on, const char* why) { d.push_back(on && f.graphHasP7 ? Engaged(id, why) : Released(id)); };
		add(kIdNoModelLate, f.lateNow, "camera: no model (late): the fit was not published before the frame; consumers keep their measured values");
		char amb[128];
		std::snprintf(amb, sizeof(amb), "camera: no dominant motion (ambiguous), model not used, %.0f%% of frames", f.ambiguousShare * 100.0);
		add(kIdNoModelAmbiguous, f.ambiguousNow, amb);
		add(kIdNoModelTiming, f.timingUnknownNow, "camera: no model (timing unknown): the pair duration could not be derived");
		add(kIdNoModelReadback, f.readbackFailedNow, "camera: no model (readback): the field copy was not ready");
		add(kIdNoModelSlip, f.slipNow, "camera: no model (slip): the pair spans a drop, repeat or resync");
		add(kIdNoModelDepth0, f.laneDepthZero, "camera: no model (no look-ahead depth): the fit cannot run ahead");
		return d;
	}

	// "camera: fits 120, published 118, late 2, unusable 3, slip 0, queue full 0"
	inline std::string StatusLine(const FitCounters& c)
	{
		char b[256];
		std::snprintf(b, sizeof(b), "camera fit: %llu fitted, %llu published, %llu late, %llu not used, %llu slip, %llu timing unknown, %llu queue full, %llu readback dropped",
			(unsigned long long)c.fitted.load(), (unsigned long long)c.published.load(), (unsigned long long)c.late.load(),
			(unsigned long long)c.unusable.load(), (unsigned long long)c.notFittedSlip.load(),
			(unsigned long long)c.timingUnknown.load(), (unsigned long long)c.queueFull.load(),
			(unsigned long long)c.readbackDropped.load());
		return b;
	}
}
