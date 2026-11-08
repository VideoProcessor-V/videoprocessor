// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-3 (doc 59 section 2.1 items d, e): the de-judder / interpolation decision, moved out of the
// renderer's UpdateDejudderEstimate, and LogInterpolationDecision. Statements are the old ones; the
// renderer's members become the policy state (DejudderPolicyState `st`, bound to local references of
// the old names), the settings (`activeSettings`) and the per-frame facts (`DecisionFacts`). Everything
// that needs the renderer (telemetry, payload / texture checks, the intent scale, the clock, the log,
// the diagnostics, the status line, the engine keeper) is a call on `host`, at the same point and in the
// same order as before. The renderer's host is DejudderPolicyRunner.inl's ImplDecisionHost; the CPU tests
// (DejudderDecisionTableTests) drive the same template with a recording host over the recorded rows.
//
// Render thread (RenderLocked, renderMutex held). Header-only template, C++14.

#include <VP-V/modules/motion/consumers/DejudderState.h>
#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/motion/estimation/MotionEngineIdle.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>

#include <cstdint>
#include <string>

namespace VpMotion
{
	// Where the decision returned (or continues: PairCheck = the pair validity check and the estimate).
	enum class DecisionOutcome { SettledOff, ReleaseBranch, NoEstimate, PairCheck };

	// The per-frame facts the decision reads (UpdateDejudderEstimate's arguments and renderer members that
	// are not this module's state). Values, read once at the call.
	struct DecisionFacts
	{
		double sourceHz = 0.0;
		bool nativeRgb = false;
		bool interlaced = false;
		bool cadenceRepeat = false;
		uint64_t sourceSequence = 0;
		bool haveNext = false;            // next != nullptr
		bool adaptiveGrainMapWanted = false;
		bool vpGapFillStandalone = false;
		bool dejudderHeld = false;        // the matcher object exists (dejudder)
		bool nextTextureHeld = false;     // dejudderNextTextures[0]
		bool dejudderRgbLuma = false;     // dejudderRgbLuma != nullptr
		bool smoothMotionEngine = false;
		double pulldownFilmHz = 0.0;
		uint64_t dejudderEstimatedSequence = 0;
		const VectorDemand::Decision* vectorDemand = nullptr;
	};

	// Delta 10 / the decision part of UpdateDejudderEstimate (moved verbatim apart from the names above).
	template<class Settings, class Host>
	DecisionOutcome RunDejudderDecision(const Settings& activeSettings, DejudderPolicyState& st, const DecisionFacts& f, Host& host,
		bool* analysisEstimateOut = nullptr)
	{
		DejudderPolicy::Reason& dejudderReason = st.dejudderReason;
		bool& dejudderExtrasAllowed = st.dejudderExtrasAllowed;
		float& dejudderEffectiveStrength = st.dejudderEffectiveStrength;
		int& dejudderEvenCycle = st.dejudderEvenCycle;
		InterpolationPolicy::Decision& interpolationDecision = st.interpolationDecision;
		bool& interpolating = st.interpolating;
		float& vpGapPhaseScale = st.vpGapPhaseScale;
		bool& dejudderMaskRateLogged = st.dejudderMaskRateLogged;
		unsigned long long& dejudderReasonChangesNotLogged = st.dejudderReasonChangesNotLogged;
		unsigned long long& dejudderReasonLogTick = st.dejudderReasonLogTick;
		unsigned long long& diagDejudderModeTick = st.diagDejudderModeTick;
		unsigned long long& diagDejudderModeSkipped = st.diagDejudderModeSkipped;
		const bool vpTimelineActive = st.vpTimelineActive;
		const VectorDemand::Decision& vectorDemand = *f.vectorDemand;
		const bool adaptiveGrainMapWanted = f.adaptiveGrainMapWanted, vpGapFillStandalone = f.vpGapFillStandalone;
		const bool nativeRgb = f.nativeRgb, interlaced = f.interlaced, cadenceRepeat = f.cadenceRepeat;
		const bool dejudderRgbLuma = f.dejudderRgbLuma;
		const double sourceHz = f.sourceHz, pulldownFilmHz = f.pulldownFilmHz;
		const uint64_t sourceSequence = f.sourceSequence;
		const int width = host.Width(), height = host.Height();

		// Off and settled: today's path, nothing else is done.
		if (!(activeSettings.dejudderStrength > 0.0) && !(activeSettings.dejudderStrengthEven > 0.0) &&
			!activeSettings.occlusionMask && !adaptiveGrainMapWanted && !vectorDemand.estimate &&
			activeSettings.interpolation == InterpolationPolicy::Mode::Off && !interpolating && !vpGapFillStandalone &&
			dejudderReason == DejudderPolicy::Reason::Off &&
			dejudderReasonChangesNotLogged == 0 && !f.dejudderHeld && !f.nextTextureHeld &&
			!(activeSettings.interpolationFillGaps && host.WarmPending()))   // H3 F4: load the idle engine first
		{
			if (activeSettings.vectorDemand && vectorDemand.mode != VectorDemand::Mode::Off)
				host.PublishDejudderStatus(false);   // the vector engine's line stays on the stats
			else
				host.ClearStatusIfPublished();   // if (dejudderStatusTick != 0) { tick = 0; clear the line under its lock }
			return DecisionOutcome::SettledOff;
		}
		DejudderPolicy::Input in;
		in.strength = activeSettings.dejudderStrength;
		in.strengthEven = activeSettings.dejudderStrengthEven;
		in.evenIncludes1001 = activeSettings.dejudderEvenIncludes1001;
		in.occlusionMask = activeSettings.occlusionMask;
		in.grainMap = adaptiveGrainMapWanted;
		in.lowLatency = host.LowLatency();
		in.sourceHz = sourceHz;
		// Render-ahead without statistics: the extras' open-loop rate (the same one
		// PresentDejudderExtras plans with), so the cadence ratio agrees.
		in.displayHz = host.DecisionDisplayHz();
		in.interlaced = interlaced;
		in.nativeRgb = nativeRgb;
		in.haveNext = f.haveNext;
		in.nextMatches = host.NextMatches();
		in.planeFormatsUsable = host.PlaneFormatsUsable(nativeRgb);
		in.smoothMotion = f.smoothMotionEngine || host.TemporalMixing();
		in.generalRates = activeSettings.generalRateFixes;   // fixed-constant batch 2026-10-18
		DejudderPolicy::Reason reason = DejudderPolicy::Decide(in);
		dejudderExtrasAllowed = DejudderPolicy::ExtrasAllowed(in);
		// Delta 10 (general): the strength the extras run at, and on an even
		// cadence its cycle N (extras at the exact phases k/N).
		dejudderEffectiveStrength = static_cast<float>(DejudderPolicy::EffectiveStrength(in));
		// Motion intent (intent interpreter item 2, 2026-10-17): the owner's strength times
		// the per-shot scale in [intent_motion_min, 1], changed only at cuts (1 while off).
		if (activeSettings.intentMotion)
			dejudderEffectiveStrength *= static_cast<float>(host.IntentMotionScale(sourceSequence));
		dejudderEvenCycle = DejudderPolicy::EvenCycle(DejudderPolicy::CadenceRatio(in.sourceHz, in.displayHz));
		// 50/60p interpolation (plan step 2 / 4): from the same measured rates; it takes
		// a source the de-judder does not (exclusive per frame).
		{
			InterpolationPolicy::Input ii;
			ii.mode = activeSettings.interpolation;
			ii.sourceClass = activeSettings.sourceClass;
			// The measured content rate and display rate the timeline schedules with.
			ii.sourceHz = pulldownFilmHz > 0.0 ? pulldownFilmHz : st.vpCaptureRateHz > 0.0 ? st.vpCaptureRateHz : in.sourceHz;
			ii.displayHz = host.TimelineDisplayHz() > 0.0 ? host.TimelineDisplayHz() : in.displayHz;
			ii.pulldownLocked = pulldownFilmHz > 0.0;
			ii.dejudderStrength = in.strength;
			ii.dejudderStrengthEven = in.strengthEven;
			ii.dejudderEvenIncludes1001 = in.evenIncludes1001;
			ii.smoothMotion = in.smoothMotion;
			ii.lowLatency = in.lowLatency;
			ii.interlaced = in.interlaced;
			ii.nativeRgb = nativeRgb || dejudderRgbLuma;   // RGB holds (owner)
			// A subtitle-composed picture holds in the extras (no source planes to interpolate), per
			// frame: not decided here, so a cue does not release and rebuild the matcher.
			ii.subtitleComposed = false;
			ii.haveNext = in.haveNext && in.nextMatches;
			ii.perVsyncAvailable = true;   // Stage B: the per-vsync timeline (ratios below 1)
			ii.classFixes = activeSettings.cadenceClassFixes;   // Hunt 2 (06-interp M2/L5)
			ii.generalRates = activeSettings.generalRateFixes;   // fixed-constant batch 2026-10-18
			interpolationDecision = InterpolationPolicy::Decide(ii);
			interpolating = interpolationDecision.Interpolates();
			host.LogInterpolationDecision(ii);
		}
		if (interpolating)
		{
			// The estimate runs for the interpolator whatever the de-judder's cadence
			// rules say (a video-rate source); the checks the policy has not made.
			reason = !in.planeFormatsUsable ? DejudderPolicy::Reason::PlaneFormat :
				!in.haveNext ? DejudderPolicy::Reason::NoNextFrame :
				!in.nextMatches ? DejudderPolicy::Reason::NextFrameDiffers : DejudderPolicy::Reason::Active;
			// Stage A (the extras) only until the per-vsync timeline presents (Stage B): it
			// covers every ratio and every refresh's true phase, A's own included.
			dejudderExtrasAllowed = !vpTimelineActive;
			dejudderEffectiveStrength = 1.0f;   // the true phase: no taste
			const int whole = InterpolationPolicy::WholeRatio(interpolationDecision.ratio);
			dejudderEvenCycle = whole >= 2 ? whole : 0;   // always: exact k/N at a whole ratio
		}
		else if (interpolationDecision.reason == InterpolationPolicy::Reason::NoNextFrame)
		{
			// Interpolating but for B (not queued yet, or a different layout): keep the matcher,
			// the interpolator and the producer (the de-judder's own rules would release them for
			// a video-rate source); this frame holds A.
			reason = !in.haveNext ? DejudderPolicy::Reason::NoNextFrame : DejudderPolicy::Reason::NextFrameDiffers;
			dejudderExtrasAllowed = false;
		}
		// Hiccup repair without motion smoothing (owner 2026-10-17, interpolation_fill_gaps):
		// a gap pair the render loop offered is presented by the timeline's interpolator,
		// with the pair's vectors, for this frame only; every other frame is decided as above.
		vpGapPhaseScale = 1.0f;
		if (vpGapFillStandalone && !interpolating && !in.lowLatency && !in.interlaced && !in.smoothMotion &&
			!nativeRgb && !dejudderRgbLuma && in.sourceHz > 0.0 && in.displayHz > 0.0)
		{
			// Bug hunt 3 F5: on a source the de-judder is active on, its decision stands for
			// this frame (the owner's strength x the intent scale, the even cycle): the timeline
			// draws the pair at phase x strength. Otherwise the true phase (strength 1).
			const InterpolationTimeline::StandaloneGapDecision gap = InterpolationTimeline::StandaloneGap(
				reason == DejudderPolicy::Reason::Active, dejudderEffectiveStrength);
			interpolating = true;
			reason = !in.planeFormatsUsable ? DejudderPolicy::Reason::PlaneFormat :
				!in.haveNext ? DejudderPolicy::Reason::NoNextFrame :
				!in.nextMatches ? DejudderPolicy::Reason::NextFrameDiffers : DejudderPolicy::Reason::Active;
			dejudderExtrasAllowed = false;   // the timeline draws this frame's refreshes
			dejudderEffectiveStrength = gap.strength;
			if (!gap.keepActive)
				dejudderEvenCycle = 0;
			vpGapPhaseScale = gap.phaseScale;
		}
		if (in.sourceHz > 0.0 && in.displayHz > 0.0)
			host.SetCadenceRatio(DejudderPolicy::CadenceRatio(in.sourceHz, in.displayHz));   // dejudderExtraStats.cadenceRatio
		if (reason == DejudderPolicy::Reason::VideoRateMaskOnly && !dejudderMaskRateLogged)
		{
			host.Log("De-judder: the occlusion mask / grain map alone is for film-like sources (source %.3f Hz is video); off for this source",
				in.sourceHz);
			dejudderMaskRateLogged = true;
		}
		// Vector engine on demand: a forward estimate for an analysis consumer although the de-judder's own
		// policy says the extras do not run. dejudderReason / dejudderExtrasAllowed stay as decided above.
		const bool analysisEstimate = vectorDemand.estimate && vectorDemand.mode == VectorDemand::Mode::Forward &&
			in.planeFormatsUsable && in.haveNext && in.nextMatches &&
			(reason == DejudderPolicy::Reason::Off || reason == DejudderPolicy::Reason::VideoRateMaskOnly ||
			reason == DejudderPolicy::Reason::IneligibleCadence ||
			(reason == DejudderPolicy::Reason::LowLatency && vectorDemand.lowLatencyAllowed) ||
			reason == DejudderPolicy::Reason::SmoothMotion ||
			(reason == DejudderPolicy::Reason::NativeRgb && dejudderRgbLuma));
		if (analysisEstimateOut)
			*analysisEstimateOut = analysisEstimate;   // (tests: the recorded rows' column)
		host.PublishDejudderStatus(reason != dejudderReason);
		// A reason that flips (e.g. the queue running low) logs at most one
		// line per 5 s; once the 5 s pass, the reason then current is logged
		// with the number of changes not shown, so the log never ends stale.
		if (reason != dejudderReason)
		{
			++dejudderReasonChangesNotLogged;
			dejudderReason = reason;
			// Report-a-problem: a = reason code, b = changes skipped by the 250 ms limit,
			// x = source Hz, y = display Hz.
			const unsigned long long diagNow = host.Tick();
			if (diagDejudderModeTick == 0 || diagNow - diagDejudderModeTick >= 250)
			{
				host.RecordDecision("dejudder-mode", DejudderPolicy::Name(reason),
					static_cast<int64_t>(reason), static_cast<int64_t>(diagDejudderModeSkipped), in.sourceHz, in.displayHz);
				diagDejudderModeTick = diagNow;
				diagDejudderModeSkipped = 0;
			}
			else
				++diagDejudderModeSkipped;
		}
		if (dejudderReasonChangesNotLogged > 0)
		{
			const unsigned long long now = host.Tick();
			if (dejudderReasonLogTick == 0 || now - dejudderReasonLogTick >= 5000)
			{
				const std::string skipped = dejudderReasonChangesNotLogged > 1 ?
					", " + std::to_string(dejudderReasonChangesNotLogged - 1) + " changes not logged" : std::string();
				host.Log("De-judder: %s (strength %.2f, extras %s, occlusion mask %s, source %.3f Hz, display %.3f Hz, match tolerance %.1f%s)",
					DejudderPolicy::Name(reason), in.strength, dejudderExtrasAllowed ? "on" : "off",
					in.occlusionMask ? "on" : "off", in.sourceHz, in.displayHz,
					activeSettings.dejudderMatchTolerance, skipped.c_str());
				dejudderReasonLogTick = now;
				dejudderReasonChangesNotLogged = 0;
			}
		}
		if (!analysisEstimate && (reason == DejudderPolicy::Reason::Off || reason == DejudderPolicy::Reason::LowLatency ||
			reason == DejudderPolicy::Reason::Interlaced || reason == DejudderPolicy::Reason::VideoRateMaskOnly ||
			reason == DejudderPolicy::Reason::IneligibleCadence || reason == DejudderPolicy::Reason::NativeRgb ||
			reason == DejudderPolicy::Reason::SmoothMotion))
		{
			// Bug hunt 3 F4 (owner 2026-10-18): with hiccup repair on, the motion engine stays
			// loaded and idle for every reason a gap pair can still be repaired under (the
			// standalone repair needs no low latency, interlacing, smooth motion or native RGB):
			// no create at a hiccup, no release after it, nothing run per frame. Low video memory
			// (an allocation failed, or DXGI reports budget pressure) releases it; the next gap
			// pair recreates it once the pressure has passed (MotionEngineIdle).
			const bool repairPossible = reason == DejudderPolicy::Reason::Off ||
				reason == DejudderPolicy::Reason::VideoRateMaskOnly || reason == DejudderPolicy::Reason::IneligibleCadence;
			const bool held = f.dejudderHeld || f.nextTextureHeld;
			if (held && activeSettings.interpolationFillGaps && repairPossible)
				host.PollMotionEngineBudget(width, height, true);
			MotionEngineIdle::Keeper& motionEngineIdle = host.MotionEngineIdle();
			const bool keepForGaps = motionEngineIdle.Keep(activeSettings.interpolationFillGaps, repairPossible);
			// H3 F4 follow-up: loaded and idle from the source's start, not at its first hiccup.
			if (!held && keepForGaps && host.WarmPending())
				host.WarmIdleMotionEngine();
			else if (!keepForGaps)
				host.SetWarmPending(false);   // not wanted for this source: the first gap pair creates it
			if (held && !keepForGaps)
			{
				host.SetWarmPending(false);   // recreated by the next gap pair, never mid-play
				unsigned long long& motionEngineIdleReleasesLogged = host.IdleReleasesLogged();
				if (activeSettings.interpolationFillGaps && repairPossible && motionEngineIdle.Pressured() &&
					motionEngineIdleReleasesLogged != motionEngineIdle.Pressures())
				{
					motionEngineIdleReleasesLogged = motionEngineIdle.Pressures();
					host.Log("Motion engine: released while idle (low video memory: an allocation failed or the "
						"budget is under pressure; %llu times); hiccup repair recreates it once the pressure has passed",
						static_cast<unsigned long long>(motionEngineIdle.Pressures()));
				}
				host.ReleaseDejudder();
			}
			return DecisionOutcome::ReleaseBranch;
		}
		if ((reason != DejudderPolicy::Reason::Active && !analysisEstimate) || cadenceRepeat ||
			sourceSequence == f.dejudderEstimatedSequence)
			return DecisionOutcome::NoEstimate;
		return DecisionOutcome::PairCheck;
	}

	// LogInterpolationDecision: the decision when it changes (at most one line per 5 s), with the
	// measured rates it came from (moved verbatim; the clock and the log are host calls).
	template<class Settings, class Host>
	void LogInterpolationDecisionFor(const Settings& activeSettings, DejudderPolicyState& st, unsigned long long interpolatedPresents,
		const InterpolationPolicy::Input& in, Host& host)
	{
		const InterpolationPolicy::Decision& interpolationDecision = st.interpolationDecision;
		int& interpolationReasonLogged = st.interpolationReasonLogged;
		unsigned long long& interpolationLogTick = st.interpolationLogTick;
		unsigned& interpolationLogCount = st.interpolationLogCount;
		if (in.mode == InterpolationPolicy::Mode::Off && interpolationReasonLogged < 0)
			return;
		const int reason = static_cast<int>(interpolationDecision.reason);
		if (reason == interpolationReasonLogged)
			return;
		// The first changes are all logged (the start: rates and the queue settling);
		// then at most one line per 5 s.
		const unsigned long long now = host.Tick();
		if (interpolationLogCount >= 20 && interpolationLogTick != 0 && now - interpolationLogTick < 5000)
			return;
		++interpolationLogCount;
		interpolationLogTick = now;
		interpolationReasonLogged = reason;
		host.Log("Interpolation: %s (interpolation=%s, fallback=%s, source_class=%s, source %.3f Hz%s, display %.3f Hz, "
			"ratio %.4f, %s; %llu interpolated pictures so far)",
			InterpolationPolicy::Name(interpolationDecision.reason), InterpolationPolicy::Name(in.mode),
			InterpolationPolicy::Name(activeSettings.interpolationFallback), InterpolationPolicy::Name(in.sourceClass),
			in.sourceHz, in.pulldownLocked ? " (pulldown film clock)" : "", in.displayHz, interpolationDecision.ratio,
			interpolationDecision.filmLike ? "film-like" : "video", interpolatedPresents);
	}
}
