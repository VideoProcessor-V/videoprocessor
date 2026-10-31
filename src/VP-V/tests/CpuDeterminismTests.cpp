// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// CPU determinism lane: the same seeded synthetic input fed to two fresh instances of a CPU analysis /
// model path gives bit-identical output (floats compared by their bytes), and where the path has a
// reset API, reset-then-replay equals a fresh instance. No clocks, no threads, no random_device.

#include <VP-V/modules/input/queue_timing/PulldownDetector.h>
#include <VP-V/core/DelayBudget.h>
#include <VP-V/core/DelayBudgetLadder.h>
#include <VP-V/nets/NetSha256.h>
#include <VP-V/nets/NetSettingsHash.h>
#include <VP-V/modules/output/presentation/RenderAheadPolicy.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CpuDeterminismTest
{
	namespace
	{
		// Appends the object representation of a scalar (exact bytes, so -0.0 / NaN payloads differ too).
		template <typename T>
		void Put(std::string& out, const T& value)
		{
			static_assert(std::is_arithmetic<T>::value, "scalars only");
			char bytes[sizeof(T)];
			std::memcpy(bytes, &value, sizeof(T));
			out.append(bytes, sizeof(T));
		}

		// ---- Pulldown::Detector + FilmClock ----
		struct Step
		{
			bool repeat = false;
			bool gap = false;
			double jitter = 0.0;   // seconds added to the arrival time
		};

		// 3:2 pulldown (24 in 60) with seeded stray flags and occasional counted gaps.
		std::vector<Step> MakeSteps(unsigned seed, int count)
		{
			std::mt19937 rng(seed);
			std::uniform_int_distribution<int> permille(0, 999);
			std::uniform_real_distribution<double> jitter(-0.002, 0.002);
			static const bool kRepeatPattern[5] = { false, true, false, true, true };
			std::vector<Step> steps(static_cast<size_t>(count));
			for (int i = 0; i < count; ++i)
			{
				Step& s = steps[static_cast<size_t>(i)];
				s.repeat = kRepeatPattern[i % 5];
				const int roll = permille(rng);
				if (roll < 15) s.repeat = !s.repeat;   // 1.5% wrong flags
				s.gap = i > 0 && roll >= 990;           // 1% counted gaps
				s.jitter = jitter(rng);
			}
			return steps;
		}

		constexpr double kSignalHz = 60.0;

		// As the renderer feeds the detector and film clock (see PulldownReliabilityTests::Replay);
		// returns every decision field and clock time as bytes.
		std::string RunPulldown(Pulldown::Detector& detector, Pulldown::FilmClock& clock, const std::vector<Step>& steps)
		{
			std::string out;
			bool wasLocked = false;
			for (size_t i = 0; i < steps.size(); ++i)
			{
				const Step& s = steps[i];
				const Pulldown::Decision d = detector.Next(s.repeat, s.gap, kSignalHz, s.gap ? 1 : -1);
				if (d.relocked || (wasLocked && !d.locked))
					clock.Reset();
				wasLocked = d.locked;
				Put(out, d.drop); Put(out, d.locked); Put(out, d.relocked); Put(out, d.update);
				Put(out, d.filmHz); Put(out, d.cycle); Put(out, d.unique); Put(out, d.filmIndex);
				Put(out, d.lockChanged); Put(out, d.rephased); Put(out, d.discontinuity);
				Put(out, d.cadenceLateness); Put(out, d.cadenceLatenessMax); Put(out, d.phaseUncertain);
				if (d.drop)
					continue;
				Put(out, detector.ConsumeDiscontinuity());
				Put(out, detector.FilmToSignalRatio());
				if (d.locked && !d.update)
				{
					const double arrival = 10.0 + static_cast<double>(i) / kSignalHz + s.jitter;
					Put(out, clock.Time(d.filmIndex, arrival, d.filmHz, d.cadenceLateness, d.cadenceLatenessMax));
				}
			}
			return out;
		}

		// ---- DelayBudget ----
		void PutPlan(std::string& out, const DelayBudget::Plan& p)
		{
			Put(out, p.queueFrames); Put(out, p.feasible); Put(out, p.degraded);
			Put(out, p.fixedMs); Put(out, p.queueMs); Put(out, p.paddingMs); Put(out, p.holdMs);
			Put(out, static_cast<uint64_t>(p.queueTargetFrames)); Put(out, p.holdFraction);
		}

		struct BudgetCase
		{
			double budgetMs, fps, signalFps, refreshHz, renderAheadMs;
			bool captureTransfer;
		};

		std::vector<BudgetCase> MakeBudgetCases(unsigned seed, int count)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> budget(60.0, 600.0), fps(15.0, 120.0), refresh(24.0, 144.0), ahead(0.0, 40.0);
			std::uniform_int_distribution<int> coin(0, 1);
			std::vector<BudgetCase> cases;
			for (double rate : DelayBudget::StandardRates())
				cases.push_back({ 250.0, rate, 0.0, 60.0, 0.0, true });
			for (int i = 0; i < count; ++i)
			{
				BudgetCase c{ budget(rng), fps(rng), 0.0, refresh(rng), ahead(rng), coin(rng) != 0 };
				c.signalFps = coin(rng) != 0 ? fps(rng) : 0.0;
				cases.push_back(c);
			}
			return cases;
		}

		std::string RunBudget(const std::vector<BudgetCase>& cases)
		{
			std::string out;
			for (const BudgetCase& c : cases)
			{
				DelayBudget::Model m;
				m.refreshHz = c.refreshHz;
				m.renderAheadMs = c.renderAheadMs;
				m.captureTransfer = c.captureTransfer;
				const DelayBudget::Inputs in = DelayBudget::InputsFor(c.budgetMs, c.fps, m, c.signalFps);
				Put(out, DelayBudget::FixedCostsMs(in));
				const DelayBudget::Plan plan = DelayBudget::Compute(in);
				PutPlan(out, plan);
				Put(out, DelayBudget::TotalMs(plan));
				Put(out, DelayBudget::BudgetForFrames(4, DelayBudget::StandardRates(), m));
				const std::vector<double> pair = { c.fps, c.fps * 0.5 };
				Put(out, DelayBudget::BudgetForFrames(2, pair, m));
			}
			return out;
		}

		// ---- Net settings hashes ----
		std::string RandomText(std::mt19937& rng, size_t length)
		{
			std::uniform_int_distribution<int> ch(1, 255);
			std::string s;
			for (size_t i = 0; i < length; ++i) s.push_back(static_cast<char>(ch(rng)));
			return s;
		}

		std::string RunHashes(unsigned seed)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> real(0.0, 40.0);
			std::uniform_int_distribution<int> length(0, 300), small(1, 8), coin(0, 1);
			std::string out;
			for (int i = 0; i < 24; ++i)
			{
				out += VpNets::Sha256Hex(RandomText(rng, static_cast<size_t>(length(rng))));

				VpNets::RebaseHashSettings r;
				r.rebaseKernelMatch = coin(rng) != 0 ? "on" : "off";
				r.perAxisInverse = coin(rng) != 0;
				r.oneDKernel = "pl_filter_lanczos r" + std::to_string(small(rng));
				VpNets::GrainSplitHashSettings g;
				g.epsCodes = real(rng);
				g.r2160 = small(rng);
				VpNets::BoundsHashSettings b;
				b.b0 = real(rng); b.b1 = real(rng); b.b2 = real(rng); b.bmax = real(rng);
				b.c0 = real(rng); b.c1 = real(rng); b.cmax = real(rng);

				out += VpNets::RebaseSettingsHash(r);
				out += VpNets::GrainSplitHash(g);
				out += VpNets::BoundsHash(b);
				out += VpNets::StageOrderHash(RandomText(rng, static_cast<size_t>(length(rng))));
			}
			return out;
		}

		// ---- Render-ahead policy (RenderAhead::, pure logic, explicit times) ----
		void PutText(std::string& out, const char* text)
		{
			const std::string s = text ? text : "";
			Put(out, static_cast<uint64_t>(s.size()));
			out += s;
		}

		// Stateless planner and helpers over seeded rates, phases and costs.
		std::string RunRenderAheadPlans(unsigned seed, int count)
		{
			using namespace RenderAhead;
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> srcHz(15.0, 120.0), dispHz(24.0, 144.0), film(0.0, 100.0),
				floorL(0.0, 0.2), offset(0.0, 0.1), strength(0.0, 1.0), jitter(0.0, 0.01);
			std::uniform_int_distribution<int> evenPick(0, 3), drops(0, 3), queue(0, 12);
			std::uniform_int_distribution<int> coin(0, 1);
			std::string out;
			for (int i = 0; i < count; ++i)
			{
				FrameInput in;
				in.sourcePeriod = 1.0 / srcHz(rng);
				in.displayPeriod = 1.0 / dispHz(rng);
				const double ratio = in.sourcePeriod / in.displayPeriod;
				in.filmTime = film(rng);
				in.latencyFloor = floorL(rng);
				in.firstVsync = in.filmTime + in.latencyFloor + offset(rng);
				in.lead = PlannedLead(in.sourcePeriod, in.displayPeriod);
				in.strength = strength(rng);
				const int even[4] = { 0, 0, 2, 3 };
				in.evenCycle = even[evenPick(rng)];
				in.maxExtras = MaxExtras(ratio);
				in.dropExtras = drops(rng);
				in.arrivalJitter = jitter(rng);

				const FramePlan plan = PlanFrame(in);
				Put(out, plan.valid); Put(out, plan.refreshes); Put(out, plan.nextInterval);
				Put(out, plan.nextFirstVsync); Put(out, plan.phi0); Put(out, plan.rendered); Put(out, plan.dropped);
				Put(out, static_cast<uint64_t>(plan.slots.size()));
				for (const Slot& s : plan.slots)
				{
					Put(out, s.vsync); Put(out, s.delta); Put(out, s.shift);
					Put(out, s.render); Put(out, s.dropped); Put(out, s.interval);
				}
				Put(out, PhaseTolerance(in));

				const size_t depth = static_cast<size_t>(queue(rng));
				Put(out, SwapchainBuffers(ratio)); Put(out, in.lead);
				Put(out, static_cast<uint64_t>(CreditFrames(in.lead, in.sourcePeriod)));
				Put(out, static_cast<uint64_t>(QueueCredit(in.lead, in.sourcePeriod, depth)));
				const BudgetDecision b = FitDelayBudget(in.sourcePeriod, in.displayPeriod, depth, coin(rng) != 0);
				Put(out, b.fits); Put(out, static_cast<uint64_t>(b.creditFrames)); Put(out, b.leadMs);
				Put(out, b.budgetMs); Put(out, b.totalMs); PutText(out, b.reason);
				const ChainDepth cd = ChainDepthFor(coin(rng) != 0, coin(rng) != 0, ratio);
				Put(out, cd.buffers); Put(out, cd.latency);
				Put(out, ExtrasPerFrame(ratio, drops(rng))); Put(out, MaxExtrasPerFrame(ratio, drops(rng)));
			}
			return out;
		}

		// Schedule (anchor, chain, lateness, arrival jitter) + CreditLatch driven by seeded film times
		// and simulated presents. Cumulative diagnostics counters are serialised as deltas from the
		// start of the run (Reset does not clear them).
		std::string RunRenderAheadSchedule(RenderAhead::Schedule& sch, RenderAhead::CreditLatch& latch, unsigned seed, int frames)
		{
			using namespace RenderAhead;
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> srcHz(20.0, 60.0), dispHz(50.0, 75.0), jit(-0.002, 0.002), early(0.0, 0.03);
			std::uniform_int_distribution<int> permille(0, 999), coin(0, 1), evenPick(0, 3), errPick(0, 5);
			const double ps = 1.0 / srcHz(rng), pd = 1.0 / dispHz(rng);
			const double ratio = ps / pd;
			const double lead = PlannedLead(ps, pd);
			const unsigned long long early0 = sch.EarlyCorrections(), reanchors0 = sch.Reanchors(),
				lowered0 = sch.AnchorsLowered(), observed0 = sch.ObservedCount();
			const double errors[6] = { 0.0, 0.0, 0.0, 1.0, -1.0, 2.0 };
			const int even[4] = { 0, 0, 2, 3 };
			std::string out;
			uint32_t presentId = 100;
			double film = 5.0;
			for (int i = 0; i < frames; ++i)
			{
				const int roll = permille(rng);
				film += ps + jit(rng);
				if (roll < 20) film += 0.7 * ps;   // a dropped source frame
				const double now = film + 0.01;
				Put(out, sch.NoteArrival(film, ps));

				const double earliest = now + early(rng);
				const double grid = coin(rng) != 0 ? 100.0 : 0.0;
				bool restart = false, behind = false;
				const double first = sch.FirstVsync(earliest, pd, grid, restart, behind, 0.5);
				const bool tooLate = sch.AnchorTooLate(first, film, lead, ps);
				if (tooLate) restart = true;
				const double floorL = sch.AnchorLatency(restart, first, film, restart && coin(rng) != 0 ? lead : 0.0);
				Put(out, first); Put(out, restart); Put(out, behind); Put(out, tooLate); Put(out, floorL);

				FrameInput in;
				in.filmTime = film; in.sourcePeriod = ps; in.displayPeriod = pd; in.firstVsync = first;
				in.latencyFloor = floorL; in.lead = lead; in.strength = 0.8;
				in.evenCycle = even[evenPick(rng)];
				in.maxExtras = MaxExtras(ratio);
				in.dropExtras = sch.TakeLateDrops();
				in.arrivalJitter = sch.ArrivalJitter();
				const FramePlan plan = PlanFrame(in);
				Put(out, plan.valid); Put(out, plan.refreshes); Put(out, plan.rendered); Put(out, plan.dropped);
				Put(out, plan.nextInterval); Put(out, plan.nextFirstVsync); Put(out, plan.phi0);

				const uint32_t ownId = presentId;
				if (plan.valid)
				{
					std::vector<std::pair<uint32_t, double>> planned;
					planned.push_back(std::make_pair(presentId++, first));
					for (const Slot& s : plan.slots)
						if (s.render)
							planned.push_back(std::make_pair(presentId++, s.vsync));
					sch.Commit(plan.nextFirstVsync, planned);
				}
				Put(out, static_cast<uint64_t>(latch.Commit(lead, ps, ratio)));

				if (roll >= 980)
				{
					sch.Break();
					Put(out, static_cast<uint64_t>(latch.Break(now)));
				}
				else
				{
					Put(out, sch.Observe(ownId, first + errors[errPick(rng)] * pd, pd));
					if (roll >= 940)
						Put(out, sch.NoteLate(1, pd));
				}
				Put(out, sch.Active()); Put(out, sch.Anchored()); Put(out, sch.NextVsync());
				Put(out, static_cast<uint64_t>(sch.Pending())); Put(out, sch.ArrivalJitter());
				Put(out, static_cast<uint64_t>(latch.Frames()));
			}
			Put(out, sch.EarlyCorrections() - early0); Put(out, sch.Reanchors() - reanchors0);
			Put(out, sch.AnchorsLowered() - lowered0); Put(out, sch.ObservedCount() - observed0);
			return out;
		}

		// Gate ladder, cost windows, sustained lateness and epoch, stepped on an explicit time axis.
		std::string RunRenderAheadGate(RenderAhead::Gate& gate, RenderAhead::KnownCost& newCost, RenderAhead::KnownCost& extraCost,
			RenderAhead::SustainedLateness& sustained, RenderAhead::GateEpoch& epoch, unsigned seed, int steps)
		{
			using namespace RenderAhead;
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> srcHz(22.0, 32.0), dispHz(55.0, 65.0), baseN(2.0, 30.0), baseX(1.0, 12.0),
				saving(0.0, 4.0), unit(0.0, 1.0);
			std::uniform_int_distribution<int> permille(0, 999);
			const double ps = 1.0 / srcHz(rng), pd = 1.0 / dispHz(rng);
			const double n0 = baseN(rng), x0 = baseX(rng);
			std::string out;
			uint64_t generation = 1;
			for (int i = 0; i < steps; ++i)
			{
				const double now = 10.0 + 0.05 * i;
				const double scale = (i / 200) % 2 == 0 ? 0.5 : 4.0;   // cheap and expensive phases
				const int roll = permille(rng);
				if (roll < 4) ++generation;
				const bool changed = epoch.Changed(generation, ps / pd);
				Put(out, changed);
				if (changed)
				{
					gate.Reset(); newCost.Reset(); extraCost.Reset(); sustained.Reset();
				}
				if (roll % 3 != 0)
				{
					newCost.Add(now, n0 * scale * (0.8 + 0.4 * unit(rng)));
					extraCost.Add(now, x0 * scale * (0.8 + 0.4 * unit(rng)));
				}
				newCost.Trim(now); extraCost.Trim(now);
				Put(out, newCost.Current()); Put(out, newCost.Known()); Put(out, static_cast<uint64_t>(newCost.Count()));
				Put(out, extraCost.Current()); Put(out, extraCost.Known());

				const bool l1 = (generation % 2) == 0;
				const GateCosts costs = ExtraCosts(extraCost.Current(), l1 ? extraCost.Current() * 0.6 : 0.0, saving(rng), l1);
				GateInput in;
				in.sourcePeriod = ps; in.displayPeriod = pd; in.l1Available = l1;
				in.costs = costs;
				in.costs.newFrameMs = newCost.Current();
				in.costs.estimateMs = 0.5;
				const bool late = roll >= 990 && sustained.Note(now);
				const GateChange c = gate.Update(now, in, late);
				Put(out, late); Put(out, c.changed); Put(out, c.from); Put(out, c.to); PutText(out, c.why);
				Put(out, gate.Level());
				for (int level = 0; level < 4; ++level)
				{
					Put(out, FrameCostMs(in, level)); Put(out, Fits(in, level, in.utilisation));
				}
				PutText(out, LevelName(gate.Level(), MaxExtras(ps / pd)).c_str());
				Put(out, RefreshGateHolds(newCost.Current() + costs.extraMs, pd));
				Put(out, ResolveGate(roll % 2 == 0 ? "refresh" : "period", roll % 5 != 0) == GateMode::Refresh);
			}
			return out;
		}
	}

	TEST_CLASS(CpuDeterminismTests)
	{
	public:
		TEST_METHOD(PulldownDetectorSameInputGivesIdenticalDecisions)
		{
			const std::vector<Step> steps = MakeSteps(20261031u, 4000);

			Pulldown::Detector a, b;
			Pulldown::FilmClock clockA, clockB;
			const std::string first = RunPulldown(a, clockA, steps);
			const std::string second = RunPulldown(b, clockB, steps);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first == second, L"two fresh detectors disagree on the same sequence");

			// Reset-then-replay equals a fresh instance, even after the detector saw other input.
			Pulldown::Detector used;
			Pulldown::FilmClock usedClock;
			RunPulldown(used, usedClock, MakeSteps(7u, 1500));
			used.Reset();
			usedClock.Reset();
			const std::string replay = RunPulldown(used, usedClock, steps);
			Assert::IsTrue(first == replay, L"reset-then-replay differs from a fresh detector");
		}

		TEST_METHOD(DelayBudgetPlanSameInputGivesIdenticalBytes)
		{
			const std::vector<BudgetCase> cases = MakeBudgetCases(424242u, 400);
			const std::string first = RunBudget(cases);
			const std::string second = RunBudget(cases);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first.size() == second.size() && std::memcmp(first.data(), second.data(), first.size()) == 0,
				L"the delay plan is not bit-identical across two runs");
		}

		TEST_METHOD(NetSettingsHashesSameInputGiveIdenticalHashes)
		{
			const std::string first = RunHashes(99u);
			const std::string second = RunHashes(99u);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first == second, L"net settings hashes differ across two runs of the same seeded input");
			Assert::IsTrue(first != RunHashes(100u), L"a different seed should give different hashes");
		}

		TEST_METHOD(RenderAheadPlannerSameInputGivesIdenticalBytes)
		{
			const std::string first = RunRenderAheadPlans(31u, 600);
			const std::string second = RunRenderAheadPlans(31u, 600);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first.size() == second.size() && std::memcmp(first.data(), second.data(), first.size()) == 0,
				L"the render-ahead planner is not bit-identical across two runs");
			Assert::IsTrue(first != RunRenderAheadPlans(32u, 600), L"a different seed should give different plans");
		}

		TEST_METHOD(RenderAheadScheduleSameInputGivesIdenticalBytesAndResetReplays)
		{
			RenderAhead::Schedule a, b;
			RenderAhead::CreditLatch latchA, latchB;
			const std::string first = RunRenderAheadSchedule(a, latchA, 5150u, 1500);
			const std::string second = RunRenderAheadSchedule(b, latchB, 5150u, 1500);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first == second, L"two fresh schedules disagree on the same sequence");

			// Reset-then-replay equals a fresh instance, even after other input.
			RenderAhead::Schedule used;
			RenderAhead::CreditLatch usedLatch;
			RunRenderAheadSchedule(used, usedLatch, 77u, 900);
			used.Reset();
			usedLatch.Reset();
			const std::string replay = RunRenderAheadSchedule(used, usedLatch, 5150u, 1500);
			Assert::IsTrue(first == replay, L"reset-then-replay differs from a fresh schedule");

			RenderAhead::Schedule other;
			RenderAhead::CreditLatch otherLatch;
			Assert::IsTrue(first != RunRenderAheadSchedule(other, otherLatch, 5151u, 1500), L"a different seed should give different output");
		}

		TEST_METHOD(RenderAheadGateSameInputGivesIdenticalBytesAndResetReplays)
		{
			RenderAhead::Gate gateA, gateB;
			RenderAhead::KnownCost newA, extraA, newB, extraB;
			RenderAhead::SustainedLateness lateA, lateB;
			RenderAhead::GateEpoch epochA, epochB;
			const std::string first = RunRenderAheadGate(gateA, newA, extraA, lateA, epochA, 808u, 2000);
			const std::string second = RunRenderAheadGate(gateB, newB, extraB, lateB, epochB, 808u, 2000);
			Assert::IsTrue(!first.empty());
			Assert::IsTrue(first == second, L"two fresh gates disagree on the same sequence");

			RenderAhead::Gate usedGate;
			RenderAhead::KnownCost usedNew, usedExtra;
			RenderAhead::SustainedLateness usedLate;
			RenderAhead::GateEpoch usedEpoch;
			RunRenderAheadGate(usedGate, usedNew, usedExtra, usedLate, usedEpoch, 12u, 1200);
			usedGate.Reset(); usedNew.Reset(); usedExtra.Reset(); usedLate.Reset(); usedEpoch.Invalidate();
			const std::string replay = RunRenderAheadGate(usedGate, usedNew, usedExtra, usedLate, usedEpoch, 808u, 2000);
			Assert::IsTrue(first == replay, L"reset-then-replay differs from a fresh gate");

			RenderAhead::Gate otherGate;
			RenderAhead::KnownCost otherNew, otherExtra;
			RenderAhead::SustainedLateness otherLate;
			RenderAhead::GateEpoch otherEpoch;
			Assert::IsTrue(first != RunRenderAheadGate(otherGate, otherNew, otherExtra, otherLate, otherEpoch, 809u, 2000),
				L"a different seed should give different output");
		}
	};
}
