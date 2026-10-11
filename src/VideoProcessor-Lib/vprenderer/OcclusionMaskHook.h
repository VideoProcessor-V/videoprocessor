#pragma once

#include <libplacebo/renderer.h>

#include <vprenderer/NlsMaskMapping.h>

// Stage 6c (M1′): the de-judder's occlusion mask, handed to the OUTPUT-stage
// sigma-delta through the alpha channel of the OUTPUT texture.
//
// A native OUTPUT hook, placed after OutputEncodingRepair and before the
// sigma-delta. It copies the picture unchanged and writes, into alpha, the
// occlusion value of the source block each output pixel shows: 1 = the
// content was visible in the previous source frame (carry allowed), 0 = new
// or uncovered content. Outside the picture (bars, blanking) alpha is 1.
//
// Alpha ownership: this hook owns alpha from here to the sigma-delta, which
// reads it as the mask and writes alpha 1. The Stage 10 range carry owns
// alpha only between PL_HOOK_RGB and PL_HOOK_SCALED, so the two never
// overlap. The hook is only installed when the sigma-delta takes the mask
// (its `occlusion_mask` parameter), so alpha never reaches the display.
//
// The output-to-source mapping is the source crop scaled onto the
// destination rectangle and, with NLS active (6c-2), the NLS shader's own
// per-axis map over the source crop.
namespace OcclusionMask
{
	class Hook
	{
	public:
		Hook();
		Hook(const Hook&) = delete;
		Hook& operator=(const Hook&) = delete;

		// The mask (one texel per 16x16 source block, r = 1 seen / 0 new) and
		// the rectangles libplacebo maps the source onto: the source crop in
		// source pixels and the destination rectangle in target pixels.
		void SetFrame(pl_tex mask, const pl_rect2df& sourceCrop, const pl_rect2df& targetCrop);
		// 6c-2: NLS active this frame. hookedRect is the rectangle, in source
		// pixels, that the NLS shader's 0..1 coordinates span: the source
		// crop (the MAIN-stage texture libplacebo hooks is the cropped
		// picture; GPU test OcclusionMaskFollowsTheNlsMap, off-centre crop).
		// Call after SetFrame, which clears it.
		void SetNls(const NlsMaskMapping::Mapping& mapping, const pl_rect2df& hookedRect);
		const pl_hook* Get() const { return &hook; }
		unsigned long long Applied() const { return applied; }
		unsigned long long Failed() const { return failed; }

	private:
		static pl_hook_res Run(void* priv, const pl_hook_params* params);
		pl_hook_res Apply(const pl_hook_params* params);

		pl_hook hook{};
		pl_tex mask = nullptr;
		pl_rect2df source{};
		pl_rect2df target{};
		NlsMaskMapping::Mapping nls{};
		pl_rect2df nlsDomain{};
		unsigned long long applied = 0;
		unsigned long long failed = 0;
	};
}
