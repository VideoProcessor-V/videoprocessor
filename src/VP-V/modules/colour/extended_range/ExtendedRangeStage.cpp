#include <pch.h>
#include "ExtendedRangeStage.h"

#include <DebugLog.h>

// WP-13: moved from LibplaceboVideoRenderer.cpp (RenderLocked's Stage 10 block, the hook-failure
// scan after the render). Statement order and log text unchanged.
namespace ExtendedRangeStage
{
	ExtendedRange::Settings SettingsFor(const Frame& frame)
	{
		ExtendedRange::Settings settings;
		settings.piecewiseTransfers = settings.wideSourcePrimaries =
			settings.calibrationLut = settings.hdrPassthrough = frame.moreCases;
		settings.hdrDynamicToneMapping = frame.hdrDynamicToneMapping;   // audit 2026-10-16
		return settings;
	}

	// Stage 10: BTB/WTW through libplacebo for SDR. Decided on this frame's source and target;
	// libplacebo is then told the source is in the target's colour space and VP's hooks do the
	// conversion.
	bool Stage::Prepare(const Frame& frame, pl_color_space& sourceColor, const pl_color_space& targetColor,
		std::vector<const pl_hook*>& list)
	{
		slotRuns = frame.slotRuns;
		ExtendedRange::Decision extended = ExtendedRange::Decide(frame.preserve && frame.slotRuns, SettingsFor(frame),
			sourceColor, targetColor, frame.targetLut, frame.targetLutType);
		if (extended.Active() && hookFailed)
			extended = ExtendedRange::Decision{ ExtendedRange::Reason::HookFailed };
		// Hunt 2 (08 #1): a late restore cannot follow libplacebo's frame mix.
		if (frame.temporalActive)
			extended = ExtendedRange::ForTemporal(extended, frame.temporalMixing);
		// Hunt 2 (08 #1): true when Prepare relabels sourceColor; the temporal renderer's retained
		// frames then get the same label, so no frame is converted twice.
		const bool relabelled = extended.Active();
		// A one-plane RGB source (the subtitle move's composed picture, an RGB capture) takes the
		// carry's texture form (ExtendedRange.cpp).
		ExtendedRange::Prepare(extended, hooks, sourceColor, targetColor, list, frame.singlePlaneRgb);
		// Logged on any change of the description (a LUT attached, a mode change while active).
		const std::string description = ExtendedRange::Describe(extended);
		if (description != described && (frame.preserve || logged >= 0))
		{
			described = description;
			logged = static_cast<int>(extended.reason);
			DebugLog::Log("Extended range (BTB/WTW): %s", description.c_str());
		}
		// Hunt 2 (08 #2): libplacebo's frame mixer linearises each frame with a clamp at 0 (no
		// fork): mixed refreshes lose BTB, WTW is kept.
		mixClipsBtb = extended.Active() && frame.temporalMixing;
		if (mixClipsBtb != mixLogged)
		{
			mixLogged = mixClipsBtb;
			if (mixClipsBtb)
				DebugLog::Log("Extended range (BTB/WTW): libplacebo smooth motion (frame mixing) clips BTB on mixed refreshes "
					"(libplacebo limitation; WTW kept); VP's own motion engine keeps both");
		}
		decision = extended;
		return relabelled;
	}

	void Stage::NoteDisabledHooks(const uint64_t* disabled, int count)
	{
		for (int i = 0; !hookFailed && i < count; ++i)
			if (hooks.Owns(disabled[i]))
			{
				hookFailed = true;
				DebugLog::Log("Extended range (BTB/WTW): WARNING libplacebo disabled its hook; off until VP restarts (today's path)");
			}
	}

	FallbackCatalog::Decision Stage::Fallback(bool preserveRequested) const
	{
		return FallbackCatalog::BtbWtw(preserveRequested && slotRuns,
			decision.reason == ExtendedRange::Reason::HookFailed ? FallbackCatalog::BtbWtwClip::HookFailed :
			decision.reason == ExtendedRange::Reason::TemporalMix ? FallbackCatalog::BtbWtwClip::FrameMixing :
			mixClipsBtb ? FallbackCatalog::BtbWtwClip::MixClipsBtb : FallbackCatalog::BtbWtwClip::None);
	}
}
