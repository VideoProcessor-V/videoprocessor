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

#include <VP-V/core/VramUsers.h>
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

		// MV7: the output texture m_out is a kept, luma-size, two-component 16-bit unorm storage texture (VpvGpu::Unorm16(gpu, 2): 4 B/px;
		// 33.2 MB at 3840x2160). It is NOT a per-call temporary, so it has a governor entry of its own BEFORE any call site exists
		// (VramUsers.h): level 0 = kept, level 1 = off (the caller keeps the plain libplacebo chroma upscale and says so). A lossy, T1 step
		// (the picture changes), shed first among T1. `Granted` is what the call site reads from the governor's plan.
		inline uint64_t TextureBytes(int lumaW, int lumaH)
		{
			return lumaW > 0 && lumaH > 0 ? static_cast<uint64_t>(lumaW) * static_cast<uint64_t>(lumaH) * 4u : 0u;
		}
		inline VpSkeleton::VramUser DeclaredUser(int lumaW, int lumaH)
		{
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserBasicChroma;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserBasicChroma);
			u.tier = VpSkeleton::Tier::T1;
			u.shedOrder = 0;
			u.lossless = false;
			u.levels.push_back(TextureBytes(lumaW, lumaH));
			u.levelText.push_back(nullptr);
			u.levels.push_back(0);
			u.levelText.push_back("off: the libplacebo chroma upscale runs");
			return u;
		}
		inline bool Granted(const VpSkeleton::GovernorPlan& plan)
		{
			const VpSkeleton::UserGrant* g = plan.Find(VpSkeleton::kUserBasicChroma);
			return g == nullptr || !g->shed;
		}


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
			uint64_t m_declared = ~uint64_t(0);   // the bytes last declared to the VRAM governor (declare on a change only)
			std::string m_error;
		};
	}
}
