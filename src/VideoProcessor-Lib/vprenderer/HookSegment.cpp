#include <pch.h>
#include "HookSegment.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/sampling.h>

#include <algorithm>
#include <cmath>

namespace HookSegment
{
	// One merged hook: the libplacebo-facing pl_hook, its inner hooks in order, its own pool
	// and timers (one per inner hook, created on first sampled use).
	struct Runner::Fused
	{
		Runner* owner = nullptr;
		pl_hook hook{};
		std::vector<const pl_hook*> inner;
		std::vector<pl_timer> timers;
		Sampler sampler;
		std::unique_ptr<TexturePool<pl_tex, pl_fmt>> pool;
	};

	namespace
	{
		uint64_t Mix(uint64_t h, uint64_t v)
		{
			// FNV-1a over the inner signatures: a stable signature for the merged hook.
			for (int i = 0; i < 8; ++i)
			{
				h ^= (v >> (i * 8)) & 0xff;
				h *= 1099511628211ull;
			}
			return h;
		}

		bool Disabled(const pl_hook* hook, const pl_render_errors* errors)
		{
			if (!errors || !hook)
				return false;
			for (int i = 0; i < errors->num_disabled_hooks; ++i)
				if (errors->disabled_hooks[i] == hook->signature)
					return true;
			return false;
		}
	}

	Runner::Runner(pl_gpu gpuIn) : gpu(gpuIn)
	{
	}

	Runner::~Runner()
	{
		Release();
	}

	pl_fmt Runner::Format() const
	{
		const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
		pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, float32 ? 32 : 16, 0, caps);
		if (!fmt && float32)
			fmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 16, 0, caps);
		return fmt;
	}

	void Runner::Fuse(const std::vector<const pl_hook*>& in, std::vector<const pl_hook*>& out,
		const std::function<bool(const pl_hook*)>& member, const pl_render_errors* errors)
	{
		std::vector<uint32_t> stages(in.size());
		std::vector<bool> members(in.size()), off(in.size());
		for (size_t i = 0; i < in.size(); ++i)
		{
			stages[i] = in[i] ? static_cast<uint32_t>(in[i]->stages) : 0u;
			members[i] = in[i] && in[i]->hook && member(in[i]);
			off[i] = Disabled(in[i], errors) || (in[i] && disabled.count(in[i]->signature));
		}
		const std::vector<int> run = GroupRuns(stages, members, off);
		used = 0;
		out.clear();
		out.reserve(in.size());
		std::vector<Fused*> byRun;
		for (size_t i = 0; i < in.size(); ++i)
		{
			if (run[i] < 0)
			{
				out.push_back(in[i]);
				continue;
			}
			const size_t r = static_cast<size_t>(run[i]);
			if (r < byRun.size() && byRun[r])
			{
				byRun[r]->inner.push_back(in[i]);
				byRun[r]->hook.signature = Mix(byRun[r]->hook.signature, in[i]->signature);
				continue;
			}
			if (used == fused.size())
			{
				std::unique_ptr<Fused> f(new Fused());
				f->owner = this;
				pl_gpu g = gpu;
				f->pool.reset(new TexturePool<pl_tex, pl_fmt>(
					[g](int w, int h, pl_fmt fmt) -> pl_tex
					{
						if (!fmt)
							return nullptr;
						pl_tex_params p{};
						p.w = w;
						p.h = h;
						p.format = fmt;
						p.sampleable = true;
						p.renderable = true;
						p.storable = (fmt->caps & PL_FMT_CAP_STORABLE) != 0;
						p.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
						p.blit_dst = p.blit_src;
						return pl_tex_create(g, &p);
					},
					[g](pl_tex t) { pl_tex_destroy(g, &t); }));
				fused.push_back(std::move(f));
			}
			Fused& f = *fused[used++];
			f.inner.assign(1, in[i]);
			f.hook = pl_hook{};
			f.hook.stages = in[i]->stages;
			f.hook.input = PL_HOOK_SIG_TEX;
			f.hook.priv = &f;
			f.hook.reset = &Runner::Reset;
			f.hook.hook = &Runner::Run;
			f.hook.signature = Mix(14695981039346656037ull, in[i]->signature);
			f.sampler.SetEvery(sampleEvery ? sampleEvery : 1);
			if (byRun.size() <= r)
				byRun.resize(r + 1, nullptr);
			byRun[r] = &f;
			out.push_back(&f.hook);
		}
	}

	void Runner::Reset(void* priv)
	{
		Fused& f = *static_cast<Fused*>(priv);
		for (const pl_hook* h : f.inner)
			if (h->reset)
				h->reset(h->priv);
	}

	pl_tex Runner::GetTex(void* priv, int width, int height)
	{
		Fused& f = *static_cast<Fused*>(priv);
		return f.pool->Get(width, height, f.owner->Format());
	}

	pl_hook_res Runner::Run(void* priv, const pl_hook_params* params)
	{
		Fused& f = *static_cast<Fused*>(priv);
		return f.owner->RunFused(f, params);
	}

	pl_hook_res Runner::RunFused(Fused& f, const pl_hook_params* params)
	{
		f.pool->BeginRun();
		const bool timed = sampleEvery != 0 && f.sampler.BeginRun();
		if (f.timers.size() < f.inner.size())
			f.timers.resize(f.inner.size(), nullptr);

		pl_tex current = params->tex;
		pl_rect2df rect = params->rect;
		pl_color_repr repr = params->repr;
		pl_color_space color = params->color;
		int components = params->components;

		for (size_t i = 0; i < f.inner.size(); ++i)
		{
			const pl_hook* h = f.inner[i];
			if (disabled.count(h->signature))
				continue;
			pl_hook_params q = *params;
			q.get_tex = &Runner::GetTex;
			q.priv = &f;
			q.tex = current;
			q.rect = rect;
			q.repr = repr;
			q.color = color;
			q.components = components;
			q.sh = nullptr;
			if (h->input == PL_HOOK_SIG_COLOR)
			{
				q.sh = pl_dispatch_begin(params->dispatch);
				pl_sample_src src{};
				src.tex = current;
				src.rect = rect;
				src.components = components;
				if (!pl_shader_sample_direct(q.sh, &src))
				{
					pl_dispatch_abort(params->dispatch, &q.sh);
					disabled.insert(h->signature);
					failures.push_back({ h->signature, "could not sample the hand-off texture" });
					continue;
				}
			}
			pl_hook_res res = h->hook(h->priv, &q);
			if (res.failed)
			{
				if (q.sh)
					pl_dispatch_abort(params->dispatch, &q.sh);
				disabled.insert(h->signature);
				failures.push_back({ h->signature, "hook failed" });
				continue;
			}
			switch (res.output)
			{
			case PL_HOOK_SIG_NONE:
				if (q.sh)
					pl_dispatch_abort(params->dispatch, &q.sh);
				break;
			case PL_HOOK_SIG_TEX:
				if (!res.tex)
				{
					disabled.insert(h->signature);
					failures.push_back({ h->signature, "hook returned no texture" });
					break;
				}
				current = res.tex;
				repr = res.repr;
				color = res.color;
				components = res.components;
				// PRE_OUTPUT / OUTPUT are not resizable: libplacebo ignores the returned rect.
				break;
			case PL_HOOK_SIG_COLOR:
			{
				const int w = static_cast<int>(std::lround(std::fabs(rect.x1 - rect.x0)));
				const int hgt = static_cast<int>(std::lround(std::fabs(rect.y1 - rect.y0)));
				pl_tex target = f.pool->Get(w, hgt, Format());
				if (!target)
				{
					pl_dispatch_abort(params->dispatch, &res.sh);
					failures.push_back({ h->signature, "no intermediate texture" });
					break;
				}
				pl_dispatch_params dp{};
				dp.shader = &res.sh;
				dp.target = target;
				if (timed)
				{
					if (!f.timers[i])
						f.timers[i] = pl_timer_create(gpu);
					dp.timer = f.timers[i];
				}
				if (!pl_dispatch_finish(params->dispatch, &dp))
				{
					disabled.insert(h->signature);
					failures.push_back({ h->signature, "dispatch failed" });
					break;
				}
				current = target;
				rect = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(hgt) };
				repr = res.repr;
				color = res.color;
				components = res.components;
				break;
			}
			default:
				break;
			}
			// Poll (never wait): 0 = not ready yet.
			if (f.timers[i])
			{
				const uint64_t ns = pl_timer_query(gpu, f.timers[i]);
				if (ns)
				{
					fresh.emplace_back(h, ns);
					auto it = std::find_if(lastNs.begin(), lastNs.end(),
						[h](const std::pair<const pl_hook*, uint64_t>& e) { return e.first == h; });
					if (it == lastNs.end())
						lastNs.emplace_back(h, ns);
					else
						it->second = ns;
				}
			}
		}

		pl_hook_res out{};
		out.output = PL_HOOK_SIG_TEX;
		out.tex = current;
		out.repr = repr;
		out.color = color;
		out.components = components;
		out.rect = rect;
		return out;
	}

	std::vector<Failure> Runner::TakeFailures()
	{
		std::vector<Failure> taken;
		taken.swap(failures);
		return taken;
	}

	std::vector<std::pair<const pl_hook*, uint64_t>> Runner::TakeTimings()
	{
		std::vector<std::pair<const pl_hook*, uint64_t>> taken;
		taken.swap(fresh);
		return taken;
	}

	uint64_t Runner::LastNs(const pl_hook* inner) const
	{
		for (const auto& e : lastNs)
			if (e.first == inner)
				return e.second;
		return 0;
	}

	void Runner::Release()
	{
		for (auto& f : fused)
		{
			for (pl_timer& t : f->timers)
				if (t)
					pl_timer_destroy(gpu, &t);
			f->timers.clear();
			if (f->pool)
				f->pool->Clear();
		}
		fused.clear();
		used = 0;
		lastNs.clear();
		fresh.clear();
	}
}
