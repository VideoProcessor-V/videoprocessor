// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-3: the recording host that drives the MOVED decision (DejudderPolicyRunner.h, the product code)
// from the same neutral Facts as the legacy copy, and maps its result to the same Observed record. The
// host's own parts mirror the copy's COPY substitutions one for one (telemetry, payload checks, clock,
// log, status publish throttle, engine keeper), so a difference in the rows is a difference in the
// moved statements. The status text uses the legacy copy's head until P5b-4 moves PublishDejudderStatus.

#include <VP-V/modules/motion/consumers/DejudderPolicyRunner.h>
#include <VP-V/modules/motion/consumers/tests/DejudderDecisionLegacyCopy.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace DejudderDecisionRunner
{
	using DejudderDecisionLegacy::Facts;
	using DejudderDecisionLegacy::Observed;
	using DejudderDecisionLegacy::Outcome;
	typedef unsigned long long ULONGLONG;

	struct Rig
	{
		DejudderDecisionLegacy::Settings settings;
		VpMotion::DejudderPolicyState st;
		VectorDemand::Decision vectorDemand;
		Facts facts;                          // the per-frame inputs
		// the host's own state (renderer members outside the module)
		ULONGLONG nowTick = 100000;
		ULONGLONG dejudderStatusTick = 0;
		bool motionEngineWarmPending = false;
		unsigned long long motionEngineIdleReleasesLogged = 0;
		MotionEngineIdle::Keeper motionEngineIdle;
		double cadenceRatio = 0.0;
		Outcome outcome = Outcome::PairCheck;
		std::vector<std::string> logs;
		int statusPublishes = 0;
		bool statusCleared = false, budgetPolled = false, warmed = false, released = false;
		std::string statusHead, firstStatusHead;

		// ---- the host interface RunDejudderDecision calls ----
		int Width() const { return 1920; }
		int Height() const { return 1080; }
		bool WarmPending() const { return motionEngineWarmPending; }
		void SetWarmPending(bool v) { motionEngineWarmPending = v; }
		bool LowLatency() const { return facts.lowLatency; }
		void SnapshotTelemetry() {}   // the facts are the snapshot
		double DecisionDisplayHz() const
		{
			return facts.measuredDisplayHz > 0.0 ? facts.measuredDisplayHz :
				facts.renderAheadActive && facts.displayRateOpenLoop ? facts.displayRateHz : facts.customHookRefreshHz;
		}
		bool NextMatches() const { return facts.haveNext && facts.nextMatches; }
		bool PlaneFormatsUsable(bool nativeRgb) const { return nativeRgb || facts.planeFormatsUsable; }
		bool TemporalMixing() const { return facts.temporalMixing; }
		double IntentMotionScale(uint64_t) const { return facts.intentMotionScale; }
		double TimelineDisplayHz() const { return facts.timelineDisplayHz; }
		void LogInterpolationDecision(const InterpolationPolicy::Input& ii)
		{
			VpMotion::LogInterpolationDecisionFor(settings, st, 0ull, ii, *this);
		}
		void SetCadenceRatio(double r) { cadenceRatio = r; }
		ULONGLONG Tick() const { return nowTick; }
		void RecordDecision(const char* kind, const char* name, int64_t a, int64_t b, double x, double y)
		{
			decisions.push_back(DejudderDecisionLegacy::DecisionLine(kind, name, a, b, x, y));
		}
		std::vector<std::string> decisions;
		void Log(const char* format, ...)
		{
			char line[512];
			va_list args;
			va_start(args, format);
			std::vsnprintf(line, sizeof(line), format, args);
			va_end(args);
			logs.push_back(line);
		}
		void PublishDejudderStatus(bool force)
		{
			if (!force && dejudderStatusTick != 0 && nowTick - dejudderStatusTick < 500)
				return;
			dejudderStatusTick = nowTick;
			++statusPublishes;
			statusHead = LegacyHead();
			if (statusPublishes == 1)
				firstStatusHead = statusHead;
		}
		void ClearStatusIfPublished()
		{
			if (dejudderStatusTick != 0)
			{
				dejudderStatusTick = 0;
				statusCleared = true;
			}
		}
		void PollMotionEngineBudget(int, int, bool) { budgetPolled = true; }
		MotionEngineIdle::Keeper& IdleKeeper() { return motionEngineIdle; }
		unsigned long long& IdleReleasesLogged() { return motionEngineIdleReleasesLogged; }
		void WarmIdleMotionEngine() { motionEngineWarmPending = false; warmed = true; }
		void ReleaseDejudder() { released = true; }

		std::string LegacyHead() const
		{
			DejudderDecisionLegacy::State v;
			v.activeSettings = settings;
			v.interpolating = st.interpolating;
			v.dejudderReason = st.dejudderReason;
			v.vpTimelineActive = st.vpTimelineActive;
			v.interpolationDecision = st.interpolationDecision;
			v.dejudderExtrasAllowed = st.dejudderExtrasAllowed;
			v.cadenceRatio = cadenceRatio;
			return v.StatusHead();
		}

		// ---- adapters from the neutral Facts ----
		void Begin(const Facts& f)
		{
			facts = f;
			settings = f.activeSettings;
			vectorDemand = f.vectorDemand;
			st.dejudderReason = f.dejudderReason;
			st.dejudderReasonChangesNotLogged = static_cast<unsigned long long>(f.dejudderReasonChangesNotLogged);
			st.dejudderReasonLogTick = f.dejudderReasonLogTick;
			st.interpolating = f.interpolating;
			st.vpTimelineActive = f.vpTimelineActive;
			st.vpCaptureRateHz = f.vpCaptureRateHz;
			st.dejudderMaskRateLogged = f.dejudderMaskRateLogged;
			nowTick = f.nowTick;
			dejudderStatusTick = f.dejudderStatusTick;
			motionEngineWarmPending = f.motionEngineWarmPending;
			motionEngineIdleReleasesLogged = f.motionEngineIdleReleasesLogged;
		}
		void ApplyInputs(const Facts& f)
		{
			facts = f;
			settings = f.activeSettings;
			vectorDemand = f.vectorDemand;
			st.vpTimelineActive = f.vpTimelineActive;
			st.vpCaptureRateHz = f.vpCaptureRateHz;
		}
		void Run()
		{
			if (facts.motionEnginePressured)   // as the copy's model of an earlier allocation failure
				motionEngineIdle.NoteAllocationFailure(1.0);
			VpMotion::DecisionFacts df;
			df.sourceHz = facts.sourceHz;
			df.nativeRgb = facts.nativeRgb;
			df.interlaced = facts.interlaced;
			df.cadenceRepeat = facts.cadenceRepeat;
			df.sourceSequence = facts.sourceSequence;
			df.haveNext = facts.haveNext;
			df.adaptiveGrainMapWanted = facts.adaptiveGrainMapWanted;
			df.vpGapFillStandalone = facts.vpGapFillStandalone;
			df.dejudderHeld = facts.dejudder;
			df.nextTextureHeld = facts.dejudderNextTexture0;
			df.dejudderRgbLuma = facts.dejudderRgbLuma;
			df.smoothMotionEngine = facts.smoothMotionEngine;
			df.pulldownFilmHz = facts.pulldownFilmHz;
			df.dejudderEstimatedSequence = facts.dejudderEstimatedSequence;
			df.vectorDemand = &vectorDemand;
			const VpMotion::DecisionOutcome o = VpMotion::RunDejudderDecision(settings, st, df, *this, &analysisEstimate);
			outcome = static_cast<Outcome>(static_cast<int>(o));
		}
		Observed Observe() const
		{
			Observed o;
			o.reason = st.dejudderReason; o.extrasAllowed = st.dejudderExtrasAllowed; o.effectiveStrength = st.dejudderEffectiveStrength;
			o.evenCycle = st.dejudderEvenCycle; o.interpolating = st.interpolating; o.interpolation = st.interpolationDecision;
			o.gapPhaseScale = st.vpGapPhaseScale; o.cadenceRatio = cadenceRatio;
			o.outcome = outcome; o.warmPendingAfter = motionEngineWarmPending; o.budgetPolled = budgetPolled;
			o.warmed = warmed; o.released = released; o.statusPublishes = statusPublishes; o.statusCleared = statusCleared;
			o.statusHead = statusHead; o.firstStatusHead = firstStatusHead; o.logs = logs;
			o.analysisEstimate = analysisEstimate;
			o.diagModeTick = st.diagDejudderModeTick; o.diagModeSkipped = st.diagDejudderModeSkipped; o.decisions = decisions;
			return o;
		}
		bool analysisEstimate = false;   // the decision's local, through RunDejudderDecision's out parameter
	};

	inline Observed RunRow(const Facts& f)
	{
		Rig r;
		r.Begin(f);
		r.Run();
		r.nowTick += 600;
		r.Run();
		return r.Observe();
	}

	inline std::vector<std::string> RunSequence(const Facts& base, const std::vector<DejudderDecisionLegacy::Step>& steps)
	{
		Rig r;
		r.Begin(base);
		Facts f = base;
		std::vector<std::string> lines;
		for (const DejudderDecisionLegacy::Step& s : steps)
		{
			if (s.edit)
				s.edit(f);
			r.ApplyInputs(f);
			r.nowTick += s.dtMs;
			const size_t from = r.logs.size();
			r.Run();
			lines.push_back(DejudderDecisionLegacy::FrameLine(r.Observe(), from));
		}
		return lines;
	}
}
