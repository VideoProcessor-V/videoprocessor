#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

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
		EvenCadence,      // a whole number of refreshes per source frame (or about one or fewer)
		NativeRgb,        // an RGB capture: no YCbCr planes to warp
		PlaneFormat,      // A's planes cannot be sampled linearly / stored
		NoNextFrame,      // B not queued or not converted yet
		NextFrameDiffers, // B has a different size, layout or video state
	};

	struct Input
	{
		double strength = 0.0;
		bool lowLatency = false;
		bool interlaced = false;
		double sourceHz = 0.0;
		double displayHz = 0.0;
		bool nativeRgb = false;
		bool haveNext = false;
		bool nextMatches = false;
		bool planeFormatsUsable = true;
	};

	// Refreshes per source frame (P_s / P_d).
	inline double CadenceRatio(double sourceHz, double displayHz)
	{
		return sourceHz > 0.0 ? displayHz / sourceHz : 0.0;
	}

	// Extras only fill an uneven cadence (planner 6b-2, item 4): 24p on 60 Hz
	// (2.5) or 25p on 60 Hz (2.4), never an N:N cadence (25p/50, 30p/60,
	// 24p/48), whatever the rate. +-0.05 absorbs 23.976/24 and 59.94/60. At
	// about one refresh per frame or fewer there is nothing to fill.
	inline bool IsUnevenCadence(double ratio)
	{
		if (!(ratio > 1.05))
			return false;
		return std::abs(ratio - std::round(ratio)) > 0.05;
	}

	// The order is the order of the checks: the first that fails is the
	// reason reported.
	inline Reason Decide(const Input& in)
	{
		if (!(in.strength > 0.0)) return Reason::Off;
		if (in.lowLatency) return Reason::LowLatency;
		if (in.interlaced) return Reason::Interlaced;
		if (!(in.sourceHz > 0.0) || !(in.displayHz > 0.0)) return Reason::UnknownRates;
		if (!IsUnevenCadence(CadenceRatio(in.sourceHz, in.displayHz))) return Reason::EvenCadence;
		if (in.nativeRgb) return Reason::NativeRgb;
		if (!in.planeFormatsUsable) return Reason::PlaneFormat;
		if (!in.haveNext) return Reason::NoNextFrame;
		if (!in.nextMatches) return Reason::NextFrameDiffers;
		return Reason::Active;
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
		case Reason::EvenCadence: return "even cadence";
		case Reason::NativeRgb: return "RGB capture";
		case Reason::PlaneFormat: return "plane format unsuitable";
		case Reason::NoNextFrame: return "next frame not ready";
		case Reason::NextFrameDiffers: return "next frame differs";
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

	// The floor of recent (first vsync - callback) values: the constant L.
	class LatencyFloor
	{
	public:
		static constexpr size_t Capacity = 120;   // 5 s at 24p
		void Reset() { count = 0; next = 0; }
		void Add(double value)
		{
			values[next] = value;
			next = (next + 1) % Capacity;
			count = std::min(count + 1, Capacity);
		}
		size_t Count() const { return count; }
		double Floor() const
		{
			if (count == 0) return 0.0;
			double floor = values[0];
			for (size_t i = 1; i < count; ++i)
				floor = std::min(floor, values[i]);
			return floor;
		}

	private:
		double values[Capacity] = {};
		size_t count = 0;
		size_t next = 0;
	};
}
