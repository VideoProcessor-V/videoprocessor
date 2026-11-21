// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// colour.appearance: the room-light reader of the appearance stage (CF-1 CAM-2).
//
// ONE sensor filter exists: the LED intent's AmbientAdapt::Filter (median of 5, log-lux EMA, hold 60 s then decay on
// loss). The LED intent PUBLISHES its state as the room_light product (Record below, held state like P14); this header
// only READS it. Nothing here is a second estimator: the extra Smoother is the appearance path's own slow adaptation
// (minutes, owner answer Q3) applied to the already filtered lux, and it is the only thing that differs from the filter.
//
//   lux -> surround (CIE dark / dim / average): F, c, Nc interpolated continuously in log-lux between lux_dark and
//          lux_lit, dim at the geometric middle (refs/125 s4).
//   lux -> screen veil (cd/m2) = lux x ambient_screen_reflect, capped by ambient_max_black_nits (the old ambient_adapt
//          physics, now inside the one owner).
//   manual presets: Cinema = dark, Living low = dim, Living lit = average (a preset ignores the sensor).
//   changes land at a cut, or as a slew below 1 %/s of the value.
//
// General, not fixed: the anchors, the reflect factor, the cap and the taus are parameters; no room, lux or rate is
// hard-coded beyond the documented defaults of the existing [ambient] keys.

#include "Cam16.h"

#include <VP-V/core/OnePole.h>
#include <VP-V/modules/colour/display_mapping/AmbientAdaptation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

namespace RoomLight
{
	// The room the appearance stage assumes. Auto reads the sensor (the published record).
	enum class Room { Cinema, LivingLow, LivingLit, Auto };

	// What the LED intent publishes each time its filter changes (the room_light product record).
	struct Record
	{
		double smoothedLux = 0.0;   // the filter's value, exactly 0 while invalid or dark
		bool valid = false;         // a reading is being tracked
		bool holding = false;       // sensor lost, last value held (up to 60 s)
		bool decaying = false;      // sensor lost, value decaying toward off
		bool lost = false;          // decayed to off
		uint64_t seq = 0;           // publish counter
	};

	inline Record MakeRecord(const AmbientAdapt::Filter& filter, uint64_t seq)
	{
		Record r;
		r.smoothedLux = filter.SmoothedLux();
		r.valid = filter.Valid();
		r.holding = filter.Holding();
		r.decaying = filter.Decaying();
		r.lost = filter.Lost();
		r.seq = seq;
		return r;
	}

	// The room_light held state (product 33): ONE writer (the LED intent, on each sensor reading), any thread reads the latest
	// sealed record (lag >= 1, no live fill). A mutex guards ~48 B read once per frame.
	class HeldState
	{
	public:
		void Reset()
		{
			std::lock_guard<std::mutex> g(m_);
			rec_ = Record();
			serial_ = 0;
		}
		uint64_t Publish(Record r)
		{
			std::lock_guard<std::mutex> g(m_);
			r.seq = ++serial_;
			rec_ = r;
			return serial_;
		}
		// False while nothing was published: the reader treats the room as unknown (dark assumed, said in the status).
		bool ReadLatest(Record& out) const
		{
			std::lock_guard<std::mutex> g(m_);
			if (serial_ == 0)
				return false;
			out = rec_;
			return true;
		}
		uint64_t Serial() const
		{
			std::lock_guard<std::mutex> g(m_);
			return serial_;
		}

	private:
		mutable std::mutex m_;
		Record rec_;
		uint64_t serial_ = 0;
	};

	struct Anchors
	{
		double luxDark = 8.0;    // [ambient] lux_dark: at or below this the room is dark
		double luxLit = 20.0;    // [ambient] lux_lit: at or above this the room is average
	};

	inline Cam16::Surround Lerp(const Cam16::Surround& a, const Cam16::Surround& b, double t)
	{
		return { a.F + (b.F - a.F) * t, a.c + (b.c - a.c) * t, a.Nc + (b.Nc - a.Nc) * t };
	}

	// Continuous surround from the smoothed lux: dark at lux_dark, dim at the geometric middle, average at lux_lit.
	inline Cam16::Surround SurroundForLux(double lux, const Anchors& a)
	{
		const double dark = std::max(0.0, a.luxDark);
		const double lit = std::max(dark, a.luxLit);
		lux = std::max(0.0, std::isfinite(lux) ? lux : 0.0);
		double t;
		if (!(lit > dark))
			t = lux >= lit ? 1.0 : 0.0;   // degenerate anchors: a step at the anchor
		else
		{
			const double off = AmbientAdapt::kLuxOffset;
			const double lo = std::log2(dark + off), hi = std::log2(lit + off);
			t = std::min(1.0, std::max(0.0, (std::log2(lux + off) - lo) / (hi - lo)));
		}
		return t <= 0.5 ? Lerp(Cam16::kSurroundDark, Cam16::kSurroundDim, t * 2.0)
		                : Lerp(Cam16::kSurroundDim, Cam16::kSurroundAverage, (t - 0.5) * 2.0);
	}

	inline Cam16::Surround PresetSurround(Room room)
	{
		switch (room)
		{
		case Room::LivingLow: return Cam16::kSurroundDim;
		case Room::LivingLit: return Cam16::kSurroundAverage;
		case Room::Cinema:
		case Room::Auto:
		default: return Cam16::kSurroundDark;
		}
	}

	// Screen veil in cd/m2: reflected room light, capped (ambient_max_black_nits). 0 lux or 0 reflect: exactly 0.
	inline double VeilNits(double lux, double reflectNitsPerLux, double maxBlackNits)
	{
		const double v = std::max(0.0, std::isfinite(lux) ? lux : 0.0) * std::max(0.0, reflectNitsPerLux);
		return std::min(v, std::max(0.0, maxBlackNits));
	}

	// The appearance path's slow adaptation of the room: an asymmetric one-pole in log-lux with MINUTES time constants
	// (up 60 s, down 180 s: refs/125 s4, owner Q3 (a)), landing at cuts or slewing below 1 %/s of the value on a static
	// picture. The LED path keeps the filter's own taus (they are the Filter's settings and are not touched).
	constexpr double kAppearanceTauUpSeconds = 60.0;
	constexpr double kAppearanceTauDownSeconds = 180.0;

	class Smoother
	{
	public:
		void Reset() { m_init = false; }
		bool Initialised() const { return m_init; }

		// `lux`: the room_light value; `atCut`: a displayed cut happened since the last step; `now`: monotonic seconds.
		double Step(double lux, bool atCut, double now, double tauUp = kAppearanceTauUpSeconds, double tauDown = kAppearanceTauDownSeconds)
		{
			const double off = AmbientAdapt::kLuxOffset;
			const double target = std::log2(std::max(0.0, std::isfinite(lux) ? lux : 0.0) + off);
			if (!m_init)
			{
				m_ema = m_applied = target;
				m_last = now;
				m_init = true;
				return Lux();
			}
			const double dt = std::max(0.0, now - m_last);
			m_last = now;
			const double tau = target > m_ema ? tauUp : tauDown;
			m_ema += (target - m_ema) * OnePole::AlphaFor(dt, tau, OnePole::DtRule::AsGiven);
			if (atCut)
				m_applied = m_ema;
			else
			{
				const double maxStep = std::log2(1.0 + AmbientAdapt::kSlewPerSecond) * dt;
				m_applied += std::min(maxStep, std::max(-maxStep, m_ema - m_applied));
			}
			return Lux();
		}

		double Lux() const { const double v = std::exp2(m_applied) - AmbientAdapt::kLuxOffset; return v < 1e-9 ? 0.0 : v; }

	private:
		bool m_init = false;
		double m_ema = 0.0;
		double m_applied = 0.0;
		double m_last = 0.0;
	};

	struct Resolved
	{
		Cam16::Surround surround = Cam16::kSurroundDark;
		double veilNits = 0.0;
		double appliedLux = 0.0;
		bool fromSensor = false;
		bool sensorHeld = false, sensorDecaying = false, sensorMissing = false;   // CAM-4: the sensor states that reach the Fallbacks line
		std::string status;          // plain text for the Ctrl+I / Config status line
	};

	struct Params
	{
		Room room = Room::Cinema;
		Anchors anchors;
		double reflectNitsPerLux = 0.01;
		double maxBlackNits = 1.0;
	};

	// One step: a preset ignores the sensor (and clears the smoother so a later Auto starts from the live value); Auto reads
	// the record. A lost sensor is never silent: the filter holds, then decays, and the status says which.
	inline Resolved Resolve(const Params& p, const Record& record, Smoother& smoother, bool atCut, double now)
	{
		Resolved r;
		if (p.room != Room::Auto)
		{
			smoother.Reset();
			r.surround = PresetSurround(p.room);
			r.status = p.room == Room::Cinema ? "room: cinema (dark)" : p.room == Room::LivingLow ? "room: living, low light (dim)" : "room: living, lit (average)";
			return r;
		}
		r.fromSensor = true;
		r.appliedLux = smoother.Step(record.smoothedLux, atCut, now);
		r.surround = SurroundForLux(r.appliedLux, p.anchors);
		r.veilNits = VeilNits(r.appliedLux, p.reflectNitsPerLux, p.maxBlackNits);
		r.sensorHeld = record.holding && !record.decaying && !record.lost;
		r.sensorDecaying = record.decaying && !record.lost;
		r.sensorMissing = record.lost || (!record.valid && !record.holding && !record.decaying);
		if (record.lost || (!record.valid && !record.holding && !record.decaying))
			r.status = "room: sensor (no reading; dark assumed)";
		else if (record.decaying)
			r.status = "room: sensor lost, value decaying toward dark";
		else if (record.holding)
			r.status = "room: sensor lost, last value held";
		else
			r.status = "room: sensor";
		return r;
	}
}
