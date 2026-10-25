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
#include <SubtitleCutPaste.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
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
		VpExtension extension;                       // re-base, LUT identity, fallbacks (H2); subtitles: NoteSubtitleComposition
		const pl_frame* image = nullptr;             // this refresh's source frame and target (for the
		const pl_frame* target = nullptr;            // cache's rounded target rectangle, as the extras)
	};

	class Glue final : public RepeatRenderReuse::IRepeatReuse
	{
	public:
		// The host's "Fallbacks engaged" registry (vp-no-silent-fallbacks, audit M4): a failed reuse draw
		// and a switched-on reuse that cannot work (a persistent refusal) are visible there, with the
		// GUI's one-time notice, not only as a ratio on the status line.
		using EngageFn = std::function<void(const char* id, const std::string& reason)>;
		using ReleaseFn = std::function<void(const char* id)>;
		static constexpr const char* kFallbackId = "repeat reuse";
		Glue(EngageFn engage = nullptr, ReleaseFn release = nullptr) : m_engage(std::move(engage)), m_release(std::move(release)) {}

		void SetOptions(const Options& o) { m_policy.SetOptions(o); }
		bool Active() const { return m_policy.Active(); }
		void SetContext(const VpContext& c) { m_ctx = c; }
		const RepeatFrameKey& Key() const { return m_key; }
		const Stats& GetStats() const override { return m_policy.GetStats(); }
		std::string StatusLine() const { return m_policy.GetStats().Line(); }

		void Release()
		{
			SetFallback(std::string());
			m_adapter.Release();
			m_policy.Reset();
			m_decision = Decision{};
		}

		// From the subtitle composition (each refresh it composes): everything that shapes the composed
		// picture besides the settings (audit H2.3).
		void NoteSubtitleComposition(const SubtitleCutPasteGeometry& move, int background, bool donorReady)
		{
			if (!Active())
				return;
			RepeatRenderReuse::Signature s;
			for (const SubtitleBoxRect* r : { &move.source, &move.destination, &move.content, &move.generatedCleanup,
				&move.pictureCapture, &move.placementLimits, &move.layoutContent, &move.layoutCoverage,
				&move.glyphLines[0], &move.glyphLines[1], &move.glyphLines[2] })
				s.Add(r->left).Add(r->top).Add(r->right).Add(r->bottom);
			s.Add(move.glyphScale).Add(move.glyphTranslateX).Add(move.glyphTranslateY).Add(move.centeredGlyphMapping)
				.Add(move.roundedCorners).Add(move.extendToBar).Add(move.pictureIntrusion).Add(move.pictureTop)
				.Add(move.pictureBottom).Add(move.valid).Add(move.fromTopBar).Add(background).Add(donorReady);
			m_subtitleSignature = s.Value();
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
			VpExtension extension = m_ctx.extension;
			extension.subtitleSignature = m_subtitleSignature;
			m_key.extension = extension.Signature();
			const double now = NowSeconds();
			if (m_key.frameGeneration != m_generation)
			{
				m_generation = m_key.frameGeneration;   // sticky per stream: a new stream starts clean
				SetFallback(std::string());
			}
			m_decision = m_policy.Decide(m_key, m_ctx.cache->Texture(m_key.sourceSequence) != nullptr, now);
			if (m_key.cadenceRepeat && !m_drawFailedThisStream)
			{
				if (m_decision.reuse)
					SetFallback(std::string());
				else if (m_policy.ArmingBackedOff(now) && Persistent(m_decision.refusal))
					SetFallback(std::string("repeats render in full (") + RepeatRenderReuse::Describe(m_decision.refusal) +
						(m_key.opaqueDetail.empty() || m_decision.refusal != Refusal::HookStateOpaque ? std::string() : ": " + m_key.opaqueDetail) + ")");
			}
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
			m_drawFailedThisStream = true;
			SetFallback("reuse draw failed; repeats render in full");
			return false;
		}

		void NoteRendered(const RepeatFrameKey&, bool rendered) override
		{
			if (Active() && m_ctx.cache && (!m_decision.reuse || m_failed))
				m_policy.NoteFullRender(m_key, rendered && m_ctx.cache->Texture(m_key.sourceSequence) != nullptr);
			m_decision = Decision{};
			m_subtitleSignature = 0;   // the next refresh's composition (or none) sets it again
		}

	private:
		void SetFallback(const std::string& reason)
		{
			if (reason == m_fallbackReason)
				return;
			if (reason.empty())
			{
				m_drawFailedThisStream = false;
				if (m_release) m_release(kFallbackId);
			}
			else if (m_engage)
				m_engage(kFallbackId, reason);
			m_fallbackReason = reason;
		}

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
		uint64_t m_subtitleSignature = 0;
		EngageFn m_engage;
		ReleaseFn m_release;
		std::string m_fallbackReason;
		uint64_t m_generation = 0;
		bool m_drawFailedThisStream = false;
		std::vector<const pl_hook*>* m_hooks = nullptr;
		pl_rect2df m_cacheTargetCrop{};
	};
}
