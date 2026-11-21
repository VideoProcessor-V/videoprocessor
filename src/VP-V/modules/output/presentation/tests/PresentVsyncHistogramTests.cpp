// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// CG-PVH: the present-vs-vsync histogram (PresentVsyncHistogram.h), driven by synthetic DXGI statistics
// sequences, no GPU. Matched 24/24, matched 60/60 and 59.94 content on a 59.95 display; a repeat, a skipped
// present, a stall, a disjoint reset, warm-up, and "no stats" that is said and never a pass. The refresh period
// is always derived from the statistics (several display rates are fed to show that nothing is fixed).
#include "pch.h"
#include "CppUnitTest.h"

// RenderAheadPolicy.h before PresenterAfterPresent.h (windows.h min/max macros), as in PresenterExtensionTests.cpp.
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/modules/output/presentation/PresentVsyncHistogram.h>
#include <VP-V/modules/output/presentation/PresenterAfterPresent.h>
#include <VP-V/modules/output/presentation/PresenterExtension.h>
#include <vprenderer/RendererExtension.h>

#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using Vpv::Presentation::PresentVsyncHistogram;
using Vpv::Presentation::PresentVsyncSample;
using Vpv::Presentation::PresenterExtension;
namespace RE = RendererExtension;

namespace
{
	constexpr int64_t kQpcHz = 10000000;   // a QPC frequency; the unit's only clock input

	// A display of period `periodQpc` showing numbered presents: present i is shown at refresh shownRef[i].
	// The statistics read after present i report that present (PresentCount = i), its refresh, and a sync one
	// refresh later (the latest vsync the driver processed).
	struct Display
	{
		double periodQpc;
		int64_t t0 = 1000000;
		uint32_t baseRef = 100;
		PresentVsyncSample Stats(uint32_t presentId, uint32_t shownRef) const
		{
			PresentVsyncSample s;
			s.read = s.available = true;
			s.presentCount = presentId;
			s.presentRefreshCount = shownRef;
			s.syncRefreshCount = shownRef + 1;
			s.syncQpc = t0 + static_cast<int64_t>((shownRef + 1 - baseRef) * periodQpc);
			return s;
		}
		int64_t TimeOf(uint32_t ref) const { return t0 + static_cast<int64_t>((ref - baseRef) * periodQpc); }
	};

	// Feeds `count` presents, one per refresh, starting at present id 1; the lead is `leadPeriods` display periods.
	void FeedMatched(PresentVsyncHistogram& h, const Display& d, int count, double leadPeriods)
	{
		for (int i = 0; i < count; ++i)
		{
			const uint32_t id = static_cast<uint32_t>(1 + i), ref = d.baseRef + static_cast<uint32_t>(i);
			h.NoteSubmitted(id, d.TimeOf(ref) - static_cast<int64_t>(leadPeriods * d.periodQpc));
			h.Observe(d.Stats(id, ref));
		}
	}

	// A swapchain whose frame statistics the test sets (the module call-site tests).
	class FakeChain final : public IDXGISwapChain
	{
	public:
		UINT presentCount = 0, presentRefreshCount = 0, syncRefreshCount = 0;
		LONGLONG syncQpc = 0;
		int reads = 0;
		HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* stats) override
		{
			++reads;
			stats->PresentCount = presentCount;
			stats->PresentRefreshCount = presentRefreshCount;
			stats->SyncRefreshCount = syncRefreshCount;
			stats->SyncQPCTime.QuadPart = syncQpc;
			stats->SyncGPUTime.QuadPart = 0;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* count) override { *count = presentCount; return S_OK; }
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = nullptr; return E_NOINTERFACE; }
		ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
		ULONG STDMETHODCALLTYPE Release() override { return 1; }
		HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID, void**) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL*, IDXGIOutput**) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return E_NOTIMPL; }
	};

	// One R12 call for present `i` of a matched 59.95 Hz run: the statistics one refresh behind the sync, the swap end
	// `leadPeriods` display periods before the display time.
	void CallAfterPresent(PresenterExtension& ext, RenderAhead::Presenter& p, FakeChain* chain, const void* host,
		uint32_t i, double leadPeriods, bool extrasSpent)
	{
		const LONGLONG period = 166667;
		if (chain)
		{
			chain->presentCount = i;
			chain->presentRefreshCount = 100 + i;
			chain->syncRefreshCount = 101 + i;
			chain->syncQpc = 1000000 + static_cast<LONGLONG>(101 + i) * period;
		}
		RE::FrameContext c;
		c.host = host;
		c.phase = RE::FramePhase::AfterPresent;
		c.generation = 1;
		RE::PresentContext pc;
		pc.presenter = &p;
		pc.native = chain;
		pc.kind = extrasSpent ? RE::AfterPresentKind::AfterLastExtra : RE::AfterPresentKind::Plain;
		pc.extrasSpentTime = extrasSpent;
		pc.rendered = pc.submitted = true;   // a real present was made (the host's facts; as in PresenterExtensionTests)
		const LONGLONG display = 1000000 + static_cast<LONGLONG>(100 + i) * period;
		pc.swapEndQpc = display - static_cast<LONGLONG>(leadPeriods * period);
		pc.now = 1.0 + 0.0167 * i;
		ext.OnAfterPresent(c, pc);
	}

	// The same call with the host's facts: whether the frame was submitted, the host's measured presentation target
	// (known + lead before this step) and the extras' open loop, and the outcome the handler answered.
	RE::PresentOutcome CallAfterPresentFacts(PresenterExtension& ext, RenderAhead::Presenter& p, FakeChain* chain, const void* host,
		uint32_t i, double leadPeriods, bool extrasSpent, bool submitted,
		bool targetTimingKnown = false, double hostTargetLeadMs = 0.0, bool openLoop = false)
	{
		const LONGLONG period = 166667;
		if (chain)
		{
			chain->presentCount = i;
			chain->presentRefreshCount = 100 + i;
			chain->syncRefreshCount = 101 + i;
			chain->syncQpc = 1000000 + static_cast<LONGLONG>(101 + i) * period;
		}
		RE::FrameContext c;
		c.host = host;
		c.phase = RE::FramePhase::AfterPresent;
		c.generation = 1;
		RE::PresentContext pc;
		pc.presenter = &p;
		pc.native = chain;
		pc.kind = extrasSpent ? RE::AfterPresentKind::AfterLastExtra : RE::AfterPresentKind::Plain;
		pc.extrasSpentTime = extrasSpent;
		pc.rendered = pc.submitted = submitted;
		pc.targetTimingKnown = targetTimingKnown;
		pc.hostTargetLeadMs = hostTargetLeadMs;
		pc.openLoop = openLoop;
		const LONGLONG display = 1000000 + static_cast<LONGLONG>(100 + i) * period;
		pc.swapEndQpc = display - static_cast<LONGLONG>(leadPeriods * period);
		pc.now = 1.0 + 0.0167 * i;
		ext.OnAfterPresent(c, pc);
		return pc.outcome;
	}
}

namespace VpvTests
{
	TEST_CLASS(PresentVsyncHistogramTests)
	{
	public:
		TEST_METHOD(MatchedSixtyOverSixtyHoldsEveryFrameOnePeriod)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 120, 1.5);
			Assert::IsTrue(h.Displayed() > 100u, L"warm-up aside, the presents are binned");
			Assert::AreEqual(h.Displayed(), h.Hold(1), L"every hold is one period");
			Assert::AreEqual(0u, h.Hold(0) + h.Hold(2) + h.Hold(3), L"no skip, repeat or stall");
			Assert::AreEqual(1.5, h.LeadPercentile(0.5), 0.01, L"lead p50 = the fed lead in periods");
			Assert::IsTrue(h.LeadCount() > 100u, L"leads are binned once T is known");
			Assert::AreEqual(h.LeadCount(), h.LeadBin(1), L"a 1.5 period lead lands in [1,2)");
			Assert::AreEqual(0u, h.Unresolved(), L"one present per read: nothing unresolved");
		}

		TEST_METHOD(MatchedTwentyFourOverTwentyFourOnA24HzDisplay)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 23.976 };
			FeedMatched(h, d, 80, 2.2);
			Assert::AreEqual(h.Displayed(), h.Hold(1));
			Assert::IsTrue(h.LeadCount() > 0u);
			Assert::AreEqual(h.LeadCount(), h.LeadBin(2), L"2.2 periods = [2,3)");
			Assert::AreEqual(kQpcHz / 23.976, h.PeriodQpcMeasured(), 1.0, L"T is derived from the statistics");
		}

		TEST_METHOD(ThePeriodIsDerivedNotFixed)
		{
			const double rates[] = { 23.976, 47.960, 50.0, 59.950, 100.0, 143.9 };
			for (double hz : rates)
			{
				PresentVsyncHistogram h;
				const Display d{ kQpcHz / hz };
				FeedMatched(h, d, 40, 1.0);
				Assert::AreEqual(kQpcHz / hz, h.PeriodQpcMeasured(), 1.0, L"T follows the display");
				Assert::AreEqual(h.Displayed(), h.Hold(1), L"one period holds at any rate");
			}
		}

		TEST_METHOD(AClockSlipRepeatIsHoldTwo)
		{
			// 59.94 content on a 59.95 display: once in a while one present is shown for two refreshes.
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			uint32_t ref = d.baseRef;
			for (uint32_t id = 1; id <= 60; ++id)
			{
				h.NoteSubmitted(id, d.TimeOf(ref) - static_cast<int64_t>(1.2 * d.periodQpc));
				h.Observe(d.Stats(id, ref));
				ref += (id == 30) ? 2u : 1u;   // present 30 stays one extra refresh
			}
			Assert::AreEqual(1u, h.Hold(2), L"one repeat (k=2)");
			Assert::AreEqual(0u, h.Hold(0), L"nothing skipped");
			Assert::AreEqual(0u, h.Hold(3), L"no 3+ hold");
			Assert::AreEqual(h.Displayed() - 1u, h.Hold(1));
		}

		TEST_METHOD(ASkippedPresentIsTheSpecSkipCase)
		{
			// Present 20 was replaced before it reached the screen: ids 19 -> 21 over ONE refresh.
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			uint32_t ref = d.baseRef;
			for (uint32_t id = 1; id <= 40; ++id)
			{
				if (id == 20)
					continue;   // never shown; no refresh passes for it
				h.NoteSubmitted(id, d.TimeOf(ref) - static_cast<int64_t>(1.2 * d.periodQpc));
				h.Observe(d.Stats(id, ref));
				++ref;
			}
			Assert::AreEqual(1u, h.Hold(0), L"one present never shown (step 2, refresh delta 1)");
			Assert::AreEqual(1u, h.Unresolved(), L"the other present of that read is unresolved, not binned");
			Assert::AreEqual(0u, h.Hold(2) + h.Hold(3), L"no false repeat or stall");
		}

		TEST_METHOD(TwoPresentsOverTwoRefreshesIsASamplingArtifactNotADropOrARepeat)
		{
			// One read saw no new present; the next saw +2 presents over +2 refreshes (two normal one-period holds).
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			for (uint32_t id = 1; id <= 40; ++id)
			{
				if (id == 21)
					continue;   // this read was missed: the next shows ids 20 -> 22
				h.NoteSubmitted(id, d.TimeOf(d.baseRef + id - 1) - static_cast<int64_t>(1.2 * d.periodQpc));
				h.Observe(d.Stats(id, d.baseRef + id - 1));
			}
			Assert::AreEqual(0u, h.Hold(0), L"nothing was dropped");
			Assert::AreEqual(0u, h.Hold(2), L"nothing was repeated");
			Assert::AreEqual(0u, h.Hold(3));
			Assert::AreEqual(2u, h.Unresolved(), L"the two presents of the long step are unresolved, visibly");
		}

		TEST_METHOD(AStallIsHoldThreePlus)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			uint32_t ref = d.baseRef;
			for (uint32_t id = 1; id <= 40; ++id)
			{
				h.NoteSubmitted(id, d.TimeOf(ref) - static_cast<int64_t>(1.2 * d.periodQpc));
				h.Observe(d.Stats(id, ref));
				ref += (id == 25) ? 5u : 1u;
			}
			Assert::AreEqual(1u, h.Hold(3), L"a five-period hold is 3+");
		}

		TEST_METHOD(NoStatisticsIsSaidAndNeverAPass)
		{
			PresentVsyncHistogram h;
			PresentVsyncSample s;
			s.read = true;
			s.available = false;
			s.hr = static_cast<int32_t>(0x887A0004);
			for (int i = 0; i < 50; ++i)
				h.Observe(s);
			Assert::AreEqual(0u, h.Displayed());
			Assert::AreEqual(50u, h.Unavailable());
			const std::string line = h.Line(kQpcHz);
			Assert::IsTrue(line.find("no stats (hr=0x887A0004") != std::string::npos, L"the code is named");
			Assert::IsTrue(line.find("not a pass") != std::string::npos, L"never a silent pass");
			Assert::IsTrue(line.find("50 reads") != std::string::npos);

			PresentVsyncHistogram none;
			PresentVsyncSample off;   // no swapchain
			none.Observe(off);
			Assert::IsTrue(none.Line(kQpcHz).find("no stats") != std::string::npos, L"no swapchain is also said");
		}

		TEST_METHOD(AWindowWithSomeMissingReadsSaysHowManyLacked)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 30, 1.0);
			PresentVsyncSample bad;
			bad.read = true;
			bad.hr = 7;
			h.Observe(bad);
			h.Observe(bad);
			Assert::IsTrue(h.Line(kQpcHz).find("reads without stats 2 of 32") != std::string::npos, L"the line counts the reads without statistics");
		}

		TEST_METHOD(WarmUpCountsLeadsAndNeverMakesASpuriousStall)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 5, 1.0);   // fewer than kMinRefreshesForPeriod refreshes seen
			Assert::AreEqual(4u, h.Displayed(), L"the holds are exact refresh deltas: binned at once");
			Assert::AreEqual(4u, h.Hold(1));
			Assert::IsTrue(h.Warmup() > 0u, L"the leads of the warm-up are counted");
			Assert::AreEqual(0u, h.LeadCount(), L"no lead without T");
			Assert::AreEqual(0.0, h.PeriodQpcMeasured(), L"no period yet");

			// Just past warm-up: the first present with a known T is a normal hold, not 3+ (audit M2).
			PresentVsyncHistogram g;
			FeedMatched(g, d, static_cast<int>(PresentVsyncHistogram::kMinRefreshesForPeriod) + 2, 1.0);
			Assert::AreEqual(0u, g.Hold(3), L"no spurious stall at the end of warm-up");
			Assert::AreEqual(g.Displayed(), g.Hold(1));
			Assert::IsTrue(g.LeadCount() > 0u);
		}

		TEST_METHOD(ADisjointReadRestartsThePairing)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 30, 1.0);
			const uint32_t before = h.Displayed();
			PresentVsyncSample dj;
			dj.read = true;
			dj.disjoint = true;
			h.Observe(dj);
			// The counters restart: present ids and refreshes begin again; no hold spans the discontinuity.
			PresentVsyncSample s = d.Stats(1, 5);
			h.Observe(s);
			Assert::AreEqual(before, h.Displayed(), L"nothing binned across the discontinuity");
		}

		TEST_METHOD(TheWindowReportsEveryTenSecondsAndRestarts)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 40, 1.0);
			Assert::IsFalse(h.ReportDue(0, kQpcHz), L"the first call starts the window");
			Assert::IsFalse(h.ReportDue(9 * kQpcHz, kQpcHz));
			Assert::IsTrue(h.ReportDue(10 * kQpcHz, kQpcHz));
			const std::string line = h.TakeLine(10 * kQpcHz, kQpcHz);
			Assert::IsTrue(line.find("Presenter present-vs-vsync: holds k0/k1/k2/k3+ = 0/") != std::string::npos);
			Assert::IsTrue(line.find("fd=59.950 Hz") != std::string::npos, L"fd is the measured rate");
			Assert::AreEqual(0u, h.Displayed(), L"the window restarted");
			Assert::IsFalse(h.ReportDue(11 * kQpcHz, kQpcHz));
			Assert::IsTrue(h.PeriodQpcMeasured() > 0.0, L"the measured period survives the window");
		}
		TEST_METHOD(ANegativeLeadIsShownAndInNoBin)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			for (int i = 0; i < 30; ++i)
			{
				const uint32_t id = static_cast<uint32_t>(1 + i), ref = d.baseRef + static_cast<uint32_t>(i);
				h.NoteSubmitted(id, d.TimeOf(ref) + static_cast<int64_t>(d.periodQpc));   // swap end AFTER the display: ids are wrong
				h.Observe(d.Stats(id, ref));
			}
			Assert::IsTrue(h.LeadNegative() > 0u);
			Assert::AreEqual(0u, h.LeadCount(), L"in no bin");
			const std::string want = std::to_string(h.LeadNegative()) + " negative)";
			Assert::IsTrue(h.Line(kQpcHz).find(want) != std::string::npos, L"the count is in the line");
		}

		TEST_METHOD(ACounterRestartClearsTheIdRing)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			FeedMatched(h, d, 20, 1.0);
			const uint32_t leadsBefore = h.LeadCount(), missingBefore = h.LeadMissing();
			// A new swapchain: ids restart from 1 at a lower refresh count. The old ids 4..20 must not pair.
			h.Observe(d.Stats(3, 50));   // PresentCount 20 -> 3: backwards
			Assert::AreEqual(1u, h.Resets());
			for (uint32_t id = 4; id <= 20; ++id)
				h.Observe(d.Stats(id, 47 + id));   // never submitted since the restart
			Assert::AreEqual(leadsBefore, h.LeadCount(), L"no lead paired with a stale id");
			Assert::IsTrue(h.LeadMissing() > missingBefore, L"counted as unmatched");
		}

		TEST_METHOD(TheModuleFeedsTheStatisticsAndThePresentIdsIntoTheHistogram)
		{
			PresenterExtension ext;
			RenderAhead::Presenter p;
			int host = 0;
			FakeChain chain;
			for (uint32_t i = 1; i <= 40; ++i)
				CallAfterPresent(ext, p, &chain, &host, i, 1.5, false);
			const std::shared_ptr<PresenterExtension::PresentEnd> end = ext.PresentEndOf(&host);
			Assert::IsTrue(end != nullptr);
			Assert::AreEqual(39u, end->vsync.Displayed(), L"one distinct present per call, the first has no previous");
			Assert::AreEqual(39u, end->vsync.Hold(1));
			Assert::IsTrue(end->vsync.LeadCount() > 20u);
			Assert::AreEqual(1.5, end->vsync.LeadPercentile(0.5), 0.02, L"the swap end was noted with the present id");
			Assert::AreEqual(40, chain.reads, L"one GetFrameStatistics per call (the Presenter's own read)");
		}

		TEST_METHOD(AnExtrasFrameNotesNoLeadButTheHoldStillCounts)
		{
			PresenterExtension ext;
			RenderAhead::Presenter p;
			int host = 0;
			FakeChain chain;
			for (uint32_t i = 1; i <= 30; ++i)
				CallAfterPresent(ext, p, &chain, &host, i, 1.5, true);
			const std::shared_ptr<PresenterExtension::PresentEnd> end = ext.PresentEndOf(&host);
			Assert::IsTrue(end != nullptr);
			Assert::AreEqual(0u, end->vsync.LeadCount(), L"the last present id of an extras frame is an extra");
			Assert::IsTrue(end->vsync.LeadMissing() > 0u, L"visible as unmatched");
			Assert::IsTrue(end->vsync.Hold(1) > 0u);
		}

		TEST_METHOD(WithoutASwapchainTheModuleSaysNoStats)
		{
			PresenterExtension ext;
			RenderAhead::Presenter p;
			int host = 0;
			for (uint32_t i = 1; i <= 5; ++i)
				CallAfterPresent(ext, p, nullptr, &host, i, 1.5, false);
			const std::shared_ptr<PresenterExtension::PresentEnd> end = ext.PresentEndOf(&host);
			Assert::IsTrue(end != nullptr);
			const std::string line = end->vsync.Line(kQpcHz);
			Assert::IsTrue(line.find("no stats") != std::string::npos);
			Assert::IsTrue(line.find("5 without a swapchain") != std::string::npos);
			Assert::IsTrue(line.find("not a pass") != std::string::npos);
		}
		TEST_METHOD(APresentThatWasNotMadeIsNotNoted)
		{
			PresenterExtension ext;
			RenderAhead::Presenter p;
			int host = 0;
			FakeChain chain;
			for (uint32_t i = 1; i <= 30; ++i)
				CallAfterPresentFacts(ext, p, &chain, &host, i, 1.5, false, false);   // R12 ran, nothing was submitted
			const std::shared_ptr<PresenterExtension::PresentEnd> end = ext.PresentEndOf(&host);
			Assert::IsTrue(end != nullptr);
			Assert::AreEqual(0u, end->vsync.LeadCount(), L"no id was noted for a present that was not made");
			Assert::IsTrue(end->vsync.LeadMissing() > 0u, L"visible as unmatched");
		}

		TEST_METHOD(ARepeatedPresentIdKeepsTheFirstSwapEnd)
		{
			PresentVsyncHistogram h;
			const Display d{ kQpcHz / 59.950 };
			for (int i = 0; i < 20; ++i)
			{
				const uint32_t id = static_cast<uint32_t>(1 + i), ref = d.baseRef + static_cast<uint32_t>(i);
				h.NoteSubmitted(id, d.TimeOf(ref) - static_cast<int64_t>(1.5 * d.periodQpc));
				h.NoteSubmitted(id, d.TimeOf(ref) + static_cast<int64_t>(5 * d.periodQpc));   // the same id again, much later
				h.Observe(d.Stats(id, ref));
			}
			Assert::AreEqual(0u, h.LeadNegative());
			Assert::AreEqual(1.5, h.LeadPercentile(0.5), 0.02, L"the first swap end of the id stands");
		}

		TEST_METHOD(TheStatisticsNeverChangeThePresentersDecision)
		{
			// The same call sequence with and without a swapchain: the outcome of every call is identical. Committed and
			// uncommitted plans, a known and an unknown target, open loop on and off, so the raise and the open-loop
			// estimate (PresenterAfterPresent's extras branch) are both reached and compared.
			PresenterExtension extA, extB;
			RenderAhead::Presenter pA, pB;
			int hostA = 0, hostB = 0;
			FakeChain chain;
			unsigned raises = 0, estimates = 0;
			for (uint32_t i = 1; i <= 40; ++i)
			{
				const bool extras = (i % 3) == 0, submitted = (i % 7) != 0;
				const bool committed = (i % 2) == 0, known = (i % 4) < 2, openLoop = (i % 5) == 0;
				const double planLeadMs = 20.0 + i;
				pA.committed = pB.committed = committed;   // this frame's plan was (not) queued
				pA.targetLeadMs = pB.targetLeadMs = planLeadMs;
				const RE::PresentOutcome a = CallAfterPresentFacts(extA, pA, nullptr, &hostA, i, 1.5, extras, submitted, known, 10.0, openLoop);
				const RE::PresentOutcome b = CallAfterPresentFacts(extB, pB, &chain, &hostB, i, 1.5, extras, submitted, known, 10.0, openLoop);
				Assert::AreEqual(a.committed, b.committed);
				Assert::AreEqual(a.raiseTargetLead, b.raiseTargetLead);
				Assert::AreEqual(a.targetLeadMs, b.targetLeadMs);
				Assert::AreEqual(a.estimatedLeadMs, b.estimatedLeadMs);
				Assert::AreEqual(pA.committed, pB.committed);
				Assert::AreEqual(pA.targetLeadMs, pB.targetLeadMs);
				if (b.raiseTargetLead)
				{
					++raises;
					Assert::AreEqual(planLeadMs, b.targetLeadMs, L"the raise is the planned lead");
				}
				if (b.estimatedLeadMs > 0.0)
				{
					++estimates;
					Assert::AreEqual(planLeadMs, b.estimatedLeadMs, L"open loop publishes the planned lead");
				}
			}
			// Extras + committed + known: i = 12, 24, 36. Extras + committed + unknown + open loop: i = 30.
			Assert::AreEqual(3u, raises, L"the raise branch was reached");
			Assert::AreEqual(1u, estimates, L"the open-loop estimate branch was reached");
			Assert::IsTrue(chain.reads > 0, L"side B really read statistics");
		}
	};
}
