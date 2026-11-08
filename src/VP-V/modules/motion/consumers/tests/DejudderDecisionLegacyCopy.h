// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-0 (doc 59 section 2.3): a TEST-ONLY verbatim copy of the decision part of the renderer's
// UpdateDejudderEstimate (vprenderer/LibplaceboVideoRenderer.cpp, fork/integ/batch1 @ 4bd69399e), and of
// the head of PublishDejudderStatus (the status-line / hold text the decision selects). It is the oracle
// the recorded rows in DejudderDecisionRows.inl were taken from; P5b-3's policy runner must give the same
// rows. Never compiled into the product. The Impl members the code reads are plain members of the same
// names here, so the copied statements are unchanged. Substitutions (marked "COPY:") are only for state
// that needs a GPU, a payload or a clock: the payload / texture comparisons (nextMatches,
// planeFormatsUsable), the presentation telemetry, the intent interpreter's scale, GetTickCount64, and
// the side effects (logs, diagnostics, release, warm, budget poll), which are recorded instead of run.
// Left out (not decision): MotionValidity NoteKept / ApplyMotionInvalidate, `dejudderSourceHz = sourceHz`,
// the vector engine's UpdateVectorEngine / ServeStatistics, the report-a-problem decision record, and
// everything after the pair check (layout, upload, estimate; the pair validity itself is covered by the
// MotionValidity* tests). The status text is the head of the line only (the ahead / render-ahead / dither /
// pulldown / mask / motion-source tail segments are out of scope: they do not depend on the decision).
//
// The inputs are a neutral Facts struct (the rows and sequences write Facts only). The copy runs on a
// State that derives from Facts (Begin / ApplyInputs are the adapters); P5b-3's runner gets its own
// adapter from the same Facts, so both are checked against the same recorded rows.

#include <VP-V/modules/motion/consumers/InterpolationPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/motion/estimation/MotionEngineIdle.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace DejudderDecisionLegacy
{
	typedef unsigned long long ULONGLONG;

	// Where the copied function returned (or would continue).
	enum class Outcome
	{
		SettledOff,      // "Off and settled: today's path, nothing else is done."
		ReleaseBranch,   // the not-running reasons: idle keep / warm / release, then return
		NoEstimate,      // not Active (and no analysis estimate), a cadence repeat, or already estimated
		PairCheck,       // continues to the pair validity check and the estimate
	};
	inline const char* Name(Outcome o)
	{
		switch (o)
		{
		case Outcome::SettledOff: return "settled-off";
		case Outcome::ReleaseBranch: return "release-branch";
		case Outcome::NoEstimate: return "no-estimate";
		case Outcome::PairCheck: return "pair-check";
		}
		return "?";
	}

	// The renderer settings the decision reads: the VpvRenderer::Fields members of the same names and
	// defaults (modules/control/config/VpvRendererFields.h), so the copy needs no libplacebo headers.
	struct Settings
	{
		double dejudderStrength = 0.0;
		double dejudderStrengthEven = 0.0;
		bool dejudderEvenIncludes1001 = false;
		double dejudderMatchTolerance = 4.5;
		bool occlusionMask = false;
		InterpolationPolicy::Mode interpolation = InterpolationPolicy::Mode::Off;
		InterpolationPolicy::Fallback interpolationFallback = InterpolationPolicy::Fallback::Hold;
		bool interpolationFillGaps = true;
		InterpolationPolicy::SourceClass sourceClass = InterpolationPolicy::SourceClass::Legacy;
		bool intentMotion = false;
		bool cadenceClassFixes = true;
		bool vectorDemand = true;
		bool generalRateFixes = true;
	};

	// The neutral inputs. "Prior" fields are the state the frame starts from (set once by Begin; a
	// sequence carries them over from frame to frame); every other field is a per-frame input.
	struct Facts
	{
		// ---- inputs: settings and Impl members of the same names ----
		Settings activeSettings;
		bool adaptiveGrainMapWanted = false;
		VectorDemand::Decision vectorDemand;
		bool interpolating = false;
		bool vpGapFillStandalone = false;
		DejudderPolicy::Reason dejudderReason = DejudderPolicy::Reason::Off;
		int dejudderReasonChangesNotLogged = 0;
		bool dejudder = false;                     // COPY: the matcher object is held
		bool dejudderNextTexture0 = false;         // COPY: dejudderNextTextures[0] is held
		bool motionEngineWarmPending = false;
		bool lowLatency = false;                   // COPY: lowLatencyQueueActive.load(acquire)
		double measuredDisplayHz = 0.0;            // COPY: presentationTelemetry.Snapshot().measuredDisplayHz
		bool renderAheadActive = false;            // COPY: RenderAheadActive()
		bool displayRateOpenLoop = false;          // COPY: dejudderDisplayRate.openLoop
		double displayRateHz = 0.0;                // COPY: dejudderDisplayRate.hz
		double customHookRefreshHz = 0.0;
		bool haveNext = false;                     // COPY: next != nullptr
		bool nextMatches = false;                  // COPY: the payload / contract / DV pairing comparison
		bool planeFormatsUsable = true;            // COPY: nativeRgb || usable(...) of the planes
		bool dejudderRgbLuma = false;              // COPY: dejudderRgbLuma != nullptr
		bool smoothMotionEngine = false;
		bool temporalMixing = false;               // COPY: temporalRenderer.Mixing()
		double intentMotionScale = 1.0;            // COPY: intentInterpreter->AppliedMotionScale(sourceSequence)
		double pulldownFilmHz = 0.0;
		double vpCaptureRateHz = 0.0;
		double timelineDisplayHz = 0.0;            // COPY: TimelineDisplayHz()
		bool vpTimelineActive = false;
		bool motionEnginePressured = false;        // COPY: motionEngineIdle had an allocation failure
		uint64_t motionEngineIdleReleasesLogged = 0;
		uint64_t dejudderEstimatedSequence = 0;
		ULONGLONG nowTick = 100000;                // COPY: GetTickCount64()
		ULONGLONG dejudderReasonLogTick = 0;
		ULONGLONG dejudderStatusTick = 0;
		bool dejudderMaskRateLogged = false;
		// call arguments
		double sourceHz = 0.0;
		bool nativeRgb = false;
		bool interlaced = false;
		bool cadenceRepeat = false;
		uint64_t sourceSequence = 10;
	};

	struct State : Facts
	{
		// Adapters: Begin = the whole Facts (prior state included); ApplyInputs = the per-frame inputs only
		// (the prior fields evolve with the copied code).
		void Begin(const Facts& f) { static_cast<Facts&>(*this) = f; }
		void ApplyInputs(const Facts& f)
		{
			const DejudderPolicy::Reason reason = dejudderReason;
			const int notLogged = dejudderReasonChangesNotLogged;
			const ULONGLONG logTick = dejudderReasonLogTick, statusTick = dejudderStatusTick, now = nowTick;
			const bool interp = interpolating, warm = motionEngineWarmPending, maskLogged = dejudderMaskRateLogged;
			const uint64_t idleLogged = motionEngineIdleReleasesLogged;
			static_cast<Facts&>(*this) = f;
			dejudderReason = reason;
			dejudderReasonChangesNotLogged = notLogged;
			dejudderReasonLogTick = logTick;
			dejudderStatusTick = statusTick;
			nowTick = now;
			interpolating = interp;
			motionEngineWarmPending = warm;
			dejudderMaskRateLogged = maskLogged;
			motionEngineIdleReleasesLogged = idleLogged;
		}

		// ---- outputs (Impl members written by the copied code) ----
		bool dejudderExtrasAllowed = false;
		float dejudderEffectiveStrength = 0.0f;
		int dejudderEvenCycle = 0;
		InterpolationPolicy::Decision interpolationDecision;
		float vpGapPhaseScale = 1.0f;
		double cadenceRatio = 0.0;                 // dejudderExtraStats.cadenceRatio
		bool analysisEstimate = false;
		Outcome outcome = Outcome::PairCheck;
		// recorded side effects
		std::vector<std::string> logs;
		int statusPublishes = 0;
		bool statusForced = false;
		bool statusCleared = false;
		bool budgetPolled = false;
		bool warmed = false;
		bool released = false;
		std::string statusHead;                    // the hold / status text the last publish showed (its head)
		std::string firstStatusHead;               // the first publish's text (on a change: the previous reason)
		// LogInterpolationDecision's state (Impl members of the same names)
		int interpolationReasonLogged = -1;
		ULONGLONG interpolationLogTick = 0;
		unsigned interpolationLogCount = 0;
		unsigned long long interpolatedPresents = 0;

		MotionEngineIdle::Keeper motionEngineIdle;

		// COPY: DebugLog::Log -> recorded text.
		void Log(const char* format, ...)
		{
			char line[512];
			va_list args;
			va_start(args, format);
			std::vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			logs.push_back(line);
		}
		// COPY: PublishDejudderStatus's throttle, then its head text (StatusHead) recorded.
		void PublishDejudderStatus(bool force = false)
		{
			const ULONGLONG now = nowTick;
			if (!force && dejudderStatusTick != 0 && now - dejudderStatusTick < 500)
				return;
			dejudderStatusTick = now;
			++statusPublishes;
			statusForced = statusForced || force;
			statusHead = StatusHead();
			if (statusPublishes == 1)
				firstStatusHead = statusHead;
		}
		double TimelineDisplayHz() const { return timelineDisplayHz; }
		bool RenderAheadActive() const { return renderAheadActive; }
		// Verbatim (GetTickCount64 and DebugLog::Log are COPY).
		void LogInterpolationDecision(const InterpolationPolicy::Input& in)
		{
			if (in.mode == InterpolationPolicy::Mode::Off && interpolationReasonLogged < 0)
				return;
			const int reason = static_cast<int>(interpolationDecision.reason);
			if (reason == interpolationReasonLogged)
				return;
			// The first changes are all logged (the start: rates and the queue settling);
			// then at most one line per 5 s.
			const ULONGLONG now = nowTick;   // COPY
			if (interpolationLogCount >= 20 && interpolationLogTick != 0 && now - interpolationLogTick < 5000)
				return;
			++interpolationLogCount;
			interpolationLogTick = now;
			interpolationReasonLogged = reason;
			Log("Interpolation: %s (interpolation=%s, fallback=%s, source_class=%s, source %.3f Hz%s, display %.3f Hz, "
				"ratio %.4f, %s; %llu interpolated pictures so far)",
				InterpolationPolicy::Name(interpolationDecision.reason), InterpolationPolicy::Name(in.mode),
				InterpolationPolicy::Name(activeSettings.interpolationFallback), InterpolationPolicy::Name(in.sourceClass),
				in.sourceHz, in.pulldownLocked ? " (pulldown film clock)" : "", in.displayHz, interpolationDecision.ratio,
				interpolationDecision.filmLike ? "film-like" : "video", interpolatedPresents);
		}
		void PollMotionEngineBudget(int, int, bool) { budgetPolled = true; }
		// COPY: EngineKeeper.inl's first statement; the warm-up itself (GPU) is recorded.
		void WarmIdleMotionEngine() { motionEngineWarmPending = false; warmed = true; }
		void ReleaseDejudder() { released = true; }

		// The decision part of UpdateDejudderEstimate (verbatim apart from the COPY: lines).
		void Run()
		{
			if (motionEnginePressured)   // COPY: not in the renderer; models an earlier allocation failure
				motionEngineIdle.NoteAllocationFailure(1.0);
			const int width = 1920, height = 1080;
			// Off and settled: today's path, nothing else is done.
			if (!(activeSettings.dejudderStrength > 0.0) && !(activeSettings.dejudderStrengthEven > 0.0) &&
				!activeSettings.occlusionMask && !adaptiveGrainMapWanted && !vectorDemand.estimate &&
				activeSettings.interpolation == InterpolationPolicy::Mode::Off && !interpolating && !vpGapFillStandalone &&
				dejudderReason == DejudderPolicy::Reason::Off &&
				dejudderReasonChangesNotLogged == 0 && !dejudder && !dejudderNextTexture0 &&   // COPY: textures
				!(activeSettings.interpolationFillGaps && motionEngineWarmPending))   // H3 F4: load the idle engine first
			{
				if (activeSettings.vectorDemand && vectorDemand.mode != VectorDemand::Mode::Off)
					PublishDejudderStatus();   // the vector engine's line stays on the stats
				else if (dejudderStatusTick != 0)
				{
					dejudderStatusTick = 0;
					statusCleared = true;   // COPY: dejudderStatus.clear() under its mutex
				}
				outcome = Outcome::SettledOff;
				return;
			}
			DejudderPolicy::Input in;
			in.strength = activeSettings.dejudderStrength;
			in.strengthEven = activeSettings.dejudderStrengthEven;
			in.evenIncludes1001 = activeSettings.dejudderEvenIncludes1001;
			in.occlusionMask = activeSettings.occlusionMask;
			in.grainMap = adaptiveGrainMapWanted;
			in.lowLatency = lowLatency;   // COPY
			in.sourceHz = sourceHz;
			// Render-ahead without statistics: the extras' open-loop rate (the same one
			// PresentDejudderExtras plans with), so the cadence ratio agrees.
			in.displayHz = measuredDisplayHz > 0.0 ? measuredDisplayHz :   // COPY: telemetry
				RenderAheadActive() && displayRateOpenLoop ? displayRateHz : customHookRefreshHz;
			in.interlaced = interlaced;
			in.nativeRgb = nativeRgb;
			in.haveNext = haveNext;       // COPY
			in.nextMatches = haveNext && nextMatches;   // COPY: next && current && (comparison)
			in.planeFormatsUsable = nativeRgb || planeFormatsUsable;   // COPY
			in.smoothMotion = smoothMotionEngine || temporalMixing;
			in.generalRates = activeSettings.generalRateFixes;   // fixed-constant batch 2026-10-18
			DejudderPolicy::Reason reason = DejudderPolicy::Decide(in);
			dejudderExtrasAllowed = DejudderPolicy::ExtrasAllowed(in);
			// Delta 10 (general): the strength the extras run at, and on an even
			// cadence its cycle N (extras at the exact phases k/N).
			dejudderEffectiveStrength = static_cast<float>(DejudderPolicy::EffectiveStrength(in));
			// Motion intent (intent interpreter item 2, 2026-10-17): the owner's strength times
			// the per-shot scale in [intent_motion_min, 1], changed only at cuts (1 while off).
			if (activeSettings.intentMotion)
				dejudderEffectiveStrength *= static_cast<float>(intentMotionScale);   // COPY
			dejudderEvenCycle = DejudderPolicy::EvenCycle(DejudderPolicy::CadenceRatio(in.sourceHz, in.displayHz));
			// 50/60p interpolation (plan step 2 / 4): from the same measured rates; it takes
			// a source the de-judder does not (exclusive per frame).
			{
				InterpolationPolicy::Input ii;
				ii.mode = activeSettings.interpolation;
				ii.sourceClass = activeSettings.sourceClass;
				// The measured content rate and display rate the timeline schedules with.
				ii.sourceHz = pulldownFilmHz > 0.0 ? pulldownFilmHz : vpCaptureRateHz > 0.0 ? vpCaptureRateHz : in.sourceHz;
				ii.displayHz = TimelineDisplayHz() > 0.0 ? TimelineDisplayHz() : in.displayHz;
				ii.pulldownLocked = pulldownFilmHz > 0.0;
				ii.dejudderStrength = in.strength;
				ii.dejudderStrengthEven = in.strengthEven;
				ii.dejudderEvenIncludes1001 = in.evenIncludes1001;
				ii.smoothMotion = in.smoothMotion;
				ii.lowLatency = in.lowLatency;
				ii.interlaced = in.interlaced;
				ii.nativeRgb = nativeRgb || dejudderRgbLuma;   // RGB holds (owner)   COPY: != nullptr
				// A subtitle-composed picture holds in the extras (no source planes to interpolate), per
				// frame: not decided here, so a cue does not release and rebuild the matcher.
				ii.subtitleComposed = false;
				ii.haveNext = in.haveNext && in.nextMatches;
				ii.perVsyncAvailable = true;   // Stage B: the per-vsync timeline (ratios below 1)
				ii.classFixes = activeSettings.cadenceClassFixes;   // Hunt 2 (06-interp M2/L5)
				ii.generalRates = activeSettings.generalRateFixes;   // fixed-constant batch 2026-10-18
				interpolationDecision = InterpolationPolicy::Decide(ii);
				interpolating = interpolationDecision.Interpolates();
				LogInterpolationDecision(ii);
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
				cadenceRatio = DejudderPolicy::CadenceRatio(in.sourceHz, in.displayHz);   // COPY: dejudderExtraStats.
			if (reason == DejudderPolicy::Reason::VideoRateMaskOnly && !dejudderMaskRateLogged)
			{
				Log("De-judder: the occlusion mask / grain map alone is for film-like sources (source %.3f Hz is video); off for this source",
					in.sourceHz);
				dejudderMaskRateLogged = true;
			}
			// Vector engine on demand: a forward estimate for an analysis consumer although the de-judder's own
			// policy says the extras do not run. dejudderReason / dejudderExtrasAllowed stay as decided above.
			analysisEstimate = vectorDemand.estimate && vectorDemand.mode == VectorDemand::Mode::Forward &&
				in.planeFormatsUsable && in.haveNext && in.nextMatches &&
				(reason == DejudderPolicy::Reason::Off || reason == DejudderPolicy::Reason::VideoRateMaskOnly ||
				reason == DejudderPolicy::Reason::IneligibleCadence ||
				(reason == DejudderPolicy::Reason::LowLatency && vectorDemand.lowLatencyAllowed) ||
				reason == DejudderPolicy::Reason::SmoothMotion ||
				(reason == DejudderPolicy::Reason::NativeRgb && dejudderRgbLuma));
			PublishDejudderStatus(reason != dejudderReason);
			// A reason that flips (e.g. the queue running low) logs at most one
			// line per 5 s; once the 5 s pass, the reason then current is logged
			// with the number of changes not shown, so the log never ends stale.
			if (reason != dejudderReason)
			{
				++dejudderReasonChangesNotLogged;
				dejudderReason = reason;
				// COPY: the report-a-problem decision record (250 ms limit) is left out.
			}
			if (dejudderReasonChangesNotLogged > 0)
			{
				const ULONGLONG now = nowTick;   // COPY
				if (dejudderReasonLogTick == 0 || now - dejudderReasonLogTick >= 5000)
				{
					const std::string skipped = dejudderReasonChangesNotLogged > 1 ?
						", " + std::to_string(dejudderReasonChangesNotLogged - 1) + " changes not logged" : std::string();
					Log("De-judder: %s (strength %.2f, extras %s, occlusion mask %s, source %.3f Hz, display %.3f Hz, match tolerance %.1f%s)",
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
				const bool repairPossible = reason == DejudderPolicy::Reason::Off ||
					reason == DejudderPolicy::Reason::VideoRateMaskOnly || reason == DejudderPolicy::Reason::IneligibleCadence;
				const bool held = dejudder || dejudderNextTexture0;   // COPY
				if (held && activeSettings.interpolationFillGaps && repairPossible)
					PollMotionEngineBudget(width, height, true);
				const bool keepForGaps = motionEngineIdle.Keep(activeSettings.interpolationFillGaps, repairPossible);
				// H3 F4 follow-up: loaded and idle from the source's start, not at its first hiccup.
				if (!held && keepForGaps && motionEngineWarmPending)
					WarmIdleMotionEngine();
				else if (!keepForGaps)
					motionEngineWarmPending = false;   // not wanted for this source: the first gap pair creates it
				if (held && !keepForGaps)
				{
					motionEngineWarmPending = false;   // recreated by the next gap pair, never mid-play
					if (activeSettings.interpolationFillGaps && repairPossible && motionEngineIdle.Pressured() &&
						motionEngineIdleReleasesLogged != motionEngineIdle.Pressures())
					{
						motionEngineIdleReleasesLogged = motionEngineIdle.Pressures();
						Log("Motion engine: released while idle (low video memory: an allocation failed or the "
							"budget is under pressure; %llu times); hiccup repair recreates it once the pressure has passed",
							static_cast<unsigned long long>(motionEngineIdle.Pressures()));
					}
					ReleaseDejudder();
				}
				outcome = Outcome::ReleaseBranch;
				return;
			}
			if ((reason != DejudderPolicy::Reason::Active && !analysisEstimate) || cadenceRepeat ||
				sourceSequence == dejudderEstimatedSequence)
			{
				outcome = Outcome::NoEstimate;
				return;
			}
			outcome = Outcome::PairCheck;
		}

		// The head of PublishDejudderStatus (the `line` it selects), counters at zero: the hold /
		// status text a decision shows. Verbatim apart from the zeroed statistics.
		std::string StatusHead() const
		{
			const bool on = activeSettings.dejudderStrength > 0.0 || activeSettings.dejudderStrengthEven > 0.0 ||
				activeSettings.occlusionMask || activeSettings.interpolation != InterpolationPolicy::Mode::Off;
			if (!on)
				return std::string();
			char line[260];
			const unsigned long long zero = 0;   // COPY: the picture / late / held counters
			const int lastExtrasPerFrame = 0;
			const double lastMs = 0.0;
			if (activeSettings.interpolation != InterpolationPolicy::Mode::Off && !interpolating &&
				dejudderReason != DejudderPolicy::Reason::Active)
				std::snprintf(line, sizeof(line), "interpolation: %s", InterpolationPolicy::Name(interpolationDecision.reason));
			else if (dejudderReason != DejudderPolicy::Reason::Active)
				std::snprintf(line, sizeof(line), "%s", DejudderPolicy::Name(dejudderReason));
			else if (interpolating && vpTimelineActive)
				std::snprintf(line, sizeof(line), "interpolating per refresh (%s), ratio %.3f, %llu pictures, %llu held, %llu frames skipped, "
					"%llu gap pairs, %llu restarts", InterpolationPolicy::Name(activeSettings.interpolationFallback),
					interpolationDecision.ratio, zero, zero, zero, zero, zero);
			else if (interpolating)
				std::snprintf(line, sizeof(line), "interpolating (%s), ratio %.3f, +%d pictures %.1f ms, late %llu, full %llu, held %llu%s",
					InterpolationPolicy::Name(activeSettings.interpolationFallback), cadenceRatio, lastExtrasPerFrame,
					lastMs, zero, zero, zero, "");
			else if (!dejudderExtrasAllowed)
				std::snprintf(line, sizeof(line), "mask only (cadence %.2f)", cadenceRatio);
			else
				std::snprintf(line, sizeof(line), "s=%.2f, cadence %.2f, +%d extras %.1f ms, late %llu, full %llu, held %llu%s",
					activeSettings.dejudderStrength, cadenceRatio, lastExtrasPerFrame, lastMs,
					zero, zero, zero, "");
			return line;
		}
	};

	// What a decision run shows (the copy and P5b-3's runner are compared through this).
	struct Observed
	{
		DejudderPolicy::Reason reason = DejudderPolicy::Reason::Off;
		bool extrasAllowed = false;
		float effectiveStrength = 0.0f;
		int evenCycle = 0;
		bool interpolating = false;
		InterpolationPolicy::Decision interpolation;
		float gapPhaseScale = 1.0f;
		double cadenceRatio = 0.0;
		bool analysisEstimate = false;
		Outcome outcome = Outcome::PairCheck;
		bool warmPendingAfter = false, budgetPolled = false, warmed = false, released = false;
		int statusPublishes = 0;
		bool statusCleared = false;
		std::string statusHead, firstStatusHead;
		std::vector<std::string> logs;
	};
	inline Observed Observe(const State& s)
	{
		Observed o;
		o.reason = s.dejudderReason; o.extrasAllowed = s.dejudderExtrasAllowed; o.effectiveStrength = s.dejudderEffectiveStrength;
		o.evenCycle = s.dejudderEvenCycle; o.interpolating = s.interpolating; o.interpolation = s.interpolationDecision;
		o.gapPhaseScale = s.vpGapPhaseScale; o.cadenceRatio = s.cadenceRatio; o.analysisEstimate = s.analysisEstimate;
		o.outcome = s.outcome; o.warmPendingAfter = s.motionEngineWarmPending; o.budgetPolled = s.budgetPolled;
		o.warmed = s.warmed; o.released = s.released; o.statusPublishes = s.statusPublishes; o.statusCleared = s.statusCleared;
		o.statusHead = s.statusHead; o.firstStatusHead = s.firstStatusHead; o.logs = s.logs;
		return o;
	}

	// A row: Begin(facts), run, 600 ms later run again.
	inline Observed RunRowLegacy(const Facts& f)
	{
		State s;
		s.Begin(f);
		s.Run();
		s.nowTick += 600;
		s.Run();
		return Observe(s);
	}

	// A multi-frame sequence: each step edits the running Facts (cumulative), advances the clock by dtMs
	// and runs one frame; one trace line per frame.
	struct Step
	{
		unsigned dtMs;
		void (*edit)(Facts&);
	};
	inline std::string FrameLine(const Observed& o, size_t logsFrom)
	{
		char head[200];
		std::snprintf(head, sizeof(head), "r=%s x=%d s=%.9g c=%d i=%d ir=%s pub=%d out=%s rel=%d warm=%d st=",
			DejudderPolicy::Name(o.reason), o.extrasAllowed ? 1 : 0, static_cast<double>(o.effectiveStrength), o.evenCycle,
			o.interpolating ? 1 : 0, InterpolationPolicy::Name(o.interpolation.reason), o.statusPublishes, Name(o.outcome),
			o.released ? 1 : 0, o.warmed ? 1 : 0);
		std::string line = std::string(head) + o.statusHead;
		for (size_t i = logsFrom; i < o.logs.size(); ++i)
			line += " | " + o.logs[i];
		return line;
	}
	inline std::vector<std::string> RunSequenceLegacy(const Facts& base, const std::vector<Step>& steps)
	{
		State s;
		s.Begin(base);
		Facts f = base;
		std::vector<std::string> lines;
		for (const Step& st : steps)
		{
			if (st.edit)
				st.edit(f);
			s.ApplyInputs(f);
			s.nowTick += st.dtMs;
			const size_t from = s.logs.size();
			s.Run();
			lines.push_back(FrameLine(Observe(s), from));
		}
		return lines;
	}
}
