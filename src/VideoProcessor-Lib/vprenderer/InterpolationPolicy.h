#pragma once

#include <vprenderer/DejudderPolicy.h>

#include <algorithm>
#include <cmath>
#include <string>

// VP's 50/60p frame interpolation: when the HDR optical-flow interpolator runs
// for a source (docs/VP-interpolator-50-60-plan.md build order step 2). Pure, so
// the rules are unit-tested without a GPU.
//
// Everything is derived from the measured rates (owner [[vp-general-not-fixed]]):
// the content rate (the pulldown film clock when locked, else the measured
// capture rate) and the measured display rate. No source rate, display rate,
// cadence or resolution is fixed here; only ratios and tolerances.
//
// The rules (owner decisions 2026-10-16, defaults; confirmation open, see the plan):
// - interpolation off by default. auto = only at a non-integer source/display
//   ratio (50p on 60 Hz); always = smooth-motion parity for video-class sources
//   (also integer ratios above 1, e.g. 30p video on 60 Hz).
// - Film class stays with the de-judder: a film-like source the de-judder has a
//   strength for is never interpolated. A film-like source the de-judder leaves
//   alone (no strength for its cadence) may be interpolated in auto at a
//   non-integer ratio (25p inside a 50 Hz stream on a 60 Hz display, through
//   the pulldown film clock), never at an integer ratio.
// - Integer ratios pass through untouched in auto (50/60 live sports on 50/60 Hz:
//   no smoothing, no de-judder).
// - Near-1 ratios (59.94 on 60) count as integer: they are left to drift
//   correction (kRatioTolerance).
// - Low latency, libplacebo's smooth motion (exclusive engines), interlaced
//   input, RGB captures and subtitle-composed pictures: no interpolation (the
//   refresh shows the source frame, today's output).
// - No next frame queued (a zero queue): nothing to interpolate towards - A is
//   shown, today's output; never wait for B.
namespace InterpolationPolicy
{
	enum class Mode { Off, Auto, Always };
	enum class Fallback { Hold, Blend };            // where vectors are untrusted
	enum class SourceClass { Legacy, Measured };    // the film / video classification

	inline Mode ParseMode(const std::string& v)
	{
		return v == "auto" ? Mode::Auto : v == "always" ? Mode::Always : Mode::Off;
	}
	inline const char* Name(Mode m)
	{
		return m == Mode::Auto ? "auto" : m == Mode::Always ? "always" : "off";
	}
	inline Fallback ParseFallback(const std::string& v) { return v == "blend" ? Fallback::Blend : Fallback::Hold; }
	inline const char* Name(Fallback f) { return f == Fallback::Blend ? "blend" : "hold"; }
	// Film grain (owner 2026-10-17: grain is never averaged away; docs/audit/grain-audit-2026-10-17.md):
	// preserve = only the low band of the warped pair is blended by phase, the high band comes from
	// the nearer frame (Catmull-Rom), energy-matched per 16x16 block; blend = the pre-2026-10-17
	// bidirectional blend of the whole signal (byte for byte).
	enum class Grain { Preserve, Blend };
	inline Grain ParseGrain(const std::string& v) { return v == "blend" ? Grain::Blend : Grain::Preserve; }
	inline const char* Name(Grain g) { return g == Grain::Blend ? "blend" : "preserve"; }
	inline SourceClass ParseSourceClass(const std::string& v)
	{
		return v == "measured" ? SourceClass::Measured : SourceClass::Legacy;
	}
	inline const char* Name(SourceClass c) { return c == SourceClass::Measured ? "measured" : "legacy"; }

	// A ratio within this share of a whole number counts as that whole number:
	// covers the 1000/1001 family (0.1%) and measured clock drift; anything
	// further from a whole number is a real rate mismatch (48 on 50 = 4%).
	constexpr double kRatioTolerance = 0.005;

	// Refreshes per source frame (display / source).
	inline double Ratio(double sourceHz, double displayHz)
	{
		return sourceHz > 0.0 && displayHz > 0.0 ? displayHz / sourceHz : 0.0;
	}

	// The nearest whole number n when the ratio is within the tolerance of it
	// (relative to n), -n when the ratio is within the tolerance of 1/n (n >= 2:
	// a source n times the display rate, shown by decimation), else 0. 60p on
	// 50 Hz (0.833) and 50p on 60 Hz (1.2) are not whole; 59.94p on 60 Hz is 1.
	inline int WholeRatio(double ratio)
	{
		if (!(ratio > 0.0))
			return 0;
		const bool below = ratio < 1.0;
		const double r = below ? 1.0 / ratio : ratio;
		const double n = std::round(r);
		if (!(n >= 1.0) || std::abs(r - n) > kRatioTolerance * n)
			return 0;
		return below && n >= 2.0 ? -static_cast<int>(n) : static_cast<int>(n);
	}

	// Film-like: legacy = DejudderPolicy::IsFilmLike (measured rates up to about
	// 30 fps and 48p HFR; fixed rates, kept as the default for existing files).
	// measured, classFixes off (the 2026-10-17 behaviour): content recovered
	// from a pulldown inside a faster signal is film; otherwise a source that
	// owns at least two refreshes per frame (minus the de-judder's cadence
	// tolerance) is film-like, anything faster is video.
	// Hunt 2 (cadence_class_fixes, 06-interp M2 / 15-intent C4): the class comes
	// from the source alone, never from the display ratio - the display rate
	// made 24p@30 and 30p@50 video (interpolated) and 60p@120 film. measured
	// with the fixes classifies the measured content rate (the pulldown film
	// rate when locked, else the capture rate) by the owner's film / video split
	// (DejudderPolicy::IsFilmLike: 24/25/30p and 48p film, 50/60p video on any
	// display); a lock alone is not film (50p carried in 120 Hz stays video).
	inline bool IsFilmLike(SourceClass sourceClass, double sourceHz, double displayHz, bool pulldownLocked,
		bool classFixes = true)
	{
		if (sourceClass == SourceClass::Legacy || classFixes)
			return DejudderPolicy::IsFilmLike(sourceHz);
		if (pulldownLocked)
			return true;
		const double ratio = Ratio(sourceHz, displayHz);
		return ratio >= 2.0 - DejudderPolicy::kCadenceTolerance;
	}

	enum class Reason
	{
		Interpolate,       // the interpolator fills the refreshes between source frames
		Off,               // interpolation off
		SmoothMotion,      // libplacebo's smooth motion is the engine (exclusive)
		LowLatency,        // the low-latency queue: no frame ahead to interpolate towards
		Interlaced,        // interlaced input
		UnknownRates,      // a rate not measured yet
		FilmToDejudder,    // a film-like source the de-judder handles
		FilmIntegerRatio,  // a film-like source at a whole ratio (the even cadence's own business)
		IntegerRatio,      // a whole ratio (or near 1): passes through untouched
		NativeRgb,         // an RGB capture: hold (owner)
		SubtitleComposed,  // a subtitle-composed picture: hold (owner)
		RatioBelowOne,     // more source frames than refreshes: needs per-vsync synthesis (Stage B, not built)
		NoNextFrame,       // B not queued (a zero queue): A, today's output
	};

	inline const char* Name(Reason r)
	{
		switch (r)
		{
		case Reason::Interpolate: return "interpolating";
		case Reason::Off: return "off";
		case Reason::SmoothMotion: return "smooth motion is the engine";
		case Reason::LowLatency: return "low-latency queue";
		case Reason::Interlaced: return "interlaced input";
		case Reason::UnknownRates: return "rates not known";
		case Reason::FilmToDejudder: return "film-like source: the de-judder";
		case Reason::FilmIntegerRatio: return "film-like source at a whole ratio";
		case Reason::IntegerRatio: return "whole ratio: passes through";
		case Reason::NativeRgb: return "RGB capture (hold)";
		case Reason::SubtitleComposed: return "subtitle-composed picture (hold)";
		case Reason::RatioBelowOne: return "source faster than the display (per-vsync synthesis not built)";
		case Reason::NoNextFrame: return "next frame not queued";
		}
		return "?";
	}

	struct Input
	{
		Mode mode = Mode::Off;
		SourceClass sourceClass = SourceClass::Legacy;
		double sourceHz = 0.0;        // the content rate: the pulldown film rate when locked, else measured capture
		double displayHz = 0.0;       // the measured display rate
		bool pulldownLocked = false;  // the content rate came from a pulldown lock
		// The de-judder's strengths (DejudderPolicy): it takes a film-like source
		// whose cadence it has a strength for.
		double dejudderStrength = 0.0;
		double dejudderStrengthEven = 0.0;
		bool dejudderEvenIncludes1001 = false;
		bool smoothMotion = false;    // libplacebo's smooth motion engine selected or mixing
		bool lowLatency = false;
		bool interlaced = false;
		bool nativeRgb = false;
		bool subtitleComposed = false;
		bool haveNext = false;        // B queued and usable
		bool perVsyncAvailable = false;  // Stage B (ratios below 1) built and enabled
		// Hunt 2 (cadence_class_fixes, default on): the source-only class (M2) and
		// film near a 1:1 ratio left to drift correction (L5).
		bool classFixes = true;
	};

	struct Decision
	{
		Reason reason = Reason::Off;
		double ratio = 0.0;           // refreshes per source frame
		bool filmLike = false;
		bool perVsync = false;        // ratio below 1: per-vsync synthesis (Stage B)
		bool Interpolates() const { return reason == Reason::Interpolate; }
	};

	// Whether the de-judder takes this source (a film-like source with a
	// strength for its cadence): the same rule as DejudderPolicy::EffectiveStrength.
	inline bool DejudderTakes(const Input& in)
	{
		DejudderPolicy::Input d;
		d.strength = in.dejudderStrength;
		d.strengthEven = in.dejudderStrengthEven;
		d.evenIncludes1001 = in.dejudderEvenIncludes1001;
		d.sourceHz = in.sourceHz;
		d.displayHz = in.displayHz;
		return DejudderPolicy::EffectiveStrength(d) > 0.0;
	}

	// The order is the order of the checks: the first that fails is the reason.
	inline Decision Decide(const Input& in)
	{
		Decision d;
		d.ratio = Ratio(in.sourceHz, in.displayHz);
		if (in.mode == Mode::Off) { d.reason = Reason::Off; return d; }
		if (in.smoothMotion) { d.reason = Reason::SmoothMotion; return d; }
		if (in.lowLatency) { d.reason = Reason::LowLatency; return d; }
		if (in.interlaced) { d.reason = Reason::Interlaced; return d; }
		if (!(in.sourceHz > 0.0) || !(in.displayHz > 0.0)) { d.reason = Reason::UnknownRates; return d; }
		d.filmLike = IsFilmLike(in.sourceClass, in.sourceHz, in.displayHz, in.pulldownLocked, in.classFixes);
		if (d.filmLike && DejudderTakes(in)) { d.reason = Reason::FilmToDejudder; return d; }
		// Film at a whole cadence (the de-judder's own tolerance): its even cadence, never interpolated.
		if (d.filmLike && DejudderPolicy::IsEvenCadence(d.ratio)) { d.reason = Reason::FilmIntegerRatio; return d; }
		// Hunt 2 (06-interp L5): film within the de-judder's cadence tolerance of 1:1 (24p on
		// 25 Hz, 25p on 25.6) is drift correction's, as near-1 video is - the relative
		// kRatioTolerance below would otherwise send it to full interpolation.
		if (in.classFixes && d.filmLike && std::abs(d.ratio - 1.0) <= DejudderPolicy::kCadenceTolerance)
		{
			d.reason = Reason::FilmIntegerRatio;
			return d;
		}
		const int whole = WholeRatio(d.ratio);
		// 1:1 or near it (drift correction), or a whole decimation (120p on 60 Hz).
		if (whole == 1 || whole <= -2) { d.reason = Reason::IntegerRatio; return d; }
		if (whole >= 2)
		{
			// Always: smooth-motion parity for video; film keeps its even cadence.
			if (d.filmLike) { d.reason = Reason::FilmIntegerRatio; return d; }
			if (in.mode != Mode::Always) { d.reason = Reason::IntegerRatio; return d; }
		}
		d.perVsync = d.ratio < 1.0;
		if (d.perVsync && !in.perVsyncAvailable) { d.reason = Reason::RatioBelowOne; return d; }
		if (in.nativeRgb) { d.reason = Reason::NativeRgb; return d; }
		if (in.subtitleComposed) { d.reason = Reason::SubtitleComposed; return d; }
		if (!in.haveNext) { d.reason = Reason::NoNextFrame; return d; }
		d.reason = Reason::Interpolate;
		return d;
	}

	// The phase of an output time between A and B: 0 at A, 1 at B (clamped).
	// Times in seconds on one clock; period = B - A (one source period).
	inline double Phase(double outputPts, double ptsA, double period)
	{
		if (!(period > 0.0))
			return 0.0;
		return (std::min)(1.0, (std::max)(0.0, (outputPts - ptsA) / period));
	}

	// Stage A's timeline: the k-th refresh after A's own (k = 1, 2, ...) is
	// still A's when its phase is below 1; it shows the interpolated picture at
	// that phase instead of repeating A. phase0 is A's own refresh's phase
	// (0 .. 1/ratio). Returns -1 when the refresh belongs to B.
	inline double RefreshPhase(double phase0, int k, double ratio)
	{
		if (!(ratio > 0.0) || k < 0)
			return -1.0;
		const double phase = phase0 + static_cast<double>(k) / ratio;
		return phase < 1.0 ? (std::max)(0.0, phase) : -1.0;
	}

	// The refreshes Stage A fills per source frame on average: the repeats it
	// replaces (ratio - 1 per frame for ratios above 1, 0 otherwise).
	inline double FilledPerFrame(double ratio)
	{
		return ratio > 1.0 ? ratio - 1.0 : 0.0;
	}
}
