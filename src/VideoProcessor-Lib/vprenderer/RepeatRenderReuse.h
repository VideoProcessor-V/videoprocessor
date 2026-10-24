#pragma once

// Captured-repeat render reuse: the hook contract (WP-24, 2026-10-24 DRAFT).
//
// UPSTREAMABLE: standard library only. A captured repeat (a 24p film frame shown again in a
// 24-in-30 or 30-in-60 signal: the queue's cadenceRepeat) today re-runs the whole libplacebo
// render: scaling, colour management, every PRE_OUTPUT hook. When nothing that feeds the picture
// changed since the source frame's first present, the picture before OUTPUT is the same, so only
// the OUTPUT stage (encoding, OUTPUT hooks, dither) needs to run again.
//
// ONE hook point in the render loop: the first statement of the draw callback (the Draw passed to
// NlsHookRecovery::RenderOwned), where the frame is about to be rendered:
//
//     if (repeatReuse && repeatReuse->TryDrawRepeat(key, destination)) return true;
//
// Everything before it still runs on every refresh: queue/dequeue, presentation timing, the crop
// and subtitle PRESENTATION decisions (their timers and holds advance per refresh), overlay
// assembly, hook-list assembly, telemetry and Present. Only the draw is replaced.
//
// The host fills a RepeatFrameKey from its own state; the reuse implementation (a fork's, e.g. VP's
// rendered-picture cache) decides with RepeatReuseRules::Compare and draws. Without an
// implementation (nullptr, or NeverReuse) the loop renders as before.

#include <cstdint>
#include <cstring>
#include <string>

namespace RepeatRenderReuse
{
	// FNV-1a over raw bytes: a cheap signature of the state that feeds the picture. Floats are
	// hashed by their bits (an exact compare: any change, however small, refuses the reuse).
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

	// What the host knows at the hook point. Each signature covers one area; equal signatures mean
	// "unchanged since the cached present" for that area.
	struct RepeatFrameKey
	{
		uint64_t sourceSequence = 0;      // the queued source frame (a repeat carries its frame's)
		bool cadenceRepeat = false;       // the queue marked this refresh a captured repeat
		uint64_t frameGeneration = 0;     // stream generation (reset/resync moves it)
		uint64_t settingsGeneration = 0;  // render settings / profile / shader selection applied
		uint64_t geometry = 0;            // source crop, target crop, target size, rotation/flip
		uint64_t colour = 0;              // source repr/colour/HDR+DV metadata, target colour/LUT
		uint64_t overlays = 0;            // subtitle composition/placement, OSD/caption overlays
		uint64_t hookState = 0;           // every hook before OUTPUT and its parameter values
		bool hookStateOpaque = false;     // a hook before OUTPUT whose state cannot be known: never reuse
		uint64_t extension = 0;           // anything a fork adds (e.g. VP's temporal stages)
		// Temporal state that advances per RENDER (not per source frame): a reuse would freeze it.
		bool temporalRenderState = false; // peak detection, frame mixing, deinterlacing history, crossfades
	};

	enum class Refusal : int
	{
		None = 0,
		Disabled,          // switched off / no implementation
		NotRepeat,         // not a captured repeat
		OtherSource,       // a different source frame than the cached one
		NoCache,           // no cached picture for that frame (first present not captured)
		StreamChanged,     // frame generation moved
		SettingsChanged,
		GeometryChanged,
		ColourChanged,
		OverlaysChanged,
		HookStateChanged,  // a pre-OUTPUT hook or one of its parameters changed (or is opaque)
		ExtensionChanged,
		TemporalState,     // per-render temporal state is active
		DrawFailed,        // the reuse draw failed: rendered in full instead (counted, visible)
	};
	constexpr int kRefusalCount = 14;

	inline const char* Describe(Refusal r)
	{
		static const char* const names[kRefusalCount] = { "reused", "off", "not a repeat", "other source frame",
			"no cached picture", "stream changed", "settings changed", "geometry changed", "colour changed",
			"overlays changed", "hook state changed", "extension changed", "temporal state active", "reuse draw failed" };
		const int i = static_cast<int>(r);
		return i >= 0 && i < kRefusalCount ? names[i] : "unknown";
	}

	struct Decision
	{
		bool reuse = false;
		Refusal refusal = Refusal::Disabled;
	};

	// The pure comparison (upstream and forks share it): the cached present's key vs this refresh's.
	struct RepeatReuseRules
	{
		static Decision Compare(const RepeatFrameKey& cached, bool cacheValid, const RepeatFrameKey& now)
		{
			Decision d;
			d.refusal = !now.cadenceRepeat ? Refusal::NotRepeat :
				now.sourceSequence == 0 || now.sourceSequence != cached.sourceSequence ? Refusal::OtherSource :
				!cacheValid ? Refusal::NoCache :
				now.frameGeneration != cached.frameGeneration ? Refusal::StreamChanged :
				now.temporalRenderState || cached.temporalRenderState ? Refusal::TemporalState :
				now.settingsGeneration != cached.settingsGeneration ? Refusal::SettingsChanged :
				now.geometry != cached.geometry ? Refusal::GeometryChanged :
				now.colour != cached.colour ? Refusal::ColourChanged :
				now.overlays != cached.overlays ? Refusal::OverlaysChanged :
				now.hookStateOpaque || cached.hookStateOpaque || now.hookState != cached.hookState ? Refusal::HookStateChanged :
				now.extension != cached.extension ? Refusal::ExtensionChanged : Refusal::None;
			d.reuse = d.refusal == Refusal::None;
			return d;
		}
	};

	// Counters for the status line ("Repeats reused: N of M (last refusal: ...)").
	struct Stats
	{
		uint64_t repeats = 0, reused = 0;
		uint64_t refusals[kRefusalCount] = {};
		Refusal last = Refusal::None;
		void Note(bool repeat, const Decision& d)
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
			}
		}
		std::string Line() const
		{
			std::string s = "Repeats reused: " + std::to_string(reused) + " of " + std::to_string(repeats);
			if (repeats > reused)
				s += std::string(" (last refusal: ") + Describe(last) + ")";
			return s;
		}
	};

	// The hook the render loop calls. The default implementation never reuses (upstream behaviour).
	template <class Frame>
	class IRepeatReuse
	{
	public:
		virtual ~IRepeatReuse() = default;
		// At the hook point: draw this refresh from the cache into `destination` and return true, or
		// return false and let the loop render in full.
		virtual bool TryDrawRepeat(const RepeatFrameKey& key, Frame& destination) = 0;
		// After a full render of a source frame's first present: the key the cache now holds.
		virtual void NoteFullRender(const RepeatFrameKey& key, bool cacheCaptured) = 0;
		virtual const Stats& GetStats() const = 0;
	};

	template <class Frame>
	class NeverReuse final : public IRepeatReuse<Frame>
	{
	public:
		bool TryDrawRepeat(const RepeatFrameKey& key, Frame&) override
		{
			m_stats.Note(key.cadenceRepeat, Decision{});
			return false;
		}
		void NoteFullRender(const RepeatFrameKey&, bool) override {}
		const Stats& GetStats() const override { return m_stats; }
	private:
		Stats m_stats;
	};
}
