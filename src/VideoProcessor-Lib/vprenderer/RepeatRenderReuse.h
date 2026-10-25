#pragma once

// Captured-repeat render reuse: the hook contract (WP-24, DRAFT; revised 2026-10-25 after audit M6).
//
// UPSTREAMABLE: standard library + libplacebo's public headers only. A captured repeat (a film
// frame shown again in a 24-in-30 / 30-in-60 signal: the queue's cadenceRepeat) re-runs the whole
// libplacebo render. When nothing that feeds the picture changed since the source frame's first
// present, the picture at the end of PRE_OUTPUT is the same, so only the OUTPUT stage needs to run.
//
// THREE call points in the render loop, one object (an IRepeatReuse; nullptr or NeverReuse = today):
//   1. PrepareFirstPresent(key, hooks)  - after hook assembly, before the draw: decides this refresh
//      (a repeat: reuse or not) and, on a first present that will repeat, may append a capture hook.
//   2. TryDrawRepeat(key, renderer, destination, params) - the FIRST statement of the draw callback
//      (the Draw passed to NlsHookRecovery::RenderOwned): true = drawn from the cache, return.
//   3. NoteRendered(key, rendered) - after the draw: the reference for the frame's repeats.
// Everything else in the loop runs on every refresh as before (presentation, overlays, telemetry,
// Present). KeyFrom() builds the generic part of the key from the frame the loop already has.

#include <libplacebo/renderer.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace RepeatRenderReuse
{
	// FNV-1a over raw bytes; floats by their bits (exact: any change refuses).
	class Signature
	{
	public:
		Signature& Bytes(const void* data, size_t size)
		{
			const unsigned char* p = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				m_value ^= p[i];
				m_value *= 1099511628211ull;
			}
			return *this;
		}
		template <class T> Signature& Add(const T& value) { return Bytes(&value, sizeof(T)); }
		Signature& Add(const std::string& text) { Add(text.size()); return Bytes(text.data(), text.size()); }
		uint64_t Value() const { return m_value; }
	private:
		uint64_t m_value = 14695981039346656037ull;
	};

	struct RepeatFrameKey
	{
		uint64_t sourceSequence = 0;      // the queued source frame (a repeat carries its frame's)
		bool cadenceRepeat = false;       // the queue marked this refresh a captured repeat
		bool nextIsRepeat = false;        // the queue already holds a repeat of this frame (arming)
		uint64_t frameGeneration = 0;     // stream generation (reset/resync moves it)
		uint64_t settingsGeneration = 0;  // render settings / profile / shader selection applied
		uint64_t geometry = 0;            // source crop, target crop, target size
		uint64_t colour = 0;              // source and target repr/colour (+HDR/DV metadata), LUT generation
		uint64_t overlays = 0;            // overlays baked into the picture (the IMAGE's overlays)
		uint64_t hookState = 0;           // every hook before OUTPUT and its parameter values
		bool hookStateOpaque = false;     // a hook before OUTPUT whose state cannot be known: never reuse
		uint64_t extension = 0;           // anything a fork adds (VP: re-base, LUT stage, subtitles, fallbacks)
		bool temporalRenderState = false; // per-RENDER temporal state: peak detection, deband grain, mixing, ...
		std::string opaqueDetail;         // what made hookStateOpaque (for the status line)
	};

	// The generic key from what the loop has at the draw. Target overlays are NOT hashed: they are
	// drawn again on every refresh (audit L3). lutGeneration: the host's LUT identity (a counter or
	// a file-version hash) instead of the pl_custom_lut pointer (ABA, audit H2).
	inline RepeatFrameKey KeyFrom(uint64_t sourceSequence, bool cadenceRepeat, bool nextIsRepeat, uint64_t frameGeneration,
		uint64_t settingsGeneration, const pl_frame& image, const pl_frame& target, const pl_render_params& params,
		uint64_t lutGeneration)
	{
		RepeatFrameKey k;
		k.sourceSequence = sourceSequence;
		k.cadenceRepeat = cadenceRepeat;
		k.nextIsRepeat = nextIsRepeat;
		k.frameGeneration = frameGeneration;
		k.settingsGeneration = settingsGeneration;
		const pl_tex fb = target.num_planes > 0 ? target.planes[0].texture : nullptr;
		k.geometry = Signature().Add(image.crop).Add(target.crop).Add(fb ? fb->params.w : 0).Add(fb ? fb->params.h : 0).Value();
		k.colour = Signature().Add(image.repr).Add(image.color).Add(target.repr).Add(target.color)
			.Add(target.lut != nullptr).Add(target.lut_type).Add(lutGeneration).Value();
		Signature o;
		o.Add(image.num_overlays);
		for (int i = 0; i < image.num_overlays; ++i)
		{
			const pl_overlay& ov = image.overlays[i];
			o.Add(static_cast<const void*>(ov.tex)).Add(ov.mode).Add(ov.repr).Add(ov.color).Add(ov.coords).Add(ov.num_parts);
			for (int p = 0; p < ov.num_parts; ++p)
				o.Add(ov.parts[p].src).Add(ov.parts[p].dst).Add(ov.parts[p].color);
		}
		k.overlays = o.Value();
		k.temporalRenderState = params.peak_detect_params != nullptr ||
			(params.deband_params != nullptr && params.deband_params->grain > 0.0f) ||   // audit L2: temporal grain
			params.frame_mixer != nullptr;
		return k;
	}

	enum class Refusal : int
	{
		None = 0,
		Disabled,
		NotRepeat,
		OtherSource,
		NoCache,
		StreamChanged,
		SettingsChanged,
		GeometryChanged,
		ColourChanged,
		OverlaysChanged,
		HookStateChanged,
		HookStateOpaque,   // a pre-OUTPUT hook whose state cannot be known (persistent: shown with its name)
		ExtensionChanged,
		TemporalState,
		DrawFailed,
	};
	constexpr int kRefusalCount = 15;

	inline const char* Describe(Refusal r)
	{
		static const char* const names[kRefusalCount] = { "reused", "off", "not a repeat", "other source frame",
			"no cached picture", "stream changed", "settings changed", "geometry changed", "colour changed",
			"overlays changed", "hook state changed", "hook state not known", "extension changed",
			"temporal state active", "reuse draw failed" };
		const int i = static_cast<int>(r);
		return i >= 0 && i < kRefusalCount ? names[i] : "unknown";
	}

	// Persistent reasons (arming then stops: no copy without a possible reuse, audit H1).
	inline bool Persistent(Refusal r) { return r == Refusal::HookStateOpaque || r == Refusal::TemporalState; }

	struct Decision
	{
		bool reuse = false;
		Refusal refusal = Refusal::Disabled;
	};

	struct RepeatReuseRules
	{
		static Decision Compare(const RepeatFrameKey& cached, bool cacheValid, const RepeatFrameKey& now)
		{
			Decision d;
			d.refusal = !now.cadenceRepeat ? Refusal::NotRepeat :
				now.sourceSequence == 0 || now.sourceSequence != cached.sourceSequence ? Refusal::OtherSource :
				now.hookStateOpaque || cached.hookStateOpaque ? Refusal::HookStateOpaque :
				now.temporalRenderState || cached.temporalRenderState ? Refusal::TemporalState :
				!cacheValid ? Refusal::NoCache :
				now.frameGeneration != cached.frameGeneration ? Refusal::StreamChanged :
				now.settingsGeneration != cached.settingsGeneration ? Refusal::SettingsChanged :
				now.geometry != cached.geometry ? Refusal::GeometryChanged :
				now.colour != cached.colour ? Refusal::ColourChanged :
				now.overlays != cached.overlays ? Refusal::OverlaysChanged :
				now.hookState != cached.hookState ? Refusal::HookStateChanged :
				now.extension != cached.extension ? Refusal::ExtensionChanged : Refusal::None;
			d.reuse = d.refusal == Refusal::None;
			return d;
		}
	};

	struct Stats
	{
		uint64_t repeats = 0, reused = 0, captures = 0;
		uint64_t refusals[kRefusalCount] = {};
		Refusal last = Refusal::None;
		std::string lastDetail;   // e.g. the opaque hook's name
		void Note(bool repeat, const Decision& d, const std::string& detail = std::string())
		{
			if (!repeat)
				return;
			++repeats;
			if (d.reuse)
				++reused;
			else
			{
				++refusals[static_cast<int>(d.refusal)];
				last = d.refusal;
				lastDetail = detail;
			}
		}
		std::string Line() const
		{
			std::string s = "Repeats reused: " + std::to_string(reused) + " of " + std::to_string(repeats);
			if (repeats > reused)
			{
				s += std::string(" (last refusal: ") + Describe(last);
				if (!lastDetail.empty())
					s += ": " + lastDetail;
				s += ")";
			}
			return s;
		}
	};

	class IRepeatReuse
	{
	public:
		virtual ~IRepeatReuse() = default;
		virtual void PrepareFirstPresent(const RepeatFrameKey& key, std::vector<const pl_hook*>& hooks) = 0;
		virtual bool TryDrawRepeat(const RepeatFrameKey& key, pl_renderer renderer, const pl_frame& destination,
			const pl_render_params& params) = 0;
		virtual void NoteRendered(const RepeatFrameKey& key, bool rendered) = 0;
		virtual const Stats& GetStats() const = 0;
	};

	// Upstream default: never reuses (today's behaviour), counts the repeats it saw.
	class NeverReuse final : public IRepeatReuse
	{
	public:
		void PrepareFirstPresent(const RepeatFrameKey& key, std::vector<const pl_hook*>&) override
		{
			m_stats.Note(key.cadenceRepeat, Decision{});
		}
		bool TryDrawRepeat(const RepeatFrameKey&, pl_renderer, const pl_frame&, const pl_render_params&) override { return false; }
		void NoteRendered(const RepeatFrameKey&, bool) override {}
		const Stats& GetStats() const override { return m_stats; }
	private:
		Stats m_stats;
	};
}
