// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// END-TO-END DELAY GATE, CPU tier (settings agent, 2026-10-24; owner decision 2026-10-24).
//
// The owner sets ONE total delay, the same at every frame rate (padded). 4 queued frames is the
// recommended setting (about 267 ms at 24p on 60 Hz); shorter settings fall back to 3 and 2 frames with
// every feature still working. This gate checks, at 23.976/24/25/29.97/30/50/59.94/60:
//   1. the planner (DelayBudget.h): the 4/3/2-frame budgets give at least that many frames at
//      every rate and the SAME total at every rate (the padding absorbs the rest);
//   2. the queue (QueueStressHarness: VP's real CPU queue/cadence/render-ahead logic on a
//      simulated clock) really holds the planned frames: the measured steady queue time plus
//      the fixed costs and the padding lands on the budget within one source period and two
//      display refreshes, with render-ahead off and on, and no invariant failure (no stall).
//
// Finding 2026-10-24 (settings agent): the queue's measured hold was about one source frame
// above the planned frames, so the total differed across rates (266-315 ms for 250 ms).
// B2 wiring (skeleton lead, 2026-10-24): the queue now realises DelayBudget's hold - target =
// queueTargetFrames, the sub-frame rest via the steering (holdFraction) - and the gate measures
// the total directly. VP_DELAY_GATE_STRICT=1: render_ahead off must give the same total at
// every rate (within one refresh) and equal the budget; render_ahead on is reported (PENDING-RA).
// Parameters (owner decision pending on the exact value, so nothing is fixed here):
//   VP_DELAY_BUDGETS_MS   extra budgets to gate, e.g. "200,250" (the 4/3/2 ladder always runs)
//   VP_DELAY_DISPLAY_HZ   the display refresh (default 60, the FL35 mode)
//   VP_DELAY_PROCESSING_MS  processing allowance in the fixed costs (default 8)
// The live renderer gate (captureToPresentationTargetMs on the GPU) is the next tier; see
// docs/design/test-structure.md.

#include <VP-V/core/DelayBudgetLadder.h>

#include "../../VideoProcessor-Test/QueueStressHarness.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DelayGateTest
{
	namespace
	{
		double EnvDouble(const char* name, double fallback)
		{
			char* value = nullptr;
			size_t length = 0;
			if (_dupenv_s(&value, &length, name) != 0 || !value) return fallback;
			const double parsed = std::atof(value);
			free(value);
			return parsed > 0.0 ? parsed : fallback;
		}

		std::vector<double> EnvList(const char* name)
		{
			std::vector<double> list;
			char* value = nullptr;
			size_t length = 0;
			if (_dupenv_s(&value, &length, name) != 0 || !value) return list;
			std::stringstream in(value);
			free(value);
			std::string item;
			while (std::getline(in, item, ','))
				if (std::atof(item.c_str()) > 0.0) list.push_back(std::atof(item.c_str()));
			return list;
		}

		void Log(const std::string& text) { Logger::WriteMessage(text.c_str()); }

		struct Budget { std::string name; double ms; int ladderFrames; };

		std::vector<Budget> Budgets(const std::vector<double>& rates, const DelayBudget::Model& o)
		{
			std::vector<Budget> list;
			for (int frames : { 4, 3, 2 })
				list.push_back({ std::to_string(frames) + "-frame", DelayBudget::BudgetForFrames(frames, rates, o), frames });
			for (double ms : EnvList("VP_DELAY_BUDGETS_MS"))
				list.push_back({ "configured", ms, 0 });
			return list;
		}
	}

	TEST_CLASS(DelayGate)
	{
	public:
		TEST_METHOD(PlannerLadderConstantTotalAtEveryRate)
		{
			const double displayHz = EnvDouble("VP_DELAY_DISPLAY_HZ", 60.0);
			DelayBudget::Model o;
			o.refreshHz = displayHz;
			o.processingMs = EnvDouble("VP_DELAY_PROCESSING_MS", o.processingMs);
			const auto rates = DelayBudget::StandardRates();
			std::ostringstream table;
			for (const Budget& b : Budgets(rates, o))
			{
				table << b.name << " budget " << b.ms << " ms:";
				for (double fps : rates)
				{
					const DelayBudget::Plan p = DelayBudget::Compute(DelayBudget::InputsFor(b.ms, fps, o));
					table << "  " << fps << "p=" << p.queueFrames << "f+" << static_cast<int>(p.paddingMs) << "ms";
					if (b.ladderFrames > 0)
						Assert::IsTrue(p.queueFrames >= b.ladderFrames, L"a ladder budget gives its frames at every rate");
					if (p.feasible)
						Assert::AreEqual(b.ms, DelayBudget::TotalMs(p), 1e-6, L"the total delay is the same at every rate");
				}
				table << "\n";
			}
			Log(table.str());
			// The recommended value the owner sees (4 frames at the slowest rate).
			// 2 swapchain refreshes + the display phase (1 refresh) + capture transfer + 8 ms + 4 frames.
			Assert::AreEqual(267.0, DelayBudget::BudgetForFrames(4, DelayBudget::StandardRates()), 1e-9,
				L"4 frames at 23.976 on 60 Hz recommend 267 ms");
		}

		TEST_METHOD(QueueHoldsThePlannedDelayAtEveryRate)
		{
			using namespace QueueStress;
			const double displayHz = EnvDouble("VP_DELAY_DISPLAY_HZ", 60.0);
			DelayBudget::Model o;
			o.refreshHz = displayHz;
			o.processingMs = EnvDouble("VP_DELAY_PROCESSING_MS", o.processingMs);
			const auto rates = DelayBudget::StandardRates();
			std::ostringstream report;
			std::string failures;
			const bool strict = EnvDouble("VP_DELAY_GATE_STRICT", 0.0) > 0.0;
			for (const Budget& b : Budgets(rates, o))
			{
				// [0] render_ahead off (the default: strict), [1] render_ahead on (reported).
				double lowTotal[2] = { 1e9, 1e9 }, highTotal[2] = { 0.0, 0.0 };
				for (double fps : rates)
				{
					const DelayBudget::Plan plan = DelayBudget::Compute(DelayBudget::InputsFor(b.ms, fps, o));
					if (!plan.feasible) continue;
					for (int ra = 0; ra < 2; ++ra)
					{
						Scenario s;
						s.name = "delay-gate";
						s.seed = 1;
						// Long enough for the timeline's steering to settle the sub-frame rest (a slow
						// proportional trim, ~1 minute time constant for a large rest at 30p).
						s.seconds = 240.0;
						s.signalHz = fps;
						s.pictureHz = fps;
						s.displayHz = displayHz;
						s.sw.renderAhead = ra == 1;
						RunOptions options;
						// B2 wiring (skeleton lead, 2026-10-24): the queue realises the plan. After a
						// dequeue it keeps queueTargetFrames frames (the look-ahead plus the whole frames
						// of the padding) and holdFraction more on average (the timeline's steering), so
						// each frame waits holdMs = budget - fixed costs. (The first mapping, look-ahead
						// + 1 with the padding added on paper, measured one source frame too many.)
						options.present.normalTarget = plan.queueTargetFrames;
						options.present.holdFraction = plan.holdFraction;
						const Result result = Run(s, options);
						const double P = 1000.0 / fps, Pd = 1000.0 / displayHz;
						const double queueMs = result.stats.maxSteadyLatency * 1000.0;
						// The padding is inside the measured hold; so is the display phase (the measured
						// sample is the worst case, at its vblank), which the plan counts as a fixed cost.
						const double total = plan.fixedMs - DelayBudget::InputsFor(b.ms, fps, o).displayPhaseRefreshes * Pd + queueMs;
						lowTotal[ra] = (std::min)(lowTotal[ra], total);
						highTotal[ra] = (std::max)(highTotal[ra], total);
						report << b.name << " " << b.ms << " ms @" << fps << (ra ? " RA" : "   ") << ": frames "
							<< plan.queueFrames << " (target " << plan.queueTargetFrames << " +" << plan.holdFraction << ") queue " << queueMs << " ms (hold " << plan.holdMs
							<< ") total " << total << "\n";
						const double tolerance = P + 2.0 * Pd;
						if (ra == 0 && (queueMs < plan.holdMs - Pd || queueMs > plan.holdMs + tolerance))
							failures += b.name + " @" + std::to_string(fps) + ": queue " + std::to_string(queueMs) +
								" ms vs planned hold " + std::to_string(plan.holdMs) + "; ";
						// The total equals the budget: never below it by more than half a refresh, never
						// above by more than a refresh (the measured sample is the worst case, shown at the
						// next vblank).
						if (ra == 0 && strict && (total < b.ms - Pd / 2.0 || total > b.ms + Pd))
							failures += b.name + " @" + std::to_string(fps) + ": total " + std::to_string(total) +
								" ms vs budget " + std::to_string(b.ms) + "; ";
						for (const Failure& f : result.failures)
							failures += b.name + " @" + std::to_string(fps) + (ra ? " RA" : "") + ": " + f.invariant + " " + f.detail + "; ";
					}
				}
				const double spreadTolerance = 1000.0 / displayHz;
				for (int ra = 0; ra < 2; ++ra)
				{
					report << "  " << b.name << (ra ? " render_ahead on" : " render_ahead off") << " total spread across rates: "
						<< lowTotal[ra] << " .. " << highTotal[ra] << " ms\n";
					if (highTotal[ra] - lowTotal[ra] <= spreadTolerance)
						continue;
					const std::string note = b.name + (ra ? " (render_ahead on)" : "") + ": total spread " +
						std::to_string(highTotal[ra] - lowTotal[ra]) + " ms across rates (tolerance " +
						std::to_string(spreadTolerance) + " ms)";
					// render_ahead off (the default) is the strict gate. render_ahead on moves latency
					// between the queue and its planned lead in display refreshes; its constant total
					// is a follow-up (PENDING-RA), reported here.
					if (ra == 0 && strict) failures += note + "; ";
					else report << (ra ? "  PENDING-RA " : "  PENDING-B2 ") << note << "\n";
				}
			}
			Log(report.str());
			Assert::IsTrue(failures.empty(), std::wstring(failures.begin(), failures.end()).c_str());
		}
	};
}
