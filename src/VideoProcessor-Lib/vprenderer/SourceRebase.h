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
	// Per axis (owner 2026-10-16): the setting may give the source's width too,
	// "WIDTHxHEIGHT" (e.g. 3840x1080: a 4K frame upscaled vertically only, or an
	// anamorphic 1440x1080). The width factor: width / native width (1 when not
	// below); a height-only setting re-bases both axes alike (the height's factor).
	double FactorX(const std::string& setting, int nativeWidth, int nativeHeight);
	// The width in a "WIDTHxHEIGHT" setting (0 = none).
	int ClassWidth(const std::string& setting);

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
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, double factor, pl_frame& out, int slot = 0)
		{
			return Apply(gpu, dispatch, source, factor, factor, out, slot);
		}
		// Per axis (owner 2026-10-15/16: a source upscaled in one direction only -
		// "decompose an upscaler and then use it in the offending axis alone"): each
		// axis by its own factor (1 = untouched). Both below 1: the polar filter as
		// before; one axis only: a 1D (orthogonal) Lanczos along that axis alone.
		bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, double factorX, double factorY, pl_frame& out, int slot = 0);
		void Release(pl_gpu gpu);
		// Kernel-matched re-base (docs/VP-stage12-build-plan.md): the identified player
		// kernel (KernelInverse candidate index, -1 = none). With one, the luma plane of an
		// axis re-based exactly 2:1 is its least-squares inverse (two separable passes);
		// chroma and every other case keep the filters above.
		void SetKernel(int candidate) { kernel = candidate; }
		int Kernel() const { return kernel; }
		bool KernelUsed() const { return kernelUsed; }
		const std::string& LastError() const { return error; }
		// C19: the last float fallback taken for a plane whose format is not renderable
		// (empty: none); the renderer logs it once per change.
		const std::string& FormatNote() const { return formatNote; }
		unsigned long long Failures() const { return failures; }
		float antiring = 0.5f;   // the faithful branch's value (Stage 8)
		// Both axes, chroma planes: a separable Lanczos (two 1D passes) - the polar filter's
		// footprint grows with the factor (5.1 ms for a 4K 2:1 frame on the laptop RTX 3050 Ti,
		// separable 2.1 ms). Luma stays polar: a 1D Lanczos rings into black bars (2.2 codes
		// below black next to the picture edge vs the polar's ~1), which show when cropping
		// is off. false = polar for chroma too.
		bool separable = true;

	private:
		static constexpr int kSlots = 2;
		pl_tex textures[kSlots][PL_MAX_PLANES] = {};
		pl_tex inverseTemp[kSlots] = {};   // the first (horizontal) inverse pass
		pl_tex separableTemp[kSlots][PL_MAX_PLANES] = {};   // the separable filter's first pass
		pl_shader_obj lutsDown[kSlots][PL_MAX_PLANES] = {};
		int kernel = -1;
		bool kernelUsed = false;
		bool InversePlane(pl_gpu gpu, pl_dispatch dispatch, pl_tex in, int w, int h, bool acrossInverse, bool downInverse,
			pl_tex out, int slot);
		pl_shader_obj luts[kSlots][PL_MAX_PLANES] = {};
		std::string error;
		std::string formatNote;
		unsigned long long failures = 0;
	};
}
