// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-0: the synthetic frame sequences TimelinePlanTraceTests replays (and the recorder used for
// TimelinePlanTraceRows.inl). Deterministic: rates, depths and events are fixed per scenario.

#include <VP-V/modules/motion/consumers/tests/TimelinePlanLegacyCopy.h>

#include <cmath>
#include <string>
#include <vector>

namespace TimelinePlanLegacy
{
	struct Scenario
	{
		std::string name;
		void (*setup)(State&) = nullptr;
		void (*beforeFrame)(State&, size_t) = nullptr;
		std::vector<Frame> frames;
	};

	inline void Interpolating(State& s, InterpolationPolicy::Mode mode = InterpolationPolicy::Mode::Auto)
	{
		s.activeSettings.interpolation = mode;
		s.interpolating = true;
		s.interpolationDecision.reason = InterpolationPolicy::Reason::Interpolate;
	}

	// count frames at sourceHz from sequence 1 (pts = sequence / rate), B known, the queue at desired.
	inline std::vector<Frame> Steady(size_t count, double sourceHz, double displayHz, size_t depth = 3, size_t desired = 3)
	{
		std::vector<Frame> frames;
		for (size_t i = 0; i < count; ++i)
		{
			Frame f;
			f.sourceSequence = i + 1;
			f.nextSourceSequence = i + 2;
			f.sourcePts = static_cast<double>(f.sourceSequence) / sourceHz;
			f.nextPts = static_cast<double>(f.nextSourceSequence) / sourceHz;
			f.captureRateHz = sourceHz;
			f.timelineDisplayHz = displayHz;
			f.queueDepthAfterDequeue = depth;
			f.desiredQueueDepth = desired;
			f.temporalOutputs.push_back(f.sourcePts);
			frames.push_back(f);
		}
		return frames;
	}

	inline std::vector<Scenario> Scenarios()
	{
		std::vector<Scenario> list;
		{
			Scenario s; s.name = "interp-50p-on-60";
			s.setup = [](State& st) { Interpolating(st); };
			s.frames = Steady(24, 50.0, 60.0);
			list.push_back(s);
		}
		{
			Scenario s; s.name = "interp-60p-on-50-skips";
			s.setup = [](State& st) { Interpolating(st); };
			s.frames = Steady(24, 60.0, 50.0);
			list.push_back(s);
		}
		{
			Scenario s; s.name = "interp-25p-on-60-queue-steered";
			s.setup = [](State& st) { Interpolating(st, InterpolationPolicy::Mode::Always); };
			s.frames = Steady(24, 25.0, 60.0, 6, 3);
			for (size_t i = 0; i < s.frames.size(); ++i)
				s.frames[i].holdFraction = 0.5;
			for (size_t i = 12; i < s.frames.size(); ++i)
				s.frames[i].queueTimingFixes = false;
			list.push_back(s);
		}
		{
			Scenario s; s.name = "low-latency-never-engages";
			s.setup = [](State& st) { Interpolating(st); st.lowLatency = true; };
			s.beforeFrame = [](State& st, size_t i) { st.lowLatency = i < 8 || i >= 16; };
			s.frames = Steady(24, 50.0, 60.0, 1, 1);
			list.push_back(s);
		}
		{
			Scenario s; s.name = "smooth-motion-owns";
			s.setup = [](State& st) { Interpolating(st); st.activeSettings.presentEveryVsync = true; };
			s.frames = Steady(16, 24.0, 60.0);
			for (size_t i = 0; i < s.frames.size(); ++i)
			{
				s.frames[i].temporalActive = i >= 4 && i < 12;
				s.frames[i].temporalMixing = i >= 6 && i < 12;
				if (s.frames[i].temporalActive)
					s.frames[i].temporalOutputs.push_back(s.frames[i].sourcePts + 1.0 / 60.0);
			}
			list.push_back(s);
		}
		{
			Scenario s; s.name = "file-frames-no-steering";
			s.setup = [](State& st) { Interpolating(st); };
			s.frames = Steady(16, 50.0, 60.0, 8, 3);
			for (size_t i = 0; i < s.frames.size(); ++i)
				s.frames[i].currentFileFrame = i < 10;
			list.push_back(s);
		}
		{
			Scenario s; s.name = "pulldown-24-in-60-updates-repeats";
			s.setup = [](State& st) { Interpolating(st); };
			// The film clock: 23.976 film frames carried at 60 Hz capture; a film update (sequence + 1 at
			// the same film time) after every third frame, a cadence repeat (same sequence) after the fifth.
			const double film = 24000.0 / 1001.0;
			uint64_t seq = 1;
			int filmFrame = 1;
			for (int k = 0; k < 20; ++k)
			{
				Frame f;
				f.sourceSequence = seq;
				f.frameFilmHz = film;
				f.captureRateHz = 60.0;
				f.arrivalQpc = static_cast<int64_t>(1000000000LL + std::llround(filmFrame / film * 10000000.0));
				f.sourcePts = f.sourceSequence / 60.0;
				f.nextSourceSequence = seq + 1;
				f.nextPts = 100.0 + (filmFrame + 1) / film;
				f.timelineDisplayHz = 60.0;
				f.queueDepthAfterDequeue = 3;
				f.desiredQueueDepth = 3;
				s.frames.push_back(f);
				if (k % 6 == 2)
				{
					Frame u = f;
					u.sourceSequence = ++seq;
					u.nextSourceSequence = seq + 1;
					s.frames.push_back(u);   // film update
				}
				if (k % 6 == 4)
					s.frames.push_back(f);   // cadence repeat
				++seq;
				++filmFrame;
			}
			list.push_back(s);
		}
		{
			Scenario s; s.name = "flush-generation-discontinuity-reanchor-hole";
			s.setup = [](State& st) { Interpolating(st); };
			s.frames = Steady(28, 50.0, 60.0, 4, 3);
			s.frames[6].queueFlushBefore = true;
			for (size_t i = 12; i < s.frames.size(); ++i)
			{
				s.frames[i].frameGeneration = 2;
				s.frames[i].readerGeneration = 2;
			}
			s.frames[18].discontinuity = true;
			s.frames[20].reanchorBits = true;
			s.frames[20].sourcePts += 0.004;
			for (size_t i = 22; i < s.frames.size(); ++i)
			{
				s.frames[i].sourceSequence += 2;   // two frames dropped before frame 22
				s.frames[i].nextSourceSequence += 2;
				s.frames[i].sourcePts = s.frames[i].sourceSequence / 50.0;
				s.frames[i].nextPts = s.frames[i].nextSourceSequence / 50.0;
			}
			list.push_back(s);
		}
		{
			Scenario s; s.name = "gap-standalone-only";
			s.beforeFrame = [](State& st, size_t i) { st.vpGapFillStandalone = i == 5 || i == 6; st.interpolating = st.vpGapFillStandalone; };
			s.frames = Steady(10, 24.0, 60.0);
			list.push_back(s);
		}
		{
			Scenario s; s.name = "present-every-vsync-either-or";
			s.setup = [](State& st) { Interpolating(st); st.activeSettings.presentEveryVsync = true; };
			s.beforeFrame = [](State& st, size_t i)
			{
				if (i == 6)
				{
					st.activeSettings.interpolation = InterpolationPolicy::Mode::Off;
					st.interpolating = false;
					st.interpolationDecision.reason = InterpolationPolicy::Reason::Off;
					st.dejudderReason = DejudderPolicy::Reason::Active;
					st.dejudderExtrasAllowed = true;
				}
				if (i == 10)
					st.dejudderExtrasAllowed = false;
			};
			s.frames = Steady(14, 50.0, 60.0);
			s.frames[12].recoveryFallback = true;
			s.frames[13].recoveryFallback = true;
			list.push_back(s);
		}
		{
			Scenario s; s.name = "no-next-keeps-schedule";
			s.setup = [](State& st) { st.activeSettings.interpolation = InterpolationPolicy::Mode::Auto; };
			s.beforeFrame = [](State& st, size_t i)
			{
				const bool noNext = i < 2 || i == 6 || i == 7;
				st.interpolating = !noNext;
				st.interpolationDecision.reason = noNext ? InterpolationPolicy::Reason::NoNextFrame :
					InterpolationPolicy::Reason::Interpolate;
			};
			s.frames = Steady(12, 50.0, 60.0);
			s.frames[6].nextSourceSequence = 0;
			s.frames[7].nextSourceSequence = 0;
			list.push_back(s);
		}
		{
			// The draw-time counters in the engaged / released lines (the renderer counts pictures between plans).
			Scenario s; s.name = "release-log-counters";
			s.setup = [](State& st) { Interpolating(st); st.vpPhasePictures = 41; st.vpHeldPictures = 3; st.vpSkippedFrames = 2;
				st.vpFilmUpdates = 5; st.vpGapPairs = 1; };
			s.beforeFrame = [](State& st, size_t i) { st.lowLatency = i >= 6; st.vpPhasePictures += 7; };
			s.frames = Steady(10, 50.0, 60.0);
			list.push_back(s);
		}
		{
			Scenario s; s.name = "rates-unknown-single-present";
			s.setup = [](State& st) { Interpolating(st); };
			s.frames = Steady(8, 50.0, 60.0);
			for (size_t i = 0; i < 4; ++i)
				s.frames[i].timelineDisplayHz = 0.0;
			s.frames[5].captureRateHz = 0.0;
			list.push_back(s);
		}
		return list;
	}

	// The trace of one scenario: one line per frame.
	inline std::vector<std::string> Trace(const Scenario& sc)
	{
		State st;
		if (sc.setup)
			sc.setup(st);
		std::vector<std::string> lines;
		for (size_t i = 0; i < sc.frames.size(); ++i)
		{
			if (sc.beforeFrame)
				sc.beforeFrame(st, i);
			st.Run(sc.frames[i]);
			lines.push_back(st.TraceLine(sc.frames[i]));
		}
		return lines;
	}
}
