// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S6 (2026-11-23_TD-consolidated-plan.md s3.2 TD-S6, s4.3 Q6 & Q7 & Q8b):
// Scheduler slice work, queue/look-ahead priority, lead E ladder, step-down tier ladder + overload arbiter,
// and late-frame carry-for-2-frames-then-0 controller.
//
// Consumes existing core scheduler contracts directly (zero private duplicates):
//   - SchedulerContract.h, PostPresentSpare.h, LaneSpareWork.h, OverloadOrder.h,
//     AdmissionTable.h, AheadCeiling.h, FallbackCatalog.h, LaneAheadPolicy.h, TdProductContracts.h.
//
// Every threshold / ratio is derived from signal physics, resolution standards, or existing VP-V contracts (Rule 1).
// Every degraded tier, reduced lead, or late-carry state is visibly recorded in StandInRegistry and/or
// FallbackCatalog::Decision (Rule 4: zero silent fallbacks).
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/AdmissionTable.h>
#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/LaneSpareWork.h>
#include <VP-V/core/OverloadOrder.h>
#include <VP-V/core/PostPresentSpare.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SchedulerContract.h>
#include <VP-V/modules/motion/estimation/LaneAheadPolicy.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace TdV2
{
	// ---- Rule-1 Contract Constants ----

	// Owner Q6a: standard queue delay budget in ms (matches AdmissionTable.h standard 250 ms latency tier).
	constexpr double kDefaultQueueBudgetMs = 250.0;

	// AdmissionTable.h AVR-safe latency ceiling in ms (200 ms ceiling, never a floor).
	constexpr double kAvrSafeQueueCeilingMs = 200.0;

	// Safe upper bound on derived queued frame count before int conversion (well above kAheadCeiling = 6, prevents FP-to-int UB).
	constexpr double kMaxDerivedQueuedFrames = 1024.0;

	// Owner Q7a: explicit 2-frame cap on late-frame carry before falling back cleanly to passthrough (0 delta).
	constexpr int kMaxLateCarryFrames = 2;

	// ITU-R BT.2020 UHD 4K horizontal resolution standard (used for Owner Q8b laptop native 4K reduced tier).
	constexpr int kUhd4kWidthPx = 3840;   // GENERAL-REF: Owner Q8b laptop native 4K reference width (ITU-R BT.2020 UHD)

	// ---- 1. Post-Present Slice Work (SpareClass::Slice, NEVER Fill, NEVER shed by regulator) ----

	class TdV2SliceWork : public VpSkeleton::ISpareWork
	{
	public:
		using RowRunner = std::function<double(uint32_t startRow, uint32_t rowCount)>;

		explicit TdV2SliceWork(uint16_t productId = VpSkeleton::kTdBlocks, uint32_t graphOrder = 0)
		{
			decl_.product = productId;
			decl_.cls = VpSkeleton::JobClass::Slice;   // Strictly Slice (OD:391; NEVER Fill, NEVER GapEligible in CoreFirst)
			decl_.graphOrder = graphOrder;
			decl_.splittable = true;
			decl_.cpu = false;
		}

		// Queue a new frame / anchor pass of `totalRows` splittable rows.
		void BeginFrameWork(uint32_t totalRows, RowRunner runner)
		{
			totalRows_ = totalRows;
			rowsDone_ = 0;
			rowsLeft_ = totalRows;
			runner_ = std::move(runner);
		}

		void ClearPendingWork()
		{
			totalRows_ = 0;
			rowsDone_ = 0;
			rowsLeft_ = 0;
			runner_ = nullptr;
		}

		void Reset()
		{
			ClearPendingWork();
			cost_.Reset();
			runs_ = 0;
			splitRuns_ = 0;
			failures_ = 0;
			totalRowsCompleted_ = 0;
		}

		// ---- VpSkeleton::ISpareWork ----
		uint16_t Id() const override { return decl_.product; }
		VpSkeleton::SpareClass Class() const override { return VpSkeleton::SpareClassOf(decl_.cls); }
		uint32_t Order() const override { return decl_.graphOrder; }
		bool HasWork() const override { return rowsLeft_ > 0; }
		uint32_t RowsLeft() const override { return rowsLeft_; }

		double PredictMs() const override
		{
			const double whole = cost_.PredictMs();
			if (!(whole > 0.0) || rowsLeft_ == 0 || totalRows_ == 0)
				return whole;
			return (rowsLeft_ < totalRows_)
				? (whole * static_cast<double>(rowsLeft_) / static_cast<double>(totalRows_))
				: whole;
		}

		double Run(uint32_t rows) override
		{
			if (rowsLeft_ == 0)
				return 0.0;
			const uint32_t count = (rows == 0 || rows > rowsLeft_) ? rowsLeft_ : rows;
			if (!runner_)
			{
				++failures_;
				return -1.0;
			}
			const double ms = runner_(rowsDone_, count);
			if (!std::isfinite(ms) || ms < 0.0)
			{
				++failures_;
				return -1.0;
			}
			cost_.Note(ms, count, totalRows_);
			rowsDone_ += count;
			rowsLeft_ -= count;
			++runs_;
			if (count < totalRows_)
				++splitRuns_;
			totalRowsCompleted_ += count;
			if (rowsLeft_ == 0)
				totalRows_ = 0;
			return ms;
		}

		// ---- Scheduler Contract Verification ----
		const VpSkeleton::JobDecl& JobDeclaration() const { return decl_; }
		bool IsGapEligible(VpSkeleton::ScheduleMode mode) const { return VpSkeleton::GapEligible(decl_, mode); }
		const VpSkeleton::SpareMeasuredCost& Cost() const { return cost_; }

		uint32_t TotalRows() const { return totalRows_; }
		uint32_t RowsDone() const { return rowsDone_; }
		uint64_t Runs() const { return runs_; }
		uint64_t SplitRuns() const { return splitRuns_; }
		uint64_t Failures() const { return failures_; }
		uint64_t TotalRowsCompleted() const { return totalRowsCompleted_; }

	private:
		VpSkeleton::JobDecl decl_{};
		VpSkeleton::SpareMeasuredCost cost_;
		RowRunner runner_;
		uint32_t totalRows_ = 0;
		uint32_t rowsDone_ = 0;
		uint32_t rowsLeft_ = 0;
		uint64_t runs_ = 0;
		uint64_t splitRuns_ = 0;
		uint64_t failures_ = 0;
		uint64_t totalRowsCompleted_ = 0;
	};

	// ---- 2. Owner Q6 Queue & Look-Ahead Priority ----

	struct QueuePriorityInput
	{
		bool tdEnabled = false;
		double queueBudgetMs = kDefaultQueueBudgetMs;    // Owner Q6a default: 250 ms
		double sourceHz = kDefaultSourceHz;              // Measured source rate (Hz)
		double motionLatencyMs = 0.0;                    // Measured motion-chain latency consuming queue time (ms)
		int queuedAheadFrames = -1;                      // >= 0 overrides frame count derived from queueBudgetMs & sourceHz
		int renderAheadCreditRequested = 0;              // Frames requested by render-ahead queue credit
		VpSkeleton::Admission::LatencyMode latencyMode = VpSkeleton::Admission::LatencyMode::Standard;
	};

	struct QueuePriorityDecision
	{
		double effectiveQueueBudgetMs = kDefaultQueueBudgetMs;
		int totalQueuedAhead = 0;
		int tdLookAheadFloor = 0;                        // VpMotion::LaneAhead::kDepthMin (2) when tdEnabled in Standard/AvrSafe
		int usableLeadE = 0;                             // Clamped to [0, VpSkeleton::kAheadCeiling]
		int renderAheadCreditGranted = 0;                // 0 when queue budget is tight and TD wins (Owner Q6b)
		bool tdWinsOverCredit = false;                   // True when tdEnabled (TD look-ahead floor outranks render-ahead credit)
		bool creditTrimmed = false;                      // True when render-ahead credit was reduced/denied to protect TD floor
		std::string statusNote;
	};

	inline QueuePriorityDecision EvaluateQueuePriority(
		const QueuePriorityInput& in,
		StandInRegistry* standIns = nullptr)
	{
		QueuePriorityDecision d;
		if (!std::isfinite(in.queueBudgetMs) || in.queueBudgetMs < 0.0 ||
		    !std::isfinite(in.sourceHz) || in.sourceHz <= 0.0 ||
		    !std::isfinite(in.motionLatencyMs) || in.motionLatencyMs < 0.0)
		{
			if (standIns != nullptr)
				standIns->Engage(StandInKind::NonFiniteClosure, "EvaluateQueuePriority invalid timing");
			d.effectiveQueueBudgetMs = 0.0;
			d.statusNote = "queue priority: invalid timing (closed to E=0)";
			return d;
		}

		// IC-2 R9: Games (1 queued) and Competitive (0 queued) do not schedule look-ahead work.
		if (in.latencyMode == VpSkeleton::Admission::LatencyMode::Games ||
		    in.latencyMode == VpSkeleton::Admission::LatencyMode::Competitive)
		{
			d.effectiveQueueBudgetMs = in.queueBudgetMs;
			d.creditTrimmed = (in.renderAheadCreditRequested > 0);
			d.statusNote = std::string("queue priority: ") +
				VpSkeleton::Admission::LatencyModeName(in.latencyMode) + " mode (no look-ahead)";
			return d;
		}

		// AdmissionTable.h: AVR-safe enforces a 200 ms ceiling on queue delay budget.
		const double effectiveBudgetMs = (in.latencyMode == VpSkeleton::Admission::LatencyMode::AvrSafe)
			? (std::min)(in.queueBudgetMs, kAvrSafeQueueCeilingMs)
			: in.queueBudgetMs;
		d.effectiveQueueBudgetMs = effectiveBudgetMs;

		int queued = in.queuedAheadFrames;
		if (queued < 0)
		{
			const double periodMs = 1000.0 / in.sourceHz;
			const double availMs = (std::max)(0.0, effectiveBudgetMs - in.motionLatencyMs);
			const double rawFrames = std::floor(availMs / periodMs);
			const double clampedFrames = (std::max)(0.0, (std::min)(kMaxDerivedQueuedFrames, rawFrames));
			queued = static_cast<int>(clampedFrames);
		}
		if (queued < 0)
			queued = 0;
		d.totalQueuedAhead = queued;

		const int ceiling = static_cast<int>(VpSkeleton::kAheadCeiling);
		const int reqCredit = in.renderAheadCreditRequested > 0 ? in.renderAheadCreditRequested : 0;

		if (!in.tdEnabled)
		{
			d.tdLookAheadFloor = 0;
			d.tdWinsOverCredit = false;
			d.renderAheadCreditGranted = (std::min)(reqCredit, queued);
			d.creditTrimmed = d.renderAheadCreditGranted < reqCredit;
			d.usableLeadE = 0;
			d.statusNote = "queue priority: TD off (render-ahead credit granted=" +
				std::to_string(d.renderAheadCreditGranted) + ")";
			return d;
		}

		// Owner Q6b: When TD is enabled, TD's look-ahead floor (LaneAhead::kDepthMin = 2) strictly outranks
		// render-ahead credit. Render-ahead credit may only take frames strictly above TD's floor.
		const int tdFloor = VpMotion::LaneAhead::kDepthMin;
		d.tdLookAheadFloor = tdFloor;
		d.tdWinsOverCredit = true;

		const int spareAfterTdFloor = (std::max)(0, queued - tdFloor);
		d.renderAheadCreditGranted = (std::min)(reqCredit, spareAfterTdFloor);
		d.creditTrimmed = d.renderAheadCreditGranted < reqCredit;

		const int remainingForTd = queued - d.renderAheadCreditGranted;
		d.usableLeadE = (std::max)(0, (std::min)(ceiling, remainingForTd));

		d.statusNote = "queue priority: TD floor " + std::to_string(tdFloor) +
			" wins over credit (E=" + std::to_string(d.usableLeadE) +
			", credit=" + std::to_string(d.renderAheadCreditGranted) + "/" + std::to_string(reqCredit) + ")";
		return d;
	}

	// ---- 3. Lead E Ladder (0..VpSkeleton::kAheadCeiling) ----

	struct LeadLadderState
	{
		int usableLeadE = 0;               // Clamped to [0, VpSkeleton::kAheadCeiling]
		bool nextPeak = false;             // Future peak reference y_2 available
		bool p15Lead2 = false;             // Full P15 lead-2 detector (false => P15c causal provisional)
		bool anchorPreShow = false;        // Anchor repair pre-show available
		bool anchorPreShowNarrow = false;  // Narrow pre-show margin at E == 3
		bool reduced = false;              // True whenever E < LaneAhead::kDepthTarget (4)
		std::string statusNote;
	};

	inline LeadLadderState EvaluateLeadLadder(int rawLeadE, StandInRegistry* standIns = nullptr)
	{
		const int ceiling = static_cast<int>(VpSkeleton::kAheadCeiling);
		const int target = VpMotion::LaneAhead::kDepthTarget; // 4
		const int e = (std::max)(0, (std::min)(ceiling, rawLeadE));

		LeadLadderState st;
		st.usableLeadE = e;
		if (e >= target)
		{
			st.nextPeak = true;
			st.p15Lead2 = true;
			st.anchorPreShow = true;
			st.anchorPreShowNarrow = false;
			st.reduced = false;
			st.statusNote = "E=" + std::to_string(e) + "/" + std::to_string(target) + ": full look-ahead";
		}
		else if (e == 3)
		{
			st.nextPeak = true;
			st.p15Lead2 = true;
			st.anchorPreShow = true;
			st.anchorPreShowNarrow = true;
			st.reduced = true;
			st.statusNote = "E=3/4: narrow anchor pre-show";
		}
		else if (e == 2)
		{
			// Central 4K24 250 ms operating point (Owner Q6a).
			st.nextPeak = true;
			st.p15Lead2 = true;
			st.anchorPreShow = false;
			st.anchorPreShowNarrow = false;
			st.reduced = true;
			st.statusNote = "E=2/4: two-sided peaks + P15 lead-2, no anchor pre-show";
		}
		else if (e == 1)
		{
			st.nextPeak = true;
			st.p15Lead2 = false;   // P15c causal provisional
			st.anchorPreShow = false;
			st.anchorPreShowNarrow = false;
			st.reduced = true;
			st.statusNote = "E=1/4: next peak only (P15c provisional)";
		}
		else
		{
			st.nextPeak = false;   // One-sided causal
			st.p15Lead2 = false;
			st.anchorPreShow = false;
			st.anchorPreShowNarrow = false;
			st.reduced = true;
			st.statusNote = "E=0/4: one-sided causal";
		}

		if (standIns != nullptr)
		{
			if (st.reduced)
				standIns->Engage(StandInKind::LeadReduced, st.statusNote.c_str());
			else
				standIns->Clear(StandInKind::LeadReduced);
		}
		return st;
	}

	// ---- 4. Step-Down Tier Ladder & Owner Q7b Overload Order ----

	enum class TdTier : uint8_t
	{
		OffBudget = 0,
		LocalPeaksOnly = 1,
		ReducedAnchor = 2,
		Full = 3
	};

	inline const char* TdTierName(TdTier t)
	{
		switch (t)
		{
		case TdTier::OffBudget: return "off (budget)";
		case TdTier::LocalPeaksOnly: return "local peaks only (tier 1)";
		case TdTier::ReducedAnchor: return "reduced anchor (tier 2)";
		case TdTier::Full: return "full (tier 3)";
		}
		return "unknown";
	}

	struct TierSpec
	{
		TdTier tier = TdTier::Full;
		int momentPoolRadiusCells = 1;
		bool enableLongAnchor = true;
		bool enableCarryForward = true;
		bool enableChroma = true;
		bool tdActive = true;
		bool preservesCellIntegrity = true;  // Invariant: STRICT/CELL integrity preserved at EVERY tier
		bool usesL3ClampForBudget = false;   // Invariant: NEVER uses L3 clamp to save GPU budget (Owner 2026-11-11 11:15)
	};

	inline TierSpec SpecForTier(TdTier tier)
	{
		TierSpec s;
		s.tier = tier;
		s.preservesCellIntegrity = true;
		s.usesL3ClampForBudget = false;
		switch (tier)
		{
		case TdTier::Full:
			s.momentPoolRadiusCells = 1; // 3x3 cells
			s.enableLongAnchor = true;
			s.enableCarryForward = true;
			s.enableChroma = true;
			s.tdActive = true;
			break;
		case TdTier::ReducedAnchor:
			s.momentPoolRadiusCells = 1; // 3x3 cells
			s.enableLongAnchor = false;  // Drops long-reach I-anchor first
			s.enableCarryForward = true;
			s.enableChroma = true;
			s.tdActive = true;
			break;
		case TdTier::LocalPeaksOnly:
			s.momentPoolRadiusCells = 0; // 1x1 cell
			s.enableLongAnchor = false;
			s.enableCarryForward = false;
			s.enableChroma = false;      // Luma-only local peaks
			s.tdActive = true;
			break;
		case TdTier::OffBudget:
			s.momentPoolRadiusCells = 0;
			s.enableLongAnchor = false;
			s.enableCarryForward = false;
			s.enableChroma = false;
			s.tdActive = false;
			break;
		}
		return s;
	}

	// Applies tier cleanly to Config while strictly preserving STRICT/CELL residual integrity and NEVER enabling L3 for budget.
	inline TierSpec ApplyTierToConfig(Config& cfg, TdTier tier, StandInRegistry* standIns = nullptr)
	{
		const TierSpec spec = SpecForTier(tier);
		cfg.momentPoolRadiusCells = spec.momentPoolRadiusCells;
		cfg.enableLongAnchor = spec.enableLongAnchor;
		cfg.enableCarryForward = spec.enableCarryForward;
		// ResidualMode (STRICT vs CELL) is NEVER weakened or substituted with L3 clamp under GPU budget pressure.
		if (!spec.tdActive)
			cfg.mode = Mode::Off;

		if (standIns != nullptr)
		{
			if (tier != TdTier::Full)
				standIns->Engage(StandInKind::TierSteppedDown, TdTierName(tier));
			else
				standIns->Clear(StandInKind::TierSteppedDown);
		}
		return spec;
	}

	// Configure-time initial tier selection handling Owner Q8b (laptop native 4K -> ReducedAnchor visibly)
	// and measured slice headroom vs tdMinMs / tdWantMs (from AdmissionTable).
	inline TdTier SelectInitialTier(
		int width,
		int height,
		double sliceMs,
		double tdMinMs,
		double tdWantMs,
		bool isLaptopBudget,
		StandInRegistry* standIns = nullptr)
	{
		if (width <= 0 || height <= 0 ||
		    !std::isfinite(sliceMs) || !std::isfinite(tdMinMs) || !std::isfinite(tdWantMs) ||
		    sliceMs < 0.0 || tdMinMs < 0.0 || tdWantMs < 0.0)
		{
			if (standIns != nullptr)
			{
				standIns->Engage(StandInKind::NonFiniteClosure, "SelectInitialTier invalid arguments");
				standIns->Engage(StandInKind::TierSteppedDown, "invalid budget/geometry -> off");
			}
			return TdTier::OffBudget;
		}

		TdTier tier = TdTier::Full;
		const char* reason = nullptr;
		// Guard inverted caller bounds (tdMinMs > tdWantMs) so tier thresholds stay monotonic.
		const double effWantMs = (tdWantMs > 0.0 && tdMinMs > tdWantMs) ? tdMinMs : tdWantMs;

		if (sliceMs <= 0.0)
		{
			tier = TdTier::OffBudget;
			reason = "zero TD slice budget";
		}
		else if (tdMinMs > 0.0 && sliceMs < tdMinMs)
		{
			// Mirrors AdmissionTable.h (tdReduced = sliceMs < tdMinMs): step down to LocalPeaksOnly visibly.
			tier = TdTier::LocalPeaksOnly;
			reason = "slice < tdMinMs (AdmissionTable tdReduced) -> local peaks only";
		}
		else if (width >= kUhd4kWidthPx && isLaptopBudget)
		{
			// Owner Q8b: laptop native 4K runs reduced tier visibly.
			tier = TdTier::ReducedAnchor;
			reason = "laptop native 4K -> reduced anchor tier";
		}
		else if (effWantMs > 0.0 && sliceMs < effWantMs)
		{
			tier = TdTier::ReducedAnchor;
			reason = "slice < tdWantMs -> reduced anchor tier";
		}

		if (standIns != nullptr)
		{
			if (tier != TdTier::Full)
				standIns->Engage(StandInKind::TierSteppedDown, reason);
			else
				standIns->Clear(StandInKind::TierSteppedDown);
		}
		return tier;
	}

	// Runtime overload tier controller wrapping VpSkeleton::OverloadArbiter (core/OverloadOrder.h).
	// Enforces exact Owner Q7b order: None -> FillOff -> TdStepDown (3->2->1->0) -> ExtrasStepDown -> CoreDeferred -> FeatureOff.
	class TdV2TierController
	{
	public:
		explicit TdV2TierController(TdTier baseTier = TdTier::Full)
			: baseTier_(baseTier), currentTier_(baseTier)
		{
		}

		TdV2TierController(const TdV2TierController&) = delete;
		TdV2TierController& operator=(const TdV2TierController&) = delete;

		void SetBaseTier(TdTier baseTier)
		{
			baseTier_ = baseTier;
			if (static_cast<uint8_t>(currentTier_) > static_cast<uint8_t>(baseTier_))
				currentTier_ = baseTier_;
		}

		void SetOptionalFills(const char* const* names, size_t count)
		{
			arbiter_.SetOptionalFills(names, count);
		}

		void Reset()
		{
			arbiter_.Reset();
			currentTier_ = baseTier_;
		}

		TdTier Step(
			double nowMs,
			const VpSkeleton::SlotBudget& budget,
			bool extrasActive,
			StandInRegistry* standIns = nullptr)
		{
			const uint8_t fullVal = static_cast<uint8_t>(baseTier_);
			const VpSkeleton::OverloadState st = arbiter_.Step(nowMs, budget, extrasActive, fullVal);
			uint8_t nextVal = st.tdTier <= fullVal ? st.tdTier : fullVal;
			if (st.step == VpSkeleton::OverloadStep::FeatureOff)
				nextVal = static_cast<uint8_t>(TdTier::OffBudget);
			currentTier_ = static_cast<TdTier>(nextVal);

			if (standIns != nullptr)
			{
				if (currentTier_ != TdTier::Full)
				{
					const char* detail = (st.step == VpSkeleton::OverloadStep::None)
						? "base tier reduced (budget/4K laptop)"
						: (st.line ? st.line : TdTierName(currentTier_));
					standIns->Engage(StandInKind::TierSteppedDown, detail);
				}
				else
				{
					standIns->Clear(StandInKind::TierSteppedDown);
				}
			}
			return currentTier_;
		}

		void NoteCoreDeferred(uint32_t n) { arbiter_.NoteCoreDeferred(n); }

		TdTier BaseTier() const { return baseTier_; }
		TdTier CurrentTier() const { return currentTier_; }
		bool ExtrasAllowed() const { return VpSkeleton::ExtrasAllowed(arbiter_.State()); }
		bool FillAllowed() const { return VpSkeleton::FillAllowed(arbiter_.State()); }
		const VpSkeleton::OverloadState& OverloadState() const { return arbiter_.State(); }
		unsigned long long StepsDown() const { return arbiter_.StepsDown(); }
		unsigned long long StepsUp() const { return arbiter_.StepsUp(); }
		unsigned long long DeferredTotal() const { return arbiter_.DeferredTotal(); }

		FallbackCatalog::Decision FallbackDecision() const
		{
			const VpSkeleton::OverloadState& st = arbiter_.State();
			if (st.step == VpSkeleton::OverloadStep::None && currentTier_ == TdTier::Full)
				return FallbackCatalog::Released("product.temporal_detail");
			std::string why = (st.step == VpSkeleton::OverloadStep::None)
				? std::string("base tier reduced (budget/4K laptop)")
				: (st.line ? std::string(st.line) : std::string("overload"));
			why += "; tier=";
			why += TdTierName(currentTier_);
			return FallbackCatalog::Engaged("product.temporal_detail", why);
		}

	private:
		VpSkeleton::OverloadArbiter arbiter_;
		TdTier baseTier_ = TdTier::Full;
		TdTier currentTier_ = TdTier::Full;
	};

	// ---- 5. Owner Q7a Late-Frame Carry Controller (2-frame cap -> passthrough, NEVER L3 clamp) ----

	struct LateCarryDecision
	{
		bool carried = false;
		bool expired = false;
		bool usesL3Clamp = false;               // Invariant: ALWAYS false (Owner Q7a)
		int lateStreak = 0;
		double decayScale = 0.0;                // 2/3 on frame 1, 1/3 on frame 2, 0.0 when expired
		TdV2Plane delta;                        // Carried & cell-bounded delta (zero plane when expired)
		std::string statusText;
		FallbackCatalog::Decision fallback;     // "product.temporal_detail"
	};

	class TdV2LateCarryController
	{
	public:
		void OnOnTimeFrame(const TdV2Plane& delta, StandInRegistry* standIns = nullptr)
		{
			prevDelta_ = delta;
			hasPrevDelta_ = !delta.Empty();
			lateStreak_ = 0;
			expired_ = false;
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryActive);
				standIns->Clear(StandInKind::LateCarryExpired);
			}
		}

		// Scene cut / geometry change / reset: clears stored delta so TD NEVER carries across cuts.
		void OnCutOrReset(StandInRegistry* standIns = nullptr)
		{
			prevDelta_ = TdV2Plane{};
			hasPrevDelta_ = false;
			lateStreak_ = 0;
			expired_ = false;
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryActive);
				standIns->Clear(StandInKind::LateCarryExpired);
			}
		}

		int LateStreak() const { return lateStreak_; }
		bool HasStoredDelta() const { return hasPrevDelta_; }
		bool IsExpiredLatched() const { return expired_; }

		LateCarryDecision OnLateFrame(
			const TdV2Plane& curClean,
			const ActiveRect& active,
			const CellMotionField* carryMotion,
			double stepTotalVar,
			int cellsW,
			int cellsH,
			int cellPx,
			StandInRegistry* standIns = nullptr)
		{
			LateCarryDecision dec;
			dec.usesL3Clamp = false;
			if (!curClean.Empty())
				dec.delta = TdV2Plane(curClean.w, curClean.h, 0.0);

			auto makeExpired = [&](const char* detail) -> LateCarryDecision
			{
				expired_ = true;
				dec.carried = false;
				dec.expired = true;
				dec.usesL3Clamp = false;
				dec.lateStreak = lateStreak_;
				dec.decayScale = 0.0;
				dec.statusText = MetaOf(StandInKind::LateCarryExpired).statusText;
				dec.fallback = FallbackCatalog::Engaged("product.temporal_detail", dec.statusText);
				if (standIns != nullptr)
				{
					standIns->Clear(StandInKind::LateCarryActive);
					standIns->Engage(StandInKind::LateCarryExpired, detail);
				}
				return dec;
			};

			if (curClean.Empty() || !std::isfinite(stepTotalVar) || stepTotalVar < 0.0)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2LateCarryController invalid frame or stepTotalVar");
				++lateStreak_;
				return makeExpired("invalid frame or stepTotalVar");
			}

			// Once any late frame expires (null motion, empty active rect, geometry mismatch, or cap reached),
			// stay expired until OnOnTimeFrame or OnCutOrReset resets the latch.
			if (expired_)
			{
				++lateStreak_;
				return makeExpired("late carry latched expired (awaiting on-time frame)");
			}

			if (!hasPrevDelta_)
			{
				++lateStreak_;
				return makeExpired("no stored delta (cut/reset)");
			}

			if (prevDelta_.w != curClean.w || prevDelta_.h != curClean.h)
			{
				++lateStreak_;
				return makeExpired("geometry mismatch vs stored delta");
			}

			if (lateStreak_ >= kMaxLateCarryFrames)
			{
				++lateStreak_;
				return makeExpired("2-frame late carry cap reached");
			}

			if (carryMotion == nullptr || !carryMotion->IsWellFormed(cellsW, cellsH) || cellPx <= 0)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2LateCarryController null/invalid carry motion");
				++lateStreak_;
				return makeExpired("carry motion unavailable");
			}

			const ActiveRect ar = active.ClampedTo(curClean.w, curClean.h);
			if (ar.Empty())
			{
				++lateStreak_;
				return makeExpired("empty active rect");
			}

			++lateStreak_;
			// Exact linear decay over kMaxLateCarryFrames + 1 steps:
			//   late frame 1 -> 1 - 1/3 = 2/3, late frame 2 -> 1 - 2/3 = 1/3, late frame 3 -> expired (0).
			const double decay = 1.0 - static_cast<double>(lateStreak_) / static_cast<double>(kMaxLateCarryFrames + 1);

			TdV2Plane zeroFallback(curClean.w, curClean.h, 0.0);
			TdV2Plane warpedDelta;
			std::vector<uint8_t> validMask;
			WarpCleanReference(
				prevDelta_, zeroFallback, ar,
				carryMotion->vx, carryMotion->vy,
				cellsW, cellsH, cellPx,
				warpedDelta, validMask, standIns);

			// Keep an un-decayed 1-hop warped delta so late frame 2 can advance accurately by another 1-hop carryMotion.
			TdV2Plane nextPrevDelta(curClean.w, curClean.h, 0.0);
			bool anyPixelBounded = false;
			bool anyNonFinitePixel = false;

			for (int y = ar.y0; y < ar.y1; ++y)
			{
				const int cy = (std::min)(cellsH - 1, y / cellPx);
				for (int x = ar.x0; x < ar.x1; ++x)
				{
					const int cx = (std::min)(cellsW - 1, x / cellPx);
					const size_t cIdx = static_cast<size_t>(cy) * static_cast<size_t>(cellsW) + static_cast<size_t>(cx);
					const size_t pIdx = static_cast<size_t>(y) * static_cast<size_t>(curClean.w) + static_cast<size_t>(x);

					// Mask out Overlay cells, DynamicTexture cells, invalid motion cells, and out-of-bounds/non-finite warp landings.
					if (validMask[pIdx] == 0u ||
					    carryMotion->valid[cIdx] == 0u ||
					    carryMotion->cls[cIdx] == RegionMotionClass::Overlay ||
					    carryMotion->cls[cIdx] == RegionMotionClass::DynamicTexture)
					{
						dec.delta.at(x, y) = 0.0;
						continue;
					}

					const double curVal = curClean.at(x, y);
					const double wDelta = warpedDelta.at(x, y);
					if (!std::isfinite(curVal) || !std::isfinite(wDelta))
					{
						anyNonFinitePixel = true;
						dec.delta.at(x, y) = 0.0;
						continue;
					}

					nextPrevDelta.at(x, y) = wDelta;
					const double scaled = wDelta * decay;
					if (scaled == 0.0)
					{
						dec.delta.at(x, y) = 0.0;
						continue;
					}

					// Bound each non-zero carried pixel via current frame's quantization cell bound without flooding StandInRegistry per pixel.
					dec.delta.at(x, y) = ApplyTransformUnknownBound(scaled, stepTotalVar, nullptr);
					anyPixelBounded = true;
				}
			}

			// Advance stored delta to the un-decayed warped delta for a subsequent 1-hop late frame.
			prevDelta_ = std::move(nextPrevDelta);

			if (standIns != nullptr)
			{
				if (anyNonFinitePixel)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2LateCarryController non-finite pixel");
				if (anyPixelBounded)
					standIns->Engage(StandInKind::TransformUnknownBound);
			}

			dec.carried = true;
			dec.expired = false;
			dec.usesL3Clamp = false;
			dec.lateStreak = lateStreak_;
			dec.decayScale = decay;
			dec.statusText = MetaOf(StandInKind::LateCarryActive).statusText;
			dec.fallback = FallbackCatalog::Engaged("product.temporal_detail", dec.statusText);
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryExpired);
				standIns->Engage(StandInKind::LateCarryActive);
			}
			return dec;
		}

	private:
		TdV2Plane prevDelta_;
		bool hasPrevDelta_ = false;
		int lateStreak_ = 0;
		bool expired_ = false;
	};
}
