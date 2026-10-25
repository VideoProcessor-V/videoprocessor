#pragma once

// VP's IRepeatReuse implementation (WP-24, VP-ONLY; audit M5 2026-10-25): every piece of VP logic
// the host used to carry inline (the temporal flags, the known-hook table, the arming, the key
// completion, the reuse draw, the reference bookkeeping) lives here. The host makes single calls:
//
//   SetOptions(...)                                   once per refresh (two booleans)
//   if (Active()) { SetContext({...}); PrepareFirstPresent(KeyFrom(...), frameHooks); }
//   if (TryDrawRepeat(Key(), renderer, destination, parameters)) return true;   (draw callback)
//   NoteRendered(Key(), rendered && !retried);
//
// Thread: the render thread only.

#include "RepeatReuseAdapter.h"

#include <vprenderer/RenderedWarp.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

namespace VpRepeatReuse
{
	// What the host knows that VP's key needs (filled with one aggregate statement per refresh).
	struct VpContext
	{
		VpTemporal temporal;
		DejudderRendered::Cache* cache = nullptr;    // the 6d rendered-picture cache (shared with the extras)
		pl_gpu gpu = nullptr;
		const pl_hook* encodingRepair = nullptr;
		std::array<KnownHook, 12> known{};           // registered hooks (no per-refresh allocation)
		int knownCount = 0;
		std::array<const pl_hook*, 4> exclude{};     // picture-neutral hooks left out of the reuse draw
		int excludeCount = 0;
		const pl_frame* image = nullptr;             // this refresh's source frame and target (for the
		const pl_frame* target = nullptr;            // cache's rounded target rectangle, as the extras)
	};

	class Glue final : public RepeatRenderReuse::IRepeatReuse
	{
	public:
		void SetOptions(const Options& o) { m_policy.SetOptions(o); }
		bool Active() const { return m_policy.Active(); }
		void SetContext(const VpContext& c) { m_ctx = c; }
		const RepeatFrameKey& Key() const { return m_key; }
		const Stats& GetStats() const override { return m_policy.GetStats(); }
		std::string StatusLine() const { return m_policy.GetStats().Line(); }

		void Release()
		{
			m_adapter.Release();
			m_policy.Reset();
			m_decision = Decision{};
		}

		void PrepareFirstPresent(const RepeatFrameKey& host, std::vector<const pl_hook*>& hooks) override
		{
			m_decision = Decision{};
			m_failed = false;
			m_hooks = &hooks;
			if (!Active() || !m_ctx.cache)
				return;
			m_adapter.Init(m_ctx.gpu);
			const std::vector<KnownHook> known(m_ctx.known.begin(), m_ctx.known.begin() + m_ctx.knownCount);
			m_key = Policy::Complete(host, m_ctx.temporal, m_adapter.HookInfos(hooks, known));
			const double now = NowSeconds();
			m_decision = m_policy.Decide(m_key, m_ctx.cache->Texture(m_key.sourceSequence) != nullptr, now);
			if (m_key.sourceSequence != 0 && !m_ctx.cache->Armed(m_key.sourceSequence) && m_policy.WantArm(m_key, now))
			{
				m_ctx.cache->Arm(m_key.sourceSequence);
				hooks.push_back(m_ctx.cache->Hook());
			}
			if (!m_key.cadenceRepeat && m_ctx.image && m_ctx.target)
			{
				const pl_tex fb = m_ctx.target->num_planes > 0 ? m_ctx.target->planes[0].texture : nullptr;
				m_cacheTargetCrop = DejudderRendered::Rounded(m_ctx.image->crop, m_ctx.target->crop,
					fb ? static_cast<float>(fb->params.w) : 0.0f, fb ? static_cast<float>(fb->params.h) : 0.0f).target;
			}
		}

		bool TryDrawRepeat(const RepeatFrameKey&, pl_renderer renderer, const pl_frame& destination,
			const pl_render_params& params) override
		{
			if (!m_decision.reuse || !m_hooks || !m_ctx.cache)
				return false;
			const std::vector<const pl_hook*> exclude(m_ctx.exclude.begin(), m_ctx.exclude.begin() + m_ctx.excludeCount);
			if (m_adapter.DrawFromCache(renderer, m_ctx.cache->Texture(m_key.sourceSequence), m_ctx.cache->Repr(),
				m_ctx.cache->Color(), m_cacheTargetCrop, params, *m_hooks, exclude, m_ctx.encodingRepair, destination))
				return true;
			m_failed = true;
			m_policy.NoteDrawFailed();
			return false;
		}

		void NoteRendered(const RepeatFrameKey&, bool rendered) override
		{
			if (Active() && m_ctx.cache && (!m_decision.reuse || m_failed))
				m_policy.NoteFullRender(m_key, rendered && m_ctx.cache->Texture(m_key.sourceSequence) != nullptr);
			m_decision = Decision{};
		}

	private:
		static double NowSeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		Policy m_policy;
		Adapter m_adapter;
		VpContext m_ctx;
		RepeatFrameKey m_key;
		Decision m_decision;
		bool m_failed = false;
		std::vector<const pl_hook*>* m_hooks = nullptr;
		pl_rect2df m_cacheTargetCrop{};
	};
}
