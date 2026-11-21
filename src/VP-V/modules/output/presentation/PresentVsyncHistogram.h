// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CG-PVH (CG-c1 runbook s8; S-3f c1, retro gate F1 / S4): the present-vs-vsync histogram.
//
// Pure and testable with no GPU, no windows.h and no logging. The presentation module's R12 AfterPresent
// handler (PresenterExtension::OnAfterPresent) feeds it the DXGI frame statistics that PresenterAfterPresent
// already read (no second GetFrameStatistics) and logs one line per window. It is log-only: nothing it
// computes reaches a present, so the picture stays byte-identical.
//
// What it measures, per distinct present seen (consecutive statistics with different PresentCount):
//  - HOLD k = the refresh-count delta (PresentRefreshCount now - before) when the PresentCount step is 1: how many
//    measured display periods the previous present stayed on screen. An exact integer that needs no period.
//    Bins 0, 1, 2 and 3+. At matched rates k is 1; a repeat is 2; a longer stall is 3+. A step of more than 1
//    (several presents between two reads) cannot be split into holds: the presents that cannot have taken a
//    refresh each (step - refresh delta) are "never shown" and go in bin 0 (the spec skip: ids 19 -> 21 over one
//    refresh); the rest are counted as "unresolved" in the line, never in a hold bin.
//  - LEAD L = (the present's display time - the time its swap ended) / T, bins [0,1) [1,2) [2,3) [3,4) 4+, with
//    p1 / p50 / p99. This is the lead the S4 plan needs ("release N+1 one display period + margin before the
//    planned vsync"). Informative; it has no pass threshold. A negative lead is shown ("negative n"), in no bin.
// A present's display time is the statistics' SyncQPCTime moved back by (SyncRefreshCount - PresentRefreshCount)
// measured periods.
//
// T (the measured refresh period, for the lead and the reported fd) is DERIVED, never fixed: the ratio of the
// SyncQPCTime and SyncRefreshCount advances over the statistics seen so far (the sums are halved so it follows a
// mode change). Until at least kMinRefreshesForPeriod refreshes were seen T is unknown: the holds are still
// binned, the leads are counted as "warm-up" and not binned.
//
// No statistics is said, never hidden (rule 5): the line reads "no stats (hr=0x........)" with the number of
// reads, and a window with some statistics reports how many reads lacked them. An unavailable window is
// never a pass.
//
// C++14. Render thread only (one instance per renderer).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Presentation
	{
		// One GetFrameStatistics read (the module maps RenderAhead::PresentStatistics onto this).
		struct PresentVsyncSample
		{
			bool read = false;        // a swapchain was there to ask
			bool available = false;   // the read succeeded
			bool disjoint = false;    // DXGI_ERROR_FRAME_STATISTICS_DISJOINT: the counters restarted
			int32_t hr = 0;           // the read's result code (shown when unavailable)
			uint32_t presentCount = 0, presentRefreshCount = 0, syncRefreshCount = 0;
			int64_t syncQpc = 0;
		};

		class PresentVsyncHistogram
		{
		public:
			static constexpr int kHoldBins = 4;          // 0, 1, 2, 3+
			static constexpr int kLeadBins = 5;          // [0,1) [1,2) [2,3) [3,4) 4+
			static constexpr size_t kRing = 64;          // submitted presents remembered (id -> swap end)
			static constexpr size_t kMaxLeadSamples = 4096;
			static constexpr uint32_t kMinRefreshesForPeriod = 8;   // refreshes seen before T is trusted (a sample count, not a rate)
			static constexpr double kReportSeconds = 10.0;

			// The present made at swap end `swapEndQpc` has id `presentId` (the swapchain's present count after it).
			void NoteSubmitted(uint32_t presentId, int64_t swapEndQpc)
			{
				if (!ring_.empty() && ring_.back().first == presentId)
					return;   // the same id again: no new present was made, the first swap end stands
				ring_.emplace_back(presentId, swapEndQpc);
				if (ring_.size() > kRing)
					ring_.erase(ring_.begin());
			}

			void Observe(const PresentVsyncSample& s)
			{
				++reads_;
				if (!s.read)
				{
					++noSwapchain_;
					return;
				}
				if (s.disjoint)
				{
					++disjoints_;
					havePrev_ = false;
					havePeriodPrev_ = false;
					ring_.clear();   // the ids restart
					return;
				}
				if (!s.available)
				{
					++unavailable_;
					lastHr_ = s.hr;
					return;
				}
				++available_;

				// The measured period: every read whose refresh counter advanced.
				if (havePeriodPrev_)
				{
					const uint32_t dRef = s.syncRefreshCount - periodPrevRef_;
					const int64_t dQpc = s.syncQpc - periodPrevQpc_;
					if (static_cast<int32_t>(dRef) < 0 || dQpc < 0)
					{
						sumRef_ = 0.0;   // counters went backwards: restart the estimate
						sumQpc_ = 0.0;
					}
					else if (dRef > 0 && dQpc > 0 && dRef < 1000000u)
					{
						sumRef_ += dRef;
						sumQpc_ += static_cast<double>(dQpc);
						if (sumRef_ > 4096.0)
						{
							sumRef_ *= 0.5;
							sumQpc_ *= 0.5;
						}
					}
				}
				periodPrevRef_ = s.syncRefreshCount;
				periodPrevQpc_ = s.syncQpc;
				havePeriodPrev_ = true;

				const double period = PeriodQpc();
				if (!havePrev_)
				{
					Remember(s);
					return;
				}
				if (s.presentCount == prevCount_)
					return;   // the same present again: nothing new
				const uint32_t dPresent = s.presentCount - prevCount_;
				const int32_t dRefreshSigned = static_cast<int32_t>(s.presentRefreshCount - prevPresentRef_);
				if (static_cast<int32_t>(dPresent) < 0 || dRefreshSigned < 0)
				{
					++resets_;   // the present counters went backwards (a new swapchain, or a restart): the old ids mean nothing
					ring_.clear();
					Remember(s);
					return;
				}
				const uint32_t dRefresh = static_cast<uint32_t>(dRefreshSigned);

				if (dPresent == 1)
				{
					// The previous present was shown for dRefresh refreshes: an exact integer, no period needed.
					++hold_[static_cast<int>((std::min<uint32_t>)(dRefresh, kHoldBins - 1))];
					++resolved_;
				}
				else
				{
					// Several presents between two reads. With flip-sequential at SyncInterval >= 1 a queued present is
					// not discarded, so only the presents that cannot have taken a refresh each are "never shown"
					// (dPresent - dRefresh, e.g. ids 19 -> 21 over one refresh). The rest were shown, but one read cannot
					// tell their individual holds: they are reported as unresolved, never put in a hold bin.
					const uint32_t neverShown = dPresent > dRefresh ? dPresent - dRefresh : 0u;
					hold_[0] += neverShown;
					unresolved_ += dPresent - neverShown;
				}

				// The lead needs the measured period and the present's swap end.
				if (period <= 0.0)
					++warmup_;
				else
				{
					bool found = false;
					for (size_t i = ring_.size(); i-- > 0;)
					{
						if (ring_[i].first == s.presentCount)
						{
							const double lead = (DisplayQpc(s, period) - static_cast<double>(ring_[i].second)) / period;
							AddLead(lead);
							found = true;
							break;
						}
					}
					if (!found)
						++leadMissing_;
				}
				Remember(s);
			}

			// True when the window has run kReportSeconds (the first call only starts the window).
			bool ReportDue(int64_t nowQpc, int64_t qpcHz)
			{
				if (qpcHz <= 0)
					return false;
				if (!windowStarted_)
				{
					windowStarted_ = true;
					windowStartQpc_ = nowQpc;
					return false;
				}
				return static_cast<double>(nowQpc - windowStartQpc_) >= kReportSeconds * static_cast<double>(qpcHz);
			}

			// The window's log line; starts the next window (the measured period and the previous present stay).
			std::string TakeLine(int64_t nowQpc, int64_t qpcHz)
			{
				const std::string text = Line(qpcHz);
				windowStarted_ = true;
				windowStartQpc_ = nowQpc;
				ClearWindow();
				return text;
			}

			std::string Line(int64_t qpcHz) const
			{
				char b[512];
				if (available_ == 0)
				{
					if (unavailable_ > 0)
						std::snprintf(b, sizeof(b), "Presenter present-vs-vsync: no stats (hr=0x%08X, %u reads without statistics, %u disjoint); no histogram, not a pass",
							static_cast<unsigned>(lastHr_), unavailable_, disjoints_);
					else
						std::snprintf(b, sizeof(b), "Presenter present-vs-vsync: no stats (%u reads, %u without a swapchain, %u disjoint); no histogram, not a pass",
							reads_, noSwapchain_, disjoints_);
					return b;
				}
				const double periodMs = PeriodQpc() > 0.0 && qpcHz > 0 ? PeriodQpc() * 1000.0 / static_cast<double>(qpcHz) : 0.0;
				char t[64];
				if (periodMs > 0.0)
					std::snprintf(t, sizeof(t), "T=%.3f ms (fd=%.3f Hz)", periodMs, 1000.0 / periodMs);
				else
					std::snprintf(t, sizeof(t), "T unknown");
				std::vector<double> sorted(leads_);
				std::sort(sorted.begin(), sorted.end());
				std::snprintf(b, sizeof(b),
					"Presenter present-vs-vsync: holds k0/k1/k2/k3+ = %u/%u/%u/%u (%u resolved, %u unresolved); lead [0,1)/[1,2)/[2,3)/[3,4)/4+ = %u/%u/%u/%u/%u, p1/p50/p99 = %.2f/%.2f/%.2f T (%u leads, %u unmatched, %u negative); %s; "
					"warm-up %u, resets %u, disjoint %u, reads without stats %u of %u",
					hold_[0], hold_[1], hold_[2], hold_[3], resolved_, unresolved_,
					leadBin_[0], leadBin_[1], leadBin_[2], leadBin_[3], leadBin_[4],
					Percentile(sorted, 0.01), Percentile(sorted, 0.50), Percentile(sorted, 0.99),
					static_cast<unsigned>(sorted.size()), leadMissing_, leadNegative_, t,
					warmup_, resets_, disjoints_, unavailable_ + noSwapchain_, reads_);
				return b;
			}

			// Accessors (tests).
			uint32_t Hold(int bin) const { return hold_[bin]; }
			uint32_t LeadBin(int bin) const { return leadBin_[bin]; }
			uint32_t Displayed() const { return resolved_; }   // presents whose hold was resolved (PresentCount step 1)
			uint32_t Unresolved() const { return unresolved_; }
			uint32_t LeadCount() const { return static_cast<uint32_t>(leads_.size()); }
			uint32_t LeadNegative() const { return leadNegative_; }
			uint32_t Warmup() const { return warmup_; }
			uint32_t Resets() const { return resets_; }
			uint32_t Unavailable() const { return unavailable_; }
			uint32_t LeadMissing() const { return leadMissing_; }
			double PeriodQpcMeasured() const { return PeriodQpc(); }   // 0 = not known yet
			double LeadPercentile(double fraction) const
			{
				std::vector<double> sorted(leads_);
				std::sort(sorted.begin(), sorted.end());
				return Percentile(sorted, fraction);
			}

		private:
			double PeriodQpc() const { return sumRef_ >= kMinRefreshesForPeriod ? sumQpc_ / sumRef_ : 0.0; }

			// The QPC time the present of statistics `s` reached the screen: the sync time moved back by the refreshes
			// between its refresh and the sync refresh.
			static double DisplayQpc(const PresentVsyncSample& s, double period)
			{
				const int32_t behind = static_cast<int32_t>(s.syncRefreshCount - s.presentRefreshCount);
				return static_cast<double>(s.syncQpc) - static_cast<double>(behind) * period;
			}

			void Remember(const PresentVsyncSample& s)
			{
				prevCount_ = s.presentCount;
				prevPresentRef_ = s.presentRefreshCount;
				havePrev_ = true;
			}

			void AddLead(double lead)
			{
				if (lead < 0.0)
				{
					++leadNegative_;   // the pairing or the ids are wrong: shown in the line, in no bin
					return;
				}
				++leadBin_[static_cast<int>((std::min)(lead, static_cast<double>(kLeadBins - 1)))];
				leads_.push_back(lead);
				if (leads_.size() > kMaxLeadSamples)
					leads_.erase(leads_.begin());
			}

			static double Percentile(const std::vector<double>& sorted, double fraction)
			{
				if (sorted.empty())
					return 0.0;
				const size_t i = static_cast<size_t>(std::floor(fraction * static_cast<double>(sorted.size() - 1) + 0.5));
				return sorted[(std::min)(i, sorted.size() - 1)];
			}

			void ClearWindow()
			{
				for (int i = 0; i < kHoldBins; ++i)
					hold_[i] = 0;
				for (int i = 0; i < kLeadBins; ++i)
					leadBin_[i] = 0;
				leads_.clear();
				resolved_ = unresolved_ = warmup_ = resets_ = disjoints_ = unavailable_ = noSwapchain_ = reads_ = leadMissing_ = leadNegative_ = available_ = 0;
			}

			std::vector<std::pair<uint32_t, int64_t>> ring_;
			uint32_t hold_[kHoldBins] = {};
			uint32_t leadBin_[kLeadBins] = {};
			std::vector<double> leads_;
			uint32_t resolved_ = 0, unresolved_ = 0, warmup_ = 0, resets_ = 0, disjoints_ = 0, unavailable_ = 0, noSwapchain_ = 0;
			uint32_t reads_ = 0, leadMissing_ = 0, leadNegative_ = 0, available_ = 0;
			int32_t lastHr_ = 0;

			// The previous distinct present.
			bool havePrev_ = false;
			uint32_t prevCount_ = 0, prevPresentRef_ = 0;

			// The measured period.
			bool havePeriodPrev_ = false;
			uint32_t periodPrevRef_ = 0;
			int64_t periodPrevQpc_ = 0;
			double sumRef_ = 0.0, sumQpc_ = 0.0;

			bool windowStarted_ = false;
			int64_t windowStartQpc_ = 0;
		};
	}
}
