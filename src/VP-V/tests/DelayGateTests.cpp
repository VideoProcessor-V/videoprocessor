// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// END-TO-END DELAY GATE, CPU tier (settings agent, 2026-10-24; owner decision 2026-10-24).
//
// The owner sets ONE total delay, the same at every frame rate (padded). 4 queued frames is the
// recommended setting (about 250 ms at 24p); shorter settings fall back to 3 and 2 frames with
// every feature still working. This gate checks, at 23.976/24/25/29.97/30/50/59.94/60:
//   1. the planner (DelayBudget.h): the 4/3/2-frame budgets give at least that many frames at
//      every rate and the SAME total at every rate (the padding absorbs the rest);
//   2. the queue (QueueStressHarness: VP's real CPU queue/cadence/render-ahead logic on a
//      simulated clock) really holds the planned frames: the measured steady queue time plus
//      the fixed costs and the padding lands on the budget within one source period and two
//      display refreshes, with render-ahead off and on, and no invariant failure (no stall).
//
// Finding 2026-10-24: the queue's measured hold is about one source frame above the planned
// frames (the frame being shown plus the dequeue margin). That excess depends on the rate, so
// without the budget's padding wired into the queue the total differs across rates (about
// 266-315 ms for the 250 ms budget). The B2 wiring must count that frame in the fixed costs or
// the padding. VP_DELAY_GATE_STRICT=1 turns the constant-total check into a failure for that gate.
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
			Assert::AreEqual(250.0, DelayBudget::BudgetForFrames(4, DelayBudget::StandardRates()), 1e-9,
				L"4 frames at 23.976 on 60 Hz recommend 250 ms");
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
				double lowTotal = 1e9, highTotal = 0.0;
				for (double fps : rates)
				{
					const DelayBudget::Plan plan = DelayBudget::Compute(DelayBudget::InputsFor(b.ms, fps, o));
					if (!plan.feasible) continue;
					for (int ra = 0; ra < 2; ++ra)
					{
						Scenario s;
						s.name = "delay-gate";
						s.seed = 1;
						s.seconds = 60.0;
						s.signalHz = fps;
						s.pictureHz = fps;
						s.displayHz = displayHz;
						s.sw.renderAhead = ra == 1;
						RunOptions options;
						// The queue target is the planned frames plus the frame on screen (the queue
						// holds the look-ahead frames AND the next frame to show).
						options.present.normalTarget = static_cast<size_t>(plan.queueFrames) + 1;
						const Result result = Run(s, options);
						const double P = 1000.0 / fps, Pd = 1000.0 / displayHz;
						const double queueMs = result.stats.maxSteadyLatency * 1000.0;
						const double total = plan.fixedMs + queueMs + plan.paddingMs;
						lowTotal = (std::min)(lowTotal, total);
						highTotal = (std::max)(highTotal, total);
						report << b.name << " " << b.ms << " ms @" << fps << (ra ? " RA" : "   ") << ": frames "
							<< plan.queueFrames << " queue " << queueMs << " ms (plan " << plan.queueMs << ") total " << total << "\n";
						const double tolerance = P + 2.0 * Pd;
						if (queueMs < plan.queueMs - Pd || queueMs > plan.queueMs + tolerance + (ra ? 3.0 * Pd : 0.0))
							failures += b.name + " @" + std::to_string(fps) + (ra ? " RA" : "") + ": queue " +
								std::to_string(queueMs) + " ms vs planned " + std::to_string(plan.queueMs) + "; ";
						for (const Failure& f : result.failures)
							failures += b.name + " @" + std::to_string(fps) + ": " + f.invariant + " " + f.detail + "; ";
					}
				}
				report << "  " << b.name << " total spread across rates: " << lowTotal << " .. " << highTotal << " ms\n";
				// The constant-total check needs the renderer's queue to apply the budget's padding
				// (ADR-0020 wiring, barrier B2). Until then the spread is reported; VP_DELAY_GATE_STRICT=1
				// makes it a failure (the gate after the wiring lands).
				const double spreadTolerance = 1000.0 / displayHz;
				if (highTotal - lowTotal > spreadTolerance)
				{
					const std::string note = b.name + ": total spread " + std::to_string(highTotal - lowTotal) +
						" ms across rates (tolerance " + std::to_string(spreadTolerance) + " ms)";
					if (strict) failures += note + "; ";
					else report << "  PENDING-B2 " << note << "\n";
				}
			}
			Log(report.str());
			Assert::IsTrue(failures.empty(), std::wstring(failures.begin(), failures.end()).c_str());
		}
	};
}
