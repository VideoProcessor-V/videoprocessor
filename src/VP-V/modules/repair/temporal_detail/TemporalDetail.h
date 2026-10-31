// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail (repair.temporal_detail, DRAFT): constants, settings, indicator-stamp reader policies.
// Header-only, C++14-compatible, no Windows headers. The algorithm is in README.md; the CPU reference model is
// TemporalDetailModel.h and the (draft, not dispatched) shaders mirror the constants below by name
// (TemporalDetailShaders.h).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace TemporalDetail
{
	// --- Shared constants (CPU model and shaders) ---
	constexpr double kReferenceHz = 23.976;     // the content rate when it is unknown
	constexpr double kBlockPx = 16.0;           // block edge at <= 2160 lines (x Scale)
	constexpr int kSubBlocks = 4;               // per block axis (grain-level sub-blocks)
	constexpr int kQuietSubBlocks = 4;          // the quietest sub-blocks averaged for the grain level
	constexpr int kGrainNeighbourhood = 2;      // grain level: minimum over (2 x 2 + 1)^2 blocks
	constexpr double kSigmaFine = 1.0;          // D1 = Y - G(kSigmaFine x Scale) Y
	constexpr double kSigmaBase = 2.5;          // base = G(kSigmaBase x Scale) Y
	constexpr double kSigmaLow = 6.0;           // E3 band = G(base) - G(kSigmaLow x Scale)
	constexpr double kGrainCal = 2.2;           // pure grain reads about its own energy (quiet sub-blocks read low)
	constexpr double kWienerKappa = 2.0;        // f = clamp(1 - kappa N / e3x3, 0, 1)
	constexpr double kEnergyFloor = 1e-3;       // codes^2, added to q and to divisions
	constexpr int kSlots = 3;                   // K past slots (one more, the future slot, in queued modes)
	constexpr int kMaxFutureFrames = 8;         // future frames the model looks at

	// Guard (README step 3)
	constexpr double kRho2Lo = 0.3, kRho2Hi = 0.5;       // E2cur / E2ref_w: gone .. coding keeps edges
	constexpr double kPullLo = 0.45, kPullHi = 0.65;     // Fcur / Fref: pulled .. kept (texture_restore uses 0.6 .. 0.8 on real clips; the model's coding simulation reaches 0.8)
	constexpr double kReboundTau = 0.3;                  // q rise (relative) after a fall that counts as a rebound
	constexpr double kReboundMinEnergy = 0.5;            // codes^2: smallest absolute q rise of a rebound
	constexpr double kEvidenceLo = 0.5, kEvidenceHi = 1.5;   // smoothstep over the decayed rebound count
	constexpr double kSoftFuture = 0.6;                  // qfut < kSoftFuture qref: the future is also soft
	constexpr double kSoftRunSeconds = 0.2;              // monotonic softening (and guard veto) that retires a reference
	constexpr double kSoftStep = 0.02;                   // relative fall per frame that counts as falling
	constexpr double kRetireMinEnergy = 0.5;             // codes^2: q below this never counts as softening
	// Validation (step 6)
	constexpr double kUncertaintyPx = 0.5;               // u <= kUncertaintyPx x Scale (smooth over the last 70%)
	constexpr double kBaseNoiseCodes = 0.75;             // base-match allowance: coding noise ...
	constexpr double kBaseGrainSigmas = 3.0;             // ... plus this many grain-level RMS (of the base band: / 4)
	constexpr double kCorrMin = 0.3;                     // corr(S, S_w) full at, zero at half
	constexpr double kSignificanceLo = 2.0, kSignificanceHi = 5.0;   // reference fine-structure energy over the grain level: grain-only blocks never serve
	constexpr double kStrongerMargin = 0.15;             // E1(S_w) > (1 + m) E1(S): zero at 1 + m, full at 1 + 3m
	constexpr double kAgeFadeShare = 0.5;                // linear fade over the last half of max_age
	// Weight and refresh (steps 4, 7)
	constexpr double kCrossfadeSeconds = 0.1;
	constexpr double kRiseSeconds = 0.15, kFallSeconds = 0.3;
	constexpr double kEnterWeight = 0.15, kLeaveWeight = 0.05;
	constexpr double kEvictWeight = 0.02;                // slot eviction: its blocks have faded below this
	// Transfer (step 8)
	constexpr double kGainLo = 0.8, kGainHi = 1.25;      // lighting gain clamp
	constexpr double kPixelCap = 1.1;                    // |S_out| <= k max(|S|, |g S_w|)
	constexpr double kCameraMinConfidence = 0.5;         // the model ignores a camera view below this

	inline double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline double Smoothstep(double e0, double e1, double x)
	{
		const double t = Clamp((x - e0) / (e1 - e0), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	// Frames of a duration at the measured content rate (round, minimum 1). hz <= 0: the reference rate.
	inline int Frames(double seconds, double hz)
	{
		if (!(hz > 0.0)) hz = kReferenceHz;
		const double n = std::floor(seconds * hz + 0.5);
		return n < 1.0 ? 1 : static_cast<int>(n);
	}

	// GrainScale (vprenderer/GrainPreserve.h): 1 at <= 2160 lines, lines / 2160 above.
	inline double Scale(double planeLines) { return planeLines > 2160.0 ? planeLines / 2160.0 : 1.0; }

	enum class Mode { Off, Steady };

	struct Settings
	{
		Mode mode = Mode::Off;
		double strength = 1.0;       // 0 .. 1
		double maxAgeSeconds = 2.0;  // 0.25 .. 4
		double boostMax = 0.15;      // 0 .. 0.3
	};

	inline std::string ToLower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	inline bool ParseNumber(const std::string& text, double lo, double hi, double& out)
	{
		if (text.empty()) return false;
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		if (end == text.c_str() || *end != '\0' || !(v >= lo && v <= hi)) return false;   // NaN fails
		out = v;
		return true;
	}

	// Keys temporal_detail (off|steady), temporal_detail_strength (0..1), temporal_detail_max_age (0.25..4),
	// temporal_detail_boost_max (0..0.3). An invalid value adds a message to errors and keeps the previous
	// value of inOut; unknown keys are ignored. Returns true when every present key was valid.
	inline bool ParseSettings(const std::map<std::string, std::string>& kv, Settings& inOut, std::vector<std::string>* errors)
	{
		bool ok = true;
		auto fail = [&](const std::string& key, const std::string& why)
		{
			ok = false;
			if (errors) errors->push_back(key + ": " + why);
		};
		auto it = kv.find("temporal_detail");
		if (it != kv.end())
		{
			const std::string v = ToLower(it->second);
			if (v == "off") inOut.mode = Mode::Off;
			else if (v == "steady") inOut.mode = Mode::Steady;
			else fail(it->first, "expected off or steady, got '" + it->second + "'");
		}
		auto number = [&](const char* key, double lo, double hi, double& target)
		{
			auto f = kv.find(key);
			if (f == kv.end()) return;
			double v = 0.0;
			if (ParseNumber(f->second, lo, hi, v)) target = v;
			else fail(key, "expected a number in " + std::to_string(lo) + " .. " + std::to_string(hi) + ", got '" + f->second + "'");
		};
		number("temporal_detail_strength", 0.0, 1.0, inOut.strength);
		number("temporal_detail_max_age", 0.25, 4.0, inOut.maxAgeSeconds);
		number("temporal_detail_boost_max", 0.0, 0.3, inOut.boostMax);
		return ok;
	}

	// --- Indicator stamps (README "Indicator stamps and resets"; design doc 25 section A2) ---
	// The host fills one StampView per frame (today from existing counters, later from core/Indicator).
	struct StampView
	{
		uint64_t full = 0;           // FULL resync counter
		uint64_t continuity = 0;     // a slip/gap: pairs spanning it are invalid
		uint64_t rebaseH = 0;        // re-base generation, horizontal
		uint64_t rebaseV = 0;        // re-base generation, vertical
		uint64_t settingsRepair = 0; // settings[repair] domain generation
		bool operator==(const StampView& o) const
		{
			return full == o.full && continuity == o.continuity && rebaseH == o.rebaseH && rebaseV == o.rebaseV && settingsRepair == o.settingsRepair;
		}
		bool operator!=(const StampView& o) const { return !(*this == o); }
	};

	// What a reader does when a counter changes (design doc 25 A7).
	enum class ReaderPolicy { Reset, Hold, Absorb, Relatch, Crossfade, Ignore };
	enum class Counter { Full, Continuity, RebaseH, RebaseV, SettingsRepair, Timing };

	struct CounterPolicy { Counter counter; ReaderPolicy policy; const char* name; const char* why; };

	// Declared reader policies. The store lives in re-based coordinates and pairs spanning a slip are invalid,
	// so full, continuity and both re-base axes reset. settings[repair] is absorbed: the model's
	// UpdateSettings resets only for a mode or max_age change (strength and boost change the target only).
	// timing is ignored (the model counts frames at the measured content rate).
	constexpr CounterPolicy kCounterPolicies[] = {
		{ Counter::Full, ReaderPolicy::Reset, "full", "all state restarts" },
		{ Counter::Continuity, ReaderPolicy::Reset, "continuity", "composed pairs spanning a slip are invalid" },
		{ Counter::RebaseH, ReaderPolicy::Reset, "rebaseH", "the store lives in re-based coordinates" },
		{ Counter::RebaseV, ReaderPolicy::Reset, "rebaseV", "the store lives in re-based coordinates" },
		{ Counter::SettingsRepair, ReaderPolicy::Absorb, "settings[repair]", "UpdateSettings decides: mode or max_age reset, strength or boost do not" },
		{ Counter::Timing, ReaderPolicy::Ignore, "timing", "frame counts follow the measured content rate" },
	};

	constexpr ReaderPolicy PolicyOf(Counter c)
	{
		for (const CounterPolicy& p : kCounterPolicies)
			if (p.counter == c) return p.policy;
		return ReaderPolicy::Ignore;
	}
}
