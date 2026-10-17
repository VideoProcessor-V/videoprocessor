#include "pch.h"
#include "CppUnitTest.h"

#include <led/LedDriveModule.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <vprenderer/InterpolationTimeline.h>
#include <vprenderer/RenderAheadPolicy.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-render-ahead-design.md "Tests / CPU": the render-ahead planner (refresh
// counts and phases), lateness, open-loop operation, the gate's budgets, ladder and
// hysteresis, a FIFO scheduler simulation with jitter, and the helpers that came with
// it (the LED look-ahead walk, the standalone gap fill, the LED's display time).
// Rates are swept, never fixed in the code under test (owner [[vp-general-not-fixed]]).
namespace RenderAheadPolicyTest
{
	using namespace RenderAhead;

	namespace
	{
		std::wstring Name(double sourceHz, double displayHz)
		{
			return std::to_wstring(sourceHz) + L" on " + std::to_wstring(displayHz);
		}

		const double kSources[] = { 24.0, 25.0, 30.0, 48.0, 50.0, 60.0,
			24000.0 / 1001.0, 30000.0 / 1001.0, 48000.0 / 1001.0, 60000.0 / 1001.0 };
		const double kDisplays[] = { 30.0, 50.0, 60.0, 120.0, 144.0, 60000.0 / 1001.0, 120000.0 / 1001.0 };

		// N of an even cadence (a whole number of refreshes per source frame, two or
		// more: the extras sit at k/N), else 0.
		int EvenCycle(double ratio)
		{
			const double n = std::round(ratio);
			return n >= 2.0 && std::abs(ratio - n) < 1e-9 ? static_cast<int>(n) : 0;
		}

		FrameInput Input(double sourceHz, double displayHz, double filmTime, double firstVsync, double floor)
		{
			FrameInput in;
			in.sourcePeriod = 1.0 / sourceHz;
			in.displayPeriod = 1.0 / displayHz;
			const double ratio = in.sourcePeriod / in.displayPeriod;
			in.filmTime = filmTime;
			in.firstVsync = firstVsync;
			in.latencyFloor = floor;
			in.lead = PlannedLead(in.sourcePeriod, in.displayPeriod);
			in.strength = 1.0;
			in.evenCycle = EvenCycle(ratio);
			in.maxExtras = MaxExtras(ratio);
			return in;
		}

		void CheckIntervals(const FramePlan& plan, const std::wstring& what)
		{
			Assert::IsTrue(plan.nextInterval >= 1 && plan.nextInterval <= kMaxSyncInterval, (what + L": next interval").c_str());
			for (const Slot& slot : plan.slots)
				if (slot.render)
					Assert::IsTrue(slot.interval >= 1 && slot.interval <= kMaxSyncInterval, (what + L": extra interval").c_str());
		}

		// --- The FIFO scheduler simulation -----------------------------------

		struct SimConfig
		{
			double sourceHz = 24.0;
			double displayHz = 60.0;
			double newFrameMs = 0.0;   // N (mean)
			double extraMs = 0.0;      // X (mean)
			double estimateMs = 0.0;   // E (mean), after the first extra
			double jitter = 0.30;      // uniform +-
			double seconds = 600.0;
			unsigned seed = 1;
			int queueDepth = static_cast<int>(kQueuedFrameLatency);   // Q
			bool openLoop = false;     // no DXGI statistics (internal panel)
		};

		struct SimResult
		{
			int frames = 0;
			int presents = 0;
			int late = 0;              // presents shown later than planned (ground truth)
			int lateObserved = 0;      // Schedule::Observe / behind reported late
			int restarts = 0;
			int dropped = 0;
			int extras = 0;
			double minError = 1e9;     // shown - planned over A's own presents
			double maxError = -1e9;
		};

		// The GPU executes N, then each extra X serially (E after the first extra),
		// each +-jitter uniform (seeded). A present's work starts only when the GPU is
		// free, its source frame has arrived, and the present Q before it has been
		// shown (SetMaximumFrameLatency). The display shows one present per vsync,
		// FIFO, no sooner than SyncInterval refreshes after the previous one. The
		// renderer plans each frame after its own present, as LibplaceboVideoRenderer
		// does: the latest present on screen (closed loop) or nothing (open loop),
		// FirstVsync, the latency floor (min), TakeLateDrops, PlanFrame, Commit.
		SimResult Simulate(const SimConfig& c)
		{
			SimResult r;
			const double Ps = 1.0 / c.sourceHz, Pd = 1.0 / c.displayHz;
			const double ratio = Ps / Pd;
			const double v0 = 0.0013;   // the display's vsync grid
			std::mt19937 rng(c.seed);
			std::uniform_real_distribution<double> jitter(1.0 - c.jitter, 1.0 + c.jitter);
			auto cost = [&](double ms) { return ms * jitter(rng) / 1000.0; };
			auto vsyncAtOrAfter = [&](double t) { return v0 + std::ceil((t - v0) / Pd - 1e-9) * Pd; };

			std::vector<double> shown, planned;
			auto present = [&](double ready, int interval, double plannedVsync)
			{
				double earliest = ready;
				if (!shown.empty())
					earliest = std::max(earliest, shown.back() + std::max(1, interval) * Pd - 1e-9);
				shown.push_back(vsyncAtOrAfter(earliest));
				planned.push_back(plannedVsync);
			};
			auto startAt = [&](double gpuFree)
			{
				const size_t k = shown.size();
				double start = gpuFree;
				if (k >= static_cast<size_t>(c.queueDepth))
					start = std::max(start, shown[k - static_cast<size_t>(c.queueDepth)]);
				return start;
			};

			Schedule schedule;
			double floor = std::numeric_limits<double>::infinity();
			double gpuFree = 2.0 * Ps;   // the queue: the first frame starts two source periods in
			int mainInterval = 1;
			const int frames = static_cast<int>(c.seconds / Ps);
			for (int i = 0; i < frames; ++i)
			{
				const double film = i * Ps;
				// A: its whole render, then its present (the interval the last plan set).
				double start = std::max(startAt(gpuFree), film);
				const double done = start + cost(c.newFrameMs);
				gpuFree = done;
				const size_t idA = shown.size();
				const double now = done;
				// The plan (after A's present call, as the renderer).
				double grid = 0.0;
				bool late = false;
				if (!c.openLoop)
				{
					grid = v0;   // a vsync seen on the display before any of ours
					// The latest present shown by now (DXGI reports the latest only).
					size_t latest = idA;
					for (size_t j = idA; j-- > 0;)
						if (shown[j] <= now) { latest = j; break; }
					if (latest < idA)
					{
						grid = shown[latest];
						late = schedule.Observe(static_cast<uint32_t>(latest + 1), shown[latest], Pd);
					}
				}
				bool restart = false, behind = false;
				const double firstVsync = schedule.FirstVsync(now, Pd, grid, restart, behind);
				present(done, mainInterval, firstVsync);
				const double lead = PlannedLead(Ps, Pd);
				floor = std::min(floor, firstVsync - film - (restart ? 0.0 : lead));
				if (late || behind) ++r.lateObserved;
				if (restart) ++r.restarts;
				FrameInput in;
				in.filmTime = film;
				in.sourcePeriod = Ps;
				in.displayPeriod = Pd;
				in.firstVsync = firstVsync;
				in.latencyFloor = floor;
				in.lead = lead;
				in.strength = 1.0;
				in.evenCycle = EvenCycle(ratio);
				in.maxExtras = MaxExtras(ratio);
				in.dropExtras = schedule.TakeLateDrops() + (behind ? 1 : 0);
				const FramePlan plan = PlanFrame(in);
				Assert::IsTrue(plan.valid);
				r.dropped += plan.dropped;
				std::vector<std::pair<uint32_t, double>> ids;
				ids.emplace_back(static_cast<uint32_t>(idA + 1), firstVsync);
				bool first = true;
				for (const Slot& slot : plan.slots)
				{
					if (!slot.render)
						continue;
					const double s = startAt(gpuFree);
					const double ready = s + cost(c.extraMs);
					gpuFree = ready;
					ids.emplace_back(static_cast<uint32_t>(shown.size() + 1), slot.vsync);
					present(ready, slot.interval, slot.vsync);
					++r.extras;
					if (first)
					{
						gpuFree += cost(c.estimateMs);
						first = false;
					}
				}
				schedule.Commit(plan.nextFirstVsync, ids);
				mainInterval = plan.nextInterval;
				if (i > 0)
				{
					const double error = shown[idA] - planned[idA];
					r.minError = std::min(r.minError, error);
					r.maxError = std::max(r.maxError, error);
				}
			}
			for (size_t k = 0; k < shown.size(); ++k)
				if (shown[k] > planned[k] + 0.5 * Pd)
					++r.late;
			r.frames = frames;
			r.presents = static_cast<int>(shown.size());
			return r;
		}

		// N for an average of `load` of P_s at the full level (FrameCostMs).
		SimConfig AtLoad(double sourceHz, double displayHz, double load, double extraMs, double estimateMs)
		{
			SimConfig c;
			c.sourceHz = sourceHz;
			c.displayHz = displayHz;
			c.extraMs = extraMs;
			c.estimateMs = estimateMs;
			const double ratio = displayHz / sourceHz;
			const double extras = ExtrasPerFrame(ratio, 0);
			c.newFrameMs = load * 1000.0 / sourceHz - (extras > 0.0 ? estimateMs : 0.0) - extras * extraMs;
			GateInput g;
			g.sourcePeriod = 1.0 / sourceHz;
			g.displayPeriod = 1.0 / displayHz;
			g.costs.newFrameMs = c.newFrameMs;
			g.costs.extraMs = extraMs;
			g.costs.estimateMs = estimateMs;
			Assert::AreEqual(load * 1000.0 / sourceHz, FrameCostMs(g, 0), 1e-9);
			return c;
		}

		// --- Look-ahead walk queues ----------------------------------------------

		struct QueuedFrame
		{
			bool cadenceRepeat = false;
		};
	}

	TEST_CLASS(RenderAheadPlannerTests)
	{
	public:
		// Chained frames (B's first vsync = A's nextFirstVsync, film time += P_s, the
		// latency floor fixed) on every source/display pair with at least one refresh
		// per frame: each frame owns floor or ceil of the ratio, the average is the
		// ratio, phases are in [0,1), even cadences shift by exactly k/N, every render
		// slot's shift is s*delta and no interval exceeds DXGI's 4.
		TEST_METHOD(ChainedRefreshCountsAndPhases)
		{
			int pairs = 0;
			for (double sourceHz : kSources)
				for (double displayHz : kDisplays)
				{
					const double ratio = displayHz / sourceHz;
					if (ratio < 1.0 - 1e-9)
						continue;   // below one refresh per frame: the queue trim drops frames, not the planner
					++pairs;
					const std::wstring what = Name(sourceHz, displayHz);
					const double Ps = 1.0 / sourceHz, Pd = 1.0 / displayHz, L = 0.050, film0 = 1000.0;
					FrameInput in = Input(sourceHz, displayHz, film0, 0.0, L);
					const int lo = static_cast<int>(std::floor(ratio + 1e-9));
					const int hi = static_cast<int>(std::ceil(ratio - 1e-9));
					const int even = in.evenCycle;
					// A steady chain: A's own refresh 0.37 refresh after phase 0.
					in.firstVsync = film0 + L + in.lead + 0.37 * Pd;
					const int frames = 20000;
					long long total = 0;
					for (int i = 0; i < frames; ++i)
					{
						in.filmTime = film0 + i * Ps;
						const FramePlan plan = PlanFrame(in);
						Assert::IsTrue(plan.valid, what.c_str());
						Assert::IsTrue(plan.refreshes == lo || plan.refreshes == hi, (what + L": refreshes").c_str());
						Assert::AreEqual(static_cast<size_t>(plan.refreshes - 1), plan.slots.size(), what.c_str());
						// A phase within the planner's 1e-9 of 1 belongs to B (phase 0).
						Assert::IsTrue(plan.phi0 >= -1e-8 && plan.phi0 < Pd / Ps + 1e-9, (what + L": phi0").c_str());
						double previous = plan.phi0;
						for (size_t k = 0; k < plan.slots.size(); ++k)
						{
							const Slot& slot = plan.slots[k];
							Assert::AreEqual(in.firstVsync + (k + 1) * Pd, slot.vsync, 1e-9, what.c_str());
							Assert::IsTrue(slot.delta > previous && slot.delta < 1.0, (what + L": phase").c_str());
							previous = slot.delta;
							if (even > 0)
								Assert::AreEqual(static_cast<float>(1.0 * (k + 1) / even), slot.shift, (what + L": k/N").c_str());
							else
								Assert::AreEqual(static_cast<float>(slot.delta), slot.shift, 1e-6f, (what + L": s*delta").c_str());
						}
						Assert::IsTrue(plan.rendered <= in.maxExtras, what.c_str());
						if (plan.refreshes - 1 <= in.maxExtras)
							Assert::AreEqual(plan.refreshes - 1, plan.rendered, (what + L": every owned refresh renders").c_str());
						Assert::AreEqual(0, plan.dropped, what.c_str());
						CheckIntervals(plan, what);
						total += plan.refreshes;
						in.firstVsync = plan.nextFirstVsync;
					}
					Assert::AreEqual(frames * ratio, static_cast<double>(total), 1.0 + 1e-6, (what + L": average").c_str());
				}
			Assert::IsTrue(pairs >= 50);
		}

		// Below one refresh per source frame each frame owns its own refresh only.
		TEST_METHOD(BelowOneRefreshPerFrameOwnsOneRefresh)
		{
			for (double sourceHz : kSources)
				for (double displayHz : kDisplays)
				{
					if (displayHz / sourceHz >= 1.0 - 1e-9)
						continue;
					FrameInput in = Input(sourceHz, displayHz, 10.0, 10.0 + 0.05 + 0.3 / displayHz, 0.05);
					const FramePlan plan = PlanFrame(in);
					Assert::IsTrue(plan.valid);
					Assert::AreEqual(1, plan.refreshes, Name(sourceHz, displayHz).c_str());
					Assert::IsTrue(plan.slots.empty());
					Assert::AreEqual(1, plan.nextInterval);
				}
		}

		// A restart: A's own present lands at film time + L (the floor measured
		// without the lead), so the lead is shown as holds of A (phase <= 0, shift 0,
		// re-presented where four holds in a row would need SyncInterval 5); B then
		// continues the steady chain at once.
		TEST_METHOD(RestartLeadAppearsAsHolds)
		{
			for (double sourceHz : kSources)
				for (double displayHz : kDisplays)
				{
					const double ratio = displayHz / sourceHz;
					if (ratio < 1.0 - 1e-9)
						continue;
					const std::wstring what = Name(sourceHz, displayHz);
					const double Ps = 1.0 / sourceHz, Pd = 1.0 / displayHz, L = 0.030, film0 = 50.0;
					FrameInput in = Input(sourceHz, displayHz, film0, film0 + L, L);
					const int leadRefreshes = static_cast<int>(std::round(in.lead / Pd));
					const FramePlan a = PlanFrame(in);
					Assert::IsTrue(a.valid, what.c_str());
					int holds = 0;
					for (const Slot& slot : a.slots)
					{
						if (slot.vsync - film0 - L - in.lead > 1e-9)
							continue;
						// Before or at A's time: no warp (a hold, or A again).
						Assert::AreEqual(0.0f, slot.shift, (what + L": lead slot shift").c_str());
						++holds;
					}
					Assert::AreEqual(leadRefreshes, holds, (what + L": lead holds").c_str());
					CheckIntervals(a, what);
					in.filmTime = film0 + Ps;
					in.firstVsync = a.nextFirstVsync;
					const FramePlan b = PlanFrame(in);
					Assert::IsTrue(b.phi0 >= -1e-9 && b.phi0 < Pd / Ps + 1e-9, (what + L": B continues the chain").c_str());
					CheckIntervals(b, what);
				}
		}

		// The drops (lateness and the ladder): the smallest s*delta first, and only
		// while every present still fits in SyncInterval 4.
		TEST_METHOD(DropsTakeTheSmallestShiftFirst)
		{
			// 24p on 60 Hz, a frame owning three refreshes (phases phi0, +0.4, +0.8).
			FrameInput in = Input(24.0, 60.0, 0.0, 0.0, 0.0);
			in.lead = 0.0;
			in.firstVsync = 0.1 / 24.0;
			FramePlan full = PlanFrame(in);
			Assert::AreEqual(3, full.refreshes);
			Assert::AreEqual(2, full.rendered);
			in.dropExtras = 1;
			FramePlan one = PlanFrame(in);
			Assert::AreEqual(1, one.dropped);
			Assert::IsTrue(one.slots[0].dropped && !one.slots[0].render, L"the smaller shift goes");
			Assert::IsTrue(one.slots[1].render);
			Assert::AreEqual(2, one.slots[1].interval);
			Assert::AreEqual(1, one.nextInterval);
			in.dropExtras = 16;   // the gate's hold
			FramePlan hold = PlanFrame(in);
			Assert::AreEqual(2, hold.dropped);
			Assert::AreEqual(0, hold.rendered);
			Assert::AreEqual(3, hold.nextInterval);

			// 24p on 144 Hz (five extras at k/6): the fourth drop cannot take slot 4
			// (five refreshes without a present), so it takes slot 5; a fifth fits nowhere.
			FrameInput six = Input(24.0, 144.0, 0.0, 0.0, 0.0);
			six.lead = 0.0;
			six.firstVsync = 0.1 / 144.0;
			six.dropExtras = 5;
			FramePlan p = PlanFrame(six);
			Assert::AreEqual(6, p.refreshes);
			Assert::AreEqual(4, p.dropped);
			Assert::IsTrue(p.slots[0].dropped && p.slots[1].dropped && p.slots[2].dropped && p.slots[4].dropped);
			Assert::IsTrue(p.slots[3].render);
			Assert::AreEqual(4, p.slots[3].interval);
			Assert::AreEqual(2, p.nextInterval);
			CheckIntervals(p, L"24 on 144 drop 5");
		}

		TEST_METHOD(SwapchainDepthLeadAndQueueCredit)
		{
			Assert::AreEqual(3, SwapchainBuffers(1.0));
			Assert::AreEqual(3, SwapchainBuffers(1.001));
			Assert::AreEqual(4, SwapchainBuffers(2.0));
			Assert::AreEqual(5, SwapchainBuffers(2.5));
			Assert::AreEqual(6, SwapchainBuffers(6.0));
			Assert::AreEqual(6, SwapchainBuffers(0.0));
			Assert::AreEqual(6, SwapchainBuffers(std::numeric_limits<double>::infinity()));
			Assert::AreEqual(0, MaxExtras(1.0));
			Assert::AreEqual(0, MaxExtras(1.001));
			Assert::AreEqual(1, MaxExtras(1.25));
			Assert::AreEqual(1, MaxExtras(2.0));
			Assert::AreEqual(2, MaxExtras(2.5));
			Assert::AreEqual(5, MaxExtras(6.0));
			Assert::AreEqual(2.0 / 60.0, PlannedLead(1.0 / 24.0, 1.0 / 60.0), 1e-12);
			Assert::AreEqual(0.0, PlannedLead(1.0 / 60.0, 1.0 / 60.0), 1e-12);
			Assert::AreEqual<size_t>(1, QueueCredit(2.0 / 60.0, 1.0 / 24.0, 6));
			Assert::AreEqual<size_t>(0, QueueCredit(0.0, 1.0 / 24.0, 6));
			Assert::AreEqual<size_t>(0, QueueCredit(2.0 / 60.0, 1.0 / 24.0, 2));
			Assert::AreEqual<size_t>(1, QueueCredit(0.5, 1.0 / 24.0, 3), L"never below 2 queued frames");
		}
	};

	TEST_CLASS(RenderAheadScheduleTests)
	{
	public:
		// A present shown a refresh late: Observe reports it, the chain and the
		// pending presents move by a refresh, TakeLateDrops asks one drop of the next
		// frame (once), and that frame drops its smallest s*delta.
		TEST_METHOD(LateFrameDropsOneExtra)
		{
			const double Pd = 1.0 / 60.0, grid = 0.995;
			Schedule schedule;
			bool restart = false, behind = false;
			const double v = schedule.FirstVsync(1.0, Pd, grid, restart, behind);
			Assert::IsTrue(restart && !behind);
			Assert::AreEqual(grid + Pd, v, 1e-12, L"on the vsync grid");
			schedule.Commit(v + 3 * Pd, { { 1u, v }, { 2u, v + Pd }, { 3u, v + 2 * Pd } });
			Assert::IsFalse(schedule.Observe(1u, v, Pd));
			Assert::IsFalse(schedule.Observe(2u, v + Pd + 0.4 * Pd, Pd), L"under half a refresh is on time");
			Assert::AreEqual(0, schedule.TakeLateDrops());
			Assert::IsTrue(schedule.Observe(3u, v + 3 * Pd, Pd));
			Assert::AreEqual(v + 4 * Pd, schedule.NextVsync(), 1e-12, L"the chain moves by the lateness");
			Assert::AreEqual(1, schedule.TakeLateDrops());
			Assert::AreEqual(0, schedule.TakeLateDrops(), L"consumed");

			// The next frame (24p on 60 Hz, three refreshes) drops one extra: the smaller.
			FrameInput in = Input(24.0, 60.0, 0.0, 0.0, 0.0);
			in.lead = 0.0;
			in.firstVsync = 0.1 / 24.0;
			in.dropExtras = 1;
			const FramePlan plan = PlanFrame(in);
			Assert::AreEqual(1, plan.dropped);
			Assert::AreEqual(1, plan.rendered);
			Assert::IsTrue(plan.slots[0].dropped);
			Assert::IsTrue(plan.slots[0].shift < plan.slots[1].shift);
		}

		TEST_METHOD(SkippedIdsAndLaterPendingMoveWithTheChain)
		{
			const double Pd = 1.0 / 60.0;
			Schedule schedule;
			schedule.Commit(1.0, { { 10u, 0.9 }, { 11u, 0.9 + Pd }, { 12u, 0.9 + 2 * Pd } });
			Assert::IsTrue(schedule.Observe(10u, 0.9 + 2 * Pd, Pd), L"two refreshes late");
			Assert::AreEqual(1.0 + 2 * Pd, schedule.NextVsync(), 1e-12);
			// 11 now expected at 0.9 + 3 Pd: on time there.
			Assert::IsFalse(schedule.Observe(11u, 0.9 + 3 * Pd, Pd));
			Assert::AreEqual<size_t>(1, schedule.Pending());
			// An id DXGI skipped (12 never reported) is dropped when 13 is seen.
			schedule.Commit(1.1, { { 13u, 1.05 } });
			Assert::IsFalse(schedule.Observe(13u, 1.05, Pd));
			Assert::AreEqual<size_t>(0, schedule.Pending());
		}

		TEST_METHOD(FirstVsyncChainBehindAndBreak)
		{
			const double Pd = 1.0 / 50.0;
			Schedule schedule;
			schedule.Commit(2.0, {});
			bool restart = true, behind = true;
			Assert::AreEqual(2.0, schedule.FirstVsync(1.97, Pd, 1.0, restart, behind));
			Assert::IsTrue(!restart && !behind, L"ahead of the chain: the chain");
			Assert::AreEqual(2.0, schedule.FirstVsync(2.0 + 0.2 * Pd, Pd, 1.0, restart, behind));
			Assert::IsTrue(!restart && !behind, L"within a quarter refresh: still the chain");
			const double v = schedule.FirstVsync(2.0 + 0.3 * Pd, Pd, 1.0, restart, behind);
			Assert::IsTrue(restart && behind, L"the chain's refresh has passed: late");
			Assert::AreEqual(2.0 + Pd, v, 1e-9, L"the next refresh on the grid");
			schedule.Break();
			Assert::IsFalse(schedule.Active());
			schedule.FirstVsync(5.0, Pd, 1.0, restart, behind);
			Assert::IsTrue(restart && !behind, L"a break restarts without a late");
		}

		// Open loop (no statistics, gridVsync 0): the chain is never corrected, but it
		// stays stable - one restart, no late reports, and the real vsync of each of
		// A's presents keeps one constant offset from the chain (no drift).
		TEST_METHOD(OpenLoopKeepsAStableChain)
		{
			const double rates[][2] = { { 24.0, 60.0 }, { 24.0, 30.0 }, { 30.0, 60.0 },
				{ 24000.0 / 1001.0, 60000.0 / 1001.0 }, { 25.0, 50.0 }, { 24.0, 144.0 } };
			for (const auto& rate : rates)
			{
				SimConfig c = AtLoad(rate[0], rate[1], 0.70, 4.0, 1.0);
				c.openLoop = true;
				c.seconds = 120.0;
				c.seed = 7;
				const SimResult r = Simulate(c);
				const std::wstring what = Name(rate[0], rate[1]);
				Assert::AreEqual(1, r.restarts, (what + L": restarts").c_str());
				Assert::AreEqual(0, r.lateObserved, what.c_str());
				Assert::AreEqual(0, r.dropped, what.c_str());
				Assert::IsTrue(r.maxError - r.minError < 1e-6, (what + L": constant offset").c_str());
				Assert::IsTrue(r.minError >= -1e-9 && r.maxError < 1.0 / rate[1], (what + L": within a refresh").c_str());
			}
		}
	};

	TEST_CLASS(RenderAheadGateTests)
	{
	public:
		TEST_METHOD(ResolveGateAndRefreshRule)
		{
			Assert::IsTrue(ResolveGate("period", false) == GateMode::Refresh, L"period only with render_ahead");
			Assert::IsTrue(ResolveGate("refresh", false) == GateMode::Refresh);
			Assert::IsTrue(ResolveGate("refresh", true) == GateMode::Refresh);
			Assert::IsTrue(ResolveGate("period", true) == GateMode::Period);
			Assert::IsTrue(ResolveGate("", true) == GateMode::Period);
			// Exactly the rule before render-ahead: workMs + 4 > the refresh in ms.
			for (double hz : kDisplays)
				for (double work = 0.0; work < 40.0; work += 0.01)
					Assert::AreEqual(work + 4.0 > 1000.0 * (1.0 / hz), RefreshGateHolds(work, 1.0 / hz));
			Assert::IsFalse(RefreshGateHolds(12.66, 1.0 / 60.0));
			Assert::IsTrue(RefreshGateHolds(12.67, 1.0 / 60.0));
		}

		TEST_METHOD(CostWindowP95)
		{
			CostWindow w;
			Assert::AreEqual(0.0, w.P95());
			for (int i = 1; i <= 100; ++i)
				w.Add(i * 0.01, static_cast<double>(i));
			Assert::AreEqual(95.0, w.P95());
			w.Add(1.0, -1.0);
			w.Add(1.0, std::numeric_limits<double>::quiet_NaN());
			Assert::AreEqual<size_t>(100, w.Count(), L"invalid samples are ignored");
			w.Add(5.505, 1.0);   // the first 50 are older than 5 s now
			Assert::AreEqual<size_t>(51, w.Count());
			Assert::AreEqual(98.0, w.P95(), L"{1, 51..100}: the 49th of 51");
		}

		// Laptop numbers (design): 24p on 60 Hz, N 27, X 7, E 2.
		TEST_METHOD(BudgetsPerSourcePeriod)
		{
			GateInput g;
			g.sourcePeriod = 1.0 / 24.0;
			g.displayPeriod = 1.0 / 60.0;
			g.costs.newFrameMs = 27.0;
			g.costs.extraMs = 7.0;
			g.costs.estimateMs = 2.0;
			Assert::AreEqual(1.5, ExtrasPerFrame(2.5, 0), 1e-12);
			Assert::AreEqual(0.5, ExtrasPerFrame(2.5, 1), 1e-12);
			Assert::AreEqual(0.0, ExtrasPerFrame(2.5, 2), 1e-12);
			Assert::AreEqual(1.0, ExtrasPerFrame(2.0, 0), 1e-12);
			Assert::AreEqual(0.25, ExtrasPerFrame(1.25, 0), 1e-12);
			Assert::AreEqual(0.0, ExtrasPerFrame(1.0, 0), 1e-12);
			Assert::AreEqual(27.0 + 2.0 + 1.5 * 7.0, FrameCostMs(g, 0), 1e-9);
			Assert::AreEqual(27.0 + 2.0 + 0.5 * 7.0, FrameCostMs(g, 2), 1e-9);
			Assert::AreEqual(27.0, FrameCostMs(g, HoldLevel(2)), 1e-9, L"no extras: no estimate either");
			Assert::IsFalse(Fits(g, 0, 0.85), L"40.5 ms > 0.85 x 41.7 ms");
			Assert::IsTrue(Fits(g, 2, 0.85));
			Assert::IsTrue(Fits(g, HoldLevel(2), 0.85));
			// L1 with a known cheaper extra.
			g.costs.extraCheaperMs = 4.0;
			Assert::AreEqual(27.0 + 2.0 + 1.5 * 4.0, FrameCostMs(g, 1), 1e-9);
			Assert::AreEqual(27.0 + 2.0 + 1.5 * 7.0, FrameCostMs(g, 0), 1e-9, L"L0 runs the extra as it is");
			Assert::IsFalse(Fits(g, 1, 0.85));   // 36 ms > 35.4
			Assert::IsTrue(Fits(g, 1, 0.90));
			// The single-frame check: N + X n <= (n + Q - 1) P_d, even with the average met.
			GateInput s = g;
			s.costs = GateCosts();
			s.costs.newFrameMs = 33.0;
			s.costs.extraMs = 0.5;
			s.queueDepth = 1;   // (2 + 1 - 1) x 16.7 = 33.3 < 33 + 2 x 0.5
			Assert::IsTrue(FrameCostMs(s, 0) + s.marginMs <= 0.85 * 1000.0 * s.sourcePeriod);
			Assert::IsFalse(Fits(s, 0, 0.85));
			s.queueDepth = 3;
			Assert::IsTrue(Fits(s, 0, 0.85));
			GateInput none = g;
			none.displayPeriod = 0.0;
			Assert::IsFalse(Fits(none, 0, 0.85));
		}

		TEST_METHOD(LadderOrder)
		{
			Assert::AreEqual(1, Gate::Down(0, true, 2));
			Assert::AreEqual(2, Gate::Down(0, false, 2), L"L1 skipped when not available");
			Assert::AreEqual(2, Gate::Down(1, true, 2));
			Assert::AreEqual(3, Gate::Down(2, true, 2));
			Assert::AreEqual(3, Gate::Down(3, true, 2), L"hold is the bottom");
			Assert::AreEqual(2, Gate::Down(0, false, 1), L"one extra: drop 1 = hold");
			Assert::AreEqual(2, HoldLevel(1));
			Assert::AreEqual(1, HoldLevel(0));
			Assert::AreEqual(2, Gate::Up(3, true));
			Assert::AreEqual(1, Gate::Up(2, true));
			Assert::AreEqual(0, Gate::Up(2, false), L"L1 skipped going up too");
			Assert::AreEqual(0, Gate::Up(1, true));
			Assert::AreEqual(0, Gate::Up(0, true));
			Assert::AreEqual(0, DropsAt(0));
			Assert::AreEqual(0, DropsAt(1));
			Assert::AreEqual(2, DropsAt(3));
			Assert::AreEqual(std::string("full"), LevelName(0, 2));
			Assert::AreEqual(std::string("hold"), LevelName(3, 2));
			Assert::AreEqual(std::string("drop 1 extra(s) per frame"), LevelName(2, 2));
			Assert::IsTrue(LevelName(1, 2).find("L1") == 0);
		}

		// Down after 2 s over budget (one step per 2 s), at once on a late frame; up
		// after 5 s under u - 0.10 and no sooner than 10 s after the last step down.
		TEST_METHOD(Hysteresis)
		{
			GateInput heavy;
			heavy.sourcePeriod = 1.0 / 24.0;
			heavy.displayPeriod = 1.0 / 60.0;
			heavy.costs.newFrameMs = 27.0;
			heavy.costs.extraMs = 7.0;
			heavy.costs.estimateMs = 2.0;
			GateInput light = heavy;
			light.costs.newFrameMs = 20.0;
			light.costs.extraMs = 4.0;
			light.costs.estimateMs = 1.0;
			auto at = [](int tenths) { return tenths / 10.0; };

			Gate gate;
			gate.Reset();
			for (int t = 0; t < 20; ++t)
				Assert::IsFalse(gate.Update(at(t), heavy, false).changed);
			GateChange down = gate.Update(at(20), heavy, false);
			Assert::IsTrue(down.changed);
			Assert::AreEqual(0, down.from);
			Assert::AreEqual(2, down.to, L"L1 not available: drop one");
			Assert::AreEqual(std::string("over budget for 2 s"), std::string(down.why));
			// Level 2 fits; the light load from 2.1 s: up only at 12.0 s (10 s after the
			// step down), although it has been under for 5 s since 7.1 s.
			for (int t = 21; t < 120; ++t)
				Assert::IsFalse(gate.Update(at(t), light, false).changed, std::to_wstring(t).c_str());
			GateChange up = gate.Update(at(120), light, false);
			Assert::IsTrue(up.changed);
			Assert::AreEqual(0, up.to);
			Assert::AreEqual(std::string("under budget for 5 s"), std::string(up.why));

			// Up waits 5 s under even long after the last step down.
			gate.Reset();
			GateChange late = gate.Update(0.0, light, true);
			Assert::IsTrue(late.changed);
			Assert::AreEqual(2, late.to);
			Assert::AreEqual(std::string("late frame"), std::string(late.why));
			for (int t = 1; t < 150; ++t)
				Assert::IsFalse(gate.Update(at(t), heavy, false).changed, L"level 2 fits the heavy load");
			for (int t = 150; t < 200; ++t)
				Assert::IsFalse(gate.Update(at(t), light, false).changed);
			Assert::IsTrue(gate.Update(at(200), light, false).changed, L"5 s under, 20 s after the down");

			// The whole ladder with L1: one step per 2 s, then hold.
			GateInput overload = heavy;
			overload.l1Available = true;
			overload.costs.newFrameMs = 60.0;
			gate.Reset();
			std::vector<int> levels;
			int lastStep = -100;
			for (int t = 0; t <= 100; ++t)
			{
				const GateChange c = gate.Update(at(t), overload, false);
				if (c.changed)
				{
					levels.push_back(c.to);
					// The over-budget clock starts again on the update after a step.
					Assert::IsTrue(levels.size() == 1 ? t == 20 : t - lastStep >= 20 && t - lastStep <= 22,
						L"a step every 2 s");
					lastStep = t;
				}
			}
			Assert::AreEqual<size_t>(3, levels.size());
			Assert::AreEqual(1, levels[0]);
			Assert::AreEqual(2, levels[1]);
			Assert::AreEqual(3, levels[2]);
			Assert::AreEqual(3, gate.Level());
			Assert::IsFalse(gate.Update(at(101), overload, true).changed, L"a late frame at hold changes nothing");
			// The cadence changes to one extra (24p on 30 Hz): the level clamps to its hold.
			GateInput thirty = overload;
			thirty.displayPeriod = 1.0 / 30.0;
			gate.Update(at(102), thirty, false);
			Assert::AreEqual(HoldLevel(1), gate.Level());
		}
	};

	TEST_CLASS(RenderAheadSchedulerSimulationTests)
	{
	public:
		// GPU work at 85% of the source period on average, +-30% jitter: no late frame
		// in ten minutes.
		TEST_METHOD(NoLateFramesAt85Percent)
		{
			const double rates[][2] = { { 24.0, 60.0 }, { 24.0, 30.0 }, { 30.0, 60.0 } };
			for (const auto& rate : rates)
				for (unsigned seed : { 1u, 2u, 3u })
				{
					SimConfig c = AtLoad(rate[0], rate[1], 0.85, 6.0, 1.5);
					c.seed = seed;
					const SimResult r = Simulate(c);
					const std::wstring what = Name(rate[0], rate[1]) + L" seed " + std::to_wstring(seed);
					Logger::WriteMessage((what + L": frames " + std::to_wstring(r.frames) + L", presents " +
						std::to_wstring(r.presents) + L", extras " + std::to_wstring(r.extras) + L", late " +
						std::to_wstring(r.late) + L"\n").c_str());
					Assert::AreEqual(0, r.late, (what + L": late").c_str());
					Assert::AreEqual(0, r.lateObserved, (what + L": observed late").c_str());
					Assert::AreEqual(0, r.dropped, (what + L": dropped").c_str());
					Assert::AreEqual(1, r.restarts, (what + L": restarts").c_str());
					const double ratio = rate[1] / rate[0];
					Assert::AreEqual(ExtrasPerFrame(ratio, 0), static_cast<double>(r.extras) / r.frames, 0.01,
						(what + L": full de-judder").c_str());
				}
		}

		// The simulation is not vacuous: overloaded (120%), frames are late, the
		// schedule sees it and extras are dropped.
		TEST_METHOD(OverloadIsDetected)
		{
			SimConfig c = AtLoad(24.0, 60.0, 1.20, 6.0, 1.5);
			c.seconds = 60.0;
			const SimResult r = Simulate(c);
			Assert::IsTrue(r.late > 0);
			Assert::IsTrue(r.lateObserved > 0);
			Assert::IsTrue(r.dropped > 0);
		}
	};

	TEST_CLASS(RenderAheadLookaheadWalkTests)
	{
	public:
		TEST_METHOD(LedDeeperThanActivePictureWalksMore)
		{
			const std::deque<QueuedFrame> queue(10);
			const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 2, 5, 8);
			Assert::AreEqual<size_t>(6, walk.window.indices.size(), L"current + 5 for the LED");
			Assert::AreEqual<size_t>(5, walk.window.effectiveFutureFrames);
			Assert::AreEqual<size_t>(9, walk.window.availableFutureFrames);
			Assert::AreEqual<size_t>(3, walk.activePictureFrames, L"the active picture keeps current + 2");
			for (size_t i = 0; i < walk.window.indices.size(); ++i)
				Assert::AreEqual(i, walk.window.indices[i]);
			// Beyond the active picture's own cap.
			const auto deep = AlphaQueuePolicy::SelectLookaheadWalk(queue, 20, 9, 8);
			Assert::AreEqual<size_t>(10, deep.window.indices.size());
			Assert::AreEqual<size_t>(9, deep.activePictureFrames, L"active picture capped at 8 future frames");
		}

		TEST_METHOD(LedAloneWithActivePictureOff)
		{
			const std::deque<QueuedFrame> queue(10);
			const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, 4, 8);
			Assert::AreEqual<size_t>(5, walk.window.indices.size());
			Assert::AreEqual<size_t>(0, walk.activePictureFrames);
			const auto none = AlphaQueuePolicy::SelectLookaheadWalk(queue, 0, 0, 8);
			Assert::IsTrue(none.window.indices.empty());
			Assert::AreEqual<size_t>(0, none.activePictureFrames);
		}

		TEST_METHOD(IdenticalToPreviewWhenLedNotDeeper)
		{
			std::deque<QueuedFrame> queue(12);
			queue[3].cadenceRepeat = true;
			queue[7].cadenceRepeat = true;
			for (size_t ap = 0; ap <= 10; ++ap)
				for (size_t led = 0; led <= std::min<size_t>(ap, 8); ++led)
				{
					const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, ap, led, 8);
					const auto preview = AlphaQueuePolicy::SelectActivePicturePreview(queue, ap, 8);
					Assert::AreEqual(preview.availableFutureFrames, walk.window.availableFutureFrames);
					Assert::AreEqual(preview.effectiveFutureFrames, walk.window.effectiveFutureFrames);
					Assert::IsTrue(preview.indices == walk.window.indices);
					Assert::AreEqual(preview.indices.size(), walk.activePictureFrames);
				}
		}

		TEST_METHOD(BoundedByQueuedFramesAndSkipsRepeats)
		{
			const std::deque<QueuedFrame> shortQueue(3);
			const auto bounded = AlphaQueuePolicy::SelectLookaheadWalk(shortQueue, 1, 6, 8);
			Assert::AreEqual<size_t>(3, bounded.window.indices.size());
			Assert::AreEqual<size_t>(2, bounded.window.availableFutureFrames);
			Assert::AreEqual<size_t>(2, bounded.activePictureFrames);

			const std::deque<QueuedFrame> queue = { { false }, { true }, { false }, { true }, { false }, { false }, { true }, { false } };
			const auto walk = AlphaQueuePolicy::SelectLookaheadWalk(queue, 1, 3, 8);
			Assert::AreEqual<size_t>(4, walk.window.indices.size());
			Assert::AreEqual<size_t>(0, walk.window.indices[0]);
			Assert::AreEqual<size_t>(2, walk.window.indices[1]);
			Assert::AreEqual<size_t>(4, walk.window.indices[2]);
			Assert::AreEqual<size_t>(5, walk.window.indices[3]);
			Assert::AreEqual<size_t>(4, walk.window.availableFutureFrames);
			Assert::AreEqual<size_t>(2, walk.activePictureFrames);

			std::deque<QueuedFrame> repeatedCurrent(6);
			repeatedCurrent.front().cadenceRepeat = true;
			const auto skipped = AlphaQueuePolicy::SelectLookaheadWalk(repeatedCurrent, 2, 4, 8);
			Assert::IsTrue(skipped.window.indices.empty(), L"a repeated current frame schedules no work");
			Assert::AreEqual<size_t>(0, skipped.activePictureFrames);
		}
	};

	TEST_CLASS(RenderAheadGapFillTests)
	{
	public:
		TEST_METHOD(GapFillPairRules)
		{
			using InterpolationTimeline::GapFillPair;
			for (uint32_t gap = 1; gap < static_cast<uint32_t>(InterpolationTimeline::kMaxSpanFrames); ++gap)
				Assert::IsTrue(GapFillPair(true, true, gap, true, false));
			Assert::IsFalse(GapFillPair(true, true, 0, true, false), L"a flush");
			Assert::IsFalse(GapFillPair(true, true, static_cast<uint32_t>(InterpolationTimeline::kMaxSpanFrames), true, false));
			Assert::IsFalse(GapFillPair(false, true, 1, true, false), L"no gap");
			Assert::IsFalse(GapFillPair(true, false, 1, true, false), L"interpolation_fill_gaps off");
			Assert::IsFalse(GapFillPair(true, true, 1, false, false), L"span out of range");
			Assert::IsFalse(GapFillPair(true, true, 1, true, true), L"low latency excluded");
		}

		// The pair is decided without interpolation; with interpolation off the
		// timeline presents it by itself, with interpolation on it is already presenting.
		TEST_METHOD(GapFillStandaloneRules)
		{
			using InterpolationTimeline::GapFillPair;
			using InterpolationTimeline::GapFillStandalone;
			const bool pair = GapFillPair(true, true, 2, true, false);
			Assert::IsTrue(GapFillStandalone(pair, false), L"interpolation off: standalone");
			Assert::IsFalse(GapFillStandalone(pair, true), L"interpolating already");
			Assert::IsFalse(GapFillStandalone(false, false));
			Assert::IsFalse(GapFillStandalone(GapFillPair(true, true, 2, true, true), false), L"low latency");
		}
	};

	TEST_CLASS(RenderAheadLedTests)
	{
	public:
		// The frame time is the predicted display time: a frame 100 ms ahead looks
		// 100 ms later to the module's frame clock (the period average moves by a
		// tenth of it); a constant lead keeps the wall clock's cadence; and the wall
		// clock bookkeeping (MsSinceLastFrame, the landing) is not moved.
		TEST_METHOD(BeginFrameAheadUsesThePredictedDisplayTime)
		{
			LedDrive::LedDriveModule module{ LedDrive::ProjectorConfig(), LedDrive::LedDriveConfig() };
			const double initial = module.FramePeriodMs();
			module.BeginFrame(1);
			module.BeginFrameAhead(2, 100.0);
			const double after = module.FramePeriodMs();
			// elapsed = 100 ms + the few microseconds between the calls
			Assert::AreEqual(initial + 0.1 * (100.0 - initial), after, 0.6);
			Assert::IsTrue(module.MsSinceLastFrame() >= 0.0 && module.MsSinceLastFrame() < 50.0,
				L"the wall clock is not moved ahead");
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			module.BeginFrameAhead(3, 100.0);
			const double steady = module.FramePeriodMs();
			// A constant lead keeps the wall cadence (about 30 ms elapsed, not 130).
			Assert::IsTrue(steady > after + 0.1 * (25.0 - after) && steady < after + 0.1 * (80.0 - after));
		}

		TEST_METHOD(NoAheadIsIdenticalToBeginFrame)
		{
			for (double ahead : { 0.0, -5.0, 1000.0, std::numeric_limits<double>::quiet_NaN() })
			{
				LedDrive::LedDriveModule plain{ LedDrive::ProjectorConfig(), LedDrive::LedDriveConfig() };
				LedDrive::LedDriveModule queued{ LedDrive::ProjectorConfig(), LedDrive::LedDriveConfig() };
				for (uint64_t sequence = 1; sequence <= 4; ++sequence)
				{
					const LedDrive::CompensationGain a = plain.BeginFrame(sequence, 24.0);
					const LedDrive::CompensationGain b = queued.BeginFrameAhead(sequence, ahead, 24.0);
					Assert::AreEqual(a.active, b.active);
					Assert::AreEqual(a.gain, b.gain);
					Assert::AreEqual(plain.FramePeriodMs(), queued.FramePeriodMs(), 1.0);
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
				}
				Assert::AreEqual(plain.SourceFramePeriodMs(), queued.SourceFramePeriodMs(), 1e-9);
			}
		}

		// The lead applies to its own frame only: a later BeginFrame is on the wall clock.
		TEST_METHOD(AheadDoesNotCarryToTheNextFrame)
		{
			LedDrive::LedDriveModule module{ LedDrive::ProjectorConfig(), LedDrive::LedDriveConfig() };
			module.BeginFrameAhead(1, 200.0);
			const double before = module.FramePeriodMs();
			module.BeginFrame(2);   // its time is now: before the previous frame's, elapsed clamps to 1 ms
			const double after = module.FramePeriodMs();
			Assert::AreEqual(before + 0.1 * (1.0 - before), after, 1e-6);
		}
	};
}
