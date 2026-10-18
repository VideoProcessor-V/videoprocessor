#pragma once

#include <string>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "CaptureTimestampGap.h"
#include <VideoFrameEncoding.h>

// Stage 6b: when the de-judder may run for a source frame, and (6b-2) whether
// one more extra present may fill the next refresh. Pure logic, so the
// renderer's rules are unit-tested without a GPU. "Off" (any reason other
// than Active) means today's path: no next-frame upload, no motion estimate,
// no extra presents.
namespace DejudderPolicy
{
	enum class Reason
	{
		Active,
		Off,              // dejudder_strength 0 (or absent)
		LowLatency,       // the Low_Latency queue is active
		Interlaced,       // an interlaced input mode (field rate unclear)
		UnknownRates,     // source or display rate not known yet
		IneligibleCadence, // no strength for this cadence, under 2 refreshes per frame, or a video-rate source
		VideoRateMaskOnly, // mask / grain map alone on a video-rate source (planner 6c, 8-3)
		NativeRgb,        // an RGB capture: no YCbCr planes to warp
		PlaneFormat,      // A's planes cannot be sampled linearly / stored
		NoNextFrame,      // B not queued or not converted yet
		NextFrameDiffers, // B has a different size, layout or video state
		SmoothMotion,     // libplacebo's smooth motion (frame mixing, v1.4.02) is active
	};

	struct Input
	{
		double strength = 0.0;
		// Delta 10 (docs/VP-delta-8-9-10.md), made general (owner 2026-10-14):
		// the strength for even cadences (a whole number of refreshes per
		// film frame), 0 = off; and whether sources at a 1000/1001 rate (23.976,
		// 29.97, 47.952: broadcast-derived) take it besides whole-number rates.
		double strengthEven = 0.0;
		bool evenIncludes1001 = false;
		bool occlusionMask = false;   // 6c: the estimate also runs for the mask alone
		// 8-3: the adaptive downscale wants the grain map. It never starts the
		// estimate on its own (owner 2026-10-14: vector users take the motion
		// when it is there and fall back otherwise); kept for the reason text.
		bool grainMap = false;
		bool lowLatency = false;
		bool interlaced = false;
		double sourceHz = 0.0;
		double displayHz = 0.0;
		bool nativeRgb = false;
		bool haveNext = false;
		bool nextMatches = false;
		bool planeFormatsUsable = true;
		// v1.4.02 merge (owner 2026-10-15): libplacebo's smooth motion is mixing
		// frames (never with the de-judder configured, see Configured; this is
		// the safety net). The de-judder, its mask and the estimate step aside;
		// with no estimate every vector consumer takes its no-vector fallback.
		bool smoothMotion = false;
		// Fixed-constant batch 2026-10-18 (general_rate_fixes, default on): the film / video
		// class from the derived rule (IsFilmLike(sourceHz, true)); off = the fixed bands.
		bool generalRates = true;
	};

	// Refreshes per source frame (P_s / P_d).
	inline double CadenceRatio(double sourceHz, double displayHz)
	{
		return sourceHz > 0.0 ? displayHz / sourceHz : 0.0;
	}

	// Everything here is general (owner 2026-10-14): no source rate, display
	// rate or cadence is fixed; only the ratio and the source's class count.
	//
	// The source class: film-like sources are the measured rates up to about
	// 30 fps and 48p HFR film; 50p and 60p (sport, video) and anything else
	// are video, never de-juddered on any display (owner).
	//
	// Legacy (general_rate_fixes off): fixed bands, up to 30.5 Hz and 47.5-48.5 Hz.
	inline bool IsFilmLike(double sourceHz)
	{
		return (sourceHz > 0.0 && sourceHz <= 30.5) || (sourceHz >= 47.5 && sourceHz <= 48.5);
	}

	// Fixed-constant batch 2026-10-18 (general_rate_fixes, owner [[vp-general-not-fixed]]):
	// the class derived from the measured content rate and one standard constant, with no
	// list of film rates. Video starts at the lowest broadcast video picture rate - 50 Hz,
	// the 625-line / 50 Hz family (the 525-line family, 60000/1001, is higher) - less a
	// relative measurement tolerance. Every slower measured rate is film-like (24, 25, 30,
	// 47.952, 48 and their 1000/1001 forms, as before, and also the odd rates between
	// 30.5 and 47.5 that the fixed bands called video); 49 Hz and up (50, 59.94, 60, 100,
	// 119.88, 120, 144, 240) is video on any display, as before. The 2% tolerance is far
	// wider than clock drift or the 1000/1001 offset (0.1%) and narrower than the gap
	// down to 48 (4%).
	constexpr double kVideoRateFloorHz = 50.0;          // standard: the 50 Hz television family
	constexpr double kVideoRateFloorTolerance = 0.02;   // relative measured-rate tolerance
	inline double VideoRateFloorHz()
	{
		return kVideoRateFloorHz * (1.0 - kVideoRateFloorTolerance);   // 49 Hz
	}
	inline bool IsFilmLike(double sourceHz, bool generalRates)
	{
		if (!generalRates)
			return IsFilmLike(sourceHz);
		return sourceHz > 0.0 && sourceHz < VideoRateFloorHz();   // false for NaN
	}

	// +-0.05 refreshes absorbs the 1000/1001 rates and measured drift.
	constexpr double kCadenceTolerance = 0.05;

	// Uneven cadence (planner 6b-2 item 4 and 6b-2a, general): more than one
	// refresh per film frame and not a whole number - 24p on 60 Hz = 2.5,
	// 25p on 60 = 2.4, 24p on 100 = 4.17, and (owner 2026-10-14) under 2 as
	// well: 24p on 30 Hz = 1.25 (the frames owning a second refresh get an
	// extra at 0.8), 25p on 30 = 1.2, 20p on 30 = 1.5. Video is excluded by
	// the source class (IsFilmLike), not by the ratio.
	inline bool IsUnevenCadence(double ratio)
	{
		if (!(ratio > 1.0 + kCadenceTolerance))
			return false;
		return std::abs(ratio - std::round(ratio)) > kCadenceTolerance;
	}

	// Even cadence (delta 10, general): a whole number N >= 2 of refreshes per
	// film frame (30p on 60, 25p on 50, 24p on 48, 24p on 120, 30p on 120).
	// 0 when not even. Its extras sit at the exact phases k/N.
	inline int EvenCycle(double ratio)
	{
		const double n = std::round(ratio);
		return n >= 2.0 && std::abs(ratio - n) <= kCadenceTolerance ? static_cast<int>(n) : 0;
	}
	inline bool IsEvenCadence(double ratio) { return EvenCycle(ratio) != 0; }

	// A source at a 1000/1001 rate (23.976, 29.97, 47.952): measured within
	// 0.05% of whole-number x 1000/1001 and not of a whole number itself.
	inline bool IsRate1001(double sourceHz)
	{
		if (!(sourceHz > 0.0))
			return false;
		const double whole = std::round(sourceHz);
		const double family = std::round(sourceHz * 1.001);
		return std::abs(sourceHz - whole) > whole * 0.0003 &&
			std::abs(sourceHz - family * 1000.0 / 1001.0) <= family * 0.0005;
	}

	// The source rates the even strength applies to: whole-number rates, and
	// the 1000/1001 rates when chosen (dejudder_even_rates).
	inline bool EvenRateSelected(double sourceHz, bool includes1001)
	{
		return includes1001 || !IsRate1001(sourceHz);
	}

	// The strength the extras run at: dejudder_strength on an uneven cadence,
	// dejudder_strength_even on a selected even cadence, else 0; only for a
	// film-like source.
	inline double EffectiveStrength(const Input& in)
	{
		if (!IsFilmLike(in.sourceHz, in.generalRates))
			return 0.0;
		const double ratio = CadenceRatio(in.sourceHz, in.displayHz);
		if (IsUnevenCadence(ratio))
			return in.strength;
		if (IsEvenCadence(ratio) && EvenRateSelected(in.sourceHz, in.evenIncludes1001))
			return in.strengthEven;
		return 0.0;
	}

	// The most extras one film frame may have: the refreshes it owns minus
	// its own (24p on 60 = 2, on 120 = 4, 25p on 120 = 4); PlanExtra's timing
	// stops earlier when the next frame is due.
	inline int MaxExtras(double ratio)
	{
		if (!(ratio > 1.0))
			return 0;
		const int owned = static_cast<int>(std::ceil(ratio - kCadenceTolerance));
		return std::min(std::max(owned - 1, 0), 15);
	}

	// The order is the order of the checks: the first that fails is the
	// reason reported.

	// Whether the motion estimate runs. It runs for the de-judder (strength
	// > 0, uneven film cadence) and, with the occlusion mask or the grain
	// map (8-3) wanting it, alone on any film-rate cadence (strength 0, no
	// extras).
	inline Reason Decide(const Input& in)
	{
		const bool alone = in.occlusionMask;   // the grain map never starts it alone
		if (!(in.strength > 0.0) && !(in.strengthEven > 0.0) && !alone) return Reason::Off;
		if (in.smoothMotion) return Reason::SmoothMotion;
		if (in.lowLatency) return Reason::LowLatency;
		if (in.interlaced) return Reason::Interlaced;
		if (!(in.sourceHz > 0.0) || !(in.displayHz > 0.0)) return Reason::UnknownRates;
		const bool extras = EffectiveStrength(in) > 0.0;
		if (!extras && !alone)
			return Reason::IneligibleCadence;
		// Without extras the estimate runs for the mask / map alone: film-like sources only.
		if (!extras && !IsFilmLike(in.sourceHz, in.generalRates))
			return Reason::VideoRateMaskOnly;
		if (in.nativeRgb) return Reason::NativeRgb;
		if (!in.planeFormatsUsable) return Reason::PlaneFormat;
		if (!in.haveNext) return Reason::NoNextFrame;
		if (!in.nextMatches) return Reason::NextFrameDiffers;
		return Reason::Active;
	}

	// v1.4.02 merge (owner 2026-10-15): de-judder and libplacebo's smooth
	// motion are mutually exclusive options. A profile with the de-judder
	// configured (either strength above 0) never runs smooth motion; Config
	// switches one off when the other is turned on.
	inline bool Configured(double strength, double strengthEven)
	{
		return strength > 0.0 || strengthEven > 0.0;
	}

	// The motion engine for this profile (motion_engine): vp, smooth_motion,
	// or auto = whichever is configured, VP's when both (existing files keep
	// their behaviour: the de-judder where it is set, smooth motion where only
	// it is). The 50/60p work joins VP's side (vpConfigured).
	enum class MotionEngine { Vp, SmoothMotion };
	inline MotionEngine ResolveEngine(const std::string& setting, bool vpConfigured, bool smoothConfigured)
	{
		if (setting == "vp") return MotionEngine::Vp;
		if (setting == "smooth_motion") return MotionEngine::SmoothMotion;
		return vpConfigured || !smoothConfigured ? MotionEngine::Vp : MotionEngine::SmoothMotion;
	}

	// Bug hunt 2026-10-17: on the Dolby Vision tunnel a frame is a trusted
	// DV picture (valid packet, not marked for black), an SDR fallback (no
	// valid packet while the tunnel is unconfirmed: allowSdrFallback, with
	// the RGB8 placement known) or
	// untrusted (presented as black). The next frame B pairs with A only when
	// both are the same trusted kind: never an untrusted B, never DV with SDR.
	// Integration 2026-10-17: an SDR fallback is a picture only where the render shows
	// it - r210 (sdrFallbackRenders). On R10b / R10l the tunnel's SDR fallback renders
	// black, so it is untrusted there.
	enum class DolbyVisionFrameKind { Picture, SdrFallback, Untrusted };
	inline DolbyVisionFrameKind ClassifyDolbyVisionFrame(bool valid, bool requiresBlackFrame, bool allowSdrFallback,
		bool rgbPlacementKnown, bool sdrFallbackRenders)
	{
		if (requiresBlackFrame) return DolbyVisionFrameKind::Untrusted;
		if (valid) return DolbyVisionFrameKind::Picture;
		return allowSdrFallback && rgbPlacementKnown && sdrFallbackRenders ?
			DolbyVisionFrameKind::SdrFallback : DolbyVisionFrameKind::Untrusted;
	}
	inline bool DolbyVisionPairTrusted(bool tunnel, DolbyVisionFrameKind a, DolbyVisionFrameKind b)
	{
		if (!tunnel) return true;
		return a == b && b != DolbyVisionFrameKind::Untrusted;
	}
	// Post-merge audit 2026-10-17: a queued payload's kind by its capture encoding
	// (captureEncoding, set on every payload), not rawEncoding - that is set only on
	// raw GPU-unpack payloads, and an SDR fallback is never raw, so it never counted
	// as r210 and r210 SDR fallback pairs were refused. Payload: a converted payload
	// (dolby, captureEncoding).
	template <typename Payload>
	inline DolbyVisionFrameKind ClassifyDolbyVisionPayload(const Payload& p)
	{
		return ClassifyDolbyVisionFrame(p.dolby.valid, p.dolby.requiresBlackFrame, p.dolby.allowSdrFallback,
			p.dolby.rgbBitAlignment == 1 || p.dolby.rgbBitAlignment == 2,
			p.captureEncoding == VideoFrameEncoding::R210);
	}
	// Hunt 2 02-dv L2 (motion_history_fixes): the DV display-management matrices a DV
	// picture's I/C samples are decoded with (the doviMetadataCache key: nonlinear,
	// nonlinearOffset, linear), kept by value so two frames compare byte for byte.
	struct DolbyVisionMatrices
	{
		float values[21] = {};
		bool operator==(const DolbyVisionMatrices& o) const
		{
			return std::memcmp(values, o.values, sizeof(values)) == 0;
		}
		bool operator!=(const DolbyVisionMatrices& o) const { return !(*this == o); }
	};
	template <typename Dolby>
	inline DolbyVisionMatrices DolbyVisionMatricesOf(const Dolby& dm)
	{
		static_assert(sizeof(dm.nonlinear) + sizeof(dm.nonlinearOffset) + sizeof(dm.linear) == sizeof(float) * 21,
			"DV matrices: 9 + 3 + 9 floats");
		DolbyVisionMatrices m;
		std::memcpy(m.values, dm.nonlinear, sizeof(dm.nonlinear));
		std::memcpy(m.values + 9, dm.nonlinearOffset, sizeof(dm.nonlinearOffset));
		std::memcpy(m.values + 12, dm.linear, sizeof(dm.linear));
		return m;
	}
	// A trusted DV pair whose two pictures were coded under different matrices is not a
	// pair (blended samples would be decoded with one matrix). Only Picture-to-Picture
	// pairs carry matrices; every other kind decides as DolbyVisionPairTrusted.
	inline bool DolbyVisionPairTrusted(bool tunnel, DolbyVisionFrameKind a, DolbyVisionFrameKind b,
		const DolbyVisionMatrices& ma, const DolbyVisionMatrices& mb, bool requireSameMatrices)
	{
		if (!DolbyVisionPairTrusted(tunnel, a, b))
			return false;
		if (!tunnel || !requireSameMatrices || a != DolbyVisionFrameKind::Picture)
			return true;
		return ma == mb;
	}
	// The dequeue's DV rule for the next frame B after A (nextMatches).
	// requireSameMatrices (motion_history_fixes): two DV pictures pair only under
	// byte-equal DM matrices.
	template <typename Payload>
	inline bool DolbyVisionNextPairs(bool tunnel, const Payload& a, const Payload& b, bool requireSameMatrices = false)
	{
		return DolbyVisionPairTrusted(tunnel, ClassifyDolbyVisionPayload(a), ClassifyDolbyVisionPayload(b),
			DolbyVisionMatricesOf(a.dolby), DolbyVisionMatricesOf(b.dolby), requireSameMatrices);
	}

	// Hunt 2 04 M1 (motion_history_fixes): the matcher's temporal state (the previous
	// searches, the H2 hysteresis, the engine's hints) belongs to the last pair it
	// estimated, ending at keptSequence. A pair whose A is not that frame (a pair was
	// never estimated in between: no B yet, a skipped output) starts the history over.
	// Sequence 0 = unknown (direct use, the tests): treated as chained (as before).
	inline bool MatcherHistoryChained(uint64_t aSequence, uint64_t keptSequence)
	{
		return aSequence == 0 || aSequence == keptSequence;
	}

	// Hunt 2 04 M3: the block search's coarse range as Search uses it - given for a
	// 1080-line working picture and scaled to the height the search runs at
	// (referenceHeight != 0), clamped to 0..16 - and the reach the optical-flow
	// engine's vectors must stay within (half-size pixels: the coarse step at 1/16 of
	// the half size, +-range, plus the finer steps).
	inline int ScaledSearchRange(int coarseRange, float referenceHeight, int height)
	{
		const int scaled = referenceHeight != 0.0f && height > 0 ?
			static_cast<int>(std::lround(coarseRange * height / 1080.0)) : coarseRange;
		return (std::max)(0, (std::min)(scaled, 16));
	}
	inline float OpticalFlowReach(int searchRange)
	{
		return static_cast<float>((std::max)(1, searchRange) * 16 + 8);
	}

	// Hunt 2 04 L5: the camera refinement's window (source pixels). The global moves
	// in half-texel steps of 4 px at full size, doubling for each halving of the
	// search (4, 8, 16 px for an 8K input halved twice); the window is one step,
	// never under 6 px (1080p: 6, 4K: 8, as before; 8K: 16 instead of 8).
	inline float CameraWindowPixels(int searchHalvings)
	{
		const int h = (std::max)(0, (std::min)(searchHalvings, 8));
		return (std::max)(6.0f, 4.0f * static_cast<float>(1 << h));
	}
	// Fixed-constant batch B4 (2026-10-18, sweep #9, general_rate_fixes): the window continuous in
	// the picture height instead of a step where the search starts halving (about 1527 lines):
	// 8 px at 2160 lines, as a share of the height, never under 6 px. Equal to the stepped window at
	// 1080 (6), 1440 (6), 2160 (8) and 4320 (16) lines; always wider than half the global's step
	// (the search runs on 764-1527 lines, so the step is at most height / 191 px: half of it is
	// under height / 382, the window height / 270). Off or no height: the stepped window.
	inline float CameraWindowPixels(int searchHalvings, int height, bool general)
	{
		if (!general || height <= 0)
			return CameraWindowPixels(searchHalvings);
		return (std::max)(6.0f, static_cast<float>(8.0 * height / 2160.0));
	}

	// Bug hunt 3 F1: the engine's whole-frame (global) vector is in cells of 8 source px at
	// full size (4 px of the half size the search runs on), doubling for each halving of the
	// search input (MotionWarp::SearchHalvings: 4K 16 px, 8K 32 px). It was read as 8 px at
	// every size, so a 4K pan read half its speed and an 8K pan a quarter.
	inline double EngineCellSourcePixels(int searchHalvings)
	{
		const int h = (std::max)(0, (std::min)(searchHalvings, 8));
		return 8.0 * static_cast<double>(1 << h);
	}
	// The global vector (gx, gy) in source pixels per frame.
	inline double EngineGlobalSourcePixels(double gx, double gy, int searchHalvings)
	{
		return EngineCellSourcePixels(searchHalvings) * std::hypot(gx, gy);
	}
	// The halvings MotionWarp makes for an input of `height` lines (HalvesForSearch: lowRes
	// and height^2 >= 2 x 1080^2, again on the inner's half size); a model for the tests.
	inline int SearchHalvingsFor(bool lowRes, int height)
	{
		int halvings = 0;
		long long h = height;
		while (lowRes && h > 0 && h * h >= 2LL * 1080 * 1080 && halvings < 8)
		{
			++halvings;
			h = (h + 1) / 2;
		}
		return halvings;
	}

	// Integration 2026-10-17: the optical-flow producer's break before a frame (no pair
	// with the previous submission), by the same rules as the dequeue's pair: a gap or
	// film update, pair times that are not adjacent (AdjacentPairTimes, with
	// timing_continuity_fixes: checkTimes) and, on the DV tunnel, a pair that is not
	// trusted (DolbyVisionPairTrusted). havePrev false: nothing submitted to pair with.
	struct MotionAheadFrame
	{
		double time = 0.0;   // seconds; 0 = unknown
		bool filmClock = false;   // time is the film clock (pulldown locked), not the capture clock
		DolbyVisionFrameKind kind = DolbyVisionFrameKind::Untrusted;
		DolbyVisionMatrices matrices;   // Hunt 2 02-dv L2: a DV picture's DM matrices
	};
	inline bool MotionAheadBreakBefore(bool gapBefore, bool filmUpdate, bool checkTimes, double pairRateHz,
		bool dvTunnel, bool havePrev, const MotionAheadFrame& previous, const MotionAheadFrame& frame,
		bool requireSameMatrices = false)
	{
		if (gapBefore || filmUpdate)
			return true;
		if (!havePrev)
			return false;
		if (checkTimes && !CaptureTimestampGap::AdjacentPairTimes(previous.time, previous.filmClock,
			frame.time, frame.filmClock, pairRateHz))
			return true;
		return !DolbyVisionPairTrusted(dvTunnel, previous.kind, frame.kind, previous.matrices, frame.matrices,
			requireSameMatrices);
	}

	// Whether extra presents may run (with Decide() == Active): strength > 0
	// and an uneven film cadence. False in mask-only mode.
	inline bool ExtrasAllowed(const Input& in)
	{
		return EffectiveStrength(in) > 0.0;
	}

	inline const char* Name(Reason reason)
	{
		switch (reason)
		{
		case Reason::Active: return "active";
		case Reason::Off: return "off";
		case Reason::LowLatency: return "low-latency queue";
		case Reason::Interlaced: return "interlaced input";
		case Reason::UnknownRates: return "rates not known";
		case Reason::IneligibleCadence: return "cadence not eligible (no strength for it, or a video-rate source)";
		case Reason::VideoRateMaskOnly: return "mask / grain map off: a video-rate source";
		case Reason::NativeRgb: return "RGB capture";
		case Reason::PlaneFormat: return "plane format unsuitable";
		case Reason::NoNextFrame: return "next frame not ready";
		case Reason::NextFrameDiffers: return "next frame differs";
		case Reason::SmoothMotion: return "smooth motion (frame mixing) active";
		}
		return "?";
	}

	// --- 6b-2: extra presents -------------------------------------------
	//
	// After A's own present, the renderer fills each further refresh A owns
	// with one extra present, one at a time: it waits until the previous
	// picture is on screen (vsync V_prev), then asks PlanExtra whether the
	// next refresh (V_prev + P_d) is still A's. Times are seconds on one
	// clock (QPC).
	//
	// Phase: a source frame's "true" time is its capture callback plus a
	// constant display latency L. L is the floor of (A's first vsync -
	// A's callback) over recent frames, so A's first refresh has phase
	// 0 ... P_d/P_s and refresh V has
	//     delta = (V - callback_A - L) / P_s.
	// The constant only fixes which refresh counts as phase 0.
	//
	// B is never delayed: no extra is presented once B has arrived, or when B
	// (predicted from the capture clock: callback_A + P_s plus A's own
	// callback-to-present latency) could still be rendered for that refresh.

	enum class Stop
	{
		None,
		NextFrameArrived,   // B (or anything newer) is already queued
		NextFrameNear,      // B could make this refresh: it is B's
		PastNextFrame,      // delta >= 1: A's time is over
		MaxExtras,          // safety cap
	};

	inline const char* Name(Stop stop)
	{
		switch (stop)
		{
		case Stop::None: return "none";
		case Stop::NextFrameArrived: return "next frame arrived";
		case Stop::NextFrameNear: return "next frame due";
		case Stop::PastNextFrame: return "past the next frame";
		case Stop::MaxExtras: return "extra limit";
		}
		return "?";
	}

	struct ExtraInput
	{
		double previousVsync = 0.0;    // V_prev: the vsync the previous picture is on
		double displayPeriod = 0.0;    // P_d
		double sourcePeriod = 0.0;     // P_s
		double callbackA = 0.0;        // A's capture callback
		double latencyFloor = 0.0;     // L
		double callToCallback = 0.0;   // A's present call - A's callback (smoothed)
		double renderAllowance = 0.0;  // time B needs from its present call to make a vsync
		bool nextFrameArrived = false;
		int extrasDone = 0;
		int maxExtras = 3;
	};

	struct ExtraDecision
	{
		bool present = false;
		double targetVsync = 0.0;
		double delta = 0.0;
		Stop stop = Stop::None;
	};

	inline ExtraDecision PlanExtra(const ExtraInput& in)
	{
		ExtraDecision decision;
		decision.targetVsync = in.previousVsync + in.displayPeriod;
		if (in.nextFrameArrived) { decision.stop = Stop::NextFrameArrived; return decision; }
		if (in.extrasDone >= in.maxExtras) { decision.stop = Stop::MaxExtras; return decision; }
		const double nextCall = in.callbackA + in.sourcePeriod + in.callToCallback;
		if (nextCall + in.renderAllowance < decision.targetVsync)
		{
			decision.stop = Stop::NextFrameNear;
			return decision;
		}
		decision.delta = (decision.targetVsync - in.callbackA - in.latencyFloor) / in.sourcePeriod;
		if (!(decision.delta < 1.0)) { decision.stop = Stop::PastNextFrame; return decision; }
		decision.delta = std::max(0.0, decision.delta);
		decision.present = true;
		return decision;
	}

	// --- Presentation telemetry without extras (audit 6b-2a) ---------------
	// DXGI present ids of recent extras, and the refresh each main present
	// showed on. Ids are compared with signed 32-bit differences (wrap-safe).

	// Number of extra presents with ids in (from, to].
	inline uint32_t ExtrasBetween(const std::deque<uint32_t>& extraIds, uint32_t from, uint32_t to)
	{
		uint32_t count = 0;
		for (uint32_t id : extraIds)
			if (static_cast<int32_t>(id - from) > 0 && static_cast<int32_t>(to - id) >= 0)
				++count;
		return count;
	}

	// When the latest present on screen (presentCount) is an extra, rewrite it
	// to the main present before it, with that present's refresh when known,
	// as the frame statistics would read without extras. Returns false (and
	// leaves the values alone) when presentCount is not an extra.
	inline bool MainPresentFor(const std::deque<uint32_t>& extraIds,
		const std::deque<std::pair<uint32_t, uint32_t>>& mainRefresh,
		uint32_t& presentCount, uint32_t& presentRefresh)
	{
		auto isExtra = [&](uint32_t id) { return std::find(extraIds.begin(), extraIds.end(), id) != extraIds.end(); };
		if (extraIds.empty() || !isExtra(presentCount))
			return false;
		uint32_t main = presentCount;
		while (main > 0 && isExtra(main))
			--main;
		presentCount = main;
		for (const auto& entry : mainRefresh)
			if (entry.first == main)
				presentRefresh = entry.second;
		return true;
	}

	// The floor of recent (first vsync - arrival) values: the constant L, over
	// the last kWindowSeconds of source frames at any rate.
	class LatencyFloor
	{
	public:
		static constexpr double kWindowSeconds = 5.0;
		static constexpr size_t Capacity = 1200;   // 5 s at up to 240 frames per second
		void Reset() { count = 0; next = 0; }
		// The window for this source rate (frames in kWindowSeconds); a change
		// starts over.
		void SetSourceRate(double sourceHz)
		{
			const size_t frames = sourceHz > 0.0 ?
				std::min(Capacity, std::max<size_t>(20, static_cast<size_t>(std::ceil(sourceHz * kWindowSeconds)))) : 120;
			if (frames != window) { window = frames; Reset(); }
		}
		size_t Window() const { return window; }
		void Add(double value)
		{
			values[next] = value;
			next = (next + 1) % window;
			count = std::min(count + 1, window);
		}
		size_t Count() const { return count; }
		// The minimum while few values are known; from 20 on the 5th
		// percentile, so one early outlier (a jittered callback) does not
		// shift every extra's phase (stream robustness, owner 2026-10-14).
		double Floor() const
		{
			if (count == 0) return 0.0;
			std::vector<double> sorted(values, values + count);
			const size_t k = count < 20 ? 0 : count / 20;
			std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
			return sorted[k];
		}

	private:
		double values[Capacity] = {};
		size_t window = 120;   // 5 s at 24p until a rate is known
		size_t count = 0;
		size_t next = 0;
	};
}
