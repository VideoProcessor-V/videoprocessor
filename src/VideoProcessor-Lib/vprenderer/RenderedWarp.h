#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/renderer.h>

#include <vprenderer/NlsMaskMapping.h>

#include <string>
#include <vector>

// Stage 6d (docs/VP-stage6d-cached-warp.md): de-judder extras warp A's
// rendered picture instead of re-rendering a warped source.
//
// - Cache: a native PRE_OUTPUT hook, last in the frame's list, copies A's
//   picture at PRE_OUTPUT (after tone map, scaling, SSIM, LED compensation
//   and the Stage 10 restore; target colour space, not yet encoded) into a
//   persistent rgba16f texture. libplacebo's stage order is scale -> colour
//   conversion -> PRE_OUTPUT -> encoding -> OUTPUT -> target overlays, so
//   subtitles and the stats overlay are not in the cache.
// - Warp: each output pixel maps back to the source position it shows (the
//   source crop onto the destination rectangle and, with NLS, the NLS
//   shader's per-axis map, as the occlusion mask does), takes the
//   de-judder's vector there exactly as the source warp does (bilinear
//   between trusted block centres; the pixel's own block decides hold), and
//   converts the displacement into output pixels with the map's local slope.
//   The cached picture is sampled with a 4-tap Catmull-Rom kernel (the warp
//   lands on the final picture), edge-clamped and never value-clamped (the
//   Stage 10 extended range is present).
// - The extra is rendered from the warped picture 1:1 with only the
//   OUTPUT-stage hooks (encoding repair, sigma-delta) and nothing that was
//   already applied to A (deband, colour adjustment, peak detection, LUT).
//
// `dejudder_warp_stage: source` keeps the earlier path (full re-render of a
// warped source) for A/B and as the fallback.
namespace DejudderRendered
{
	struct Geometry
	{
		pl_rect2df sourceCrop{};   // source pixels libplacebo maps onto...
		pl_rect2df targetCrop{};   // ...this destination rectangle (target pixels)
		NlsMaskMapping::Mapping nls{};   // valid = NLS active this frame
		pl_rect2df nlsDomain{};    // the source rectangle NLS's 0..1 spans
	};

	class Cache
	{
	public:
		Cache();
		Cache(const Cache&) = delete;
		Cache& operator=(const Cache&) = delete;
		~Cache();

		// Arm before A's render: the hook then copies A's PRE_OUTPUT picture.
		void Arm(uint64_t sequence) { armed = sequence; valid = 0; }
		void Disarm() { armed = 0; }
		// Frees the texture while its device still exists (audit stage10-6d
		// blocker 1: before the renderer destroys the device).
		void Release();
		const pl_hook* Hook() const { return &hook; }
		// The cached picture of this source frame, or nullptr.
		pl_tex Texture(uint64_t sequence) const { return sequence != 0 && valid == sequence ? tex : nullptr; }
		const pl_color_repr& Repr() const { return repr; }
		const pl_color_space& Color() const { return color; }
		unsigned long long Copies() const { return copies; }
		unsigned long long Failed() const { return failed; }

	private:
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		pl_hook_res Apply(const pl_hook_params* params);

		pl_hook hook{};
		pl_gpu gpu = nullptr;
		pl_tex tex = nullptr;
		pl_color_repr repr{};
		pl_color_space color{};
		uint64_t armed = 0;
		uint64_t valid = 0;
		unsigned long long copies = 0;
		unsigned long long failed = 0;
	};

	class Warp
	{
	public:
		// hostReadable: the warped picture can be downloaded (tests only).
		Warp(pl_log log, pl_gpu gpu, bool hostReadable = false);
		Warp(const Warp&) = delete;
		Warp& operator=(const Warp&) = delete;
		~Warp();

		// The cached picture warped by shift x the de-judder's A->B field.
		// field / frameTrust: DejudderWarp::Field() / FrameTrust().
		// subField: DejudderWarp::SubField() (11a E2), or nullptr.
		bool Run(pl_tex cached, pl_tex field, pl_tex frameTrust, float minTrustedShare,
			float shift, const Geometry& geometry, pl_tex& out, pl_tex subField = nullptr);
		const std::string& Error() const { return error; }

	private:
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		pl_tex warped = nullptr;
		bool hostReadable = false;
		std::string error;
	};

	// libplacebo renders onto the destination rectangle rounded to whole
	// pixels (clipped to the framebuffer) and moves the source rectangle with
	// it (renderer.c, the dst_rect rounding). The cache is that rounded
	// rectangle, so the warp's map and the extra's target crop use the same
	// rounding: the extra is then exactly 1:1 (audit stage10-6d item 2).
	struct Rects
	{
		pl_rect2df source{};
		pl_rect2df target{};
	};
	Rects Rounded(const pl_rect2df& source, const pl_rect2df& target, float framebufferW, float framebufferH);

	// A frame showing `tex` (the warped picture) 1:1 on the rounded target
	// crop, in the colour space the cache recorded (the target's: no
	// conversion).
	pl_frame Frame(pl_tex tex, const pl_color_repr& repr, const pl_color_space& color);

	// A's parameters for an extra: only hooks that act at PL_HOOK_OUTPUT,
	// and nothing applied to A already (deband, colour adjustment, peak
	// detection, custom LUT, distortion, cone distortion).
	pl_render_params OutputOnly(const pl_render_params& a, const std::vector<const pl_hook*>& hooks,
		std::vector<const pl_hook*>& kept);
}
