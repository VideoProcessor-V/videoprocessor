#pragma once

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>

// texture_restore: off | statistical (experimental; owner 2026-10-17; docs/VP-detail-restore.md).
// Repair towards the original: per tile the luma detail is split into grain (unstructured,
// spatially uncorrelated) and scene texture (structured). Grain is scaled towards the
// best-coded recent frame's grain level (only the grain this frame already has: nothing is
// synthesised); scene texture is scaled towards the reference texture energy only where local
// focus evidence says the tile is in focus. References follow the vectors when valid and
// restart where the picture changed without them. Runs after detail_pulse_stabilise; never
// touches the source-resolution monitor's input; never starts the matcher.
namespace TextureRestore
{
	constexpr double kDefaultCap = 1.5;
	constexpr double kCapMin = 1.0, kCapMax = 2.0;

	struct Params
	{
		double cap = kDefaultCap;      // texture_restore_cap, clamped to [kCapMin, kCapMax]
		int colorDepth = 10;
		double planeLines = 0.0;       // the picture's lines in this plane; 0 = the luma plane's height
		double sourceHz = 0.0;         // measured content rate; 0 = unknown (reference 23.976)
		double bitrateQuality = 1.0;
		pl_tex field = nullptr;        // as DetailPulse::Params::field (valid vectors only)
		double fieldScale = 1.0;
		double fieldScaleX = 0.0;
		uint64_t sequence = 0;
	};

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;

		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
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
