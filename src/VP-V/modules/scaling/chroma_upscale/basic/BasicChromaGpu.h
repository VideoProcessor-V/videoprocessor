// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale / basic on the GPU: the polyphase lanczos upsample of BasicChroma.h as ONE libplacebo compute pass. It runs
// the same INTEGER arithmetic as the CPU model (Q14 weights from the same tables, int32 sums, the same rounding and clamps), so
// its output is byte-identical to BasicChroma::UpsamplePlane on any adapter (the test compares bytes, not a tolerance).
//
// Input: a two-component 16-bit unorm texture at the chroma size holding (Cb, Cr) or (P, T) in the container codes the plane
// carries. Output: a two-component 16-bit unorm storage texture at the luma size. Not wired into a render in reset 1 (the key
// chroma_upscale_basic is deferred with the SCHEMA lock); the caller owns the frame plumbing.
#pragma once

#include <VP-V/modules/scaling/chroma_upscale/basic/BasicChroma.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <string>

namespace BasicChroma
{
	namespace Gpu
	{
		// The GLSL header of a geometry: the defines and the two Q14 weight tables (generated from BuildPhaseTable). Exposed so the
		// test can check that no float constant enters the arithmetic.
		std::string BuildHeader(const Geometry& g);
		// The pass body (compute, one thread per output pixel, 16 x 16 groups).
		const char* Body();

		class Module
		{
		public:
			Module() = default;
			~Module() = default;
			Module(const Module&) = delete;
			Module& operator=(const Module&) = delete;

			// `src` (rg16 unorm, chroma size) to `out` (rg16 unorm, luma size, owned by the module until the next Apply or Release).
			// False with LastError on any failure; the caller then keeps the plain libplacebo chroma upscale and shows
			// "Chroma upscale: libplacebo (<reason>)".
			bool Apply(pl_gpu gpu, pl_dispatch dispatch, pl_tex src, const Geometry& g, pl_tex& out);
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }

		private:
			pl_tex m_out = nullptr;
			std::string m_error;
		};
	}
}
