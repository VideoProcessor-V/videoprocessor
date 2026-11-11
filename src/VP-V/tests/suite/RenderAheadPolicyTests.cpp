#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/drive/led_compensation/LedDriveModule.h>
#include <vprenderer/AlphaQueuePolicy.h>
#include <VP-V/modules/input/queue_timing/AlphaQueuePolicyVpCompat.h>
#include <VP-V/modules/input/queue_timing/VpFixedBudgetPolicy.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>
#include <VP-V/modules/motion/consumers/InterpolationTimeline.h>
#include <VP-V/modules/motion/estimation/MotionAhead.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>
#include <VP-V/modules/output/presentation/OpenLoopDisplayRate.h>
#include <DisplayRefreshRatePolicy.h>

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
		// FirstVsync, L at the anchor (AnchorLatency), TakeLateDrops, PlanFrame, Commit.
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
				// As the renderer: an anchor too late for a second is replaced (a healthy
				// chain never asks; counted as a restart so the assertions see it).
				if (!restart && schedule.AnchorTooLate(firstVsync, film, lead, Ps))
				{
					schedule.Break();
					restart = true;
				}
				const double floor = schedule.AnchorLatency(restart, firstVsync, film);
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
			// floor(lead / P_s) (bug hunt 05 M4): a 33 ms lead at 24/60 gives back no whole frame.
			Assert::AreEqual<size_t>(0, QueueCredit(2.0 / 60.0, 1.0 / 24.0, 6));
			Assert::AreEqual<size_t>(1, QueueCredit(1.0 / 24.0, 1.0 / 24.0, 6), L"a whole frame's lead");
			Assert::AreEqual<size_t>(1, QueueCredit(1.9 / 24.0, 1.0 / 24.0, 6));
			Assert::AreEqual<size_t>(0, QueueCredit(0.0, 1.0 / 24.0, 6));
			Assert::AreEqual<size_t>(0, QueueCredit(2.0 / 60.0, 1.0 / 24.0, 2));
			Assert::AreEqual<size_t>(1, QueueCredit(0.5, 1.0 / 24.0, 3), L"never below 2 queued frames");
		}
	};

	// Bug hunt 05 H1/H2/M3: the planner as the renderer drives it (FirstVsync, a restart's
	// Break, L at the anchor, PlanFrame, Commit; a hold-only frame committed when it fits)
	// over thousands of frames with deterministic arrival jitter, on measured-looking
	// rates. The average refreshes per frame is the ratio (H1: the floor ratcheted on
	// every inexact cadence), L stays bounded, and on a whole ratio no frame owns n+1 or
	// n-1 refreshes because of jitter (H2), hold-only frames included (M3).
	TEST_CLASS(RenderAheadJitterSimulationTests)
	{
	public:
		struct ChainResult
		{
			int frames = 0;
			long long refreshes = 0;   // chained frames' refreshes (restart frames excluded)
			int chained = 0;
			int restarts = 0;
			int holdsCommitted = 0;
			int holdsBroken = 0;
			int offByOne = 0;          // whole ratio: chained frames not owning exactly n
			double minL = 1e9, maxL = -1e9;
		};

		static ChainResult RunChain(double sourceHz, double displayHz, int frames, double jitter, int holdEvery,
			unsigned seed)
		{
			ChainResult r;
			const double Ps = 1.0 / sourceHz, Pd = 1.0 / displayHz;
			const double ratio = DejudderPolicy::CadenceRatio(sourceHz, displayHz);
			const double v0 = 0.37 * Pd;          // the display's vsync grid
			const double call = 0.4 * Ps;         // arrival to A's present returning
			const int maxExtras = DejudderPolicy::MaxExtras(ratio);
			const double lead = PlannedLead(Ps, Pd);
			const double maxAhead = lead + (static_cast<int>(kQueuedFrameLatency) + 1) *
				std::ceil(ratio - kTolerance) * Pd;
			const bool whole = std::abs(ratio - std::round(ratio)) < 1e-6;
			const int n = static_cast<int>(std::round(ratio));
			uint32_t state = seed;
			auto uniform = [&state]()
			{
				state = state * 1664525u + 1013904223u;   // deterministic LCG
				return static_cast<double>(state >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
			};
			Schedule schedule;
			for (int i = 0; i < frames; ++i)
			{
				const double film = 10.0 + i * Ps + jitter * uniform();
				const double now = film + call;
				bool restart = false, behind = false;
				const double first = schedule.FirstVsync(now, Pd, v0, restart, behind, maxAhead);
				if (restart)
				{
					schedule.Break();
					++r.restarts;
				}
				const double L = schedule.AnchorLatency(restart, first, film);
				r.minL = std::min(r.minL, L);
				r.maxL = std::max(r.maxL, L);
				const bool hold = holdEvery > 0 && i % holdEvery == holdEvery - 1;
				FrameInput in;
				in.filmTime = film;
				in.sourcePeriod = Ps;
				in.displayPeriod = Pd;
				in.firstVsync = first;
				in.latencyFloor = L;
				in.lead = lead;
				in.strength = hold ? 0.0 : 1.0;
				in.evenCycle = DejudderPolicy::EvenCycle(ratio);
				in.maxExtras = maxExtras;
				in.dropExtras = hold ? 0 : schedule.TakeLateDrops() + (behind ? 1 : 0);
				in.arrivalJitter = schedule.NoteArrival(film, Ps);
				const FramePlan plan = PlanFrame(in);
				Assert::IsTrue(plan.valid);
				if (hold)
				{
					// As the renderer: committed when nothing must be rendered, else the chain ends.
					if (plan.rendered == 0 && IntervalsFit(plan))
						++r.holdsCommitted;
					else
					{
						++r.holdsBroken;
						schedule.Break();
						continue;
					}
				}
				else
				{
					Assert::IsTrue(IntervalsFit(plan));
					if (!restart)
						Assert::IsTrue(plan.rendered <= maxExtras);
				}
				schedule.Commit(plan.nextFirstVsync, {});
				if (!restart)
				{
					++r.chained;
					r.refreshes += plan.refreshes;
					// After the jitter estimate's first second (it starts at kTolerance).
					if (whole && plan.refreshes != n && i >= static_cast<int>(sourceHz))
						++r.offByOne;
				}
			}
			r.frames = frames;
			return r;
		}

		TEST_METHOD(AverageIsTheRatioAndLatencyStaysBounded)
		{
			const double sources[] = { 24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0 };
			const double displays[] = { 50.0, 60000.0 / 1001.0, 60.0, 120.0, 144.0 };
			const double jitter = 0.0005;   // +-0.5 ms arrival jitter
			const int frames = 6000;
			for (int holds = 0; holds < 2; ++holds)
				for (double sourceHz : sources)
					for (double displayHz : displays)
					{
						const double ratio = displayHz / sourceHz;
						const double Pd = 1.0 / displayHz, Ps = 1.0 / sourceHz;
						const ChainResult r = RunChain(sourceHz, displayHz, frames, jitter, holds ? 37 : 0, 11u);
						const std::wstring what = Name(sourceHz, displayHz) + (holds ? L" (holds)" : L"");
						const double average = static_cast<double>(r.refreshes) / std::max(1, r.chained);
						wchar_t line[320];
						swprintf_s(line, L"%s: ratio %.6f average %.6f restarts %d L %.3f..%.3f ms off-by-one %d holds %d/%d",
							what.c_str(), ratio, average, r.restarts, r.minL * 1000.0, r.maxL * 1000.0, r.offByOne,
							r.holdsCommitted, r.holdsBroken);
						Logger::WriteMessage(line);
						Assert::IsTrue(r.chained > frames / 2, (what + L": chained").c_str());
						// The average: the ratio within a few refreshes over the whole run
						// (each restart moves the chain by at most the lead and a refresh).
						const double slack = (2.0 + ratio) * (1 + r.restarts + r.holdsBroken);
						Assert::AreEqual(ratio, average, slack / r.chained, (what + L": average").c_str());
						// L bounded: the anchor's latency (call + at most a refresh), however long the run.
						Assert::IsTrue(r.maxL - r.minL <= Pd + 2.0 * jitter + 1e-9, (what + L": L spread").c_str());
						Assert::IsTrue(r.maxL <= 0.4 * Ps + Pd + jitter + 1e-9, (what + L": L bound").c_str());
						if (holds == 0)
							Assert::AreEqual(1, r.restarts, (what + L": one anchor").c_str());
						// A whole ratio: every chained frame owns exactly n (H2).
						if (std::abs(ratio - std::round(ratio)) < 1e-6)
							Assert::AreEqual(0, r.offByOne, (what + L": n+-1 split").c_str());
						if (holds && std::ceil(ratio - 1e-9) <= kMaxSyncInterval)   // every frame fits a hold-only plan
							Assert::AreEqual(0, r.holdsBroken, (what + L": hold-only frames continue the chain").c_str());
					}
		}

		// A jitter larger than the tolerance still never ratchets: it may split a whole
		// ratio, but the average stays the ratio and L never moves without a restart.
		TEST_METHOD(LargeJitterDoesNotRatchet)
		{
			const double rates[][2] = { { 24000.0 / 1001.0, 60.0 }, { 30000.0 / 1001.0, 60.0 },
				{ 25.0, 60.0 }, { 24000.0 / 1001.0, 60000.0 / 1001.0 }, { 30.0, 60.0 } };
			for (const auto& rate : rates)
			{
				const ChainResult r = RunChain(rate[0], rate[1], 3000, 0.002, 0, 5u);
				const double ratio = rate[1] / rate[0];
				const double average = static_cast<double>(r.refreshes) / std::max(1, r.chained);
				const std::wstring what = Name(rate[0], rate[1]);
				Assert::AreEqual(ratio, average, (2.0 + ratio) * (1 + r.restarts) / r.chained, (what + L": average").c_str());
				Assert::IsTrue(r.maxL - r.minL <= 1.0 / rate[1] + 0.004 + 1e-9, (what + L": L bounded").c_str());
			}
		}

		// L: fixed at a restart, kept by chained frames and by corrections, re-anchored
		// by the next restart.
		TEST_METHOD(AnchorLatencyMovesOnlyAtARestart)
		{
			Schedule schedule;
			Assert::IsFalse(schedule.Anchored());
			Assert::AreEqual(0.025, schedule.AnchorLatency(false, 1.025, 1.0), 1e-12, L"the first frame anchors");
			Assert::AreEqual(0.025, schedule.AnchorLatency(false, 2.100, 2.0), 1e-12, L"chained: kept");
			// The first observation after a new anchor re-anchors only (bug hunt 05 M2).
			schedule.Commit(2.9, { { 1u, 2.8 } });
			Assert::IsFalse(schedule.Observe(1u, 2.8 + 2.0 / 60.0, 1.0 / 60.0), L"first after the anchor: re-anchor");
			Assert::AreEqual(0.025, schedule.AnchorLatency(false, 2.9 + 2.0 / 60.0, 2.8), 1e-12, L"a re-anchor keeps L");
			schedule.Commit(3.0, { { 2u, 2.9 } });
			Assert::IsTrue(schedule.Observe(2u, 2.9 + 2.0 / 60.0, 1.0 / 60.0), L"late: the chain moves");
			Assert::AreEqual(0.025, schedule.AnchorLatency(false, 3.0 + 2.0 / 60.0, 2.9), 1e-12, L"a correction keeps L");
			Assert::AreEqual(0.031, schedule.AnchorLatency(true, 4.031, 4.0), 1e-12, L"a restart re-anchors");
			// A re-anchor after a backlog flush takes the lead off: A's own refresh at phase 0.
			const double Ps = 1001.0 / 24000.0, Pd = 1.0 / 60.0, lead = PlannedLead(Ps, Pd);
			const double L = schedule.AnchorLatency(true, 5.2, 5.0, lead);
			Assert::AreEqual(0.2 - lead, L, 1e-12, L"without the lead");
			FrameInput in;
			in.filmTime = 5.0; in.sourcePeriod = Ps; in.displayPeriod = Pd; in.firstVsync = 5.2;
			in.latencyFloor = L; in.lead = lead; in.strength = 1.0; in.maxExtras = MaxExtras(Ps / Pd);
			const FramePlan plan = PlanFrame(in);
			Assert::AreEqual(0.0, plan.phi0, 1e-9, L"no lead hold");
			Assert::IsTrue(plan.slots.size() >= 1 && plan.slots[0].render, L"the first refresh after A's own is an extra");
			schedule.Reset();
			Assert::IsFalse(schedule.Anchored());
		}

		// H2 at the planner: a whole ratio's frame with phases moved by less than the
		// tolerance either way owns exactly n refreshes, its extras at k/N.
		TEST_METHOD(WholeRatioToleratesSmallPhaseShifts)
		{
			const double pairs[][2] = { { 30.0, 60.0 }, { 25.0, 50.0 }, { 30.0, 120.0 }, { 24.0, 120.0 },
				{ 60.0, 120.0 }, { 30000.0 / 1001.0, 60000.0 / 1001.0 } };
			for (const auto& pr : pairs)
			{
				const double Ps = 1.0 / pr[0], Pd = 1.0 / pr[1];
				const int n = static_cast<int>(std::round(Ps / Pd));
				for (double shift : { -0.9, -0.5, -0.1, 0.0, 0.1, 0.5, 0.9 })
				{
					FrameInput in = Input(pr[0], pr[1], 100.0, 0.0, 0.020);
					in.firstVsync = 100.0 + 0.020 + in.lead;
					in.filmTime = 100.0 + shift * kTolerance * Pd;   // within the tolerance
					const FramePlan plan = PlanFrame(in);
					const std::wstring what = Name(pr[0], pr[1]) + L" shift " + std::to_wstring(shift);
					Assert::AreEqual(n, plan.refreshes, what.c_str());
					Assert::IsTrue(plan.phi0 >= 0.0, (what + L": phi0 near 0 counts as 0").c_str());
					for (size_t k = 0; k < plan.slots.size(); ++k)
						if (plan.slots[k].render)
							Assert::AreEqual(static_cast<float>(1.0 * (k + 1) / n), plan.slots[k].shift, 1e-6f, (what + L": k/N").c_str());
					Assert::AreEqual(std::min(n - 1, in.maxExtras), plan.rendered, what.c_str());
				}
			}
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
			schedule.Commit(0.8, { { 9u, 0.85 } });
			Assert::IsFalse(schedule.Observe(9u, 0.85, Pd), L"the anchor, on time");
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

		// Render-ahead follow-up 2026-10-17: a present shown earlier than planned means the
		// chain ran ahead of the screen; it moves back (not late, no drop).
		TEST_METHOD(EarlyShownPresentMovesTheChainBack)
		{
			const double Pd = 1.0 / 60.0;
			Schedule schedule;
			schedule.Commit(1.0, { { 20u, 0.9 }, { 21u, 0.9 + Pd } });
			Assert::IsFalse(schedule.Observe(20u, 0.9 - 0.4 * Pd, Pd), L"under half a refresh early: no change");
			Assert::AreEqual(1.0, schedule.NextVsync(), 1e-12);
			Assert::IsFalse(schedule.Observe(21u, 0.9 - 2 * Pd, Pd), L"early is never late");
			Assert::AreEqual(1.0 - 3 * Pd, schedule.NextVsync(), 1e-12, L"the chain moves back by the difference");
			Assert::AreEqual(0, schedule.TakeLateDrops());
			Assert::AreEqual(1ull, schedule.EarlyCorrections());
		}

		// A chain further ahead than any queued present can land restarts (not behind).
		TEST_METHOD(FirstVsyncChainTooFarAheadRestarts)
		{
			const double Pd = 1.0 / 60.0;
			Schedule schedule;
			schedule.Commit(1.5, {});
			bool restart = false, behind = true;
			Assert::AreEqual(1.5, schedule.FirstVsync(1.0, Pd, 0.0, restart, behind));
			Assert::IsTrue(!restart && !behind, L"no bound: the chain");
			Assert::AreEqual(1.5, schedule.FirstVsync(1.0, Pd, 0.0, restart, behind, 0.6));
			Assert::IsTrue(!restart, L"within the bound: the chain");
			const double v = schedule.FirstVsync(1.0, Pd, 0.995, restart, behind, 0.2);
			Assert::IsTrue(restart && !behind, L"beyond the bound: a restart, not late");
			Assert::AreEqual(0.995 + Pd, v, 1e-12, L"on the vsync grid");
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
	// Render-ahead without DXGI frame statistics (follow-up 2026-10-17): the open-loop
	// display-rate source selection, its lateness by QPC and the general display bounds
	// (the DWM rate used to be accepted only in 20..120 Hz). Rates swept, none fixed.
	TEST_CLASS(RenderAheadOpenLoopRateTests)
	{
	public:
		static OpenLoopDisplayRate::Candidates All(double hz)
		{
			OpenLoopDisplayRate::Candidates c;
			c.displayConfigHz = hz;
			c.dwmHz = hz;
			c.hostHz = hz;
			c.modeIntegerHz = std::floor(hz);
			c.cadenceHz = hz;
			return c;
		}

		TEST_METHOD(StatisticsAlwaysWin)
		{
			for (bool renderAhead : { false, true })
			{
				OpenLoopDisplayRate::Candidates c = All(60.0);
				c.statisticsHz = 59.94;
				const OpenLoopDisplayRate::Choice choice = OpenLoopDisplayRate::Select(c, renderAhead);
				Assert::AreEqual(59.94, choice.hz);
				Assert::IsTrue(choice.source == OpenLoopDisplayRate::Source::Statistics);
				Assert::IsFalse(choice.openLoop, L"closed loop");
			}
		}

		// render_ahead off: no fallback, so the old "timing not known: display rate" hold stays.
		TEST_METHOD(RenderAheadOffKeepsTheHold)
		{
			const OpenLoopDisplayRate::Choice choice = OpenLoopDisplayRate::Select(All(60.0), false);
			Assert::AreEqual(0.0, choice.hz);
			Assert::IsTrue(choice.source == OpenLoopDisplayRate::Source::None);
			Assert::IsFalse(choice.openLoop);
		}

		TEST_METHOD(OrderWithoutStatistics)
		{
			using S = OpenLoopDisplayRate::Source;
			const double hz = 60000.0 / 1001.0;
			OpenLoopDisplayRate::Candidates c = All(hz);
			c.dwmHz = 59.95;   // DWM's measured period: close to, not exactly, the mode
			// The old order (queue_timing_fixes off): the whole-Hz mode before the cadence.
			auto pick = [&]() { return OpenLoopDisplayRate::Select(c, true, false); };
			Assert::IsTrue(pick().source == S::DisplayConfig, L"the monitor's exact mode first");
			Assert::AreEqual(hz, pick().hz);
			Assert::IsTrue(pick().openLoop);
			c.displayConfigHz = 0.0;
			Assert::IsTrue(pick().source == S::Dwm, L"then DWM (agrees with the whole-Hz mode 59 within 2%)");
			Assert::AreEqual(59.95, pick().hz);
			c.dwmHz = 0.0;
			Assert::IsTrue(pick().source == S::Host, L"then the application's rate");
			c.hostHz = 0.0;
			Assert::IsTrue(pick().source == S::ModeInteger, L"then the whole-Hz mode");
			Assert::AreEqual(59.0, pick().hz);
			c.modeIntegerHz = 0.0;
			Assert::IsTrue(pick().source == S::VBlankCadence, L"then the vblank cadence");
			c.cadenceHz = 0.0;
			Assert::IsTrue(pick().source == S::None, L"nothing: no rate (the hold)");
			Assert::AreEqual(0.0, pick().hz);
		}

		// Mixed-rate desktop: DWM at another monitor's rate is skipped while this monitor's
		// mode is known; without a mode DWM is taken as it is.
		TEST_METHOD(DwmAtAnotherMonitorsRateIsSkipped)
		{
			using S = OpenLoopDisplayRate::Source;
			OpenLoopDisplayRate::Candidates c;
			c.dwmHz = 144.0;
			c.modeIntegerHz = 60.0;
			c.hostHz = 60.0;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::Host);
			c.hostHz = 0.0;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::ModeInteger);
			c.modeIntegerHz = 0.0;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::Dwm, L"no mode to disagree with");
			c.displayConfigHz = 143.856;
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::DisplayConfig);
			c.displayConfigHz = 0.0;
			c.modeIntegerHz = 143.0;   // 144 vs 143: within 2%
			Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == S::Dwm);
		}

		// Every source alone, across the general display range (no rate is special).
		TEST_METHOD(EachSourceAloneAcrossTheRange)
		{
			using S = OpenLoopDisplayRate::Source;
			const double rates[] = { 10.0, 23.976, 24.0, 30.0, 48.0, 50.0, 59.94, 60.0, 75.0, 90.0, 100.0, 119.88, 120.0,
				144.0, 165.0, 240.0, 360.0, 480.0, 500.0 };
			for (double hz : rates)
			{
				for (int which = 0; which < 5; ++which)
				{
					OpenLoopDisplayRate::Candidates c;
					(which == 0 ? c.displayConfigHz : which == 1 ? c.dwmHz : which == 2 ? c.hostHz :
						which == 3 ? c.modeIntegerHz : c.cadenceHz) = hz;
					const OpenLoopDisplayRate::Choice choice = OpenLoopDisplayRate::Select(c, true);
					const S expected = which == 0 ? S::DisplayConfig : which == 1 ? S::Dwm : which == 2 ? S::Host :
						which == 3 ? S::ModeInteger : S::VBlankCadence;
					Assert::IsTrue(choice.source == expected, (L"source at " + std::to_wstring(hz)).c_str());
					Assert::AreEqual(hz, choice.hz);
					Assert::IsTrue(choice.openLoop);
				}
			}
		}

		TEST_METHOD(OutOfBoundsCandidatesAreIgnored)
		{
			const double bad[] = { 0.0, -60.0, 9.99, 500.01, 1000.0, std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity() };
			for (double hz : bad)
			{
				OpenLoopDisplayRate::Candidates c;
				c.statisticsHz = hz;
				c.displayConfigHz = hz;
				c.dwmHz = hz;
				c.hostHz = hz;
				c.modeIntegerHz = hz;
				c.cadenceHz = hz;
				Assert::IsTrue(OpenLoopDisplayRate::Select(c, true).source == OpenLoopDisplayRate::Source::None);
				c.cadenceHz = 75.0;
				Assert::AreEqual(75.0, OpenLoopDisplayRate::Select(c, true).hz, L"the next valid source");
			}
		}

		// The general display bounds: DisplayMode's floor to 500 Hz (the DWM rate was 20..120).
		TEST_METHOD(GeneralDisplayBoundsReplaceTheDwmWindow)
		{
			Assert::AreEqual(10.0, DisplayRefreshBounds::kMinimumHz);
			Assert::AreEqual(500.0, DisplayRefreshBounds::kMaximumHz);
			for (double hz = 20.0; hz <= 500.0; hz += 0.5)
				Assert::IsTrue(DisplayRefreshBounds::Accepts(hz), std::to_wstring(hz).c_str());
			for (double hz : { 10.0, 144.0, 165.0, 240.0, 360.0, 480.0, 500.0 })
				Assert::IsTrue(DisplayRefreshBounds::Accepts(hz));
			for (double hz : { 0.0, 9.999, 500.001, -1.0, std::numeric_limits<double>::quiet_NaN(),
				std::numeric_limits<double>::infinity() })
				Assert::IsFalse(DisplayRefreshBounds::Accepts(hz));
		}

		// The vblank cadence: the median interval, robust to a missed and a doubled wait.
		TEST_METHOD(VBlankCadenceMedian)
		{
			for (double hz : { 50.0, 60000.0 / 1001.0, 120.0, 144.0, 240.0 })
			{
				OpenLoopDisplayRate::VBlankCadence cadence;
				const double P = 1.0 / hz;
				double t = 100.0;
				cadence.Add(t);
				for (size_t i = 0; i + 1 < OpenLoopDisplayRate::VBlankCadence::kMinIntervals; ++i)
				{
					t += P;
					cadence.Add(t);
				}
				Assert::AreEqual(0.0, cadence.Hz(), L"too few intervals");
				std::mt19937 random(11);
				std::uniform_real_distribution<double> jitter(-0.03, 0.03);
				for (int i = 0; i < 40; ++i)
				{
					t += P * (i == 10 ? 2.0 : 1.0 + jitter(random));   // one missed vblank
					cadence.Add(t);
					if (i == 20)
					{
						t += 1e-5;   // a doubled wait
						cadence.Add(t);
					}
				}
				Assert::AreEqual(hz, cadence.Hz(), hz * 0.01, std::to_wstring(hz).c_str());
				cadence.Reset();
				Assert::AreEqual<size_t>(0, cadence.Count());
				Assert::AreEqual(0.0, cadence.Hz());
			}
		}

		// Open-loop lateness by QPC: more than one refresh after the planned vsync (the
		// open-loop grid may be up to a refresh off the real vsyncs).
		TEST_METHOD(LateRefreshesByPresentReturn)
		{
			for (double hz : { 24.0, 50.0, 60.0, 144.0 })
			{
				const double P = 1.0 / hz, v = 10.0;
				Assert::AreEqual(0, OpenLoopDisplayRate::LateRefreshes(v - 3.0 * P, v, P), L"queued ahead");
				Assert::AreEqual(0, OpenLoopDisplayRate::LateRefreshes(v + 0.9 * P, v, P));
				Assert::AreEqual(0, OpenLoopDisplayRate::LateRefreshes(v + P, v, P), L"one refresh: grid uncertainty");
				Assert::AreEqual(2, OpenLoopDisplayRate::LateRefreshes(v + 1.1 * P, v, P));
				Assert::AreEqual(3, OpenLoopDisplayRate::LateRefreshes(v + 2.5 * P, v, P));
			}
			Assert::AreEqual(0, OpenLoopDisplayRate::LateRefreshes(5.0, 1.0, 0.0), L"no period");
			Assert::AreEqual(0, OpenLoopDisplayRate::LateRefreshes(5.0, 0.0, 0.01), L"no plan");
		}

		// Schedule::NoteLate (open loop) acts as an observed late present.
		TEST_METHOD(NoteLateMovesTheChainAndDropsOne)
		{
			const double Pd = 1.0 / 60.0;
			Schedule schedule;
			schedule.Commit(1.0, { { 1u, 0.95 }, { 2u, 0.95 + Pd } });
			Assert::IsFalse(schedule.NoteLate(0, Pd));
			Assert::AreEqual(1.0, schedule.NextVsync());
			Assert::AreEqual(0, schedule.TakeLateDrops(), L"on time: nothing");
			// The first after the anchor re-anchors only (bug hunt 05 M2): the chain moves,
			// nothing is dropped, not late.
			Assert::IsFalse(schedule.NoteLate(1, Pd));
			Assert::AreEqual(1.0 + Pd, schedule.NextVsync(), 1e-12);
			Assert::AreEqual(0, schedule.TakeLateDrops());
			Assert::AreEqual(1ull, schedule.Reanchors());
			Assert::IsTrue(schedule.NoteLate(2, Pd));
			Assert::AreEqual(1.0 + 3 * Pd, schedule.NextVsync(), 1e-12);
			Assert::AreEqual(1, schedule.TakeLateDrops());
			Assert::AreEqual(0, schedule.TakeLateDrops(), L"consumed");
			// The pending plans moved too: present 1 at its moved vsync is on time.
			Assert::IsFalse(schedule.Observe(1u, 0.95 + 3 * Pd, Pd));
		}

		TEST_METHOD(ChangeWorthALine)
		{
			OpenLoopDisplayRate::Choice a, b;
			a.hz = 60.0;
			a.source = OpenLoopDisplayRate::Source::Dwm;
			b = a;
			b.hz = 60.03;
			Assert::IsFalse(OpenLoopDisplayRate::Changed(a, b), L"under 0.1%");
			b.hz = 60.1;
			Assert::IsTrue(OpenLoopDisplayRate::Changed(a, b));
			b = a;
			b.source = OpenLoopDisplayRate::Source::Host;
			Assert::IsTrue(OpenLoopDisplayRate::Changed(a, b), L"a new source");
			Assert::IsTrue(OpenLoopDisplayRate::Changed(OpenLoopDisplayRate::Choice(), a), L"the first rate");
		}
	};

	// Bug hunt 05 (render-ahead gate, queue credit, chain depth, re-anchoring).
	TEST_CLASS(RenderAheadGateFixTests)
	{
	public:
		static GateInput Heavy()
		{
			GateInput g;
			g.sourcePeriod = 1.0 / 24.0;
			g.displayPeriod = 1.0 / 60.0;
			g.costs.newFrameMs = 27.0;
			g.costs.extraMs = 7.0;
			g.costs.estimateMs = 2.0;
			return g;
		}

		// H3: a new generation, a new cadence or render_ahead switched on is a new epoch,
		// and a reset gate starts at full, free to step down again at once.
		TEST_METHOD(EpochChangesResetTheGate)
		{
			GateEpoch epoch;
			Assert::IsTrue(epoch.Changed(1, 2.5), L"the first frame");
			Assert::IsFalse(epoch.Changed(1, 2.5));
			Assert::IsFalse(epoch.Changed(1, 2.51), L"jitter in the measured rates");
			Assert::IsTrue(epoch.Changed(2, 2.51), L"a new generation");
			Assert::IsFalse(epoch.Changed(2, 2.51));
			Assert::IsTrue(epoch.Changed(2, 1.0), L"a new cadence (60p)");
			Assert::IsTrue(epoch.Changed(2, 2.5), L"and back");
			epoch.Invalidate();
			Assert::IsTrue(epoch.Changed(2, 2.5), L"render_ahead switched on");
			Assert::IsFalse(epoch.Changed(2, 2.5));
			// Small steps never add up to a missed change: the epoch keeps its own ratio.
			GateEpoch slow;
			slow.Changed(1, 2.5);
			bool changed = false;
			for (int i = 1; i <= 10; ++i)
				changed = slow.Changed(1, 2.5 + 0.005 * i) || changed;
			Assert::IsTrue(changed, L"0.05 in 0.005 steps");

			Gate gate;
			gate.Reset();
			GateInput light = Heavy();
			light.costs.newFrameMs = 20.0;
			Assert::IsTrue(gate.Update(0.0, light, true).changed);
			Assert::AreNotEqual(0, gate.Level());
			gate.Reset();
			Assert::AreEqual(0, gate.Level());
			const GateChange again = gate.Update(0.1, light, true);
			Assert::IsTrue(again.changed, L"a reset gate steps down again from full");
		}

		// H3: a cost window that empties keeps its last p95 for 30 s, then is unknown.
		TEST_METHOD(KnownCostHoldsThenForgets)
		{
			KnownCost x(4);
			Assert::IsFalse(x.Known());
			for (int i = 0; i < 3; ++i)
				x.Add(i * 0.1, 7.0);
			x.Trim(0.3);
			Assert::IsFalse(x.Measured());
			Assert::AreEqual(0.0, x.Current(), L"three samples: not measured, nothing held");
			x.Add(0.4, 7.0);
			x.Trim(0.4);
			Assert::IsTrue(x.Measured());
			Assert::AreEqual(7.0, x.Current());
			x.Trim(6.0);   // the window empties (5 s)
			Assert::IsFalse(x.Measured());
			Assert::IsTrue(x.Known());
			Assert::AreEqual(7.0, x.Current(), L"held");
			x.Trim(0.4 + KnownCost::kHoldSeconds + 0.1);
			Assert::IsFalse(x.Known(), L"30 s old: unknown again");
			Assert::AreEqual(0.0, x.Current());
			x.Add(40.0, 5.0);
			x.Reset();
			Assert::AreEqual<size_t>(0, x.Count());
			Assert::IsFalse(x.Known());
		}

		// M4: floor(lead / P_s), latched while the cadence holds; a single break keeps it.
		TEST_METHOD(QueueCreditLatches)
		{
			Assert::AreEqual<size_t>(0, CreditFrames(2.0 / 60.0, 1.0 / 24.0), L"33 ms of 41.7 ms: no whole frame");
			Assert::AreEqual<size_t>(1, CreditFrames(1.0 / 24.0, 1.0 / 24.0));
			Assert::AreEqual<size_t>(2, CreditFrames(2.5 / 30.0, 1.0 / 30.0));
			Assert::AreEqual<size_t>(0, CreditFrames(0.05, 0.0));

			const double Ps = 1.0 / 30.0, lead = 2.2 * Ps;
			CreditLatch latch;
			Assert::AreEqual<size_t>(2, latch.Commit(lead, Ps, 3.0));
			Assert::AreEqual<size_t>(2, latch.Break(10.0), L"a single break keeps the credit");
			Assert::AreEqual<size_t>(2, latch.Commit(lead, Ps, 3.0));
			Assert::AreEqual<size_t>(2, latch.Break(11.0));
			Assert::AreEqual<size_t>(2, latch.Break(11.0 + CreditLatch::kReleaseSeconds - 0.01));
			Assert::AreEqual<size_t>(0, latch.Break(11.0 + CreditLatch::kReleaseSeconds), L"broken for 2 s: returned");
			Assert::AreEqual<size_t>(0, latch.Frames());
			Assert::AreEqual<size_t>(2, latch.Commit(lead, Ps, 3.0));
			Assert::AreEqual<size_t>(2, latch.Commit(lead * 0.9, Ps, 3.01), L"the same cadence: latched");
			Assert::AreEqual<size_t>(1, latch.Commit(1.5 * Ps, Ps, 2.0), L"a new cadence: its own credit");
			latch.Reset();
			Assert::AreEqual<size_t>(0, latch.Frames());
			Assert::AreEqual<size_t>(0, latch.Break(20.0));
		}

		// M6: low latency and content without extras keep today's chain.
		TEST_METHOD(ChainDepthKeepsTodayWithoutExtras)
		{
			const ChainDepth today;
			Assert::AreEqual(3, today.buffers);
			Assert::AreEqual(2u, today.latency);
			Assert::IsTrue(ChainDepthFor(false, false, 2.5) == today, L"render_ahead off");
			Assert::IsTrue(ChainDepthFor(true, true, 2.5) == today, L"low latency");
			Assert::IsTrue(ChainDepthFor(true, false, 0.0) == today, L"cadence not known");
			Assert::IsTrue(ChainDepthFor(true, false, 1.0) == today, L"60p on 60 Hz: no extras");
			Assert::IsTrue(ChainDepthFor(true, false, 1.001) == today, L"59.94 on 60: no extras");
			const ChainDepth film = ChainDepthFor(true, false, 2.5);
			Assert::AreEqual(SwapchainBuffers(2.5), film.buffers);
			Assert::AreEqual(kQueuedFrameLatency, film.latency);
			Assert::IsTrue(ChainDepthFor(true, false, 2.0) != film);
		}

		// M7: X per vector mode; each level priced with its own, the saving counted once.
		TEST_METHOD(ExtraCostPerVectorMode)
		{
			// Both measured: level 0 = own, L1 = external; no estimate.
			GateCosts c = ExtraCosts(7.0, 4.0, 2.5, true);
			Assert::AreEqual(7.0, c.extraMs);
			Assert::AreEqual(4.0, c.extraCheaperMs, L"measured at L1: the saving is in it already");
			// Only the L1 window measured (the ladder sits at L1): level 0 = X_ext + saving,
			// not the cheaper X (before: level 0 priced cheap, the ladder oscillated).
			c = ExtraCosts(0.0, 4.0, 2.5, true);
			Assert::AreEqual(6.5, c.extraMs);
			Assert::AreEqual(4.0, c.extraCheaperMs);
			// Only level 0 measured: L1 = X_own - saving.
			c = ExtraCosts(7.0, 0.0, 2.5, true);
			Assert::AreEqual(7.0, c.extraMs);
			Assert::AreEqual(4.5, c.extraCheaperMs);
			c = ExtraCosts(7.0, 0.0, -1.0, true);
			Assert::AreEqual(7.0, c.extraCheaperMs, L"a negative saving counts as none");
			// No L1 (one mode for every level): the dearer, no cheaper price.
			c = ExtraCosts(5.0, 6.0, 2.5, false);
			Assert::AreEqual(6.0, c.extraMs);
			Assert::AreEqual(0.0, c.extraCheaperMs);
			c = ExtraCosts(0.0, 0.0, 2.5, true);
			Assert::AreEqual(0.0, c.extraMs);
			Assert::AreEqual(0.0, c.extraCheaperMs);

			// The oscillation: at L1 with only external extras timed, the up-check prices
			// level 0 with X_own (6.5 ms here), which does not fit, so the ladder stays.
			GateInput g = Heavy();
			g.l1Available = true;
			g.costs.newFrameMs = 20.0;
			const GateCosts atL1 = ExtraCosts(0.0, 4.0, 2.5, true);
			g.costs.extraMs = atL1.extraMs;
			g.costs.extraCheaperMs = atL1.extraCheaperMs;
			Assert::IsTrue(Fits(g, 1, 0.85), L"L1 fits: 20 + 2 + 1.5 x 4 = 28");
			Assert::IsFalse(Fits(g, 0, 0.85 - Gate::kUpMargin), L"level 0 at 6.5 ms does not fit u - 0.10");
			GateInput old = g;
			old.costs.extraMs = 4.0;   // the old single window at L1
			Assert::IsTrue(Fits(old, 0, 0.85 - Gate::kUpMargin), L"before: level 0 priced with the L1 extra");
		}

		// L1: the cadence clamp is not a step down: no change, no lastDown (recovery is
		// not delayed by 10 s).
		TEST_METHOD(CadenceClampIsNotAStepDown)
		{
			GateInput overload = Heavy();
			overload.l1Available = true;
			overload.costs.newFrameMs = 60.0;
			Gate gate;
			gate.Reset();
			for (int t = 0; t <= 100; ++t)
				gate.Update(t / 10.0, overload, false);
			Assert::AreEqual(HoldLevel(2), gate.Level());
			// 30 s later the cadence changes to one extra: the level clamps to its hold.
			GateInput thirty = overload;
			thirty.displayPeriod = 1.0 / 30.0;
			const GateChange clamp = gate.Update(40.0, thirty, false);
			Assert::IsFalse(clamp.changed, L"no change logged");
			Assert::AreEqual(HoldLevel(1), gate.Level());
			Assert::AreEqual(std::string(""), std::string(clamp.why));
			// A light load at the new cadence steps up after 5 s under, not 10 s after the clamp.
			GateInput light = thirty;
			light.costs.newFrameMs = 20.0;
			light.costs.extraMs = 4.0;
			light.costs.estimateMs = 1.0;
			bool up = false;
			for (int t = 401; t <= 460 && !up; ++t)
				up = gate.Update(t / 10.0, light, false).changed;
			Assert::IsTrue(up, L"up 5 s after the clamp");
		}

		// M2: the first observation after a new anchor re-anchors only; later lateness is
		// late as before. A break re-arms it.
		TEST_METHOD(FirstObservationAfterRestartReanchors)
		{
			const double Pd = 1.0 / 60.0;
			Schedule schedule;
			bool restart = false, behind = false;
			const double v = schedule.FirstVsync(1.0, Pd, 0.995, restart, behind);
			Assert::IsTrue(restart);
			schedule.Commit(v + 3 * Pd, { { 1u, v }, { 2u, v + Pd }, { 3u, v + 2 * Pd } });
			// A's GPU work was still running: it shows two refreshes after the anchor.
			Assert::IsFalse(schedule.Observe(1u, v + 2 * Pd, Pd), L"re-anchor, not late");
			Assert::AreEqual(0, schedule.TakeLateDrops(), L"nothing dropped");
			Assert::AreEqual(v + 5 * Pd, schedule.NextVsync(), 1e-12, L"the chain moved");
			Assert::AreEqual(1ull, schedule.Reanchors());
			// The pending presents moved with it.
			Assert::IsFalse(schedule.Observe(2u, v + 3 * Pd, Pd));
			// Genuine lateness afterwards is late.
			Assert::IsTrue(schedule.Observe(3u, v + 6 * Pd, Pd));
			Assert::AreEqual(1, schedule.TakeLateDrops());
			// A chained commit does not re-arm; a break does.
			schedule.Commit(2.0, { { 4u, 1.9 } });
			Assert::IsTrue(schedule.Observe(4u, 1.9 + Pd, Pd), L"chained: late");
			schedule.TakeLateDrops();
			schedule.Break();
			schedule.Commit(3.0, { { 5u, 2.9 } });
			Assert::IsFalse(schedule.Observe(5u, 2.9 + Pd, Pd), L"after a break: re-anchor");
			Assert::AreEqual(2ull, schedule.Reanchors());
			// An on-time first observation also uses up the re-anchor.
			schedule.Break();
			schedule.Commit(4.0, { { 6u, 3.9 }, { 7u, 3.9 + Pd } });
			Assert::IsFalse(schedule.Observe(6u, 3.9, Pd));
			Assert::IsTrue(schedule.Observe(7u, 3.9 + 2 * Pd, Pd));
		}

		// On-screen 2026-10-17: an anchor taken in a start-up backlog (L 1.2 s) while the
		// real latency is 0.2 s is replaced after a second of film time; one short dip and
		// a healthy chain (phi0 from the restart's -lead/P_s upward) never replace it.
		TEST_METHOD(AnchorTooLateIsReplacedWhenSustained)
		{
			const double Ps = 1001.0 / 24000.0, Pd = 1.0 / 60.0;
			const double lead = PlannedLead(Ps, Pd);
			Schedule schedule;
			Assert::IsFalse(schedule.AnchorTooLate(1.0, 0.0, lead, Ps), L"not anchored");
			schedule.AnchorLatency(true, 1.2, 0.0);   // the backlog anchor
			// One frame that comes early, then a frame on the anchor: no replacement.
			Assert::IsFalse(schedule.AnchorTooLate(10.0 * Ps + 0.2, 10.0 * Ps, lead, Ps));
			Assert::IsFalse(schedule.AnchorTooLate(11.0 * Ps + 1.2 + lead, 11.0 * Ps, lead, Ps));
			// Sustained: replaced once after a second, then the run starts again.
			int replaced = 0, at = -1;
			for (int i = 20; i < 20 + 48; ++i)
				if (schedule.AnchorTooLate(i * Ps + 0.2, i * Ps, lead, Ps))
				{
					++replaced;
					if (at < 0) at = i;
					schedule.AnchorLatency(true, i * Ps + 0.2, i * Ps);
				}
			Assert::AreEqual(1, replaced, L"replaced once");
			Assert::IsTrue(at >= 20 + 23 && at <= 20 + 25, L"after a second of film time");
			Assert::AreEqual(1ull, schedule.AnchorsLowered());
			// A healthy chain: the restart's phi0 is -lead/P_s, chained phases up to 1.
			Schedule healthy;
			healthy.AnchorLatency(true, 0.15, 0.0);
			for (int i = 1; i < 2000; ++i)
			{
				const double phi0 = -lead / Ps + std::fmod(i * 0.37, 1.0 + lead / Ps);
				Assert::IsFalse(healthy.AnchorTooLate(i * Ps + 0.15 + lead + phi0 * Ps, i * Ps, lead, Ps));
			}
			// Within the margin (less than a source period below the restart's phase).
			for (int i = 1; i < 100; ++i)
				Assert::IsFalse(healthy.AnchorTooLate(i * Ps + 0.15 - 0.9 * Ps, i * Ps, lead, Ps));
		}

		// M5: open-loop drift corrections reach the gate only when sustained (2 in 2 s).
		TEST_METHOD(DriftCorrectionsOnlyWhenSustained)
		{
			SustainedLateness drift;
			Assert::IsFalse(drift.Note(0.0), L"one correction: not the gate's");
			Assert::IsFalse(drift.Note(2.5), L"2.5 s later: not sustained");
			Assert::IsTrue(drift.Note(3.5), L"two in 2 s");
			Assert::IsFalse(drift.Note(3.6), L"the count starts again");
			Assert::IsTrue(drift.Note(4.0));
			drift.Reset();
			Assert::IsFalse(drift.Note(4.1));

			// A slow drift (one correction every ~170 s, 100 ppm at 60 Hz) never moves the
			// ladder; the gate with a light load stays at full.
			Gate gate;
			gate.Reset();
			GateInput light = Heavy();
			light.costs.newFrameMs = 20.0;
			light.costs.extraMs = 4.0;
			SustainedLateness slow;
			for (int i = 0; i < 20; ++i)
			{
				const double now = i * 170.0;
				gate.Update(now, light, slow.Note(now));
				Assert::AreEqual(0, gate.Level());
			}
			// Sustained lateness (every frame late) still steps down.
			SustainedLateness fast;
			bool stepped = false;
			for (int i = 0; i < 4 && !stepped; ++i)
			{
				const double now = 4000.0 + i / 24.0;
				stepped = gate.Update(now, light, fast.Note(now)).changed;
			}
			Assert::IsTrue(stepped);
		}
	};

	// M2 P6-4: the Presenter API. Its methods are the renderer's RenderAheadBreak /
	// ResetRenderAheadGate / UpdateRenderAheadGate / TakeRenderAheadMainInterval bodies
	// moved without change; the legacy copies below are those bodies as they stood at
	// integ b756faa01 (LibplaceboVideoRenderer.cpp), so a drift in either shows here.
	namespace PresenterLegacy
	{
		struct State
		{
			Schedule schedule;
			Gate gate;
			CostWindow newFrameCost;
			KnownCost extraCostOwn{ 4 }, extraCostExternal{ 4 }, estimateCost{ 1 };
			SustainedLateness driftLateness;
			CreditLatch credit;
			ChainDepth chainDepth;
			unsigned long long gateResets = 0;
			bool l1 = false;
			int mainInterval = 1;
			double targetLeadMs = 0.0;
			double estimatedLeadMs = 0.0;
			size_t creditFrames = 0;
			int lastMainInterval = 1;
			unsigned long long intervalPresents = 0;
		};

		inline void Break(State& s, double now)
		{
			s.schedule.Break();
			s.mainInterval = 1;
			s.targetLeadMs = 0.0;
			s.creditFrames = s.credit.Break(now);
			s.estimatedLeadMs = 0.0;
		}

		inline int ResetGate(State& s)
		{
			const int level = s.gate.Level();
			s.gate.Reset();
			s.newFrameCost.Reset();
			s.extraCostOwn.Reset();
			s.extraCostExternal.Reset();
			s.estimateCost.Reset();
			s.driftLateness.Reset();
			s.credit.Reset();
			s.creditFrames = 0;
			s.l1 = false;
			++s.gateResets;
			return level;
		}

		struct GateOut
		{
			int level = 0;
			bool evaluated = false;
			bool probing = false;
			GateChange change;
			GateInput input;
		};

		inline GateOut UpdateGate(State& s, double now, double sourcePeriod, double displayPeriod, bool late,
			int maxExtras, double utilisation, bool vpOwnedSwapchain, bool l1Available, double ditherMsOwn,
			double ditherMsExternal)
		{
			GateOut out;
			s.newFrameCost.Trim(now);
			s.extraCostOwn.Trim(now);
			s.extraCostExternal.Trim(now);
			s.estimateCost.Trim(now);
			GateInput g;
			g.sourcePeriod = sourcePeriod;
			g.displayPeriod = displayPeriod;
			g.utilisation = utilisation;
			g.queueDepth = vpOwnedSwapchain ? static_cast<int>(s.chainDepth.latency) : 2;
			g.l1Available = l1Available;
			g.costs.newFrameMs = s.newFrameCost.P95();
			g.costs.estimateMs = s.estimateCost.Current();
			const double saving = ditherMsOwn > 0.0 && ditherMsExternal > 0.0 ? ditherMsOwn - ditherMsExternal : 0.0;
			const GateCosts extra = ExtraCosts(s.extraCostOwn.Current(), s.extraCostExternal.Current(), saving,
				g.l1Available);
			g.costs.extraMs = extra.extraMs;
			g.costs.extraCheaperMs = extra.extraCheaperMs;
			const bool degraded = s.gate.Level() > 0;
			const bool extraMeasured = s.extraCostOwn.Measured() || s.extraCostExternal.Measured();
			const bool extraKnown = s.extraCostOwn.Known() || s.extraCostExternal.Known();
			out.probing = degraded && !extraKnown;
			const bool measured = s.newFrameCost.Count() >= 10 && (maxExtras == 0 || extraMeasured || degraded);
			if (measured || late)
			{
				if (!measured)
					g.costs = GateCosts();
				out.change = s.gate.Update(now, g, late);
				out.evaluated = true;
			}
			out.input = g;
			const int level = s.gate.Level();
			s.l1 = g.l1Available && level >= 1 && level < HoldLevel(maxExtras);
			out.level = level;
			return out;
		}

		inline int TakeMainInterval(State& s, int pending)
		{
			const int interval = std::max(1, std::min(pending, kMaxSyncInterval));
			s.lastMainInterval = interval;
			if (interval > 1)
				++s.intervalPresents;
			return interval;
		}
	}

	TEST_CLASS(PresenterApiTests)
	{
	public:
		TEST_METHOD(DefaultsMatchTheRendererMembers)
		{
			Presenter p;
			Assert::AreEqual(0, p.gate.Level());
			Assert::AreEqual(1, p.mainInterval);
			Assert::AreEqual(1, p.lastMainInterval);
			Assert::AreEqual(0.0, p.targetLeadMs);
			Assert::AreEqual(0.0, p.estimatedLeadMs.load());
			Assert::AreEqual<size_t>(0, p.creditFrames.load());
			Assert::IsFalse(p.committed);
			Assert::IsFalse(p.l1);
			Assert::AreEqual(0ull, p.gateResets);
			Assert::AreEqual(0ull, p.intervalPresents);
			Assert::IsTrue(p.chainDepth == ChainDepth());
			Assert::IsFalse(p.schedule.Active());
		}

		TEST_METHOD(BreakKeepsTheLatchedCreditThenReturnsIt)
		{
			Presenter p;
			PresenterLegacy::State s;
			p.creditFrames.store(p.credit.CommitFrames(1, 2.5));
			s.creditFrames = s.credit.CommitFrames(1, 2.5);
			p.mainInterval = 3;
			s.mainInterval = 3;
			p.targetLeadMs = 12.0;
			s.targetLeadMs = 12.0;
			p.estimatedLeadMs.store(9.0);
			s.estimatedLeadMs = 9.0;
			Assert::IsTrue(p.creditFrames.load() > 0, L"one frame latched");
			const double times[] = { 10.0, 10.5, 11.9, 12.0, 12.1 };
			for (double now : times)
			{
				p.Break(now);
				PresenterLegacy::Break(s, now);
				Assert::AreEqual(s.creditFrames, p.creditFrames.load());
				Assert::AreEqual(1, p.mainInterval);
				Assert::AreEqual(0.0, p.targetLeadMs);
				Assert::AreEqual(0.0, p.estimatedLeadMs.load());
				Assert::IsFalse(p.schedule.Active());
			}
			Assert::AreEqual<size_t>(0, p.creditFrames.load(), L"returned after CreditLatch::kReleaseSeconds broken");
		}

		TEST_METHOD(ResetGateStartsTheEpochAgain)
		{
			Presenter p;
			for (int i = 0; i < 20; ++i)
			{
				p.newFrameCost.Add(i * 0.01, 30.0);
				p.extraCostOwn.Add(i * 0.01, 9.0);
				p.extraCostExternal.Add(i * 0.01, 8.0);
				p.estimateCost.Add(i * 0.01, 2.0);
			}
			p.creditFrames.store(p.credit.CommitFrames(1, 2.5));
			p.l1 = true;
			// Over budget: a late frame steps the ladder down at once.
			PresenterGateFrame f;
			f.sourcePeriod = 1.0 / 24.0;
			f.displayPeriod = 1.0 / 60.0;
			f.late = true;
			f.maxExtras = 2;
			p.UpdateGate(1.0, f);
			const int before = p.gate.Level();
			Assert::IsTrue(before > 0);
			Assert::AreEqual(before, p.ResetGate());
			Assert::AreEqual(0, p.gate.Level());
			Assert::AreEqual<size_t>(0, p.newFrameCost.Count());
			Assert::IsFalse(p.extraCostOwn.Known());
			Assert::IsFalse(p.extraCostExternal.Known());
			Assert::IsFalse(p.estimateCost.Known());
			Assert::AreEqual<size_t>(0, p.credit.Frames());
			Assert::AreEqual<size_t>(0, p.creditFrames.load());
			Assert::IsFalse(p.l1);
			Assert::AreEqual(1ull, p.gateResets);
			Assert::AreEqual(0, p.ResetGate());
			Assert::AreEqual(2ull, p.gateResets);
		}

		TEST_METHOD(TakeMainIntervalClampsAndCounts)
		{
			Presenter p;
			PresenterLegacy::State s;
			const int pending[] = { -3, 0, 1, 2, 3, 4, 5, 9, 1, 2 };
			for (int value : pending)
			{
				Assert::AreEqual(PresenterLegacy::TakeMainInterval(s, value), p.TakeMainInterval(value));
				Assert::AreEqual(s.lastMainInterval, p.lastMainInterval);
				Assert::AreEqual(s.intervalPresents, p.intervalPresents);
			}
			Assert::AreEqual(6ull, p.intervalPresents);
			Assert::AreEqual(kMaxSyncInterval, p.TakeMainInterval(100));
		}

		// The gate, step for step against the legacy body: random costs, cadences, chain
		// depths, lateness and vector modes over 4000 source periods (seeded).
		TEST_METHOD(UpdateGateMatchesTheLegacyBody)
		{
			Presenter p;
			PresenterLegacy::State s;
			std::mt19937 rng(20261110u);
			std::uniform_real_distribution<double> unit(0.0, 1.0);
			const double displays[] = { 1.0 / 60.0, 1.0 / 59.94, 1.0 / 50.0, 1.0 / 120.0 };
			const double sources[] = { 1.0 / 24.0, 1.001 / 24.0, 1.0 / 25.0, 1.0 / 30.0, 1.0 / 60.0 };
			int changes = 0, evaluated = 0, levelsSeen = 0;
			double now = 0.0;
			for (int i = 0; i < 4000; ++i)
			{
				if (i % 700 == 0)
				{
					const ChainDepth depth = ChainDepthFor(true, false, 2.5);
					p.chainDepth = (i / 700) % 2 ? depth : ChainDepth();
					s.chainDepth = p.chainDepth;
				}
				if (i == 2500)
				{
					Assert::AreEqual(PresenterLegacy::ResetGate(s), p.ResetGate());
				}
				const double sourcePeriod = sources[(i / 400) % 5];
				const double displayPeriod = displays[(i / 900) % 4];
				now += sourcePeriod;
				const double load = (i / 300) % 3 == 1 ? 3.0 : 1.0;   // phases over budget
				if (unit(rng) < 0.9)
				{
					const double ms = 4.0 + load * 9.0 * unit(rng);
					p.newFrameCost.Add(now, ms);
					s.newFrameCost.Add(now, ms);
				}
				if (unit(rng) < 0.5)
				{
					const double ms = 2.0 + load * 4.0 * unit(rng);
					(i % 2 ? p.extraCostOwn : p.extraCostExternal).Add(now, ms);
					(i % 2 ? s.extraCostOwn : s.extraCostExternal).Add(now, ms);
				}
				if (unit(rng) < 0.7)
				{
					const double ms = 1.0 + unit(rng);
					p.estimateCost.Add(now, ms);
					s.estimateCost.Add(now, ms);
				}
				PresenterGateFrame f;
				f.sourcePeriod = sourcePeriod;
				f.displayPeriod = displayPeriod;
				f.utilisation = (i / 1000) % 2 ? 0.85 : 0.6;
				f.vpOwnedChain = (i / 250) % 2 == 0;
				f.l1Available = (i / 500) % 2 == 1;
				f.ditherMsOwn = (i / 350) % 3 == 0 ? 0.0 : 1.5;
				f.ditherMsExternal = (i / 450) % 2 == 0 ? 0.0 : 0.6;
				f.late = unit(rng) < 0.01;
				f.maxExtras = MaxExtras(sourcePeriod / displayPeriod);
				const PresenterGateResult r = p.UpdateGate(now, f);
				const PresenterLegacy::GateOut o = PresenterLegacy::UpdateGate(s, now, sourcePeriod, displayPeriod,
					f.late, f.maxExtras, f.utilisation, f.vpOwnedChain, f.l1Available, f.ditherMsOwn, f.ditherMsExternal);
				Assert::AreEqual(o.level, r.level);
				Assert::AreEqual(o.evaluated, r.evaluated);
				Assert::AreEqual(o.probing, r.probing);
				Assert::AreEqual(o.change.changed, r.change.changed);
				Assert::AreEqual(o.change.from, r.change.from);
				Assert::AreEqual(o.change.to, r.change.to);
				Assert::AreEqual(std::string(o.change.why), std::string(r.change.why));
				Assert::AreEqual(o.input.queueDepth, r.input.queueDepth);
				Assert::AreEqual(o.input.l1Available, r.input.l1Available);
				Assert::AreEqual(o.input.utilisation, r.input.utilisation);
				Assert::AreEqual(o.input.costs.newFrameMs, r.input.costs.newFrameMs);
				Assert::AreEqual(o.input.costs.extraMs, r.input.costs.extraMs);
				Assert::AreEqual(o.input.costs.extraCheaperMs, r.input.costs.extraCheaperMs);
				Assert::AreEqual(o.input.costs.estimateMs, r.input.costs.estimateMs);
				Assert::AreEqual(s.l1, p.l1);
				changes += r.change.changed ? 1 : 0;
				evaluated += r.evaluated ? 1 : 0;
				levelsSeen |= 1 << std::min(r.level, 15);
			}
			// The sequence exercises the ladder (not a run that never moved).
			Assert::IsTrue(changes >= 4, L"the gate changed level");
			Assert::IsTrue(evaluated > 1000, L"the gate was evaluated");
			Assert::IsTrue((levelsSeen & ~1) != 0, L"a degraded level was reached");
		}
	};

	// M2 P6 rest: the Presenter's budget, commit, restart and bookkeeping methods. The
	// legacy copies are the renderer's bodies at r2/p6-4 7e521b9aa
	// (LibplaceboVideoRenderer.cpp: UpdateRenderAheadBudget :6434, ResolveRenderAheadTimers
	// :5440, TakeRenderAheadExtraTimerMode :5468, the commit sites :15488 and :16180, the
	// estimate :25904, the bookkeeping :15257, :15352-15372, :15652, :16055, :16074),
	// written against a second Presenter's members, so a drift in either shows here.
	namespace PresenterRestLegacy
	{
		inline void Budget(Presenter& r, const BudgetDecision& d, double now)
		{
			r.budgetCredit = d.fits ? d.creditFrames : 0;
			const bool veto = !d.fits;
			if (veto != r.budgetVeto)
			{
				if (veto)
				{
					r.Break(now);
					r.credit.Reset();
					r.creditFrames.store(0, std::memory_order_release);
				}
				else
					r.epoch.Invalidate();
				r.budgetVeto = veto;
			}
		}

		inline void CommitHoldOnly(Presenter& r, const FramePlan& plan,
			const std::vector<std::pair<uint32_t, double>>& plannedIds, uint32_t lastPresent)
		{
			r.schedule.Commit(plan.nextFirstVsync, plannedIds);
			r.mainInterval = plan.nextInterval;
			r.expectedPresent = lastPresent != 0 ? lastPresent + 1 : 0;
			r.committed = true;
		}

		inline void CommitExtras(Presenter& r, const FramePlan& plan,
			const std::vector<std::pair<uint32_t, double>>& plannedIds, uint32_t lastPresent,
			int openLoopLateRefreshes, double displayPeriod, bool queueCredit, double ratio)
		{
			r.schedule.Commit(plan.nextFirstVsync, plannedIds);
			if (openLoopLateRefreshes > 0)
				r.openLoopLate = r.schedule.NoteLate(openLoopLateRefreshes, displayPeriod);
			r.mainInterval = plan.nextInterval;
			r.expectedPresent = lastPresent != 0 ? lastPresent + 1 : 0;
			r.committed = true;
			r.creditFrames.store(queueCredit ? r.credit.CommitFrames(r.budgetCredit, ratio) : 0,
				std::memory_order_release);
		}

		inline bool TakeMode(std::deque<std::pair<unsigned long long, bool>>& modes, unsigned long long serial,
			bool lastExternal)
		{
			bool external = lastExternal;
			while (!modes.empty() && modes.front().first <= serial)
			{
				if (modes.front().first == serial)
					external = modes.front().second;
				modes.pop_front();
			}
			return external;
		}

		inline void Observation(Presenter& r)
		{
			if (r.schedule.ObservedCount() != r.observedSeen)
			{
				r.observedSeen = r.schedule.ObservedCount();
				const uint32_t id = r.schedule.LastObservedId();
				const auto known = std::find_if(r.presentIntervals.begin(), r.presentIntervals.end(),
					[id](const std::pair<uint32_t, int>& p) { return p.first == id; });
				if (known != r.presentIntervals.end())
				{
					const int kind = known->second > 1 ? 1 : 0;
					const double e = r.schedule.LastObservedError();
					r.shownError[kind] += e;
					++r.shownCount[kind];
					if (std::abs(e) <= 0.5) ++r.shownOnTime[kind];
				}
			}
		}

		// A stand-in for AlphaD3D11FrameGpuTimer: its Sample / ResolveResult shape.
		struct FakeTimer
		{
			struct Sample
			{
				uint64_t submissionSerial = 0;
				double milliseconds = 0.0;
			};
			enum class ResolveResult
			{
				Empty,
				NotReady,
				Discarded,
				Resolved,
			};
			std::deque<std::pair<ResolveResult, Sample>> results;
			int calls = 0;
			ResolveResult TryResolveOldest(int* context, Sample& sample)
			{
				++calls;
				if (!context || results.empty())
					return ResolveResult::Empty;
				const std::pair<ResolveResult, Sample> r = results.front();
				results.pop_front();
				sample = r.second;
				return r.first;
			}
		};

		inline void SameSchedule(Presenter& a, Presenter& b)
		{
			Assert::AreEqual(a.schedule.Active(), b.schedule.Active());
			Assert::AreEqual(a.schedule.Pending(), b.schedule.Pending());
			Assert::AreEqual(a.schedule.Reanchors(), b.schedule.Reanchors());
			Assert::AreEqual(a.schedule.TakeLateDrops(), b.schedule.TakeLateDrops());
		}
	}

	TEST_CLASS(PresenterRestApiTests)
	{
	public:
		TEST_METHOD(MovedMembersKeepTheRendererDefaults)
		{
			Presenter p;
			Assert::IsFalse(p.budgetVeto);
			Assert::AreEqual<size_t>(0, p.budgetCredit);
			Assert::IsTrue(p.extraTimerModes.empty());
			Assert::IsTrue(p.extraIds.empty());
			Assert::IsTrue(p.mainRefresh.empty());
			Assert::IsTrue(p.idsSwapchain == nullptr);
			Assert::AreEqual(0.0, p.displayRate.hz);
			Assert::IsTrue(p.displayRate.source == OpenLoopDisplayRate::Source::None);
			Assert::IsFalse(p.displayRate.openLoop);
			Assert::AreEqual(0u, p.expectedPresent);
			Assert::AreEqual(0.0, p.lead);
			Assert::AreEqual(0ull, p.frames + p.restarts + p.late + p.dropped + p.held);
			Assert::IsTrue(p.presentIntervals.empty());
			Assert::AreEqual(0ull, p.observedSeen);
			Assert::AreEqual(0.0, p.shownError[0] + p.shownError[1]);
			Assert::AreEqual(0ull, p.shownCount[0] + p.shownCount[1] + p.shownOnTime[0] + p.shownOnTime[1]);
			Assert::AreEqual(0ull, p.openLoopFrames + p.lateByPresent + p.flushReanchors);
			Assert::IsFalse(p.openLoopLate);
			Assert::IsTrue(p.Active(true));
			Assert::IsFalse(p.Active(false));
			const double a = Presenter::Now();
			const double b = Presenter::Now();
			Assert::IsTrue(a > 0.0 && b >= a, L"the steady clock");
		}

		// UpdateBudget + ApplyBudgetVeto (the caller logs between them) against the
		// renderer's body over a seeded run of fits, vetoes and credits.
		TEST_METHOD(BudgetMatchesTheLegacyBody)
		{
			Presenter p, r;
			std::mt19937 random(6061);
			std::uniform_int_distribution<int> coin(0, 3), frames(0, 3);
			int changes = 0;
			for (int i = 0; i < 400; ++i)
			{
				const double now = 1.0 + i * 0.3;
				if (coin(random) == 0)
				{
					// A queued extras frame latches the credit (so a veto has one to return).
					PresenterCommit c;
					c.storeCredit = true;
					c.queueCredit = true;
					c.cadenceRatio = 2.5;
					const std::vector<std::pair<uint32_t, double>> none;
					p.Commit(c, none);
					FramePlan plan;
					PresenterRestLegacy::CommitExtras(r, plan, none, 0, 0, 0.0, true, 2.5);
				}
				BudgetDecision d;
				d.fits = coin(random) != 0;
				d.creditFrames = static_cast<size_t>(frames(random));
				const PresenterBudgetChange change = p.UpdateBudget(d);
				if (change.changed)
				{
					p.ApplyBudgetVeto(change.veto, now);
					++changes;
				}
				PresenterRestLegacy::Budget(r, d, now);
				Assert::AreEqual(r.budgetVeto, p.budgetVeto);
				Assert::AreEqual(r.budgetCredit, p.budgetCredit);
				Assert::AreEqual(r.creditFrames.load(), p.creditFrames.load());
				Assert::AreEqual(r.credit.Frames(), p.credit.Frames());
				Assert::AreEqual(r.Active(true), p.Active(true));
				Assert::AreEqual(r.mainInterval, p.mainInterval);
				PresenterRestLegacy::SameSchedule(p, r);
				Assert::AreEqual(r.epoch.Changed(7, 2.5), p.epoch.Changed(7, 2.5));
			}
			Assert::IsTrue(changes > 20, L"the veto changed");
			p.ClearBudget();
			Assert::IsFalse(p.budgetVeto);
			Assert::AreEqual<size_t>(0, p.budgetCredit);
		}

		// Both commit sites, against their legacy bodies, with open-loop lateness after a
		// new anchor (re-anchor only) and in a running chain (late).
		TEST_METHOD(CommitMatchesBothRendererSites)
		{
			Presenter p, r;
			p.budgetCredit = r.budgetCredit = 2;
			std::mt19937 random(6062);
			std::uniform_int_distribution<int> kind(0, 5), late(0, 3), interval(1, 4);
			uint32_t id = 10;
			for (int i = 0; i < 300; ++i)
			{
				FramePlan plan;
				plan.nextFirstVsync = 2.0 + i / 24.0;
				plan.nextInterval = interval(random);
				std::vector<std::pair<uint32_t, double>> planned;
				planned.emplace_back(++id, plan.nextFirstVsync - 0.01);
				planned.emplace_back(++id, plan.nextFirstVsync - 0.005);
				const uint32_t lastPresent = i % 7 == 0 ? 0u : id;
				const int k = kind(random);
				p.BeginFrame();
				r.committed = false;
				if (k == 0)
				{
					p.RestartChain();
					r.schedule.Break();
				}
				PresenterCommit c;
				c.nextFirstVsync = plan.nextFirstVsync;
				c.nextInterval = plan.nextInterval;
				c.expectedPresent = lastPresent != 0 ? lastPresent + 1 : 0;
				if (k == 1)
				{
					p.Commit(c, planned);
					PresenterRestLegacy::CommitHoldOnly(r, plan, planned, lastPresent);
				}
				else if (k > 1)
				{
					c.lateRefreshes = late(random);
					c.displayPeriod = 1.0 / 60.0;
					c.storeCredit = true;
					c.queueCredit = k != 2;
					c.cadenceRatio = 2.5;
					p.Commit(c, planned);
					PresenterRestLegacy::CommitExtras(r, plan, planned, lastPresent, c.lateRefreshes, c.displayPeriod,
						c.queueCredit, 2.5);
				}
				Assert::AreEqual(r.committed, p.committed);
				Assert::AreEqual(r.mainInterval, p.mainInterval);
				Assert::AreEqual(r.expectedPresent, p.expectedPresent);
				Assert::AreEqual(r.openLoopLate, p.openLoopLate);
				Assert::AreEqual(r.creditFrames.load(), p.creditFrames.load());
				PresenterRestLegacy::SameSchedule(p, r);
				// The estimate the renderer publishes after the extras.
				const bool known = i % 3 == 0;
				p.displayRate.openLoop = r.displayRate.openLoop = i % 2 == 0;
				p.targetLeadMs = r.targetLeadMs = 5.0 + i;
				p.PublishEstimatedLead(known);
				r.estimatedLeadMs.store(r.committed && !known && r.displayRate.openLoop ? r.targetLeadMs : 0.0,
					std::memory_order_relaxed);
				Assert::AreEqual(r.estimatedLeadMs.load(), p.estimatedLeadMs.load());
			}
			Assert::IsTrue(p.schedule.Reanchors() > 0, L"open-loop lateness after a new anchor re-anchored");
		}

		// RestartChain is the schedule's break only, unlike Break(now).
		TEST_METHOD(RestartChainKeepsTheCommitState)
		{
			Presenter p;
			p.budgetCredit = 1;
			PresenterCommit c;
			c.nextInterval = 3;
			c.storeCredit = true;
			c.queueCredit = true;
			c.cadenceRatio = 2.5;
			std::vector<std::pair<uint32_t, double>> planned{ { 5u, 1.0 } };
			p.Commit(c, planned);
			p.targetLeadMs = 12.0;
			p.estimatedLeadMs.store(9.0);
			Assert::IsTrue(p.schedule.Active());
			Assert::AreEqual<size_t>(1, p.creditFrames.load());
			p.RestartChain();
			Assert::IsFalse(p.schedule.Active());
			Assert::AreEqual<size_t>(0, p.schedule.Pending());
			Assert::AreEqual(3, p.mainInterval);
			Assert::AreEqual(12.0, p.targetLeadMs);
			Assert::AreEqual(9.0, p.estimatedLeadMs.load());
			Assert::AreEqual<size_t>(1, p.creditFrames.load());
			Assert::IsTrue(p.committed);
			p.Break(100.0);
			Assert::AreEqual(1, p.mainInterval, L"Break(now) is the other one");
		}

		// The GPU timers: X by its serial's vector mode, E into its own window, at most 8
		// per timer, a pending one stops that timer; against the legacy loop.
		TEST_METHOD(ResolveTimersFeedsTheCostWindows)
		{
			using PresenterRestLegacy::FakeTimer;
			Presenter p;
			std::deque<std::pair<unsigned long long, bool>> legacyModes;
			for (unsigned long long serial = 1; serial <= 12; ++serial)
			{
				const bool external = serial % 3 == 0;
				p.NoteExtraTimerMode(serial, external);
				legacyModes.emplace_back(serial, external);
			}
			FakeTimer extra, estimate;
			double ownExpected = 0.0, externalExpected = 0.0;
			int own = 0, external = 0;
			for (unsigned long long serial = 1; serial <= 10; ++serial)
			{
				FakeTimer::Sample sample;
				sample.submissionSerial = serial;
				sample.milliseconds = static_cast<double>(serial);
				extra.results.emplace_back(serial == 4 ? FakeTimer::ResolveResult::Discarded :
					FakeTimer::ResolveResult::Resolved, sample);
			}
			FakeTimer::Sample e;
			e.milliseconds = 2.0;
			estimate.results.emplace_back(FakeTimer::ResolveResult::Resolved, e);
			estimate.results.emplace_back(FakeTimer::ResolveResult::NotReady, e);
			estimate.results.emplace_back(FakeTimer::ResolveResult::Resolved, e);
			// The legacy loop's choices, by serial (8 per call; serial 4 discarded).
			for (unsigned long long serial = 1; serial <= 8; ++serial)
			{
				if (serial == 4)
					continue;
				if (PresenterRestLegacy::TakeMode(legacyModes, serial, false))
				{
					externalExpected = static_cast<double>(serial);
					++external;
				}
				else
				{
					ownExpected = static_cast<double>(serial);
					++own;
				}
			}
			int context = 0;
			p.ResolveTimers(extra, estimate, &context, 5.0, false);
			Assert::AreEqual(8, extra.calls, L"at most 8 per timer");
			Assert::AreEqual(2, estimate.calls, L"a pending timer stops");
			Assert::AreEqual<size_t>(static_cast<size_t>(own), p.extraCostOwn.Count());
			Assert::AreEqual<size_t>(static_cast<size_t>(external), p.extraCostExternal.Count());
			Assert::AreEqual<size_t>(1, p.estimateCost.Count());
			Assert::IsTrue(ownExpected > 0.0 && externalExpected > 0.0);
			Assert::AreEqual(legacyModes.size(), p.extraTimerModes.size(), L"the same modes consumed");
			// An unknown serial takes the last extra's mode; the 64 newest are kept.
			Assert::IsTrue(p.TakeExtraTimerMode(1000, true));
			Assert::IsTrue(p.extraTimerModes.empty());
			for (unsigned long long serial = 0; serial < 100; ++serial)
				p.NoteExtraTimerMode(serial, false);
			Assert::AreEqual<size_t>(64, p.extraTimerModes.size());
			Assert::AreEqual(36ull, p.extraTimerModes.front().first);
		}

		// The bookkeeping the extension reads: one swapchain's ids, the renderer's caps,
		// a main refresh kept once and never for an extra, the shown-error diagnostics.
		TEST_METHOD(PresentBookkeepingKeepsTheRendererRules)
		{
			Presenter p, r;
			int chainA = 0, chainB = 0;
			p.NoteIdsSwapchain(&chainA);
			for (uint32_t id = 1; id <= 300; ++id)
				p.NoteExtraPresent(id);
			Assert::AreEqual<size_t>(256, p.extraIds.size());
			Assert::AreEqual(45u, p.extraIds.front());
			for (uint32_t id = 1000; id < 1100; ++id)
				p.NoteMainRefresh(id, id * 2);
			Assert::AreEqual<size_t>(64, p.mainRefresh.size());
			p.NoteIdsSwapchain(&chainA);
			Assert::AreEqual<size_t>(256, p.extraIds.size(), L"the same swapchain keeps its ids");
			p.NoteIdsSwapchain(&chainB);
			Assert::IsTrue(p.extraIds.empty() && p.mainRefresh.empty());
			Assert::IsTrue(p.idsSwapchain == &chainB);
			p.NoteExtraPresent(7);
			p.NoteStatisticsPresent(7, 70);
			Assert::IsTrue(p.mainRefresh.empty(), L"an extra is not a main present");
			p.NoteStatisticsPresent(8, 80);
			p.NoteStatisticsPresent(8, 81);
			Assert::AreEqual<size_t>(1, p.mainRefresh.size(), L"kept once");
			Assert::AreEqual(80u, p.mainRefresh.back().second);
			p.ClearPresentIds();
			Assert::IsTrue(p.extraIds.empty() && p.mainRefresh.empty() && p.idsSwapchain == nullptr);
			// Intervals: uncapped at the main present, 128 kept after an extra.
			for (uint32_t id = 1; id <= 200; ++id)
				p.NotePresentInterval(id, 1, 0);
			Assert::AreEqual<size_t>(200, p.presentIntervals.size());
			p.NotePresentInterval(201, 2, 128);
			Assert::AreEqual<size_t>(128, p.presentIntervals.size());
			// Observations, against the legacy diagnostics: planned presents shown late,
			// on time and early, after holds and not.
			p.presentIntervals.clear();
			const double period = 1.0 / 60.0;
			std::vector<std::pair<uint32_t, double>> planned;
			for (uint32_t id = 1; id <= 40; ++id)
			{
				planned.emplace_back(id, id * period);
				p.NotePresentInterval(id, id % 4 == 0 ? 2 : 1, 128);
				r.presentIntervals.emplace_back(id, id % 4 == 0 ? 2 : 1);
			}
			p.schedule.Commit(41 * period, planned);
			r.schedule.Commit(41 * period, planned);
			for (uint32_t id = 1; id <= 40; ++id)
			{
				const double shown = id * period + (id % 5 == 0 ? 0.7 : id % 3 == 0 ? -0.2 : 0.1) * period;
				p.schedule.Observe(id, shown, period);
				r.schedule.Observe(id, shown, period);
				p.NoteObservation();
				PresenterRestLegacy::Observation(r);
				p.NoteObservation();   // nothing new: no second count
				for (int kindIndex = 0; kindIndex < 2; ++kindIndex)
				{
					Assert::AreEqual(r.shownError[kindIndex], p.shownError[kindIndex]);
					Assert::AreEqual(r.shownCount[kindIndex], p.shownCount[kindIndex]);
					Assert::AreEqual(r.shownOnTime[kindIndex], p.shownOnTime[kindIndex]);
				}
				Assert::AreEqual(r.observedSeen, p.observedSeen);
			}
			Assert::IsTrue(p.shownCount[0] > 0 && p.shownCount[1] > 0, L"both kinds seen");
		}
	};
	// P6 rest S1 (TD-readiness audit 93 (a)2): the analysis floor is an input of the delay
	// budget and the floor-vs-credit policy is selectable; with no floor above
	// kBudgetMinQueued the decision is today's in both policies.
	TEST_CLASS(PresenterFloorTests)
	{
	public:
		TEST_METHOD(AnalysisFloorTakesTheDeclaredLeadPlusTwo)
		{
			Assert::AreEqual(size_t{ 0 }, VpQueueRules::AnalysisFloorFrames(0, 0));
			Assert::AreEqual(size_t{ 3 }, VpQueueRules::AnalysisFloorFrames(3, 0), L"no declared lead: the motion floor");
			Assert::AreEqual(size_t{ 4 }, VpQueueRules::AnalysisFloorFrames(3, 2), L"lead 2 + 2 above the motion floor");
			Assert::AreEqual(size_t{ 5 }, VpQueueRules::AnalysisFloorFrames(5, 2), L"the motion floor above lead + 2");
			Assert::AreEqual(size_t{ 3 }, VpQueueRules::AnalysisFloorFrames(0, 1));
		}

		TEST_METHOD(NoFloorAboveTheMinimumKeepsTodaysDecision)
		{
			const double sources[] = { 23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0 };
			const double displays[] = { 48.0, 50.0, 59.94, 60.0, 100.0, 120.0, 144.0 };
			for (double sh : sources)
				for (double dh : displays)
					for (size_t q = 0; q <= 12; ++q)
						for (int c = 0; c < 2; ++c)
							for (size_t f = 0; f <= kBudgetMinQueued; ++f)
								for (int pol = 0; pol < 3; ++pol)
								{
									const BudgetDecision a = FitDelayBudget(1.0 / sh, 1.0 / dh, q, c != 0);
									const BudgetDecision b = FitDelayBudget(1.0 / sh, 1.0 / dh, q, c != 0, f,
										static_cast<FloorPolicy>(pol));
									Assert::IsFalse(b.floorCompetes);
									Assert::AreEqual(size_t(0), b.raisedQueueFrames);
									const std::wstring n = Name(sh, dh) + L" q " + std::to_wstring(q) + L" f " + std::to_wstring(f);
									Assert::AreEqual(a.fits, b.fits, n.c_str());
									Assert::AreEqual(a.creditFrames, b.creditFrames, n.c_str());
									Assert::AreEqual(a.leadMs, b.leadMs, n.c_str());
									Assert::AreEqual(a.budgetMs, b.budgetMs, n.c_str());
									Assert::AreEqual(a.totalMs, b.totalMs, n.c_str());
									Assert::AreEqual(std::string(a.reason), std::string(b.reason), n.c_str());
									Assert::IsFalse(b.floorCompetes, n.c_str());
									Assert::AreEqual(b.totalMs, b.realTotalMs, n.c_str());
									Assert::IsTrue(Presenter::FloorReport(b, FloorPolicy::CreditWins).empty(), n.c_str());
								}
		}

		TEST_METHOD(FloorAboveTheMotionDepthCreditWinsReportsTheRealDelay)
		{
			// 24p on 60 Hz: lead 2 extras = 33.3 ms, credit 1 frame; queue 4; motion floor 3,
			// a declared analysis lead 2 -> floor 4 > 4 - 1.
			const double ps = 1.0 / 24.0, pd = 1.0 / 60.0;
			const size_t floorFrames = VpQueueRules::AnalysisFloorFrames(3, 2);
			const BudgetDecision d = FitDelayBudget(ps, pd, 4, true, floorFrames, FloorPolicy::CreditWins);
			const BudgetDecision today = FitDelayBudget(ps, pd, 4, true);
			Assert::IsTrue(d.floorCompetes);
			Assert::AreEqual(today.fits, d.fits, L"CreditWins keeps today's decision");
			Assert::AreEqual(today.creditFrames, d.creditFrames);
			Assert::AreEqual(size_t{ 1 }, d.creditFrames);
			Assert::AreEqual(today.totalMs, d.totalMs);
			Assert::AreEqual((4.0 * ps + 2.0 * pd) * 1000.0, d.realTotalMs, 1e-9, L"the queue stays at the floor");
			Assert::IsTrue(d.realTotalMs > d.budgetMs, L"the real delay exceeds the budget");
			const std::string report = Presenter::FloorReport(d, FloorPolicy::CreditWins);
			Assert::IsTrue(report.find("credit wins") != std::string::npos);
			Assert::IsTrue(report.find("200 ms") != std::string::npos, L"the real delay is shown");

			Presenter p;
			p.analysisFloor.store(floorFrames);
			const BudgetDecision viaPresenter = p.FitBudget(ps, pd, 4, true);
			Assert::AreEqual(d.creditFrames, viaPresenter.creditFrames, L"the default policy is CreditWins");
			Assert::IsFalse(p.UpdateBudget(viaPresenter).veto);
			Assert::AreEqual(size_t{ 1 }, p.budgetCredit);
		}

		TEST_METHOD(FloorAboveTheMotionDepthFloorWinsShrinksTheCreditAndVetoesVisibly)
		{
			const double ps = 1.0 / 24.0, pd = 1.0 / 60.0;
			const size_t floorFrames = VpQueueRules::AnalysisFloorFrames(3, 2);
			const BudgetDecision d = FitDelayBudget(ps, pd, 4, true, floorFrames, FloorPolicy::FloorWins);
			Assert::IsTrue(d.floorCompetes);
			Assert::IsFalse(d.fits, L"the frames the floor leaves do not cover the lead");
			Assert::AreEqual(size_t{ 0 }, d.creditFrames, L"the credit shrinks to what the floor leaves");
			Assert::AreEqual(std::string("the analysis floor keeps the frames the lead needs"), std::string(d.reason));
			Assert::IsTrue(Presenter::FloorReport(d, FloorPolicy::FloorWins).find("vetoed") != std::string::npos);

			Presenter p;
			p.analysisFloor.store(floorFrames);
			p.floorPolicy = FloorPolicy::FloorWins;
			const PresenterBudgetChange change = p.UpdateBudget(p.FitBudget(ps, pd, 4, true));
			Assert::IsTrue(change.veto && change.changed);
			Assert::AreEqual(size_t{ 0 }, p.budgetCredit);

			// A queue long enough for both: no competition, the credit as today.
			const BudgetDecision longer = FitDelayBudget(ps, pd, 6, true, floorFrames, FloorPolicy::FloorWins);
			Assert::IsFalse(longer.floorCompetes);
			Assert::IsTrue(longer.fits);
			Assert::AreEqual(size_t{ 1 }, longer.creditFrames);
		}

		TEST_METHOD(AvrTier200MsAt24p)
		{
			// The 200 ms AVR tier at 24p: the whole frames inside it (4 = 166.7 ms); a TD lead 3
			// declares floor 5, capped at the queue. Both policies report; FloorWins vetoes.
			const double ps = 1.0 / 24.0, pd = 1.0 / 60.0;
			const size_t q = static_cast<size_t>(std::floor(0.200 / ps + 1e-9));
			Assert::AreEqual(size_t{ 4 }, q);
			const size_t floorFrames = VpQueueRules::AnalysisFloorFrames(MotionAhead::QueueFramesFor(2), 3);
			Assert::AreEqual(size_t{ 5 }, floorFrames);
			const BudgetDecision credit = FitDelayBudget(ps, pd, q, true, floorFrames, FloorPolicy::CreditWins);
			Assert::IsTrue(credit.fits && credit.floorCompetes);
			Assert::IsTrue(credit.totalMs <= 200.0 + 0.5, L"the counted delay fits the tier");
			Assert::AreEqual((4.0 * ps + 2.0 * pd) * 1000.0, credit.realTotalMs, 1e-9);
			Assert::IsFalse(Presenter::FloorReport(credit, FloorPolicy::CreditWins).empty(), L"visible");
			const BudgetDecision floorWins = FitDelayBudget(ps, pd, q, true, floorFrames, FloorPolicy::FloorWins);
			Assert::IsFalse(floorWins.fits);
			Assert::IsTrue(floorWins.totalMs <= floorWins.budgetMs + floorWins.leadMs + 1e-9);
			Assert::IsFalse(Presenter::FloorReport(floorWins, FloorPolicy::FloorWins).empty(), L"visible");
			// The queue rule keeps the floor (capped at the target) in both.
			Assert::AreEqual(q, VpQueueRules::CombineQueueTarget(q, credit.creditFrames, floorFrames, true));
			// Audit SHOULD 2 (f445678ad cap, TD design (a)4): the floor the host now passes is capped at the desired queue (5 -> 4).
			// CreditWins still fits (never a TD veto); FloorWins keeps its definition: credit q - max(4, 3) = 0 < 1 wanted, a veto.
			const size_t capped = VpQueueRules::AnalysisFloorFrames(MotionAhead::QueueFramesFor(2), 3, q, false);
			Assert::AreEqual(size_t{ 4 }, capped);
			const BudgetDecision cappedCredit = FitDelayBudget(ps, pd, q, true, capped, FloorPolicy::CreditWins);
			Assert::IsTrue(cappedCredit.fits && cappedCredit.floorCompetes);
			Assert::AreEqual((4.0 * ps + 2.0 * pd) * 1000.0, cappedCredit.realTotalMs, 1e-9);
			Assert::IsFalse(FitDelayBudget(ps, pd, q, true, capped, FloorPolicy::FloorWins).fits);
		}

		// S1 amendment (TD design (a)4/(b)6): the floor is capped at the desired queue (200 ms at
		// 24p = 4 frames, 3 ahead: a TD lead 3 is given what the queue holds, never a veto); the
		// RaiseQueue choice leaves it uncapped.
		TEST_METHOD(TheFloorIsCappedAtTheDesiredQueueUnlessRaised)
		{
			const double ps = 1.0 / 24.0;
			const size_t q = static_cast<size_t>(std::floor(0.200 / ps + 1e-9));
			Assert::AreEqual(size_t(4), q);
			Assert::AreEqual(size_t(4), VpQueueRules::AnalysisFloorFrames(3, 3, q, false), L"capped at the desired queue");
			Assert::AreEqual(size_t(5), VpQueueRules::AnalysisFloorFrames(3, 3, q, true), L"RaiseQueue: uncapped");
			Assert::AreEqual(size_t(5), VpQueueRules::AnalysisFloorFrames(3, 3, 0, false), L"desired not known: no cap");
			Assert::AreEqual(size_t(3), VpQueueRules::AnalysisFloorFrames(3, 0, q, false), L"the motion floor as today");
			for (size_t m = 0; m <= 8; ++m)
				for (unsigned l = 0; l <= 6; ++l)
					for (size_t d = 0; d <= 10; ++d)
					{
						const size_t capped = VpQueueRules::AnalysisFloorFrames(m, l, d, false);
						const size_t raw = VpQueueRules::AnalysisFloorFrames(m, l);
						Assert::AreEqual(d == 0 ? raw : (std::min)(raw, d), capped);
						Assert::AreEqual(raw, VpQueueRules::AnalysisFloorFrames(m, l, d, true));
					}
			// The capped floor never vetoes the credit by itself under the default policy.
			const BudgetDecision dflt = FitDelayBudget(ps, 1.0 / 60.0, q, true, VpQueueRules::AnalysisFloorFrames(3, 3, q, false),
				FloorPolicy::CreditWins);
			Assert::IsTrue(dflt.fits);
			Assert::AreEqual(size_t(0), dflt.raisedQueueFrames);
		}

		TEST_METHOD(CombineQueueTargetRaiseOffIsTodaysRule)
		{
			for (size_t desired = 0; desired <= 12; ++desired)
				for (size_t credit = 0; credit <= 3; ++credit)
					for (size_t floor = 0; floor <= 14; ++floor)
						for (int fixes = 0; fixes < 2; ++fixes)
						{
							const size_t caps[] = { 0, 8, 16 };
							for (size_t cap : caps)
							{
								const size_t today = VpQueueRules::CombineQueueTarget(desired, credit, floor, fixes != 0);
								Assert::AreEqual(today, VpQueueRules::CombineQueueTarget(desired, credit, floor, fixes != 0, false, cap));
								const size_t raised = VpQueueRules::CombineQueueTarget(desired, credit, floor, fixes != 0, true, cap);
								Assert::IsTrue(raised >= today);
								if (!fixes)
									Assert::AreEqual(today, raised, L"no queue_timing_fixes: no floor, nothing raised");
								else
								{
									const size_t f = cap > 0 ? (std::min)(floor, cap) : floor;
									Assert::AreEqual((std::max)(today, f), raised);
									if (cap > 0)
										Assert::IsTrue(raised <= (std::max)(today, cap), L"never above capacity");
								}
							}
						}
		}

		TEST_METHOD(RaiseQueueKeepsTheCreditAndReportsTheRaisedQueue)
		{
			const double ps = 1.0 / 24.0, pd = 1.0 / 60.0;
			const size_t floorFrames = VpQueueRules::AnalysisFloorFrames(3, 3, 4, true);
			Assert::AreEqual(size_t(5), floorFrames);
			const BudgetDecision today = FitDelayBudget(ps, pd, 4, true);
			const BudgetDecision d = FitDelayBudget(ps, pd, 4, true, floorFrames, FloorPolicy::RaiseQueue);
			Assert::AreEqual(today.fits, d.fits);
			Assert::AreEqual(today.creditFrames, d.creditFrames, L"the credit as today");
			Assert::AreEqual(today.totalMs, d.totalMs);
			Assert::IsTrue(d.floorCompetes);
			Assert::AreEqual(size_t(5), d.raisedQueueFrames);
			Assert::AreEqual(size_t(4), d.queueFrames);
			Assert::AreEqual((5.0 * ps + PlannedLead(ps, pd)) * 1000.0, d.realTotalMs, 1e-9);
			const std::string report = Presenter::FloorReport(d, FloorPolicy::RaiseQueue);
			Assert::IsTrue(report.find("raises the queue above the desired 4 frames") != std::string::npos);
			// Not raised (the floor inside the queue): the credit-wins line.
			const BudgetDecision inside = FitDelayBudget(ps, pd, 4, true, 4, FloorPolicy::RaiseQueue);
			Assert::AreEqual(size_t(0), inside.raisedQueueFrames);
			Assert::IsTrue(Presenter::FloorReport(inside, FloorPolicy::RaiseQueue).find("the credit wins") != std::string::npos);
		}

		// P6-rest audit S1: the two RaiseQueue switches (the queue target's VpFixedBudgetPolicy::SetRaiseQueueForAnalysis and
		// the budget's Presenter::floorPolicy = RaiseQueue) agree: whenever the budget reports a raised queue the queue
		// target is that queue, and the reverse; both are capped at the queue's capacity (0 = no cap), and the reported
		// real delay is the raised queue's.
		TEST_METHOD(TheTwoRaiseQueueSwitchesAgreeOnTheRaisedQueue)
		{
			const double ps = 1.0 / 24.0, pd = 1.0 / 60.0;
			VpQueueTiming::VpFixedBudgetPolicy policy;
			policy.SetRaiseQueueForAnalysis(true);
			Presenter p;
			p.floorPolicy = FloorPolicy::RaiseQueue;
			size_t compared = 0, raisedSeen = 0;
			for (size_t q = 3; q <= 8; ++q)
				for (size_t floorFrames = 0; floorFrames <= 12; ++floorFrames)
					for (size_t cap = 0; cap <= 10; ++cap)
					{
						if (cap > 0 && cap < q)
							continue;   // a capacity is never below the desired queue the host clamps to it
						p.analysisFloor.store(floorFrames);
						p.queueCapacity.store(cap);
						const BudgetDecision d = p.FitBudget(ps, pd, q, true);
						if (d.creditFrames == 0)
							continue;   // the credit path not taken: no queue decision to compare
						QueueTimingPolicy::TargetContext c;
						c.useFrameQueue = true;
						c.capacity = cap;
						c.cadenceScaledDepth = q;
						c.renderAheadCreditFrames = 0;   // the raise compared on the queue the floor holds
						c.lookaheadFloor = floorFrames;
						const size_t target = policy.QueueTarget(c);
						Assert::AreEqual(d.raisedQueueFrames > 0, target > q, L"both raise or neither");
						if (d.raisedQueueFrames > 0)
						{
							Assert::AreEqual(target, d.raisedQueueFrames, L"the same raised queue");
							Assert::IsTrue(cap == 0 || d.raisedQueueFrames <= cap, L"never above capacity");
							Assert::AreEqual((double(d.raisedQueueFrames) * ps + PlannedLead(ps, pd)) * 1000.0, d.realTotalMs, 1e-9,
								L"the real delay is the raised queue's");
							++raisedSeen;
						}
						++compared;
					}
			Assert::IsTrue(compared > 100 && raisedSeen > 10, L"the sweep compared raised and unraised cases");
		}

		// S1 amendment (TD design (b)6): GateCosts' spare-slot GPU ms is additive in the average
		// check, absent from the single-frame check, 0 = today.
		TEST_METHOD(TheSpareSlotCostIsAdditiveInTheAverageOnly)
		{
			GateInput in;
			in.sourcePeriod = 1.0 / 24.0;
			in.displayPeriod = 1.0 / 60.0;
			in.costs.newFrameMs = 10.0;
			in.costs.extraMs = 4.0;
			in.costs.estimateMs = 1.0;
			const double base = FrameCostMs(in, 0);
			const bool fitsBase = Fits(in, 0, in.utilisation);
			Assert::IsTrue(fitsBase);
			in.costs.spareSlotMs = 0.0;
			Assert::AreEqual(base, FrameCostMs(in, 0), L"0 = today exactly");
			in.costs.spareSlotMs = 2.5;
			Assert::AreEqual(base + 2.5, FrameCostMs(in, 0), 1e-12);
			in.costs.spareSlotMs = in.utilisation * 1000.0 * in.sourcePeriod;   // the whole average budget
			Assert::IsFalse(Fits(in, 0, in.utilisation), L"the average check sees it");
			// The single-frame check alone: an average budget so wide only it can fail.
			GateInput single = in;
			single.costs.spareSlotMs = 0.0;
			single.costs.newFrameMs = 60.0;
			const bool a = Fits(single, 0, 100.0);
			single.costs.spareSlotMs = 30.0;
			Assert::AreEqual(a, Fits(single, 0, 100.0), L"the single-frame check does not see it");

			Presenter p;
			PresenterGateFrame f;
			f.sourcePeriod = in.sourcePeriod;
			f.displayPeriod = in.displayPeriod;
			Assert::AreEqual(0.0, p.UpdateGate(1.0, f).input.costs.spareSlotMs, L"default none");
			p.spareSlotMs.store(1.5);
			Assert::AreEqual(1.5, p.UpdateGate(2.0, f).input.costs.spareSlotMs, L"the Presenter's input reaches the gate");
		}

		// Retro gate F2: the Presenter and its epoch key on render_ahead OR TD; TD off is today's rule.
		TEST_METHOD(ThePresenterAndItsEpochKeyOnRenderAheadOrTemporalDetail)
		{
			// TD off: Active and the epoch follow render_ahead alone, as before.
			for (int before = 0; before < 2; ++before)
				for (int now = 0; now < 2; ++now)
				{
					Presenter p;
					Assert::IsTrue(p.epoch.Changed(1, 2.5), L"the first call starts the epoch");
					Assert::IsFalse(p.epoch.Changed(1, 2.5), L"settled epoch");
					const bool on = p.ActivatesOn(before != 0, now != 0);
					Assert::AreEqual(before == 0 && now != 0, on, L"TD off: today's render_ahead switched-on rule");
					Assert::AreEqual(on, p.epoch.Changed(1, 2.5), L"the epoch restarts exactly when the Presenter switches on");
					Assert::AreEqual(now != 0, p.Active(now != 0), L"TD off: Active is render_ahead");
				}

			// TD on with render_ahead off: active, and switching TD on starts the gate again.
			Presenter p;
			p.epoch.Changed(7, 2.5);
			p.SetTemporalDetailOn(true, false);
			Assert::IsTrue(p.Active(false), L"TD on: the Presenter runs without render_ahead");
			Assert::IsTrue(p.epoch.Changed(7, 2.5), L"TD switched on: a new epoch");
			Assert::IsFalse(p.epoch.Changed(7, 2.5));
			// render_ahead switched on while TD is on: the Presenter was already on, the epoch stands.
			Assert::IsFalse(p.ActivatesOn(false, true));
			Assert::IsFalse(p.epoch.Changed(7, 2.5), L"already active: no reset");
			// TD switched on while render_ahead is on: no reset either.
			Presenter q;
			q.epoch.Changed(3, 2.0);
			q.SetTemporalDetailOn(true, true);
			Assert::IsFalse(q.epoch.Changed(3, 2.0), L"render_ahead on already: no reset");
			// TD off again: back to render_ahead alone; the budget veto still wins.
			q.SetTemporalDetailOn(false, true);
			Assert::IsFalse(q.Active(false));
			q.budgetVeto = true;
			Assert::IsFalse(q.Active(true), L"the budget veto applies to both switches");
			q.SetTemporalDetailOn(true, false);
			Assert::IsFalse(q.Active(false), L"the budget veto applies with TD on");
		}
	};
}
