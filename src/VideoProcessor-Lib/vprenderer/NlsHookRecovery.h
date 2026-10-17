#pragma once
#include <libplacebo/renderer.h>
#include <cstddef>
#include <vector>

namespace NlsHookRecovery
{
    inline void ResetFailedHook(pl_renderer renderer, uint64_t signature)
    {
        if (!renderer || !signature) return;
        pl_render_errors selected{};
        selected.errors = PL_RENDER_ERR_HOOKS;
        selected.disabled_hooks = &signature;
        selected.num_disabled_hooks = 1;
        pl_renderer_reset_errors(renderer, &selected);
    }

    struct Result
    {
        bool rendered = false;
        bool retried = false;
        uint64_t failedSignature = 0;
        unsigned errors = 0;
    };

    // Bug hunt 2026-10-17: withdraws `hook` from the caller-owned list and
    // points params at that list (it outlives the render: the de-judder extras
    // read the frame's parameters afterwards). Returns true when it was there.
    inline bool WithdrawHook(std::vector<const pl_hook*>& hooks, pl_render_params& params, const pl_hook* hook)
    {
        const size_t before = hooks.size();
        for (size_t i = 0; i < hooks.size();)
            if (hooks[i] == hook)
                hooks.erase(hooks.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
        params.hooks = hooks.empty() ? nullptr : hooks.data();
        params.num_hooks = static_cast<int>(hooks.size());
        return hooks.size() != before;
    }

    // libplacebo may return success after disabling a failing user hook. Never
    // present that unintended full-screen linear stretch. One retry uses the
    // already-selected source pixels and rebuilt ordinary destination/overlays.
    // Bug hunt 2026-10-17: `hooks` is the caller-owned list behind
    // params.hooks; on a retry the failed NLS hook is removed from it and
    // params points at it, so params never refers to storage local to Render.
    template<class RebuildFallback, class Draw>
    Result RenderOwned(pl_renderer renderer, pl_gpu gpu, const pl_frame& source,
        pl_frame& target, const pl_frame& fullTarget, pl_render_params& params,
        std::vector<const pl_hook*>& hooks,
        const pl_hook* activeNls, RebuildFallback rebuildFallback, Draw draw)
    {
        Result result;
        result.rendered = draw(source,target,params);
        const auto errors = pl_renderer_get_errors(renderer);
        result.errors = static_cast<unsigned>(errors.errors);
        if (!activeNls) return result;
        bool disabled = false;
        for (int i = 0; i < errors.num_disabled_hooks; ++i)
            disabled |= errors.disabled_hooks[i] == activeNls->signature;
        if (!disabled) return result;
        result.retried = true;
        result.failedSignature = activeNls->signature;
        // Only the failed NLS hook is withdrawn; custom hooks keep running.
        WithdrawHook(hooks, params, activeNls);
        rebuildFallback();
        const float black[] = { 0.0f, 0.0f, 0.0f };
        pl_frame_clear(gpu, &fullTarget, black);
        result.rendered = draw(source,target,params);
        return result;
    }
    // A copy of params' list is used for the retry; params' list is restored
    // afterwards (Bug hunt 2026-10-17: it pointed at a local before), so a
    // caller that reads params later must use RenderOwned instead.
    template<class RebuildFallback, class Draw>
    Result Render(pl_renderer renderer, pl_gpu gpu, const pl_frame& source,
        pl_frame& target, const pl_frame& fullTarget, pl_render_params& params,
        const pl_hook* activeNls, RebuildFallback rebuildFallback, Draw draw)
    {
        std::vector<const pl_hook*> hooks(params.hooks, params.hooks + (params.hooks ? params.num_hooks : 0));
        const pl_hook* const* originalHooks = params.hooks;
        const int originalCount = params.num_hooks;
        pl_render_params local = params;
        local.hooks = hooks.empty() ? nullptr : hooks.data();
        const Result result = RenderOwned(renderer, gpu, source, target, fullTarget, local, hooks, activeNls,
            rebuildFallback, draw);
        params = local;
        params.hooks = originalHooks;
        params.num_hooks = originalCount;
        return result;
    }
    template<class RebuildFallback>
    Result Render(pl_renderer renderer, pl_gpu gpu, const pl_frame& source,
        pl_frame& target, const pl_frame& fullTarget, pl_render_params& params,
        const pl_hook* activeNls, RebuildFallback rebuildFallback) {
        return Render(renderer,gpu,source,target,fullTarget,params,activeNls,rebuildFallback,
            [renderer](const pl_frame& image,const pl_frame& destination,pl_render_params& parameters){return pl_render_image(renderer,&image,&destination,&parameters);});
    }
}
