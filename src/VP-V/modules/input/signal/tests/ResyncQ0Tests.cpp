// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Q0-A part 2: SettingsStamp, FramesInFlight, ResyncInject and the LED reader helper.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/FramesInFlight.h>
#include <VP-V/core/Indicator.h>
#include <VP-V/modules/drive/led_compensation/LedDriveConfig.h>
#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <VP-V/modules/drive/led_compensation/LedResyncPolicy.h>
#include <VP-V/modules/input/signal/ResyncInject.h>
#include <VP-V/modules/input/signal/SettingsStamp.h>

#include <cstdint>
#include <limits>
#include <map>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace VpResync;
using VpIndicator::Bit;

namespace Tests
{
	namespace ResyncQ0
	{
		using Map = std::map<std::string, std::string>;

		SettingsStamp::Getter From(const Map& m)
		{
			return [&m](const std::string& key, std::string& value) {
				const auto it = m.find(key);
				if (it == m.end())
					return false;
				value = it->second;
				return true;
			};
		}

		struct FakeLed
		{
			int restarts = 0;
			uint64_t seq = 0;
			double fps = -1.0;
			void OnSourceRestart(uint64_t s, double f) { ++restarts; seq = s; fps = f; }
		};

		ResyncInject::Getter Env(const char* value)
		{
			return [value](const char* name) -> const char* { return std::strcmp(name, "VP_RESYNC_INJECT") == 0 ? value : nullptr; };   // only the arming variable
		}
	}

	TEST_CLASS(SettingsStampTests)
	{
	public:
		TEST_METHOD(FirstConfigureOnlyRecords)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			SettingsStamp stamp;
			ResyncQ0::Map m{ { "motion_engine", "nvof" }, { "render_ahead", "auto" } };
			const SettingsStamp::Result r = stamp.Configure(ResyncQ0::From(m), &ind, 5);
			Assert::IsTrue(r.first);
			Assert::AreEqual<uint32_t>(0, r.advanced);
			for (int c = 0; c < int(VpIndicator::kCounterCount); ++c)
				Assert::AreEqual<uint32_t>(0, ind.Total(VpIndicator::Counter(c)));
		}

		TEST_METHOD(UnchangedReconfigureAdvancesNothing)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			SettingsStamp stamp;
			ResyncQ0::Map m{ { "motion_engine", "nvof" } };
			stamp.Configure(ResyncQ0::From(m), &ind, 1);
			const SettingsStamp::Result r = stamp.Configure(ResyncQ0::From(m), &ind, 2);
			Assert::IsFalse(r.first);
			Assert::AreEqual<uint32_t>(0, r.advanced);
			Assert::AreEqual<uint32_t>(0, ind.Total(VpIndicator::kSettingsMotion));
		}

		TEST_METHOD(OnlyTheChangedDomainAdvances)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			SettingsStamp stamp;
			ResyncQ0::Map m{ { "motion_engine", "nvof" }, { "rebase_kernel_match", "1" }, { "subtitle_fit", "0" },
				{ "render_ahead", "auto" } };
			stamp.Configure(ResyncQ0::From(m), &ind, 1);

			m["motion_engine"] = "matcher";
			SettingsStamp::Result r = stamp.Configure(ResyncQ0::From(m), &ind, 3);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsMotion), r.advanced);
			Assert::AreEqual<uint32_t>(1, ind.Total(VpIndicator::kSettingsMotion));
			Assert::AreEqual<uint32_t>(0, ind.Total(VpIndicator::kSettingsRebase));
			Assert::AreEqual<uint32_t>(0, ind.Total(VpIndicator::kSettingsAnalysis));
			Assert::AreEqual<uint32_t>(0, ind.Total(VpIndicator::kSettingsOutput));

			m["rebase_kernel_match"] = "0";
			r = stamp.Configure(ResyncQ0::From(m), &ind, 4);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsRebase), r.advanced);
			m["subtitle_fit"] = "1";
			r = stamp.Configure(ResyncQ0::From(m), &ind, 5);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsAnalysis), r.advanced);
			m["render_ahead"] = "off";
			r = stamp.Configure(ResyncQ0::From(m), &ind, 6);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsOutput), r.advanced);
			Assert::AreEqual<uint32_t>(1, ind.Total(VpIndicator::kSettingsOutput));

			// The Advance carries the SettingsApply cause and the stamp's fromSeq.
			VpIndicator::Event ev[8];
			const size_t n = ind.Snapshot(ev, 8);
			Assert::AreEqual<size_t>(4, n);
			Assert::IsTrue(ev[0].cause == Cause::SettingsApply && ev[0].counter == VpIndicator::kSettingsMotion);
			Assert::AreEqual<uint64_t>(3, ev[0].fromSeq);
		}

		TEST_METHOD(AKeyInTwoDomainsMovesBoth)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			SettingsStamp stamp;
			ResyncQ0::Map m{ { "source_resolution", "auto" } };
			stamp.Configure(ResyncQ0::From(m), &ind, 1);
			m["source_resolution"] = "1080";
			const SettingsStamp::Result r = stamp.Configure(ResyncQ0::From(m), &ind, 2);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsRebase) | Bit(VpIndicator::kSettingsAnalysis), r.advanced);
		}

		TEST_METHOD(KeyOrderDoesNotMatter)
		{
			SettingsStamp a, b;
			a.SetDomainKeys(SettingsStamp::Domain::Motion, { "k1", "k2", "k3" });
			b.SetDomainKeys(SettingsStamp::Domain::Motion, { "k3", "k1", "k2", "k1" });
			ResyncQ0::Map m{ { "k1", "a" }, { "k2", "b" }, { "k3", "c" } };
			Assert::IsTrue(a.HashDomain(SettingsStamp::Domain::Motion, ResyncQ0::From(m)) ==
				b.HashDomain(SettingsStamp::Domain::Motion, ResyncQ0::From(m)));
			ResyncQ0::Map changed{ { "k1", "a" }, { "k2", "B" }, { "k3", "c" } };
			Assert::IsTrue(a.HashDomain(SettingsStamp::Domain::Motion, ResyncQ0::From(m)) !=
				b.HashDomain(SettingsStamp::Domain::Motion, ResyncQ0::From(changed)));
		}

		TEST_METHOD(MissingKeyDiffersFromEmptyValue)
		{
			SettingsStamp s;
			s.SetDomainKeys(SettingsStamp::Domain::Output, { "only" });
			ResyncQ0::Map missing;
			ResyncQ0::Map empty{ { "only", "" } };
			Assert::IsTrue(s.HashDomain(SettingsStamp::Domain::Output, ResyncQ0::From(missing)) !=
				s.HashDomain(SettingsStamp::Domain::Output, ResyncQ0::From(empty)));

			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			s.Configure(ResyncQ0::From(missing), &ind, 1);
			const SettingsStamp::Result r = s.Configure(ResyncQ0::From(empty), &ind, 2);
			Assert::AreEqual<uint32_t>(Bit(VpIndicator::kSettingsOutput), r.advanced, L"missing -> empty is a change");
		}

		TEST_METHOD(NullIndicatorOnlyRecords)
		{
			SettingsStamp s;
			ResyncQ0::Map m{ { "motion_engine", "a" } };
			s.Configure(ResyncQ0::From(m), nullptr, 1);
			m["motion_engine"] = "b";
			const SettingsStamp::Result r = s.Configure(ResyncQ0::From(m), nullptr, 2);
			Assert::AreEqual<uint32_t>(0, r.advanced);
			ResyncQ0::Map again{ { "motion_engine", "b" } };
			Assert::AreEqual<uint32_t>(0, s.Configure(ResyncQ0::From(again), nullptr, 3).advanced);
		}

		TEST_METHOD(ReKeyingADomainRerecordsWithoutAdvance)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			SettingsStamp s;
			ResyncQ0::Map m{ { "motion_engine", "a" }, { "x", "1" } };
			s.Configure(ResyncQ0::From(m), &ind, 1);
			s.SetDomainKeys(SettingsStamp::Domain::Motion, { "motion_engine", "x" });
			Assert::IsFalse(s.Recorded(SettingsStamp::Domain::Motion));
			const SettingsStamp::Result r = s.Configure(ResyncQ0::From(m), &ind, 2);
			Assert::AreEqual<uint32_t>(0, r.advanced);
			Assert::IsTrue(r.first);
		}

		TEST_METHOD(DefaultTableIsFilled)
		{
			SettingsStamp s;
			Assert::IsTrue(s.DomainKeys(SettingsStamp::Domain::Motion).size() > 10);
			Assert::AreEqual<size_t>(3, s.DomainKeys(SettingsStamp::Domain::Rebase).size());
			Assert::IsTrue(!s.DomainKeys(SettingsStamp::Domain::Analysis).empty());
			Assert::IsTrue(!s.DomainKeys(SettingsStamp::Domain::Output).empty());
		}

		TEST_METHOD(AdvanceGraphWritesTheGraphCounter)
		{
			VpIndicator::Indicator ind(VpIndicator::NextEra(), 64);
			Assert::AreEqual<uint64_t>(7, SettingsStamp::AdvanceGraph(ind, 7));
			Assert::AreEqual<uint32_t>(1, ind.Total(VpIndicator::kGraph));
			VpIndicator::Event ev[2];
			Assert::AreEqual<size_t>(1, ind.Snapshot(ev, 2));
			Assert::IsTrue(ev[0].cause == Cause::GraphRebuild && ev[0].counter == VpIndicator::kGraph);
		}
	};

	TEST_CLASS(FramesInFlightTests)
	{
	public:
		TEST_METHOD(IsTheSumPlusOne)
		{
			Assert::AreEqual<size_t>(1, FramesInFlight(0, 0, 0, 0, 0));
			Assert::AreEqual<size_t>(1 + 20 + 6 + 4 + 2 + 1, FramesInFlight(20, 6, 4, 2, 1));
		}

		TEST_METHOD(SaturatesInsteadOfWrapping)
		{
			const size_t max = std::numeric_limits<size_t>::max();
			Assert::IsTrue(FramesInFlight(max, 5, 5, 5, 5) == max);
			Assert::IsTrue(FramesInFlight(max - 1, 1, 0, 0, 0) == max);
			Assert::IsTrue(FramesInFlight(1, max, max, max, max) == max);
		}

		TEST_METHOD(RingHoldsEveryFrameInFlightAtRenderAheadTwoLaneLeadOne)
		{
			const size_t frames = FramesInFlight(20, 6, 4, 2, 1);   // 34
			Assert::AreEqual<size_t>(34, frames);
			VpIndicator::Indicator ind(VpIndicator::NextEra(), VpIndicator::Indicator::RequiredCapacity(frames));
			Assert::AreEqual<size_t>(256, ind.Capacity());   // NextPow2(34 * 4)
			const VpIndicator::Counter counters[VpIndicator::Indicator::kMaxEventsPerFrame] = {
				VpIndicator::kContinuity, VpIndicator::kTiming, VpIndicator::kContinuity, VpIndicator::kGraph };
			for (size_t f = 1; f <= frames; ++f)
				for (size_t e = 0; e < VpIndicator::Indicator::kMaxEventsPerFrame; ++e)
					ind.Advance(counters[e], Cause::GapOther, f);   // nothing sealed: all unsealed frames
			Assert::AreEqual<uint32_t>(0, ind.PrematureFolds());
		}

		TEST_METHOD(UndersizedRingFoldsPrematurely)
		{
			// Without the render-ahead depth and the lane lead: 31 frames -> 124 events -> a 128 ring,
			// but 34 frames write 136 events into it.
			const size_t frames = FramesInFlight(20, 6, 4, 2, 1);
			const size_t undersized = frames - 2 - 1;
			VpIndicator::Indicator ind(VpIndicator::NextEra(), VpIndicator::Indicator::RequiredCapacity(undersized));
			Assert::AreEqual<size_t>(128, ind.Capacity());
			for (size_t f = 1; f <= frames; ++f)
				for (size_t e = 0; e < VpIndicator::Indicator::kMaxEventsPerFrame; ++e)
					ind.Advance(VpIndicator::kContinuity, Cause::GapOther, f);
			Assert::IsTrue(ind.PrematureFolds() > 0);
		}
	};

	TEST_CLASS(ResyncInjectTests)
	{
	public:
		TEST_METHOD(UnarmedIsInert)
		{
			ResyncInject inj(ResyncQ0::Env(nullptr));
			Assert::IsFalse(inj.Armed());
			Assert::AreEqual<std::string>("not armed", inj.Invoke("resync.inject", "seek"));
			Assert::AreEqual<std::string>("not armed", inj.Invoke("resync.inject", "gap 5"));
			ResyncInject::Request r;
			Assert::IsFalse(inj.Poll(r));
			Assert::AreEqual<uint32_t>(0, inj.TakeGapFramesToDrop());
			Assert::AreEqual<uint32_t>(0, inj.TakeStallMs());
			Assert::AreEqual(1.0, inj.TimestampScale(), 0.0);
			Assert::AreEqual<std::string>("", inj.StatusText());
			inj.ApplyRate(500);
			Assert::AreEqual(1.0, inj.TimestampScale(), 0.0, L"ApplyRate ignored unarmed");
		}

		TEST_METHOD(OnlyExactlyOneArms)
		{
			const char* const values[] = { "0", "", "true", "11", "1 ", "yes" };
			for (const char* v : values)
			{
				ResyncInject inj(ResyncQ0::Env(v));
				Assert::IsFalse(inj.Armed());
			}
			ResyncInject armed(ResyncQ0::Env("1"));
			Assert::IsTrue(armed.Armed());
			Assert::AreEqual<std::string>("resync injection armed", armed.StatusText());
		}

		TEST_METHOD(EveryKindParsesWhenArmed)
		{
			ResyncInject inj(ResyncQ0::Env("1"));
			Assert::AreEqual<std::string>("ok: queued seek", inj.Invoke("resync.inject", "seek"));
			Assert::AreEqual<std::string>("ok: queued reset", inj.Invoke("resync.inject", "reset"));
			Assert::AreEqual<std::string>("ok: queued inplace", inj.Invoke("resync.inject", "inplace"));
			Assert::AreEqual<std::string>("ok: queued rebuild", inj.Invoke("resync.inject", "rebuild"));
			Assert::AreEqual<std::string>("ok: queued gap", inj.Invoke("resync.inject", "gap 3"));
			Assert::AreEqual<std::string>("ok: queued rate", inj.Invoke("resync.inject", "rate -250"));
			Assert::AreEqual<std::string>("ok: queued stall", inj.Invoke("resync.inject", "stall 40"));
			Assert::AreEqual<std::string>("ok: queued capture_restart", inj.Invoke("resync.inject", "capture_restart"));
			const ResyncInject::Kind order[] = { ResyncInject::Kind::Seek, ResyncInject::Kind::Reset,
				ResyncInject::Kind::InPlace, ResyncInject::Kind::Rebuild, ResyncInject::Kind::Gap,
				ResyncInject::Kind::Rate, ResyncInject::Kind::Stall, ResyncInject::Kind::CaptureRestart };
			const int64_t values[] = { 0, 0, 0, 0, 3, -250, 40, 0 };
			for (size_t i = 0; i < 8; ++i)
			{
				ResyncInject::Request r;
				Assert::IsTrue(inj.Poll(r));
				Assert::IsTrue(r.kind == order[i]);
				Assert::IsTrue(r.value == values[i]);
			}
			ResyncInject::Request none;
			Assert::IsFalse(inj.Poll(none));
		}

		TEST_METHOD(BadArgumentsAreRejected)
		{
			ResyncInject inj(ResyncQ0::Env("1"));
			const char* const bad[] = { "", "   ", "bogus", "gap", "gap 0", "gap -1", "gap 1001", "gap abc", "gap 1.5",
				"gap 3 4", "rate", "rate 1000001", "rate -1000001", "rate x", "rate -", "stall", "stall 0",
				"stall 60001", "stall 1234567890", "seek 3", "reset 1", "capture_restart 2" };
			for (const char* a : bad)
			{
				const std::string out = inj.Invoke("resync.inject", a);
				Assert::IsTrue(out.rfind("error: ", 0) == 0);
			}
			Assert::IsTrue(inj.Invoke("other.command", "seek").rfind("error: ", 0) == 0);
			ResyncInject::Request r;
			Assert::IsFalse(inj.Poll(r), L"rejected requests queue nothing");
			Assert::AreEqual<uint32_t>(0, inj.TakeGapFramesToDrop());
			Assert::AreEqual(1.0, inj.TimestampScale(), 0.0);
		}

		TEST_METHOD(BoundariesAreAccepted)
		{
			ResyncInject inj(ResyncQ0::Env("1"));
			Assert::AreEqual<std::string>("ok: queued gap", inj.Invoke("resync.inject", "gap 1000"));
			Assert::AreEqual<std::string>("ok: queued stall", inj.Invoke("resync.inject", "stall 60000"));
			Assert::AreEqual<std::string>("ok: queued rate", inj.Invoke("resync.inject", "rate 1000000"));
			Assert::AreEqual<std::string>("ok: queued rate", inj.Invoke("resync.inject", "rate -1000000"));
			Assert::AreEqual<std::string>("ok: queued rate", inj.Invoke("resync.inject", "rate 0"));
			Assert::AreEqual(1.0, inj.TimestampScale(), 0.0);
		}

		TEST_METHOD(QueueIsBoundedAtSixteen)
		{
			ResyncInject inj(ResyncQ0::Env("1"));
			for (int i = 0; i < 16; ++i)
				Assert::AreEqual<std::string>("ok: queued seek", inj.Invoke("resync.inject", "seek"));
			Assert::AreEqual<std::string>("error: queue full", inj.Invoke("resync.inject", "seek"));
			// A full queue applies nothing: no gap was added.
			Assert::AreEqual<std::string>("error: queue full", inj.Invoke("resync.inject", "gap 9"));
			Assert::AreEqual<uint32_t>(0, inj.TakeGapFramesToDrop());
			ResyncInject::Request r;
			Assert::IsTrue(inj.Poll(r));
			Assert::AreEqual<std::string>("ok: queued gap", inj.Invoke("resync.inject", "gap 9"), L"a slot is free again");
			size_t n = 0;
			while (inj.Poll(r))
				++n;
			Assert::AreEqual<size_t>(16, n);
		}

		TEST_METHOD(AdapterAccessors)
		{
			ResyncInject inj(ResyncQ0::Env("1"));
			inj.Invoke("resync.inject", "gap 3");
			inj.Invoke("resync.inject", "gap 4");
			Assert::AreEqual<uint32_t>(7, inj.TakeGapFramesToDrop());
			Assert::AreEqual<uint32_t>(0, inj.TakeGapFramesToDrop(), L"taken once");
			inj.Invoke("resync.inject", "stall 120");
			Assert::AreEqual<uint32_t>(120, inj.TakeStallMs());
			Assert::AreEqual<uint32_t>(0, inj.TakeStallMs());
			Assert::AreEqual(1.0, inj.TimestampScale(), 0.0);
			inj.Invoke("resync.inject", "rate 1000");
			Assert::AreEqual(1.001, inj.TimestampScale(), 1e-12);
			inj.Invoke("resync.inject", "rate -500000");
			Assert::AreEqual(0.5, inj.TimestampScale(), 1e-12);
			inj.ApplyRate(2000000);   // clamped to the range
			Assert::AreEqual(2.0, inj.TimestampScale(), 1e-12);
		}
	};

	TEST_CLASS(LedResyncPolicyTests)
	{
	public:
		TEST_METHOD(OnlyFullRestartsTheLed)
		{
			ResyncQ0::FakeLed led;
			Assert::IsFalse(VpResync::ApplyFullToLed(led, 0, 10, 24.0));
			Assert::IsFalse(VpResync::ApplyFullToLed(led, Bit(VpIndicator::kContinuity) | Bit(VpIndicator::kTiming), 10, 24.0));
			Assert::AreEqual(0, led.restarts);
			Assert::IsTrue(VpResync::ApplyFullToLed(led, Bit(VpIndicator::kFull) | Bit(VpIndicator::kContinuity), 42, 59.94));
			Assert::AreEqual(1, led.restarts);
			Assert::AreEqual<uint64_t>(42, led.seq);
			Assert::AreEqual(59.94, led.fps, 1e-12);
			Assert::IsTrue(VpResync::ApplyFullToLed(led, VpIndicator::kAllCounters, 43, 0.0));
			Assert::AreEqual(2, led.restarts);
			Assert::AreEqual(0.0, led.fps, 0.0, L"0 = rate unknown, passed through");
		}

		TEST_METHOD(RealModuleRestartHoldsGainAndBaselinesCntr)
		{
			LedDrive::LedDriveConfig config;
			config.enabled = true;
			config.mode = LedDrive::Mode::Compensate;
			config.cntrAssist = false;
			LedDrive::LedDriveModule module(LedDrive::ProjectorConfig(), config);
			module.BeginFrameAt(100, 1000.0, 24.0);
			module.BeginFrameAt(101, 1041.7, 24.0);
			const uint64_t epoch = module.LookaheadEpoch();
			Assert::IsFalse(VpResync::ApplyFullToLed(module, Bit(VpIndicator::kTiming), 102, 60.0));
			Assert::AreEqual(epoch, module.LookaheadEpoch());
			Assert::IsTrue(VpResync::ApplyFullToLed(module, Bit(VpIndicator::kFull), 102, 60.0));
			Assert::IsTrue(module.LookaheadEpoch() > epoch);
			Assert::IsFalse(module.HaveStatsForTest());
			Assert::AreEqual(50, module.PairedContrastForTest());
			Assert::AreEqual(1000.0 / 60.0, module.FramePeriodMsForTest(), 1e-9);
		}
	};
}
