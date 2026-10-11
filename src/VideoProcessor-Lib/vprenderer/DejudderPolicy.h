#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>

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
		IneligibleCadence, // a whole number of refreshes per source frame, or under 2 (video)
		VideoRateMaskOnly, // occlusion mask alone, source above film rates (planner 6c)
		NativeRgb,        // an RGB capture: no YCbCr planes to warp
		PlaneFormat,      // A's planes cannot be sampled linearly / stored
		NoNextFrame,      // B not queued or not converted yet
		NextFrameDiffers, // B has a different size, layout or video state
	};

	struct Input
	{
		double strength = 0.0;
		bool occlusionMask = false;   // 6c: the estimate also runs for the mask alone
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

	// Extras only fill an uneven film cadence (planner 6b-2 item 4 and 6b-2a):
	// 24p on 60 Hz (2.5) or 25p on 60 Hz (2.4), never an N:N cadence (25p/50,
	// 30p/60, 24p/48), and never under 2 refreshes per frame (50p on 60 Hz is
	// video: its one-in-five extra would be motion smoothing). +-0.05 absorbs
	// 23.976/24 and 59.94/60.
	inline bool IsUnevenCadence(double ratio)
	{
		if (!(ratio >= 1.95))
			return false;
		return std::abs(ratio - std::round(ratio)) > 0.05;
	}

	// The order is the order of the checks: the first that fails is the
	// reason reported.
	// Mask-only mode is for film rates (planner 6b-2b/6c, item 3).
	constexpr double kMaskOnlyMaxSourceHz = 30.5;

	// Whether the motion estimate runs. It runs for the de-judder (strength
	// > 0, uneven film cadence) and, with the occlusion mask on, for the mask
	// alone on any cadence (mask-only mode: strength 0, no extras).
	inline Reason Decide(const Input& in)
	{
		if (!(in.strength > 0.0) && !in.occlusionMask) return Reason::Off;
		if (in.lowLatency) return Reason::LowLatency;
		if (in.interlaced) return Reason::Interlaced;
		if (!(in.sourceHz > 0.0) || !(in.displayHz > 0.0)) return Reason::UnknownRates;
		const bool extras = in.strength > 0.0 && IsUnevenCadence(CadenceRatio(in.sourceHz, in.displayHz));
		if (!extras && !in.occlusionMask)
			return Reason::IneligibleCadence;
		// Without extras the estimate runs for the mask alone: film rates only.
		if (!extras && in.sourceHz > kMaskOnlyMaxSourceHz)
			return Reason::VideoRateMaskOnly;
		if (in.nativeRgb) return Reason::NativeRgb;
		if (!in.planeFormatsUsable) return Reason::PlaneFormat;
		if (!in.haveNext) return Reason::NoNextFrame;
		if (!in.nextMatches) return Reason::NextFrameDiffers;
		return Reason::Active;
	}

	// Whether extra presents may run (with Decide() == Active): strength > 0
	// and an uneven film cadence. False in mask-only mode.
	inline bool ExtrasAllowed(const Input& in)
	{
		return in.strength > 0.0 && IsUnevenCadence(CadenceRatio(in.sourceHz, in.displayHz));
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
		case Reason::IneligibleCadence: return "cadence not eligible (even, or video)";
		case Reason::VideoRateMaskOnly: return "mask only above film rates";
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
