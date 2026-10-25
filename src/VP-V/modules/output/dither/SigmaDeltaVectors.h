#pragma once

#include <VP-V/modules/output/dither/CustomHookChain.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <vector>

// WP-13 (integration batch 1): the sigma-delta's motion-vector inputs, moved out of the renderer
// host (LibplaceboVideoRenderer.cpp: the three static helpers and RenderLocked's NextVectorGuard).
// Header-only: PL_VP_MPV_USER_SHADER_SET_TEXTURE comes from the libplacebo headers each project
// builds against (the fork's patch 0004 in libplacebo 377; 7.360 has no such call and the
// sigma-delta searches itself), so each translation unit answers for its own libplacebo, exactly
// as the static helpers did. Threading: render thread only.
namespace SigmaDeltaVectors
{
	// Delta 8 R1: the sigma-delta's VPVEC texture can be set on its hook.
	inline bool Supported()
	{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
		return true;
#else
		return false;
#endif
	}

	// Planner D2: the per-pixel displacement (VPPIX), the same fork call.
	inline bool SetPixel(const pl_hook* hook, pl_tex displacement)
	{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
		return hook && pl_mpv_user_shader_set_texture(hook, "VPPIX", displacement);
#else
		(void)hook;
		(void)displacement;
		return false;
#endif
	}

	inline bool Set(const pl_hook* hook, pl_tex vectors)
	{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
		return hook && pl_mpv_user_shader_set_texture(hook, "VPVEC", vectors);
#else
		(void)hook;
		(void)vectors;
		return false;
#endif
	}

	// Efficiency (owner 2026-10-14): B's own first present takes the sigma-delta's vectors made
	// from A's field at the end of A's extras (same sequence and geometry only; the host checks
	// that); its per-pixel gate checks every carried value, so a cut or a wrong vector falls back to
	// the plain gate. Restored (vector_source 0, no texture) when released or destroyed, i.e.
	// after this render.
	class NextVectorGuard
	{
	public:
		NextVectorGuard() = default;
		NextVectorGuard(const NextVectorGuard&) = delete;
		NextVectorGuard& operator=(const NextVectorGuard&) = delete;
		~NextVectorGuard() { Release(); }

		// Sets vector_source 1 and the vectors on every sigma-delta hook of the frame; a hook that
		// does not take the texture goes back to vector_source 0. True when any hook took them.
		bool Apply(const pl_hook* const* hooks, int count, pl_tex vectors)
		{
			for (int i = 0; i < count; ++i)
			{
				const pl_hook* hook = hooks[i];
				if (CustomHookChain::SetHookParameter(hook, "vector_source", 1.0))
				{
					if (Set(hook, vectors))
						applied.push_back(hook);
					else
						CustomHookChain::SetHookParameter(hook, "vector_source", 0.0);
				}
			}
			return !applied.empty();
		}

		void Release()
		{
			for (const pl_hook* hook : applied)
			{
				CustomHookChain::SetHookParameter(hook, "vector_source", 0.0);
				Set(hook, nullptr);
			}
			applied.clear();
		}

	private:
		std::vector<const pl_hook*> applied;
	};
}

// The host's call-site names (kept so the renderer's de-judder extras code is unchanged).
inline bool SigmaDeltaVectorsSupported() { return SigmaDeltaVectors::Supported(); }
inline bool SetSigmaDeltaPixelVectors(const pl_hook* hook, pl_tex displacement) { return SigmaDeltaVectors::SetPixel(hook, displacement); }
inline bool SetSigmaDeltaVectors(const pl_hook* hook, pl_tex vectors) { return SigmaDeltaVectors::Set(hook, vectors); }
