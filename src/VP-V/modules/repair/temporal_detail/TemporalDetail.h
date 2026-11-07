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
	constexpr int kSlots = 5;                   // K past slots (one more, the future slot, in queued modes)
	constexpr uint32_t kUnitVersion = 1;        // THIS module's code is still v1 (td.analysis / td.refstore / td.admission layouts, 5 slots, ~415 MB at 4K); v2 is the doc 72 contract in repair/temporal_detail/products (TdProducts::kUnitVersion, the catalog's kExpectedTemporalDetailUnit) and is raised here only when the v2 engine replaces this code. The v1 GPU module (TemporalDetailGpu) is not wired into the renderer today (no call site outside its own tests).
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
	constexpr double kPixelCap = 1.1;                    // |S_out| <= k max(|S|, |T|)
	// Grain-free transfer (step 8): the reference template is the q-weighted average of every valid slot's clean
	// structure (warped once each); the current frame's structure enters only as its per-band projection onto it.
	constexpr double kTemplateMinHard = 0.5;             // a non-selected slot joins the template when its hard gates reach this
	constexpr double kCoherenceLo = 0.3, kCoherenceHi = 0.6;   // per-band corr(S_cur, T): misaligned / doubled bands pass nothing
	constexpr double kGrainBudget = 0.08;                // template grain carried into the output <= this x the current grain RMS (KeepsGrain low band binds; 0.085 is the edge)
	// Multi-frame template cleaner (step 4a): at admission, a slot's grain-free bands are cleaned by a motion-aligned
	// spatio-temporal Wiener in local DCT patches; the other valid slots (already warped for the template) are the
	// coherent evidence. Grain is temporally independent, structure coherent along motion.
	constexpr double kCleanPatchPx = 8.0;                // DCT patch edge at <= 2160 lines (x Scale, even), sine window, step half a patch
	constexpr double kCleanKappa = 4.0;                  // Wiener gain P / (P + kappa V) (2 .. 8 within 0.2 dB; 4 best at the strict bounds)
	constexpr double kCleanMemberMin = 0.5;              // a slot joins the evidence where its alignment gates (motion, base match, correlation) reach this
	constexpr double kBoostGrainShare = 0.04;            // fallback boost: the current grain is scaled by at most 1 + this
	// Output-vs-input grain guard (step 10): tile statistics on the shared grain map, pooled over 3 x 3 blocks
	constexpr double kGuardStatsSeconds = 0.25;          // EMA time constant of the tile sums
	constexpr double kGuardDropTrip = 0.04;              // current-grain loss (projection or level) that trips
	constexpr double kGuardRiseTrip = 0.05;              // output grain level above the input's that trips
	constexpr double kGuardFreezeTrip = 0.03;            // lag-1 grain correlation increase that trips (0.05 before the structure-transfer exclusion)
	constexpr double kGuardRelease = 0.5;                // hysteresis: released below this share of the trip level
	// Per-block gate factors (FrameIn::blockGates / Gpu::Params::blockGates), sanitised identically on the CPU model and the
	// GPU (merge audit 2026-11-01): a non-finite factor is 1; the three threshold factors are bounded to
	// [kBlockGateMin, kBlockGateMax] (a 0 would make a smoothstep edge pair equal); the strength factor to [0, 1] (never above
	// the user's strength, so no lift past the grain-side cap).
	constexpr double kBlockGateMin = 0.25, kBlockGateMax = 4.0;
	constexpr int kRunSaturate = 1 << 20;       // softRun / vetoRun saturate here (model and shader alike; keeps the float state exact)
	inline double SanitizeBlockGate(double v, int k)
	{
		if (!(v == v) || v > 1e30 || v < -1e30) return 1.0;
		const double lo = k == 3 ? 0.0 : kBlockGateMin, hi = k == 3 ? 1.0 : kBlockGateMax;
		return v < lo ? lo : (v > hi ? hi : v);
	}
	constexpr double kGuardFallSeconds = 0.1, kGuardRiseSeconds = 2.0;   // guard factor rate limits (slow recovery: no trip/release cycling under damage)
	constexpr double kGuardAddBackMin = 0.01;            // measured current-grain loss below this is not added back
	constexpr double kGuardMaxShare = 0.2;               // guard statistics use grain-dominated pixels only (Wiener share f below this)
	constexpr double kGuardTemplateShare = 0.25;         // the guard ignores the template's structure transfer where T^2 > this x the grain level N
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

	// The active picture (the renderer's trusted black-bar / picture rect, scaled to this plane; right/bottom exclusive).
	// Everything (band splits, statistics, gates, templates, transfer, boost, cleaner, grain guard and add-back) runs
	// inside it with its border treated as the plane edge (taps clamp there, never reach into a bar); pixels outside are
	// copied unchanged. Empty (right <= left or bottom <= top) = the whole plane.
	struct ActiveRect
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		bool operator==(const ActiveRect& o) const { return left == o.left && top == o.top && right == o.right && bottom == o.bottom; }
		bool operator!=(const ActiveRect& o) const { return !(*this == o); }
		// Clamped to a w x h plane; the whole plane when empty or degenerate.
		ActiveRect Resolve(int w, int h) const
		{
			ActiveRect r;
			r.left = (std::max)(0, (std::min)(w, left)); r.right = (std::max)(0, (std::min)(w, right));
			r.top = (std::max)(0, (std::min)(h, top)); r.bottom = (std::max)(0, (std::min)(h, bottom));
			if (r.right <= r.left || r.bottom <= r.top) { r.left = 0; r.top = 0; r.right = w; r.bottom = h; }
			return r;
		}
		int Width() const { return right - left; }
		int Height() const { return bottom - top; }
	};

	// off; steady (always on); auto (on only while the source-ID consensus says the stream is low quality: AutoGate).
	enum class Mode { Off, Steady, Auto };

	struct Settings
	{
		Mode mode = Mode::Off;
		double strength = 1.0;       // 0 .. 1
		double maxAgeSeconds = 2.0;  // 0.25 .. 4
		bool boost = false;          // the fallback boost (owner 2026-10-31: off unless re-validated on real encodes)
		double boostMax = 0.15;      // 0 .. 0.3 (used when boost is on)
	};

	// The gate thresholds real B-frames will re-tune (owner 2026-10-31): defaults are the model's constants.
	struct GateTuning
	{
		double strongerMargin = kStrongerMargin;     // reference stronger: E1(S_w) > (1 + m) E1(S)
		double significanceLo = kSignificanceLo;     // reference fine energy over the grain level: zero at
		double significanceHi = kSignificanceHi;     // ... full at
		double baseNoiseCodes = kBaseNoiseCodes;     // base match allowance: coding noise ...
		double baseGrainSigmas = kBaseGrainSigmas;   // ... plus this many grain RMS (of the base band: / 4)
		double uncertaintyPx = kUncertaintyPx;       // motion uncertainty limit (x Scale)
	};

	// AUTO mode (temporal_detail = auto): engages only while the source-ID consensus (input.source_analysis identity,
	// SourceIdConsensus Result.value.qualityTier: 0 = lowest .. 3, -1 = unknown; already latched with its own dwell) says
	// the stream is low quality, with hysteresis in time on top: engage after the tier has been <= engageTier for
	// engageSeconds, release after it has been above engageTier (or unknown) for releaseSeconds. Frame counts follow the
	// measured content rate. Status() is the visible line ("auto: on (stream quality tier 1)", ...).
	class AutoGate
	{
	public:
		struct Params
		{
			int engageTier = 1;            // tiers 0..1 = bitrate quality below 0.5 (SourceIdVote qualityTierEdges)
			double engageSeconds = 1.0;
			double releaseSeconds = 3.0;
		};
		AutoGate() = default;
		explicit AutoGate(const Params& p) : m_p(p) {}
		// One presented source frame; returns whether temporal detail runs on it.
		bool Update(int qualityTier, double hz)
		{
			const bool low = qualityTier >= 0 && qualityTier <= m_p.engageTier;
			m_tier = qualityTier;
			if (low == m_on) m_run = 0;
			else if (++m_run >= Frames(m_on ? m_p.releaseSeconds : m_p.engageSeconds, hz)) { m_on = low; m_run = 0; }
			return m_on;
		}
		bool On() const { return m_on; }
		void Reset() { m_on = false; m_run = 0; m_tier = -1; }
		std::string Status() const
		{
			if (m_tier < 0) return m_on ? "auto: on (stream quality unknown, releasing)" : "auto: idle (no stream quality estimate)";
			const std::string t = std::to_string(m_tier);
			if (m_on) return "auto: on (stream quality tier " + t + (m_tier > m_p.engageTier ? ", releasing)" : ")");
			return "auto: idle (stream quality tier " + t + (m_tier <= m_p.engageTier ? ", engaging)" : ")");
		}

	private:
		Params m_p;
		bool m_on = false;
		int m_run = 0, m_tier = -1;
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

	// Keys temporal_detail (off|steady|auto), temporal_detail_strength (0..1), temporal_detail_max_age (0.25..4),
	// temporal_detail_boost (on|off, default off), temporal_detail_boost_max (0..0.3). An invalid value adds a message to errors and keeps the previous
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
			else if (v == "auto") inOut.mode = Mode::Auto;
			else fail(it->first, "expected off, steady or auto, got '" + it->second + "'");
		}
		it = kv.find("temporal_detail_boost");
		if (it != kv.end())
		{
			const std::string v = ToLower(it->second);
			if (v == "on") inOut.boost = true;
			else if (v == "off") inOut.boost = false;
			else fail(it->first, "expected on or off, got '" + it->second + "'");
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
