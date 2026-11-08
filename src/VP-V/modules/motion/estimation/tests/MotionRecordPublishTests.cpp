// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV motion record publish (row 24). CPU: the motion group of a frame-analysis record carries the shared engine's global for a known
// pan (the same x / y the engine answers through EngineGlobal), an invalid / ambiguous / absent global publishes ABSENT (never the
// previous value), a discontinuity refuses the write, and no other group of the record is touched.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/motion/estimation/MotionRecordPublish.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		FrameAnalysis::Identity IdOf(uint64_t seq)
		{
			FrameAnalysis::Identity id;
			id.seq = seq;
			id.width = 1920;
			id.height = 1080;
			return id;
		}
	}

	TEST_CLASS(MotionRecordPublishTests)
	{
	public:
		TEST_METHOD(ThePublishedRecordMatchesTheEngineGlobalForAKnownPan)
		{
			using namespace VpMotion;
			const int halvings = 1;   // cell = 8 * 2^1 = 16 source px per D1 texel
			const float global[4] = { 2.5f, -0.5f, 0.2f, 1.0f };   // x, y, ambiguity, validity
			VectorEngine engine;
			engine.NoteEngineGlobal(10, 11, global, halvings);
			GlobalVec fromEngine;
			Assert::IsTrue(engine.EngineGlobal(11, fromEngine) && fromEngine.valid);
			Assert::AreEqual(40.0, fromEngine.x, 1e-9, L"2.5 texels x 16 px");
			Assert::AreEqual(-8.0, fromEngine.y, 1e-9, L"-0.5 texels x 16 px");

			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.Begin(IdOf(11), FrameAnalysis::kMotion);
			Assert::IsTrue(MotionRecord::Publish(&store, 11, MotionRecord::SummaryOf(true, 10, 11, global, halvings)));
			FrameAnalysis::Record record;
			Assert::IsTrue(store.Read(11, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kMotion, record) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(record.motion.vectorsValid);
			Assert::AreEqual(fromEngine.x, record.motion.globalX, 1e-9);
			Assert::AreEqual(fromEngine.y, record.motion.globalY, 1e-9);
			Assert::AreEqual(uint64_t(10), record.motion.pairFrom);
			Assert::IsTrue(record.motion.source == FrameAnalysis::VectorSource::EngineAhead);

			// A pair spanning two periods is per FRAME: half of the pair's motion.
			const FrameAnalysis::MotionSummary twoPeriods = MotionRecord::SummaryOf(true, 10, 12, global, halvings);
			Assert::AreEqual(20.0, twoPeriods.globalX, 1e-9);
			Assert::AreEqual(-4.0, twoPeriods.globalY, 1e-9);
		}

		TEST_METHOD(AnInvalidAmbiguousOrAbsentGlobalPublishesAbsentNeverTheLastValue)
		{
			using namespace VpMotion;
			const float good[4] = { 2.5f, -0.5f, 0.2f, 1.0f };
			const float ambiguous[4] = { 2.5f, -0.5f, 0.95f, 1.0f };   // z >= 0.9: not usable (VectorDemand::SampleGlobal)
			VectorEngine engine;
			engine.NoteEngineGlobal(10, 11, ambiguous, 1);
			GlobalVec fromEngine;
			Assert::IsFalse(engine.EngineGlobal(11, fromEngine), L"the engine itself reports no usable global");

			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.Begin(IdOf(11), FrameAnalysis::kMotion);
			Assert::IsTrue(MotionRecord::Publish(&store, 11, MotionRecord::SummaryOf(true, 10, 11, good, 1)));
			FrameAnalysis::Record record;
			Assert::IsTrue(store.Read(11, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kMotion, record) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(record.motion.vectorsValid);

			struct Case { bool pairValid; const float* global; uint64_t from, to; const wchar_t* why; };
			const Case cases[] = {
				{ true, ambiguous, 10, 11, L"ambiguous global" },
				{ false, good, 10, 11, L"the vectors-valid state does not hold the pair" },
				{ true, nullptr, 10, 11, L"no engine pair" },
				{ true, good, 0, 11, L"no earlier frame" },
				{ true, good, 11, 11, L"not a pair" },
			};
			for (const Case& c : cases)
			{
				Assert::IsTrue(MotionRecord::Publish(&store, 11, MotionRecord::SummaryOf(true, 10, 11, good, 1)));   // a valid value first
				Assert::IsTrue(MotionRecord::Publish(&store, 11, MotionRecord::SummaryOf(c.pairValid, c.from, c.to, c.global, 1)), c.why);
				Assert::IsTrue(store.Read(11, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kMotion, record) == FrameAnalysis::ReadStatus::Ok, c.why);
				Assert::IsFalse(record.motion.vectorsValid, c.why);
				Assert::AreEqual(0.0, record.motion.globalX, 0.0, c.why);
				Assert::AreEqual(0.0, record.motion.globalY, 0.0, c.why);
				Assert::AreEqual(uint64_t(0), record.motion.pairFrom, c.why);
				Assert::IsTrue(record.motion.source == FrameAnalysis::VectorSource::None, c.why);
			}
		}

		TEST_METHOD(ADiscontinuityRefusesTheWriteAndNoOtherGroupIsTouched)
		{
			using namespace VpMotion;
			const float good[4] = { 1.0f, 2.0f, 0.1f, 1.0f };
			Assert::IsFalse(MotionRecord::Publish(nullptr, 5, MotionRecord::SummaryOf(true, 4, 5, good, 0)), L"no store (Original pathway / store off)");

			FrameAnalysis::Store store(FrameAnalysis::RingDepth(4, 4));
			store.Begin(IdOf(5), FrameAnalysis::kMeasuredGroups);
			FrameAnalysis::Record before;
			Assert::IsTrue(store.Read(5, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kIdentity, before) == FrameAnalysis::ReadStatus::Ok);
			Assert::IsTrue(MotionRecord::Publish(&store, 5, MotionRecord::SummaryOf(true, 4, 5, good, 0)));
			FrameAnalysis::Record after;
			Assert::IsTrue(store.Read(5, FrameAnalysis::Use::SingleFrame, FrameAnalysis::kIdentity, after) == FrameAnalysis::ReadStatus::Ok);
			Assert::AreEqual(uint32_t(FrameAnalysis::kMotion), after.valid & ~before.valid, L"only the motion bit was added");
			Assert::AreEqual(before.expected, after.expected);
			Assert::AreEqual(before.Complete(), after.Complete(), L"completeness of the measured groups is unchanged");

			store.Invalidate(FrameAnalysis::Discontinuity::DroppedFrame);
			Assert::IsFalse(MotionRecord::Publish(&store, 5, MotionRecord::SummaryOf(true, 4, 5, good, 0)), L"a pair never spans a discontinuity");
			Assert::IsFalse(MotionRecord::Publish(&store, 5, MotionRecord::Absent()));
			Assert::IsFalse(MotionRecord::Publish(&store, 0, MotionRecord::Absent()), L"no sequence");
		}
	};
}
