#pragma once

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>

// detail_pulse_stabilise (owner 2026-10-17; docs/VP-detail-restore.md, research option e in
// docs/audit/detail-retrieval-research-2026-10-17.md): a starved encode codes I-frames finely
// and B-frames coarsely, so fine detail and grain pulse with the GOP. Each frame's OWN luma
// detail (two bands) is scaled up towards its best-coded recent neighbour's level; nothing is
// copied between frames, so grain stays per-frame and is never averaged. Runs after the VP
// debander on the plane libplacebo scales from; never touches the source-resolution monitor's
// input; never starts the matcher.
namespace DetailPulse
{
	constexpr double kDefaultCap = 1.35;
	constexpr double kCapMin = 1.0, kCapMax = 1.5;

	struct Params
	{
		double cap = kDefaultCap;      // detail_pulse_cap, clamped to [kCapMin, kCapMax]
		int colorDepth = 10;           // the signal's code depth
		double planeLines = 0.0;       // the picture's lines in this plane (re-based height); 0 = the luma plane's height
		double sourceHz = 0.0;         // the measured content rate; 0 = unknown (reference 23.976)
		double bitrateQuality = 1.0;   // SourceResolutionDetector reading (coding-noise allowance)
		// Vectors (optional): the de-judder's block field of this source frame (rgba = vx, vy in
		// native luma px, trust, error; one texel per 16x16 native block), only when valid for
		// this frame (MotionValidity). Null = the no-vector fallback.
		pl_tex field = nullptr;
		double fieldScale = 1.0;       // native px per plane px down (1 / re-base factor)
		double fieldScaleX = 0.0;      // across (0 = fieldScale)
		uint64_t sequence = 0;         // this source frame; a repeat (same) never advances the history
	};

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		// `source` with its luma plane stabilised into this module's texture (other planes
		// pass through); false (and `out` untouched) on any failure: the caller renders the
		// frame as it was.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
		// Optional look-ahead: measure the next source frame's band energies (its luma plane,
		// in the same domain as Apply's: only when no re-base is engaged) so the reference
		// can include it. Safe to skip.
		bool MeasureAhead(pl_gpu gpu, pl_dispatch dispatch, pl_tex luma, uint64_t sequence, const Params& params);
		// A discontinuity (MotionValidity invalidation, re-base factor change, cut known to
		// the caller): the history restarts at the next frame.
		void Forget();
		void Release(pl_gpu gpu);
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		unsigned long long Frames() const { return frames; }

	private:
		struct Impl;
		Impl* impl = nullptr;
		std::string error;
		unsigned long long failures = 0, frames = 0;
	};
}
