#pragma once

// libplacebo side of VP's captured-repeat render reuse (WP-24, VP-ONLY; revised 2026-10-25 after
// audit M1/M2/L4). Included by RepeatReuseGlue.h. Two jobs:
//   1. HookInfos(): classify the frame's hook list for RepeatReusePolicy. mpv-style hooks are
//      recognised by libplacebo's own mpv hook function (from a parsed one-line probe shader).
//      - a hook with an OUTPUT pass AND another stage is opaque: OutputOnly would drop its OUTPUT
//        pass from the reuse draw (M1);
//      - an mpv shader noted temporal (frame / random / //!BUFFER) is opaque (M2);
//      - the shader-set generation joins the hook state (L4).
//   2. DrawFromCache(): the extras' 6d path with zero warp - the cached PRE_OUTPUT picture rendered
//      1:1 into its rounded target rectangle with only the OUTPUT-stage hooks (encoding repair only
//      if still needed), the target LUT cleared (the cached picture already carries it), target
//      overlays and dither as on every refresh.

#include "RepeatReusePolicy.h"
#include "TemporalShaders.h"

#include <vprenderer/RenderedWarp.h>
#include <vprenderer/OutputEncodingRepair.h>

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace VpRepeatReuse
{
	struct KnownHook
	{
		const pl_hook* hook = nullptr;
		HookKind kind = HookKind::Static;
		uint64_t generation = 0;
		const char* name = nullptr;   // shown when the hook makes the key opaque
	};

	inline bool MultiStageWithOutput(const pl_hook* h)
	{
		const unsigned stages = static_cast<unsigned>(h->stages);
		return (stages & static_cast<unsigned>(PL_HOOK_OUTPUT)) != 0 && (stages & ~static_cast<unsigned>(PL_HOOK_OUTPUT)) != 0;
	}

	// Pure (no GPU calls): the classification, given libplacebo's mpv hook entry (nullptr = unknown).
	inline std::vector<HookInfo> ClassifyHooks(const std::vector<const pl_hook*>& hooks, const std::vector<KnownHook>& known,
	const void* mpvHookFn)
	{
		const TemporalShaderRegistry& temporal = TemporalShaderRegistry::Global();
		std::vector<HookInfo> out;
		out.reserve(hooks.size() + 1);
		for (const pl_hook* h : hooks)
		{
			if (!h)
				continue;
			HookInfo info;
			info.id = h;
			info.beforeOutput = (static_cast<unsigned>(h->stages) & ~static_cast<unsigned>(PL_HOOK_OUTPUT)) != 0;
			const auto k = std::find_if(known.begin(), known.end(), [h](const KnownHook& x) { return x.hook == h; });
			if (k != known.end())
			{
				info.kind = k->kind;
				info.value = k->generation;
				info.name = k->name;
			}
			else if (MultiStageWithOutput(h))
			{
				info.kind = HookKind::Opaque;
				info.name = "a shader with an OUTPUT pass and an earlier stage";
			}
			else if (mpvHookFn && reinterpret_cast<const void*>(h->hook) == mpvHookFn)
			{
				if (temporal.IsTemporal(h))
				{
					info.kind = HookKind::Opaque;
					info.name = "a shader using frame/random/buffer state";
				}
				else
				{
					info.kind = HookKind::Parameters;
					Signature s;
					for (int i = 0; i < h->num_parameters; ++i)
						if (h->parameters[i].data)
							s.Add(i).Add(h->parameters[i].type).Bytes(h->parameters[i].data, sizeof(pl_var_data));
					info.value = s.Value();
				}
			}
			else
			{
				info.kind = HookKind::Opaque;
				info.name = "an unregistered native hook";
			}
			out.push_back(info);
		}
		HookInfo set;   // L4: the shader set itself
		set.kind = HookKind::Static;
		set.value = temporal.Generation();
		out.push_back(set);
		return out;
	}

	class Adapter
	{
	public:
		~Adapter() { Release(); }

		void Release()
		{
			if (m_probe)
				pl_mpv_user_shader_destroy(&m_probe);
			m_mpvHookFn = nullptr;
		}

		// libplacebo's mpv hook entry, from a trivial parsed shader (once per GPU).
		void Init(pl_gpu gpu)
		{
			if (m_probe || !gpu)
				return;
			static const char kProbe[] =
				"//!HOOK OUTPUT\n//!BIND HOOKED\n//!DESC vp repeat reuse probe\nvec4 hook() { return HOOKED_tex(HOOKED_pos); }\n";
			m_probe = pl_mpv_user_shader_parse(gpu, kProbe, std::strlen(kProbe));
			m_mpvHookFn = m_probe ? reinterpret_cast<const void*>(m_probe->hook) : nullptr;
		}

		std::vector<HookInfo> HookInfos(const std::vector<const pl_hook*>& hooks, const std::vector<KnownHook>& known) const
		{
			return ClassifyHooks(hooks, known, m_mpvHookFn);
		}

		bool DrawFromCache(pl_renderer renderer, pl_tex cached, const pl_color_repr& repr, const pl_color_space& color,
			const pl_rect2df& cacheTargetCrop, const pl_render_params& params, const std::vector<const pl_hook*>& hooks,
			const std::vector<const pl_hook*>& exclude, const pl_hook* encodingRepair, const pl_frame& destination)
		{
			if (!renderer || !cached)
				return false;
			m_hooks.clear();
			for (const pl_hook* h : hooks)
				if (std::find(exclude.begin(), exclude.end(), h) == exclude.end())
					m_hooks.push_back(h);
			pl_render_params rendered = DejudderRendered::OutputOnly(params, m_hooks, m_output);
			const auto repair = std::find(m_output.begin(), m_output.end(), encodingRepair);
			if (repair != m_output.end())
			{
				std::vector<const pl_hook*> withoutRepair(m_output);
				withoutRepair.erase(withoutRepair.begin() + (repair - m_output.begin()));
				const pl_render_errors errors = pl_renderer_get_errors(renderer);
				if (!OutputEncodingRepair::Needed(withoutRepair, &errors, PL_LUT_UNKNOWN))
					m_output = std::move(withoutRepair);
				rendered.hooks = m_output.empty() ? nullptr : m_output.data();
				rendered.num_hooks = static_cast<int>(m_output.size());
			}
			const pl_frame image = DejudderRendered::Frame(cached, repr, color);
			pl_frame target = destination;
			target.crop = cacheTargetCrop;
			target.lut = nullptr;
			target.lut_type = PL_LUT_UNKNOWN;
			return pl_render_image(renderer, &image, &target, &rendered);
		}

	private:
		const pl_hook* m_probe = nullptr;
		const void* m_mpvHookFn = nullptr;
		std::vector<const pl_hook*> m_hooks, m_output;
	};
}
