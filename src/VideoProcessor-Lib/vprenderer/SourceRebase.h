#pragma once

#include <libplacebo/renderer.h>
#include <libplacebo/shaders.h>

#include <string>

// Stage 12 step A (docs/VP-stage12-build-plan.md; owner 2026-10-15):
// source_resolution: native | auto | any height in lines (1440, 1080, 720 ...). A stream upscaled
// upstream (Crave, upscaled SDR releases) arrives as 4K; re-based to the class
// height it reaches VP's own upscale path exactly as a native 1080p / 720p
// source would (FSRCNNX / ArtCNN, CfL, SSimSuperRes, the 8b blend), and
// Stage 8's downscale no longer runs on it (S8-3).
//
// The re-base downscales each plane of the frame handed to libplacebo, in the
// signal domain (owner: a genuine 4:2:0 frame for CfL and the chroma path, and
// the inverse of an upscaler that worked in the signal domain), with the
// faithful kernel: polar EWA Lanczos sharp with anti-ringing. Luma and an RGB
// plane go to the class size; chroma keeps its subsampling (half of it for
// 4:2:0). The crop scales by the same factor. Everything that decides geometry
// (the active picture, NLS, subtitles, the LED measurement) stays in native
// pixels; only the textures libplacebo samples change.
namespace SourceRebase
{
	// The setting's height in lines, any whole number from kMinimumHeight to
	// kMaximumHeight (general: not a fixed list of classes); 0 = native (no
	// re-base). auto is native until detection (step B) exists.
	constexpr int kMinimumHeight = 144;
	constexpr int kMaximumHeight = 8640;
	int ClassHeight(const std::string& setting);

	// The factor for a native frame `nativeHeight` lines tall: class / native,
	// or 1 when the class is not below the native height (or native).
	double Factor(const std::string& setting, int nativeHeight);

	// A plane's re-based size: its own size times the factor, rounded, at
	// least 1. Chroma follows from its own size, so 4:2:0 stays 4:2:0.
	int Scaled(int size, double factor);

	class Module
	{
	public:
		Module() = default;
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;
		~Module();

		// `source` with every plane downscaled by `factor` (< 1) into this
		// module's textures and its crop scaled to match. False (and `out`
		// untouched) on any failure: the caller renders the native frame.
		// `slot` keeps separate textures for frames that must coexist (the
		// main frame and a de-judder extra).
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, double factor, pl_frame& out, int slot = 0);
		void Release(pl_gpu gpu);
		const std::string& LastError() const { return error; }
		unsigned long long Failures() const { return failures; }
		float antiring = 0.5f;   // the faithful branch's value (Stage 8)

	private:
		static constexpr int kSlots = 2;
		pl_tex textures[kSlots][PL_MAX_PLANES] = {};
		pl_shader_obj luts[kSlots][PL_MAX_PLANES] = {};
		std::string error;
		unsigned long long failures = 0;
	};
}
