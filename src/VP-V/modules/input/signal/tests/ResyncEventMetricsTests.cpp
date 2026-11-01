// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Item 15: per-event resync metrics (ResyncEventMetrics.h) and the resync / resync_settle line grammar.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/Indicator.h>
#include <VP-V/core/ResyncCause.h>
#include <VP-V/modules/control/diagnostics/VpEventLine.h>
#include <VP-V/modules/input/signal/ResyncEventMetrics.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;

namespace Tests
{
	namespace ResyncMetrics
	{
		struct Rig
		{
			EventMetrics m;
			std::vector<std::string> lines;
			std::shared_ptr<VpIndicator::Indicator> ind;

			Rig()
			{
				ind = std::make_shared<VpIndicator::Indicator>(VpIndicator::NextEra(), 64);
				m.SetSink([this](const std::string& l) { lines.push_back(l); });
				m.AddReader("render-ahead");
				m.AddReader("timeline");
				m.SetFrameRate(50.0);
			}

			void Seal(uint64_t seq, double ms) { m.OnSealed(ind, seq, 0, ms); }
			void BothAct(uint64_t seq)
			{
				m.OnReaderAct("render-ahead", seq);
				m.OnReaderAct("timeline", seq);
			}
		};

		inline bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
		inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	}

	TEST_CLASS(ResyncEventMetricsTests)
	{
	public:
		TEST_METHOD(SingleFullSettlesAfterReadersAndVectors)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(10, Level::Full, Cause::InPlaceSourceChange, 0, false);
			r.m.OnVectorsValid(false);   // the renderer invalidated them at the discontinuity
			r.BothAct(10);
			r.Seal(10, 1000.0);          // carrying frame: readers acted, vectors still invalid
			r.Seal(11, 1020.0);
			r.Seal(12, 1040.0);
			Assert::AreEqual<size_t>(0, r.lines.size());
			Assert::AreEqual<size_t>(1, r.m.PendingCount());
			r.m.OnVectorsValid(true);
			r.Seal(13, 1060.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::AreEqual<std::string>(
				"VPEV1 t=1060.000 kind=resync_settle seq=10 settle_ms=60.000 settle_frames=3 drops=0 repeats=0 "
				"relock=na relock_frames=0 vectors=valid cause=in_place_source_change", r.lines[0]);
			Assert::AreEqual<uint64_t>(1, r.m.Tracked());
			Assert::AreEqual<uint64_t>(1, r.m.Settled());
			Assert::AreEqual<uint64_t>(0, r.m.Unsettled());
			Assert::AreEqual<size_t>(0, r.m.PendingCount());
			Assert::AreEqual<size_t>(1, r.lines.size(), L"exactly one line per event");
			r.Seal(14, 1080.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
		}

		TEST_METHOD(EveryDueReaderMustAct)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(5, Level::Full, Cause::RendererReset, 0, true);
			r.m.OnReaderAct("render-ahead", 5);
			r.Seal(5, 100.0);
			r.Seal(6, 120.0);
			Assert::AreEqual<size_t>(0, r.lines.size(), L"timeline has not acted");
			r.m.OnReaderAct("timeline", 7);
			r.Seal(7, 140.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "settle_frames=2 "));
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "settle_ms=40.000 "));
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "cause=renderer_reset"));
		}

		TEST_METHOD(ReaderActForAnEarlierSeqDoesNotCoverALaterEvent)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(20, Level::Full, Cause::FileDiscontinuity, 0, false);
			r.BothAct(19);
			r.Seal(20, 0.0);
			r.Seal(21, 20.0);
			Assert::AreEqual<size_t>(0, r.lines.size());
			r.BothAct(21);
			r.Seal(22, 40.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
		}

		TEST_METHOD(RelockPendingDelaysSettle)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(30, Level::Full, Cause::InPlaceSourceChange, 0, false, true);   // touches the pulldown
			r.BothAct(30);
			r.Seal(30, 0.0);
			r.Seal(31, 20.0);
			r.Seal(32, 40.0);
			Assert::AreEqual<size_t>(0, r.lines.size(), L"waiting for the relock");
			r.m.OnRelock(true);
			r.Seal(33, 60.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "relock=yes relock_frames=2 vectors=valid"), ResyncMetrics::W(r.lines[0]).c_str());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "settle_frames=3 "));
		}

		TEST_METHOD(RelockStartedAfterTheEventAlsoWaits)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(40, Level::Full, Cause::LiveQueueReset, 0, false);
			r.m.OnRelock(false);   // the pulldown started relocking after the event was recorded
			r.BothAct(40);
			r.Seal(40, 0.0);
			r.Seal(41, 20.0);
			Assert::AreEqual<size_t>(0, r.lines.size());
			r.m.OnRelock(true);
			r.Seal(42, 40.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "relock=yes relock_frames=1 "));
			// An event recorded while a relock is already pending waits for it too.
			r.m.OnRelock(false);
			r.m.OnEvent(50, Level::Full, Cause::RendererReset, 0, false);
			r.BothAct(50);
			r.Seal(50, 100.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			r.m.OnRelock(true);
			r.Seal(51, 120.0);
			Assert::AreEqual<size_t>(2, r.lines.size());
		}

		TEST_METHOD(StormOfFiveFullsEachTracked)
		{
			ResyncMetrics::Rig r;
			for (uint64_t s = 10; s < 15; ++s)
				r.m.OnEvent(s, Level::Full, Cause::LiveQueueReset, 0, true);
			Assert::AreEqual<size_t>(5, r.m.PendingCount());
			for (uint64_t s = 10; s < 14; ++s)
				r.Seal(s, static_cast<double>(s) * 20.0);
			r.BothAct(14);
			r.Seal(14, 280.0);
			Assert::AreEqual<size_t>(5, r.lines.size());
			for (size_t i = 0; i < 5; ++i)
			{
				const std::string seq = "seq=" + std::to_string(10 + i) + " ";
				Assert::IsTrue(ResyncMetrics::Has(r.lines[i], seq.c_str()), ResyncMetrics::W(r.lines[i]).c_str());
				const std::string frames = "settle_frames=" + std::to_string(4 - i) + " ";
				Assert::IsTrue(ResyncMetrics::Has(r.lines[i], frames.c_str()), ResyncMetrics::W(r.lines[i]).c_str());
			}
			Assert::AreEqual<uint64_t>(5, r.m.Tracked());
			Assert::AreEqual<uint64_t>(5, r.m.Settled());
			Assert::AreEqual<size_t>(0, r.m.PendingCount());
		}

		TEST_METHOD(PendingRingOverflowIsCountedAndShown)
		{
			ResyncMetrics::Rig r;
			for (uint64_t s = 1; s <= 70; ++s)
				r.m.OnEvent(s, Level::Full, Cause::LiveQueueReset, 0, false);
			Assert::AreEqual<uint64_t>(64, r.m.Tracked());
			Assert::AreEqual<uint64_t>(6, r.m.Overflowed());
			Assert::AreEqual<size_t>(64, r.m.PendingCount());
			Assert::IsTrue(ResyncMetrics::Has(r.m.StatusText(), "6 FULL not tracked"), ResyncMetrics::W(r.m.StatusText()).c_str());
			// the tracked ones still settle normally
			r.BothAct(70);
			r.Seal(70, 0.0);
			Assert::AreEqual<size_t>(64, r.lines.size());
			Assert::AreEqual<uint64_t>(6, r.m.Overflowed());
		}

		TEST_METHOD(UnsettledEventIsAbandonedAndCounted)
		{
			ResyncMetrics::Rig r;
			r.m.SetFrameRate(10.0);
			r.m.SetAbandonSeconds(0.5);
			Assert::AreEqual<uint32_t>(5, r.m.MaxSettleFrames(), L"abandon seconds times the measured rate");
			r.m.OnEvent(60, Level::Full, Cause::InPlaceSourceChange, 0, false, true);
			r.m.OnVectorsValid(false);
			for (uint64_t s = 60; s < 65; ++s)
				r.Seal(s, static_cast<double>(s - 60) * 40.0);
			Assert::AreEqual<size_t>(0, r.lines.size());
			r.Seal(65, 200.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "settle_frames=5 "));
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "relock=no relock_frames=0 vectors=invalid"), ResyncMetrics::W(r.lines[0]).c_str());
			Assert::AreEqual<uint64_t>(1, r.m.Unsettled());
			Assert::AreEqual<uint64_t>(0, r.m.Settled());
			Assert::AreEqual<size_t>(0, r.m.PendingCount());
			Assert::IsTrue(ResyncMetrics::Has(r.m.StatusText(), "UNSETTLED"));
		}

		TEST_METHOD(NoMeasuredRateMeansNoAbandonBoundAndSaysSo)
		{
			ResyncMetrics::Rig r;
			r.m.SetFrameRate(0.0);
			Assert::AreEqual<uint32_t>(0, r.m.MaxSettleFrames());
			r.m.OnEvent(1, Level::Full, Cause::RendererReset, 0, false);
			for (uint64_t s = 1; s < 2000; ++s)
				r.Seal(s, static_cast<double>(s));
			Assert::AreEqual<size_t>(0, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.m.StatusText(), "no abandon bound"));
			r.m.SetMaxSettleFrames(7);
			Assert::AreEqual<uint32_t>(7, r.m.MaxSettleFrames(), L"an explicit bound wins");
			r.Seal(2000, 2000.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::AreEqual<uint64_t>(1, r.m.Unsettled());
		}

		TEST_METHOD(DerivedBoundFollowsTheMeasuredRate)
		{
			ResyncMetrics::Rig r;
			r.m.SetFrameRate(59.94);
			Assert::AreEqual<uint32_t>(600, r.m.MaxSettleFrames());   // ceil(10 s * 59.94)
			r.m.SetFrameRate(24.0);
			Assert::AreEqual<uint32_t>(240, r.m.MaxSettleFrames());
		}

		TEST_METHOD(DropsAndRepeatsAttributeToTheNewestEarlierEvent)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(10, Level::Full, Cause::LiveQueueReset, 0, false);
			r.m.OnEvent(20, Level::Full, Cause::RendererReset, 0, false);
			r.m.OnDrop(5);      // before any event
			r.m.OnDrop(12);     // event 10
			r.m.OnRepeat(25);   // event 20
			r.m.OnDrop(21);     // event 20
			Assert::AreEqual<uint64_t>(1, r.m.UnattributedDrops());
			r.Seal(10, 0.0);
			r.Seal(20, 100.0);
			r.BothAct(25);
			r.Seal(25, 150.0);
			Assert::AreEqual<size_t>(2, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "seq=10 "));
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "drops=1 repeats=0 "), ResyncMetrics::W(r.lines[0]).c_str());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[1], "seq=20 "));
			Assert::IsTrue(ResyncMetrics::Has(r.lines[1], "drops=1 repeats=1 "), ResyncMetrics::W(r.lines[1]).c_str());
			r.m.OnRepeat(99);   // nothing pending any more
			Assert::AreEqual<uint64_t>(1, r.m.UnattributedRepeats());
		}

		TEST_METHOD(MinorAndTimingAreOnlyCounted)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(3, Level::Minor, Cause::MissedFrames, 2, false);
			r.m.OnEvent(4, Level::Timing, Cause::RateStep, 0, false);
			r.m.OnEvent(5, Level::None, Cause::None, 0, false);
			Assert::AreEqual<uint64_t>(1, r.m.MinorCount());
			Assert::AreEqual<uint64_t>(1, r.m.TimingCount());
			Assert::AreEqual<uint64_t>(0, r.m.Tracked());
			Assert::AreEqual<size_t>(0, r.m.PendingCount());
			r.Seal(3, 0.0);
			Assert::AreEqual<size_t>(0, r.lines.size());
		}

		TEST_METHOD(VectorsNotRequiredAndBackwardsTimeAndUnknownReader)
		{
			ResyncMetrics::Rig r;
			r.m.SetVectorsRequired(false);
			r.m.OnVectorsValid(false);
			r.m.OnEvent(8, Level::Full, Cause::FileDiscontinuity, 0, false);
			r.m.OnReaderAct("nobody", 8);
			Assert::AreEqual<uint64_t>(1, r.m.UnknownReaderActs());
			r.Seal(8, 500.0);
			r.BothAct(9);
			r.Seal(9, 100.0);   // source time stepped back (a seek): clamps to 0
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "settle_ms=0.000 settle_frames=1 "), ResyncMetrics::W(r.lines[0]).c_str());
			Assert::IsTrue(ResyncMetrics::Has(r.lines[0], "vectors=valid"));
			r.m.OnSealed(std::shared_ptr<VpIndicator::Indicator>(), 10, 0, 0.0);   // no Indicator: ignored
		}

		TEST_METHOD(ClockSuppliesTheLineTimeNotTheDecision)
		{
			ResyncMetrics::Rig r;
			r.m.SetClock([]() { return 123456.789; });
			r.m.OnEvent(1, Level::Full, Cause::RendererReset, 0, false);
			r.BothAct(1);
			r.Seal(1, 0.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			Assert::IsTrue(r.lines[0].rfind("VPEV1 t=123456.789 kind=resync_settle ", 0) == 0);
		}

		TEST_METHOD(ResyncExtrasAreAdditiveAndQuoted)
		{
			ResyncMetrics::Rig r;
			Assert::AreEqual<std::string>("render-ahead,timeline", r.m.DueReadersText());
			std::vector<VpEvent::Field> f;
			f.emplace_back("level", "full");
			f.emplace_back("cause", "renderer_reset");
			f.emplace_back("seq", uint64_t(7));
			f.emplace_back("mode", "on");
			EventMetrics::AppendResyncExtras(f, true, r.m.DueReadersText());
			const std::string line = VpEvent::FormatRange(VpEvent::Kind::resync, 1.0, f.data(), f.size());
			Assert::AreEqual<std::string>(
				"VPEV1 t=1.000 kind=resync level=full cause=renderer_reset seq=7 mode=on injected=1 readers=\"render-ahead,timeline\"", line);
			std::vector<VpEvent::Field> g(f.begin(), f.begin() + 4);
			EventMetrics::AppendResyncExtras(g, false, std::string());
			Assert::AreEqual<std::string>(
				"VPEV1 t=1.000 kind=resync level=full cause=renderer_reset seq=7 mode=on injected=0",
				VpEvent::FormatRange(VpEvent::Kind::resync, 1.0, g.data(), g.size()));
			// the pre-existing field set is still valid and unchanged
			const std::string old = VpEvent::Format(VpEvent::Kind::resync, 1.0,
				{ { "level", "minor" }, { "cause", "missed_frames" }, { "seq", uint64_t(42) }, { "gap", uint64_t(3) }, { "mode", "shadow" } });
			Assert::AreEqual<std::string>("VPEV1 t=1.000 kind=resync level=minor cause=missed_frames seq=42 gap=3 mode=shadow", old);
		}

		TEST_METHOD(SettleLineKeysMatchThePythonFieldSet)
		{
			ResyncMetrics::Rig r;
			r.m.OnEvent(1, Level::Full, Cause::RendererReset, 0, false);
			r.BothAct(1);
			r.Seal(1, 0.0);
			Assert::AreEqual<size_t>(1, r.lines.size());
			const char* const keys[] = { " seq=", " settle_ms=", " settle_frames=", " drops=", " repeats=", " relock=",
				" relock_frames=", " vectors=", " cause=" };
			size_t at = r.lines[0].find("kind=resync_settle");
			Assert::IsTrue(at != std::string::npos);
			for (const char* k : keys)
			{
				const size_t p = r.lines[0].find(k, at);
				Assert::IsTrue(p != std::string::npos, ResyncMetrics::W(k).c_str());
				at = p;
			}
			VpEvent::Kind k = VpEvent::Kind::flag;
			Assert::IsTrue(VpEvent::KindFromToken("resync_settle", k) && k == VpEvent::Kind::resync_settle);
		}
	};
}
