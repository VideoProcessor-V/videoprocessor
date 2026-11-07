// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainGovernor (SID-C2b, doc 78 section 8): the CPU-share governor of the chain estimator worker. Pure arithmetic, no clock of
// its own (the caller passes a monotonic `nowMs`), no threads, deterministic.
//
//   interval = max(nominal, costMs / share)       nominal: 1 s while acquiring, 5 s while LATCHED with power
//   share    = 15 % of one core while acquiring (UNKNOWN / PROVISIONAL / CANDIDATE), 2 % while LATCHED with power
//
// `costMs` is the wall time of one evaluation as the worker measured it; `cold` (operators were built in that evaluation)
// keeps a one-off cold cost from stretching the cadence for good: a warm cost, once seen, replaces the cold one. The interval is
// capped (maxIntervalSec) so a pathological cost cannot stop the estimator for minutes. An evaluation above the per-evaluation
// budget (250 ms) is flagged `overBudget` (the Ctrl+Alt+D text says so: the cadence is stretching, visibly).
#include <algorithm>
#include <cstdint>

namespace SourceIdentity
{
	namespace Chain
	{
		struct GovernorSettings
		{
			double acquiringIntervalSec = 1.0;   // nominal evaluation interval while acquiring
			double latchedIntervalSec = 5.0;     // nominal interval while latched with power
			double acquiringShare = 0.15;        // share of one core while acquiring
			double latchedShare = 0.02;          // share of one core while latched with power
			double budgetEvalMs = 250.0;         // per-evaluation wall time above which the cadence is flagged as stretched
			double maxIntervalSec = 120.0;       // the interval never exceeds this
		};

		struct GovernorPlan
		{
			bool latched = false;                // the latched-with-power regime
			double nominalSec = 0.0;
			double share = 0.0;
			double costMs = 0.0;                 // the cost the interval was computed from
			bool cold = false;                   // that cost is a cold one (no warm evaluation seen yet)
			double intervalSec = 0.0;
			bool stretched = false;              // intervalSec > nominalSec: the budget, not the nominal cadence, sets it
			bool overBudget = false;             // costMs > budgetEvalMs
		};

		class Governor
		{
		public:
			explicit Governor(const GovernorSettings& s = GovernorSettings()) : s_(s) {}

			const GovernorSettings& Settings() const { return s_; }

			static double NominalSec(const GovernorSettings& s, bool latchedWithPower)
			{
				return latchedWithPower ? s.latchedIntervalSec : s.acquiringIntervalSec;
			}
			static double ShareOf(const GovernorSettings& s, bool latchedWithPower)
			{
				const double v = latchedWithPower ? s.latchedShare : s.acquiringShare;
				return v > 1e-6 ? v : 1e-6;
			}
			// The doc 78 formula as a pure function.
			static double IntervalSec(const GovernorSettings& s, bool latchedWithPower, double costMs)
			{
				const double nominal = NominalSec(s, latchedWithPower);
				const double budget = (costMs > 0.0 ? costMs : 0.0) / 1000.0 / ShareOf(s, latchedWithPower);
				const double v = (std::max)(nominal, budget);
				return (std::min)(v, (std::max)(nominal, s.maxIntervalSec));
			}

			// Forget everything (a discontinuity does not need this: the cost of the machine did not change).
			void Reset()
			{
				haveWarm_ = false;
				haveCold_ = false;
				warmMs_ = coldMs_ = 0.0;
				haveStart_ = false;
				lastStartMs_ = 0.0;
				evaluations_ = 0;
			}

			// One finished evaluation: its wall time and whether operators were built in it.
			void NoteCost(double costMs, bool cold)
			{
				if (!(costMs >= 0.0))
					costMs = 0.0;
				++evaluations_;
				if (cold)
				{
					coldMs_ = costMs;
					haveCold_ = true;
				}
				else
				{
					warmMs_ = haveWarm_ ? 0.5 * warmMs_ + 0.5 * costMs : costMs;
					haveWarm_ = true;
				}
			}

			// The cost the interval is computed from: the smoothed warm cost once one has been seen, else the cold one.
			double CostMs() const { return haveWarm_ ? warmMs_ : (haveCold_ ? coldMs_ : 0.0); }
			bool CostIsCold() const { return !haveWarm_ && haveCold_; }
			uint64_t Evaluations() const { return evaluations_; }

			GovernorPlan Plan(bool latchedWithPower) const
			{
				GovernorPlan p;
				p.latched = latchedWithPower;
				p.nominalSec = NominalSec(s_, latchedWithPower);
				p.share = ShareOf(s_, latchedWithPower);
				p.costMs = CostMs();
				p.cold = CostIsCold();
				p.intervalSec = IntervalSec(s_, latchedWithPower, p.costMs);
				p.stretched = p.intervalSec > p.nominalSec + 1e-9;
				p.overBudget = p.costMs > s_.budgetEvalMs;
				return p;
			}

			// The evaluation started at `nowMs` (monotonic milliseconds); the next one is due Plan().intervalSec later.
			void NoteStart(double nowMs)
			{
				haveStart_ = true;
				lastStartMs_ = nowMs;
			}
			bool HasStarted() const { return haveStart_; }
			double LastStartMs() const { return lastStartMs_; }

			// An evaluation is due when none ran yet, or the interval has passed since the last start.
			bool Due(double nowMs, bool latchedWithPower) const
			{
				if (!haveStart_)
					return true;
				return nowMs - lastStartMs_ >= Plan(latchedWithPower).intervalSec * 1000.0;
			}

			// Milliseconds until the next evaluation is due (0 = due now).
			double MsUntilDue(double nowMs, bool latchedWithPower) const
			{
				if (!haveStart_)
					return 0.0;
				const double left = lastStartMs_ + Plan(latchedWithPower).intervalSec * 1000.0 - nowMs;
				return left > 0.0 ? left : 0.0;
			}

		private:
			GovernorSettings s_;
			bool haveWarm_ = false, haveCold_ = false;
			double warmMs_ = 0.0, coldMs_ = 0.0;
			bool haveStart_ = false;
			double lastStartMs_ = 0.0;
			uint64_t evaluations_ = 0;
		};
	}
}
