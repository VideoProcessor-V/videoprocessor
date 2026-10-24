// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"
#include <VP-V/core/PassTimers.h>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Efficiency timer plan (audit 2026-10-24 section 4): sampling, statistics, the waits kept out
// of the work totals, and the Ctrl+Alt+D readout. The GPU side (no waits, no drains) is in the
// GPU test plan, docs/design/efficiency-byte-identical-tests.md (T1-T5).
namespace VideoProcessorTest
{
	TEST_CLASS(PassTimersTests)
	{
	public:
		TEST_METHOD(SamplerTakesOneFrameInN)
		{
			PassTimers::Sampler s(8);
			int taken = 0;
			for (uint64_t f = 0; f < 800; ++f) taken += s.Take(f) ? 1 : 0;
			Assert::AreEqual(100, taken);
			s.SetEveryN(0);   // never divides by zero: every frame
			Assert::IsTrue(s.Take(7) && s.EveryN() == 1);
		}

		TEST_METHOD(UnreadGpuTimersAreNotSamples)
		{
			PassTimers::Accumulator a;
			a.AddGpuPassNs("VP extended range: clipped picture + remainder", 0);   // not read back yet
			Assert::IsTrue(a.Empty());
			a.AddGpuPassNs("VP extended range: clipped picture + remainder", 500000);
			a.AddGpuPassNs("VP extended range: clipped picture + remainder", 1500000);
			const PassTimers::Stat* s = a.Find("VP extended range: clipped picture + remainder");
			Assert::IsNotNull(s);
			Assert::AreEqual(2ull, static_cast<unsigned long long>(s->samples));
			Assert::AreEqual(1.0, s->MeanMs(), 1e-9);
			Assert::AreEqual(1.5, s->maxMs, 1e-9);
			a.AddGpuPassNs(nullptr, 1000000);
			Assert::IsNotNull(a.Find("(unnamed pass)"));
		}

		TEST_METHOD(WaitsAreNeverCountedAsWork)
		{
			PassTimers::Accumulator a;
			PassTimers::CpuSplit split;
			split.Begin(0.0);
			split.Mark(a, "analysis", PassTimers::Kind::CpuWork, 1.0);
			split.Mark(a, "hook assembly", PassTimers::Kind::CpuWork, 1.5);
			split.Mark(a, "vblank wait", PassTimers::Kind::CpuWait, 9.5);
			split.Mark(a, "submit", PassTimers::Kind::CpuWork, 10.0);
			split.End();
			split.Mark(a, "after end", PassTimers::Kind::CpuWork, 50.0);   // ignored
			Assert::AreEqual(2.0, a.TotalMeanMs(PassTimers::Kind::CpuWork), 1e-9);
			Assert::AreEqual(8.0, a.TotalMeanMs(PassTimers::Kind::CpuWait), 1e-9);
			Assert::IsNull(a.Find("after end"));
		}

		TEST_METHOD(ReadoutListsLargestFirstPerKind)
		{
			PassTimers::Accumulator a;
			a.Add(PassTimers::Kind::GpuPass, "small", 0.1);
			a.Add(PassTimers::Kind::GpuPass, "large", 2.0);
			a.Add(PassTimers::Kind::GpuGroup, "entrance: deband", 0.4);
			a.Add(PassTimers::Kind::CpuWork, "upload CPU copy", 2.4);
			a.Add(PassTimers::Kind::CpuWait, "early-slot fence wait", 0.05);
			const std::string r = a.Readout(8, 1);
			Assert::IsTrue(r.find("sampled 1 in 8") != std::string::npos);
			Assert::IsTrue(r.find("large") != std::string::npos);
			Assert::IsTrue(r.find("small") == std::string::npos, L"maxLines 1 keeps the largest only");
			Assert::IsTrue(r.find("... 1 more") != std::string::npos);
			Assert::IsTrue(r.find("GPU group") < r.find("CPU work") && r.find("CPU work") < r.find("CPU wait"));
			Assert::IsTrue(PassTimers::Accumulator().Readout(4).find("no samples yet") != std::string::npos);
		}
	};
}
