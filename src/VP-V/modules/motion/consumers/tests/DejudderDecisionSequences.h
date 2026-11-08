// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-0 (audit round 1): multi-frame de-judder decision sequences, written as neutral Facts edits
// (DejudderDecisionLegacyCopy.h). Recorded in DejudderDecisionSequenceRows.inl; DejudderDecisionTableTests
// replays them through the copy (and, from P5b-3, through the policy runner). They pin what single rows
// cannot: the 5 s reason-log throttle with its "changes not logged" count, the forced status publish that
// shows the previous reason, LogInterpolationDecision's "Interpolation:" line and its first-20-then-5 s
// throttle, and the release when interpolation goes from on to off.

#include <VP-V/modules/motion/consumers/tests/DejudderDecisionLegacyCopy.h>

#include <string>
#include <vector>

namespace DejudderDecisionLegacy
{
	struct Sequence
	{
		std::string name;
		Facts base;
		std::vector<Step> steps;
	};

	inline Facts Dejudder24On60()
	{
		Facts f;
		f.activeSettings.dejudderStrength = 0.5;
		f.sourceHz = 23.976;
		f.measuredDisplayHz = 60.0;
		f.haveNext = true;
		f.nextMatches = true;
		return f;
	}
	inline Facts Interpolate50On60()
	{
		Facts f;
		f.activeSettings.interpolation = InterpolationPolicy::Mode::Auto;
		f.sourceHz = 50.0;
		f.measuredDisplayHz = 60.0;
		f.haveNext = true;
		f.nextMatches = true;
		return f;
	}

	inline void NextMissing(Facts& f) { f.haveNext = false; }
	inline void NextQueued(Facts& f) { f.haveNext = true; }
	inline void NextDiffers(Facts& f) { f.nextMatches = false; }
	inline void NextMatches(Facts& f) { f.nextMatches = true; }

	inline std::vector<Sequence> Sequences()
	{
		std::vector<Sequence> list;
		{
			// The queue running low: B missing every other frame (Active <-> NoNextFrame) at 10 frames/s for
			// 3 s, then steady; the reason line at most once per 5 s with the changes not shown.
			Sequence s; s.name = "reason-flip-throttle"; s.base = Dejudder24On60();
			for (int i = 0; i < 30; ++i)
				s.steps.push_back({ 100u, i % 2 ? NextQueued : NextMissing });
			for (int i = 0; i < 4; ++i)
				s.steps.push_back({ 100u, NextQueued });
			s.steps.push_back({ 5000u, nullptr });
			s.steps.push_back({ 600u, nullptr });
			list.push_back(s);
		}
		{
			// Interpolation on (held matcher), then off: the reason and the release; then on again.
			Sequence s; s.name = "interpolation-on-to-off-release"; s.base = Interpolate50On60();
			s.base.dejudder = true;
			for (int i = 0; i < 4; ++i)
				s.steps.push_back({ 100u, nullptr });
			s.steps.push_back({ 100u, [](Facts& f) { f.activeSettings.interpolation = InterpolationPolicy::Mode::Off; } });
			for (int i = 0; i < 3; ++i)
				s.steps.push_back({ 600u, nullptr });
			s.steps.push_back({ 600u, [](Facts& f) { f.dejudder = false; } });
			s.steps.push_back({ 600u, [](Facts& f) { f.activeSettings.interpolation = InterpolationPolicy::Mode::Auto; } });
			s.steps.push_back({ 600u, nullptr });
			list.push_back(s);
		}
		{
			// "Interpolation:" lines: every change logged for the first 20, then at most one per 5 s.
			Sequence s; s.name = "interpolation-log-throttle"; s.base = Interpolate50On60();
			for (int i = 0; i < 30; ++i)
				s.steps.push_back({ 100u, i % 2 ? NextMatches : NextDiffers });
			s.steps.push_back({ 5000u, NextDiffers });
			s.steps.push_back({ 100u, NextMatches });
			list.push_back(s);
		}
		{
			// A first frame that changes the reason: the forced publish shows the previous reason's text,
			// the next unforced publish (500 ms) the new one.
			Sequence s; s.name = "first-publish-shows-previous-reason"; s.base = Dejudder24On60();
			s.base.dejudderReason = DejudderPolicy::Reason::NoNextFrame;
			s.steps.push_back({ 0u, nullptr });
			s.steps.push_back({ 100u, nullptr });
			s.steps.push_back({ 500u, nullptr });
			list.push_back(s);
		}
		return list;
	}
}
