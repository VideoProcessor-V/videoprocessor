#pragma once

// Stage 6b: when the de-judder may run for a source frame. Pure logic, so the
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
		NotFilmCadence,   // source rate >= display rate / 1.5 (nothing to fill)
		UnknownRates,     // source or display rate not known yet
		NativeRgb,        // an RGB capture: no YCbCr planes to warp
		NoNextFrame,      // B not queued or not converted yet
		NextFrameDiffers, // B has a different size, layout or video state
		PlaneFormat,      // A's planes cannot be sampled linearly / stored
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

	// The order is the order of the checks: the first that fails is the
	// reason reported.
	inline Reason Decide(const Input& in)
	{
		if (!(in.strength > 0.0)) return Reason::Off;
		if (in.lowLatency) return Reason::LowLatency;
		if (in.interlaced) return Reason::Interlaced;
		if (!(in.sourceHz > 0.0) || !(in.displayHz > 0.0)) return Reason::UnknownRates;
		if (in.sourceHz >= in.displayHz / 1.5) return Reason::NotFilmCadence;
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
		case Reason::NotFilmCadence: return "not film cadence";
		case Reason::UnknownRates: return "rates not known";
		case Reason::NativeRgb: return "RGB capture";
		case Reason::NoNextFrame: return "next frame not ready";
		case Reason::NextFrameDiffers: return "next frame differs";
		case Reason::PlaneFormat: return "plane format unsuitable";
		}
		return "?";
	}
}
