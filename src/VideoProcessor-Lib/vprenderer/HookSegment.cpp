#include <pch.h>
#include "HookSegment.h"

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/sampling.h>

#include <algorithm>

namespace HookSegment
{
	// One merged hook: the libplacebo-facing pl_hook, its inner hooks in order, its own pool
	// and timers (keyed by inner hook, created on first sampled use).
	struct Runner::Fused
	{
		Runner* owner = nullptr;
		pl_hook hook{};
		std::vector<const pl_hook*> inner;
		std::vector<std::pair<const pl_hook*, pl_timer>> timers;
		Sampler sampler;
		pl_fmt format = nullptr;   // this run's intermediate format (from libplacebo's input)
		std::unique_ptr<TexturePool<pl_tex, pl_fmt>> pool;

		pl_timer& TimerFor(const pl_hook* h)
		{
			for (auto& t : timers)
				if (t.first == h)
					return t.second;
			timers.emplace_back(h, nullptr);
			return timers.back().second;
		}
	};

	namespace
	{
		uint64_t Mix(uint64_t h, uint64_t v)
		{
			// FNV-1a over the inner signatures (and the generation): a stable signature per
			// configuration for the merged hook.
			for (int i = 0; i < 8; ++i)
			{
				h ^= (v >> (i * 8)) & 0xff;
				h *= 1099511628211ull;
			}
			return h;
		}

		bool Disabled(uint64_t signature, const pl_render_errors* errors)
		{
			if (!errors)
				return false;
			for (int i = 0; i < errors->num_disabled_hooks; ++i)
				if (errors->disabled_hooks[i] == signature)
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

	// The intermediates take libplacebo's own FBO format (audit M1): the texture it handed in. A
	// texture with fewer than 4 components gets the 4-component float format of the same depth.
	pl_fmt Runner::FormatFor(pl_tex input) const
	{
		if (!input || !input->params.format)
			return nullptr;
		const pl_fmt in = input->params.format;
		if (in->num_components == 4 && (in->caps & PL_FMT_CAP_RENDERABLE) && (in->caps & PL_FMT_CAP_SAMPLEABLE))
			return in;
		const enum pl_fmt_caps caps = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
		return pl_find_fmt(gpu, PL_FMT_FLOAT, 4, in->component_depth[0] >= 32 ? 32 : 16, 0, caps);
	}

	void Runner::Fuse(const std::vector<const pl_hook*>& in, std::vector<const pl_hook*>& out,
		const std::function<bool(const pl_hook*)>& member, const pl_render_errors* errors)
	{
		const size_t n = in.size();
		stageBits.assign(n, 0u);
		memberFlags.assign(n, false);
		transparentFlags.assign(n, false);
		for (size_t i = 0; i < n; ++i)
		{
			const pl_hook* h = in[i];
			if (!h)
				continue;
			stageBits[i] = static_cast<uint32_t>(h->stages);
			transparentFlags[i] = Disabled(h->signature, errors);
			// A hook that failed inside a merged hook leaves the merge (audit H1): it runs separately
			// at its own place, so it splits the run instead of being moved.
			memberFlags[i] = h->hook && !failedSignatures.count(h->signature) && member(h);
		}
		GroupRuns(stageBits, memberFlags, transparentFlags, runs);
		used = 0;
		out.clear();
		out.reserve(n);
		byRun.clear();
		for (size_t i = 0; i < n; ++i)
		{
			if (runs[i] < 0)
			{
				out.push_back(in[i]);
				continue;
			}
			const size_t r = static_cast<size_t>(runs[i]);
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
						if (!fmt || !g)
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
					[g](pl_tex t) { if (g) pl_tex_destroy(g, &t); }));
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
			f.hook.signature = Mix(Mix(14695981039346656037ull, generation), in[i]->signature);
			f.sampler.SetEvery(sampleEvery ? sampleEvery : 1);
			if (byRun.size() <= r)
				byRun.resize(r + 1, nullptr);
			byRun[r] = &f;
			out.push_back(&f.hook);
		}
		// Merged slots not used by this Fuse give their textures back (audit M3).
		for (size_t k = used; k < fused.size(); ++k)
			if (fused[k]->pool && fused[k]->pool->Size())
				fused[k]->pool->Clear();
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
		return f.pool->Get(width, height, f.format);
	}

	pl_hook_res Runner::Run(void* priv, const pl_hook_params* params)
	{
		Fused& f = *static_cast<Fused*>(priv);
		return f.owner->RunFused(f, params);
	}

	pl_hook_res Runner::RunFused(Fused& f, const pl_hook_params* params)
	{
		f.pool->BeginRun();
		const uint64_t createdBefore = f.pool->Created();
		const bool timed = sampleEvery != 0 && f.sampler.BeginRun();
		f.format = FormatFor(params->tex);

		pl_tex current = params->tex;
		pl_color_repr repr = params->repr;
		pl_color_space color = params->color;
		int components = params->components;
		pl_shader pendingColor = nullptr;   // a shader result not yet dispatched (audit L2)

		// Finishes a pending shader result into a pool texture of the current size; the rect stays
		// libplacebo's (the texture is copied 1:1).
		auto flush = [&](const pl_hook* owner) -> bool
		{
			if (!pendingColor)
				return true;
			pl_tex target = current ? f.pool->Get(current->params.w, current->params.h, f.format) : nullptr;
			if (!target)
			{
				pl_dispatch_abort(params->dispatch, &pendingColor);
				MarkFailed(owner, "no intermediate texture");
				return false;
			}
			pl_dispatch_params dp{};
			dp.shader = &pendingColor;
			dp.target = target;
			if (timed)
			{
				pl_timer& t = f.TimerFor(owner);
				if (!t)
					t = pl_timer_create(gpu);
				dp.timer = t;
			}
			if (!pl_dispatch_finish(params->dispatch, &dp))
			{
				MarkFailed(owner, "dispatch failed");
				return false;
			}
			current = target;
			return true;
		};

		const pl_hook* pendingOwner = nullptr;
		for (size_t i = 0; i < f.inner.size(); ++i)
		{
			const pl_hook* h = f.inner[i];
			if (failedSignatures.count(h->signature))
				continue;
			flush(pendingOwner);   // a failure is reported; the picture continues without that stage
			pendingOwner = nullptr;
			pl_hook_params q = *params;
			q.get_tex = &Runner::GetTex;
			q.priv = &f;
			q.tex = current;
			q.repr = repr;
			q.color = color;
			q.components = components;
			q.sh = nullptr;
			if (h->input == PL_HOOK_SIG_COLOR)
			{
				// As libplacebo's img_sh: the whole texture, default components.
				q.sh = pl_dispatch_begin(params->dispatch);
				pl_sample_src src{};
				src.tex = current;
				if (!pl_shader_sample_direct(q.sh, &src))
				{
					pl_dispatch_abort(params->dispatch, &q.sh);
					MarkFailed(h, "could not sample the hand-off texture");
					continue;
				}
			}
			pl_hook_res res = h->hook(h->priv, &q);
			if (res.failed)
			{
				if (q.sh)
					pl_dispatch_abort(params->dispatch, &q.sh);
				MarkFailed(h, "hook failed");
				continue;
			}
			switch (res.output)
			{
			case PL_HOOK_SIG_NONE:
				if (q.sh)
					pl_dispatch_abort(params->dispatch, &q.sh);
				break;
			case PL_HOOK_SIG_TEX:
				if (!res.tex || !current || res.tex->params.w != current->params.w || res.tex->params.h != current->params.h)
				{
					// PRE_OUTPUT / OUTPUT are not resizable: libplacebo would reject this hook too.
					MarkFailed(h, res.tex ? "hook resized a non-resizable stage" : "hook returned no texture");
					break;
				}
				current = res.tex;
				repr = res.repr;
				color = res.color;
				components = res.components;
				break;
			case PL_HOOK_SIG_COLOR:
				pendingColor = res.sh;
				pendingOwner = h;
				repr = res.repr;
				color = res.color;
				components = res.components;
				break;
			default:
				break;
			}
		}

		// Poll every timer (never wait): 0 = not ready yet.
		for (auto& t : f.timers)
		{
			if (!t.second)
				continue;
			const uint64_t ns = pl_timer_query(gpu, t.second);
			if (!ns)
				continue;
			fresh.emplace_back(t.first, ns);
			auto it = std::find_if(lastNs.begin(), lastNs.end(),
				[&t](const std::pair<const pl_hook*, uint64_t>& e) { return e.first == t.first; });
			if (it == lastNs.end())
				lastNs.emplace_back(t.first, ns);
			else
				it->second = ns;
		}
		// A size or format change created textures: free the ones this run did not use (audit M3).
		if (f.pool->Created() != createdBefore)
			f.pool->Trim();

		pl_hook_res out{};
		if (pendingColor)
		{
			// A terminal shader result goes back to libplacebo, which folds it into its next pass.
			out.output = PL_HOOK_SIG_COLOR;
			out.sh = pendingColor;
		}
		else
		{
			out.output = PL_HOOK_SIG_TEX;
			out.tex = current;
		}
		out.repr = repr;
		out.color = color;
		out.components = components;
		out.rect = params->rect;
		return out;
	}

	void Runner::MarkFailed(const pl_hook* hook, const std::string& reason)
	{
		if (!hook)
			return;
		failedSignatures.insert(hook->signature);
		failures.push_back({ hook, hook->signature, reason });
	}

	bool Runner::Failed(const pl_hook* hook) const
	{
		return hook && failedSignatures.count(hook->signature) != 0;
	}

	void Runner::ClearFailed()
	{
		failedSignatures.clear();
		++generation;
	}

	std::vector<std::vector<const pl_hook*>> Runner::DisabledRuns(const pl_render_errors* errors) const
	{
		std::vector<std::vector<const pl_hook*>> runsOut;
		for (size_t k = 0; k < used; ++k)
			if (Disabled(fused[k]->hook.signature, errors))
				runsOut.push_back(fused[k]->inner);
		return runsOut;
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
			for (auto& t : f->timers)
				if (t.second && gpu)
					pl_timer_destroy(gpu, &t.second);
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
