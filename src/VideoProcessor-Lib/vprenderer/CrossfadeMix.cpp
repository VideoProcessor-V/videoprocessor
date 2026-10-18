#include <pch.h>
#include "CrossfadeMix.h"

// Phase 1 (tests first): not implemented yet.
namespace CrossfadeMix
{
	double MixValue(double, double to, double) { return to; }
	std::vector<const pl_hook*> FromPathHooks(const pl_hook* const* hooks, int count, const pl_hook*)
	{
		return std::vector<const pl_hook*>(hooks, hooks + count);
	}
	size_t MixInsertIndex(const std::vector<const pl_hook*>&, const pl_hook*) { return 0; }
	Mixer::Mixer() {}
	Mixer::~Mixer() {}
	bool Mixer::RenderFrom(pl_renderer, pl_gpu, const pl_frame&, const pl_frame&, const pl_render_params&, const pl_hook*) { return false; }
	const pl_hook* Mixer::MixHook(double) { return nullptr; }
	std::vector<const pl_hook*> Mixer::WithMix(const pl_hook* const* hooks, int count, const pl_hook*, double)
	{
		return std::vector<const pl_hook*>(hooks, hooks + count);
	}
	void Mixer::Release(pl_gpu) {}
	pl_hook_res Mixer::RunCapture(void*, const pl_hook_params*) { return pl_hook_res{}; }
	pl_hook_res Mixer::RunMix(void*, const pl_hook_params*) { return pl_hook_res{}; }
	bool Mixer::EnsureFloat(pl_gpu, pl_tex&, int, int) { return false; }
}
