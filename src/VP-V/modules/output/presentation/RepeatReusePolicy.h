#pragma once

// VP's captured-repeat render reuse policy (WP-24, VP-ONLY, 2026-10-24 draft).
//
// Pure and header-only (no libplacebo): decides, from the upstream key (RepeatRenderReuse.h), whether
// a captured repeat is drawn from the rendered-picture cache (the de-judder extras' 6d cache, the
// source frame's picture at the end of PRE_OUTPUT) with only the OUTPUT stage, and whether a first
// present should arm the cache at all.
//
// - repeat_render_reuse (Config, Output unit): on | off. Draft default OFF until the GPU byte-identity
//   gate passes; then the owner's rule applies (features default on when faithful on the 3090).
//   The Original pathway sets it off (upstream renders every refresh).
// - Arming: the cache copy costs one output-size copy per source frame, so it is armed only while
//   repeats are being seen (a captured repeat within the last kArmWindowSeconds - time based, no
//   fixed cadence list: vp-general-not-fixed), or when the de-judder arms it anyway.
// - Hook state: every hook that runs before OUTPUT contributes to the key. mpv-style hooks (their
//   pointer and the current value of every parameter: LED compensation, tone shape, DV trims, intent
//   values change through parameters), registered static C hooks (their pointer), picture-neutral
//   hooks (LED measurement, the cache copy) are skipped, and any OTHER hook is opaque: it refuses
//   the reuse (safe default; a module opts in by registering a state generation).
// - Visible: the counter line "Repeats reused: N of M (last refusal: ...)" goes on the de-judder /
//   output status (Ctrl+I) and into the problem report; a failed reuse draw is counted and rendered
//   in full on the same refresh (never a dropped refresh, never log-only).

#include <vprenderer/RepeatRenderReuse.h>

#include <cstdint>
#include <string>
#include <vector>

namespace VpRepeatReuse
{
	using namespace RepeatRenderReuse;

	constexpr double kArmWindowSeconds = 1.0;   // repeats seen within the last second keep the cache armed

	enum class HookKind : int
	{
		Parameters,      // mpv-style hook: pointer + every parameter's current value
		Static,          // registered C hook whose output depends only on the source frame + settings
		Generation,      // registered C hook with a module-provided state generation
		PictureNeutral,  // reads the picture only (LED measurement) or copies it (the cache)
		Opaque,          // unknown C hook: refuses the reuse
	};

	struct HookInfo
	{
		const void* id = nullptr;
		bool beforeOutput = true;   // runs at a stage before OUTPUT (OUTPUT-only hooks run again anyway)
		HookKind kind = HookKind::Opaque;
		uint64_t value = 0;         // Parameters: hash of the parameter values; Generation: the generation
		const char* name = nullptr; // for the status line when the hook is opaque
	};

	// The hookState signature; `opaque` set when a pre-OUTPUT hook's state cannot be known.
	inline uint64_t HookStateSignature(const std::vector<HookInfo>& hooks, bool& opaque, std::string* detail = nullptr)
	{
		Signature s;
		opaque = false;
		for (const HookInfo& h : hooks)
		{
			if (!h.beforeOutput || h.kind == HookKind::PictureNeutral)
				continue;
			if (h.kind == HookKind::Opaque)
			{
				if (!opaque && detail)
					*detail = std::string(h.name ? h.name : "a hook") + " has no registered state";
				opaque = true;
			}
			s.Add(h.id).Add(static_cast<int>(h.kind)).Add(h.value);
		}
		return s.Value();
	}

	// VP stages whose state advances per refresh/render: while any is active the repeat renders in full.
	struct VpTemporal
	{
		bool phasePicture = false;       // VP timeline / interpolation draws a new picture per refresh
		bool dejudderExtras = false;     // the extras draw this frame's refreshes themselves
		bool crossfade = false;          // source-resolution crossfade (rebaseFade < 1)
		bool startupHoldBlack = false;   // h3-latch K3 black hold
		bool calibrationPattern = false;
		bool deinterlacing = false;      // upstream's temporal renderer (history per render)
		bool frameMixing = false;        // libplacebo smooth motion / temporal mixer
		bool peakDetection = false;      // libplacebo HDR peak detection smooths per render
		bool Any() const
		{
			return phasePicture || dejudderExtras || crossfade || startupHoldBlack || calibrationPattern || deinterlacing ||
				frameMixing || peakDetection;
		}
	};

	struct Options
	{
		bool enabled = false;            // repeat_render_reuse (draft default off)
		bool originalPathway = false;    // upstream behaviour: never reuse
	};

	class Policy
	{
	public:
		void SetOptions(const Options& o) { m_options = o; }
		const Options& GetOptions() const { return m_options; }
		bool Active() const { return m_options.enabled && !m_options.originalPathway; }

		// Combine the host key with VP's temporal flags and hook state.
		static RepeatFrameKey Complete(RepeatFrameKey key, const VpTemporal& temporal, const std::vector<HookInfo>& hooks)
		{
			bool opaque = false;
			key.hookState = HookStateSignature(hooks, opaque, &key.opaqueDetail);
			key.temporalRenderState = key.temporalRenderState || temporal.Any();
			key.hookStateOpaque = key.hookStateOpaque || opaque;   // refuses as HookStateChanged
			return key;
		}

		// At the hook point. nowSeconds: the render thread's clock (for the arm window).
		Decision Decide(const RepeatFrameKey& now, bool cacheValid, double nowSeconds)
		{
			if (now.cadenceRepeat)
				m_lastRepeatSeconds = nowSeconds;
			Decision d;
			if (!Active())
				d.refusal = Refusal::Disabled;
			else
				d = RepeatReuseRules::Compare(m_cached, cacheValid && m_cachedValid, now);
			m_stats.Note(now.cadenceRepeat, d, d.refusal == Refusal::HookStateOpaque ? now.opaqueDetail : std::string());
			return d;
		}

		// A reuse draw failed after Decide said reuse: counted, and the loop renders in full.
		void NoteDrawFailed()
		{
			if (m_stats.reused > 0)
				--m_stats.reused;
			++m_stats.refusals[static_cast<int>(Refusal::DrawFailed)];
			m_stats.last = Refusal::DrawFailed;
		}

		// Arm the cache on this first present? (The de-judder may arm it regardless.)
		bool WantArm(const RepeatFrameKey& now, double nowSeconds) const
		{
			return Active() && !now.cadenceRepeat && !now.temporalRenderState && m_lastRepeatSeconds >= 0.0 &&
				nowSeconds - m_lastRepeatSeconds <= kArmWindowSeconds;
		}

		// After a full render that is a source frame's first present.
		void NoteFullRender(const RepeatFrameKey& key, bool cacheCaptured)
		{
			if (key.cadenceRepeat)
				return;   // a refused repeat rendered in full: the cached first present stays the reference
			m_cached = key;
			m_cachedValid = cacheCaptured;
		}

		void Reset() { m_cachedValid = false; m_cached = RepeatFrameKey{}; m_lastRepeatSeconds = -1.0; }
		const Stats& GetStats() const { return m_stats; }

	private:
		Options m_options;
		RepeatFrameKey m_cached;
		bool m_cachedValid = false;
		double m_lastRepeatSeconds = -1.0;
		Stats m_stats;
	};
}
