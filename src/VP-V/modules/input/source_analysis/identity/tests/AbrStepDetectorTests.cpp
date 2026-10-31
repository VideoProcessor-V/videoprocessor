/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// ABR boundary detection and the one-time notice (input.source_analysis (identity/)). All series are
// synthetic, built with a fixed-seed LCG at 4 samples per second of SOURCE time (never a random
// device, never a clock).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/AbrNoticePolicy.h>
#include <VP-V/modules/input/source_analysis/identity/AbrStepDetector.h>
#include <VP-V/modules/input/source_analysis/identity/SourceIdConsensus.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace AbrSynth
	{
		using SourceIdentity::AbrDirection;
		using SourceIdentity::AbrEvent;
		using SourceIdentity::AbrStepDetector;
		using SourceIdentity::QualitySample;

		constexpr int64_t kStepUs = 250000;   // 4 analysed frames per second

		struct Lcg
		{
			uint32_t x;
			explicit Lcg(uint32_t seed) : x(seed) {}
			double Next() { x = x * 1664525u + 1013904223u; return static_cast<double>(x >> 8) / 16777216.0; }
			double Noise(double amplitude) { return (Next() * 2.0 - 1.0) * amplitude; }
		};

		inline QualitySample Sample(int i, double quality)
		{
			QualitySample s;
			s.seq = static_cast<uint64_t>(i);
			s.timeUs = static_cast<int64_t>(i) * kStepUs;
			s.quality = quality;
			s.deliveredRatio = 1.0;
			s.apl = 0.5;
			return s;
		}

		inline std::vector<AbrEvent> Run(AbrStepDetector& d, const std::vector<QualitySample>& v)
		{
			std::vector<AbrEvent> events;
			for (size_t i = 0; i < v.size(); ++i)
			{
				AbrEvent e;
				if (d.Push(v[i], e))
					events.push_back(e);
			}
			return events;
		}

		// Quality 0.85 until firstStepUs, then it alternates 0.55 / 0.85 every periodUs.
		inline std::vector<QualitySample> Staircase(int64_t firstStepUs, int64_t periodUs, int64_t durationUs, uint32_t seed)
		{
			Lcg g(seed);
			std::vector<QualitySample> v;
			for (int i = 0; static_cast<int64_t>(i) * kStepUs < durationUs; ++i)
			{
				const int64_t t = static_cast<int64_t>(i) * kStepUs;
				double level = 0.85;
				if (t >= firstStepUs)
					level = (((t - firstStepUs) / periodUs) % 2 == 0) ? 0.55 : 0.85;
				v.push_back(Sample(i, level + g.Noise(0.02)));
			}
			return v;
		}

		// Quality 0.85 +/- 0.02 for durationUs.
		inline std::vector<QualitySample> Steady(int64_t durationUs, uint32_t seed)
		{
			Lcg g(seed);
			std::vector<QualitySample> v;
			for (int i = 0; static_cast<int64_t>(i) * kStepUs < durationUs; ++i)
				v.push_back(Sample(i, 0.85 + g.Noise(0.02)));
			return v;
		}

		inline std::string Trace(const std::vector<AbrEvent>& events)
		{
			std::string s;
			for (size_t i = 0; i < events.size(); ++i)
				s += SourceIdentity::SerializeEvent(events[i]) + "\n";
			return s;
		}
	}

	TEST_CLASS(AbrStepDetectorTests)
	{
	public:
		TEST_METHOD(ADropAtASegmentBoundaryIsDetectedExactlyOnce)
		{
			using namespace AbrSynth;
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, Staircase(12000000, 100000000, 30000000, 11u));
			Assert::IsTrue(e.size() == 1, L"one boundary, one event");
			Assert::IsTrue(e[0].direction == AbrDirection::Drop);
			Assert::IsTrue(e[0].timeUs == 12000000, L"the boundary is the first sample on the new level");
			Assert::IsTrue(e[0].magnitude > 0.25 && e[0].magnitude < 0.35);
			Assert::IsTrue(e[0].confidence >= 0.5);
		}

		TEST_METHOD(FlickerNoiseAroundAConstantIsNotAStep)
		{
			using namespace AbrSynth;
			Lcg g(7u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 240; ++i)             // 60 s
				v.push_back(Sample(i, 0.7 + g.Noise(0.1)));
			AbrStepDetector d;
			Assert::IsTrue(Run(d, v).empty());
		}

		TEST_METHOD(ASceneCutThatReadsLowerWithoutARatioStepIsConfounded)
		{
			using namespace AbrSynth;
			Lcg g(21u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 120; ++i)
			{
				QualitySample s = Sample(i, (i < 48 ? 0.85 : 0.55) + g.Noise(0.02));
				s.cut = (i == 48);
				v.push_back(s);
			}
			AbrStepDetector d;
			Assert::IsTrue(Run(d, v).empty(), L"off the grid, at a cut, no ratio step: not evidence of an ABR switch");
			Assert::IsTrue(d.Confounded() == 1);
		}

		TEST_METHOD(TheSameCutWithAConcurrentRatioStepIsAnEvent)
		{
			using namespace AbrSynth;
			Lcg g(21u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 120; ++i)
			{
				QualitySample s = Sample(i, (i < 48 ? 0.85 : 0.55) + g.Noise(0.02));
				s.cut = (i == 48);
				s.deliveredRatio = i < 48 ? 1.0 : 0.67;
				v.push_back(s);
			}
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, v);
			Assert::IsTrue(e.size() == 1);
			Assert::IsTrue(e[0].cutConfound);
			Assert::IsTrue(e[0].ratioStep < -0.3 && e[0].ratioStep > -0.36);
			Assert::IsTrue(e[0].direction == AbrDirection::Drop);
		}

		TEST_METHOD(AFadeToBlackAndBackIsNotAStep)
		{
			using namespace AbrSynth;
			Lcg g(33u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 120; ++i)
			{
				QualitySample s = Sample(i, 0.85 + g.Noise(0.02));
				if (i >= 40 && i < 46)                 // fade down: quality collapses with the picture
				{
					s.fade = true;
					s.quality = 0.85 - 0.12 * static_cast<double>(i - 39);
					s.apl = 0.5 - 0.08 * static_cast<double>(i - 39);
				}
				else if (i >= 46 && i < 50)            // black
				{
					s.blank = true;
					s.quality = 0.0;
					s.apl = 0.0;
				}
				else if (i >= 50 && i < 56)            // fade up
				{
					s.fade = true;
					s.quality = 0.1 + 0.12 * static_cast<double>(i - 49);
					s.apl = 0.08 * static_cast<double>(i - 49);
				}
				v.push_back(s);
			}
			AbrStepDetector d;
			Assert::IsTrue(Run(d, v).empty());
		}

		TEST_METHOD(ALongDarkSceneWithNoisyQualityIsNotAStep)
		{
			using namespace AbrSynth;
			Lcg g(45u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 160; ++i)
			{
				QualitySample s = Sample(i, 0.85 + g.Noise(0.02));
				if (i >= 40 && i < 80)                 // 10 s below the dark threshold
				{
					s.apl = 0.01;
					s.quality = 0.1 + 0.8 * g.Next();
				}
				v.push_back(s);
			}
			AbrStepDetector d;
			Assert::IsTrue(Run(d, v).empty(), L"quality is unreliable in the dark: abstain");
		}

		TEST_METHOD(TheSegmentPeriodIsMeasuredFromStepsEveryFourSeconds)
		{
			using namespace AbrSynth;
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, Staircase(12000000, 4000000, 35000000, 5u));
			Assert::IsTrue(e.size() == 6);
			for (size_t i = 0; i < e.size(); ++i)
			{
				Assert::IsTrue(e[i].timeUs == 12000000 + static_cast<int64_t>(i) * 4000000);
				Assert::IsTrue(e[i].direction == ((i % 2 == 0) ? AbrDirection::Drop : AbrDirection::Rise));
			}
			Assert::IsTrue(!e[0].onGrid, L"no period known at the first step");
			Assert::IsTrue(e[3].onGrid && e[4].onGrid && e[5].onGrid, L"later steps land on the measured grid");
			Assert::IsTrue(d.MeasuredPeriodUs() > 3990000 && d.MeasuredPeriodUs() < 4010000);
		}

		TEST_METHOD(TheSegmentPeriodIsMeasuredFromStepsEveryTwoAndAHalfSeconds)
		{
			using namespace AbrSynth;
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, Staircase(12000000, 2500000, 26500000, 9u));
			Assert::IsTrue(e.size() == 6);
			Assert::IsTrue(e[3].onGrid && e[4].onGrid && e[5].onGrid, L"the period is not an assumed constant");
			Assert::IsTrue(d.MeasuredPeriodUs() > 2490000 && d.MeasuredPeriodUs() < 2510000);
		}

		TEST_METHOD(ARiseAfterADropIsReportedAsARise)
		{
			using namespace AbrSynth;
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, Staircase(10000000, 10000000, 30000000, 3u));
			Assert::IsTrue(e.size() == 2);
			Assert::IsTrue(e[0].direction == AbrDirection::Drop && e[0].timeUs == 10000000);
			Assert::IsTrue(e[1].direction == AbrDirection::Rise && e[1].timeUs == 20000000);
		}

		TEST_METHOD(ASingleDropIsDetectedOnceAtTwoSamplesPerSecond)
		{
			using namespace AbrSynth;
			Lcg g(61u);
			std::vector<QualitySample> v;
			for (int i = 0; i < 60; ++i)              // 30 s at 2 samples per second (the monitor's cadence)
			{
				QualitySample s = Sample(i, (i < 24 ? 0.85 : 0.55) + g.Noise(0.02));
				s.timeUs = static_cast<int64_t>(i) * 500000;
				v.push_back(s);
			}
			AbrStepDetector d;
			const std::vector<AbrEvent> e = Run(d, v);
			Assert::IsTrue(e.size() == 1);
			Assert::IsTrue(e[0].direction == AbrDirection::Drop && e[0].timeUs == 12000000);
		}

		TEST_METHOD(TheSameSamplesGiveTheSameEvents)
		{
			using namespace AbrSynth;
			AbrStepDetector a, b;
			const std::string ta = Trace(Run(a, Staircase(12000000, 4000000, 35000000, 5u)));
			const std::string tb = Trace(Run(b, Staircase(12000000, 4000000, 35000000, 5u)));
			Assert::IsTrue(!ta.empty() && ta == tb);
		}
	};

	TEST_CLASS(AbrNoticePolicyTests)
	{
	public:
		static SourceIdentity::AbrEvent Drop(double confidence)
		{
			SourceIdentity::AbrEvent e;
			e.direction = SourceIdentity::AbrDirection::Drop;
			e.confidence = confidence;
			return e;
		}

		// A consensus result in which the quality tier latched lower (3 -> 1).
		static SourceIdentity::Result Latched()
		{
			SourceIdentity::Result r;
			r.before.qualityTier = 3;
			r.value.qualityTier = 1;
			r.changedMask = SourceIdentity::FieldBit(SourceIdentity::Field::Quality);
			return r;
		}

		TEST_METHOD(FiresOnceForAConfirmedDropAndNotAgainInTheSameTitle)
		{
			SourceIdentity::NoticePolicy p;
			p.NewTitle(1);
			std::string text;
			Assert::IsTrue(p.Consider(Drop(0.8), Latched(), text));
			Assert::IsTrue(!text.empty());
			text.clear();
			Assert::IsTrue(!p.Consider(Drop(0.9), Latched(), text), L"once per title");
			Assert::IsTrue(text.empty());
			p.NewTitle(1);
			Assert::IsTrue(!p.Consider(Drop(0.9), Latched(), text), L"the same title key does not re-arm");
			p.NewTitle(2);
			Assert::IsTrue(p.Consider(Drop(0.9), Latched(), text), L"a new title re-arms");
		}

		TEST_METHOD(AStaleDropEventDoesNotPairWithALaterLatch)
		{
			SourceIdentity::NoticePolicy p;
			std::string text;
			SourceIdentity::Result late = Latched();
			late.timeUs = 60000000;
			Assert::IsTrue(!p.Consider(Drop(0.9), late, text), L"a drop remembered from a minute ago");
			SourceIdentity::Result soon = Latched();
			soon.timeUs = 5000000;
			Assert::IsTrue(p.Consider(Drop(0.9), soon, text), L"a latch within the allowed delay");
		}

		TEST_METHOD(AFlickerThatNeverLatchedNeverFires)
		{
			SourceIdentity::NoticePolicy p;
			std::string text;
			SourceIdentity::Result held;                 // consensus held: nothing changed
			held.before.qualityTier = 3;
			held.value.qualityTier = 3;
			Assert::IsTrue(!p.Consider(Drop(0.9), held, text));
			// A rise, or a drop of too little confidence, does not fire either.
			SourceIdentity::AbrEvent rise = Drop(0.9);
			rise.direction = SourceIdentity::AbrDirection::Rise;
			Assert::IsTrue(!p.Consider(rise, Latched(), text));
			Assert::IsTrue(!p.Consider(Drop(0.2), Latched(), text));
			// A latched change upward is not a drop.
			SourceIdentity::Result up = Latched();
			up.before.qualityTier = 1;
			up.value.qualityTier = 3;
			Assert::IsTrue(!p.Consider(Drop(0.9), up, text));
		}

		TEST_METHOD(AnAxisRatioDropLatchedByConsensusCountsToo)
		{
			SourceIdentity::NoticePolicy p;
			std::string text;
			SourceIdentity::Result r;
			r.before.ratioV = 1.0;
			r.value.ratioV = 0.5;
			r.changedMask = SourceIdentity::FieldBit(SourceIdentity::Field::AxisV);
			Assert::IsTrue(p.Consider(Drop(0.8), r, text));
		}

		TEST_METHOD(DisabledNeverFires)
		{
			SourceIdentity::NoticePolicy::Settings s;
			s.enabled = false;
			SourceIdentity::NoticePolicy p(s);
			std::string text;
			Assert::IsTrue(!p.Consider(Drop(0.9), Latched(), text));
		}

		TEST_METHOD(WordingWithoutAMeasuredResolutionNamesNoNumbers)
		{
			const std::string plain = SourceIdentity::AbrNoticeText(SourceIdentity::NoticeContext());
			for (size_t i = 0; i < plain.size(); ++i)
				Assert::IsTrue(!(plain[i] >= '0' && plain[i] <= '9'), L"no digits when nothing was measured");
			Assert::IsTrue(plain.find("Restarting playback") != std::string::npos);
			SourceIdentity::NoticeContext c;
			c.fromHeight = 2160;
			c.toHeight = 1080;
			const std::string known = SourceIdentity::AbrNoticeText(c);
			Assert::IsTrue(known.find("2160p to 1080p") != std::string::npos);
		}

		TEST_METHOD(TheDecisionUsesTheStableId)
		{
			const FallbackCatalog::Decision on = SourceIdentity::NoticeDecision(true, "text");
			Assert::IsTrue(on.engaged && on.id == "abr_quality_drop" && on.reason == "text");
			const FallbackCatalog::Decision off = SourceIdentity::NoticeDecision(false, "text");
			Assert::IsTrue(!off.engaged && off.id == "abr_quality_drop");
		}
	};
}
