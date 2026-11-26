// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Top-level stateful orchestrator combining Stage A (3-frame symmetric residual + streak moments),
// Stage B (camera-stabilized multi-hypothesis trajectory classifier & tracker), Stage B.5 (ROI-only
// masked pyramid ZNCC refinement), and Stage C (Dormant/Active scheduler, OverloadArbiter budget
// shedding hook, instant cut/flash/resync reset, and zero-cost OFF / OriginalPathway gates).
// Pure C++14/17, deterministic, zero Windows headers, zero static/thread_local mutable state.
#pragma once

#include <cstdint>

#include "FastObjectRefineRef.h"
#include "FastObjectResidualRef.h"
#include "FastObjectTrajectory.h"
#include "FastObjectTypes.h"

namespace FastObjects
{
	// Frame submission bundle for PushFrame / ProcessFrame.
	struct TrackerFrameInput
	{
		uint64_t frameIndex = 0;
		double timestampSec = 0.0;        // Cumulative presentation time in seconds
		double dtFromPrevSec = 0.0;       // Measured inter-frame duration (PairTiming::Seconds)
		uint64_t resyncGeneration = 0;    // U10 resync/rebase generation (triggers immediate scene reset on change)

		// Stage A triplet + camera homographies + noise/guard maps.
		ResidualInput residual;
	};

	// Optional full-resolution pair planes for Stage B.5 masked pyramid ZNCC refinement at ResolveAt(t).
	struct TrackerRefinePlanes
	{
		const float* lumaA = nullptr;     // Frame t full-res (or finest) luma
		const float* lumaB = nullptr;     // Frame t+1 full-res (or finest) luma
		int width = 0;
		int height = 0;
		float planeScale = 1.0f;          // 1.0 for full-res, 2.0 for half-res
		CameraModel::Params camAToB;      // H_{t -> t+1}
		bool camUsable = false;
		CameraModel::Rect picture;
	};

	class FastObjectTracker
	{
	public:
		FastObjectTracker();

		// Pre-allocates internal workspaces for zero steady-state heap allocations.
		void Reserve(int residualW, int residualH, int guardGridW = 0, int guardGridH = 0, int maxRefinePatch = 96);

		// OverloadArbiter add-on budget shedding hook (spec C.4.7):
		// Steps Fast Objects down cleanly (ActiveT2 -> ActiveT1 -> Dormant -> ShedBudget/Off)
		// before any core video processing stage.
		void SetBudgetTier(SchedulerMode tier);
		SchedulerMode BudgetTier() const { return budgetTier_; }

		// Resets tracks, look-ahead ring, and clutter-hotspot map (preserves session ball prior).
		void ResetScene();

		// Full reset including session ball prior (call on source/channel/resolution change).
		void ResetSession();

		// Stage A + Stage B ingestion for a newly arrived frame (supports look-ahead windows up to kAheadCeiling).
		// Returns true when Stage A executed or skipped cleanly in Dormant; returns false on hard abstain / OFF.
		bool PushFrame(const Config& cfg, const TrackerFrameInput& in, ResidualOutput* outStageA = nullptr);

		// Stage B resolution + Stage B.5 ROI-only masked refinement at `targetFrameIndex`.
		// Populates `outTrackSet` with full tracks, scheduler mode, abstain reason, and diagnostics.
		bool ResolveAt(
			const Config& cfg,
			uint64_t targetFrameIndex,
			const TrackerRefinePlanes* refinePlanes,
			FastObjectTrackSet& outTrackSet);

		// Convenience single-frame streaming entry point (calls PushFrame + ResolveAt for in.frameIndex).
		bool ProcessFrame(
			const Config& cfg,
			const TrackerFrameInput& in,
			const TrackerRefinePlanes* refinePlanes,
			FastObjectTrackSet& outTrackSet);

		// Read-only scheduler/diagnostic inspection.
		SchedulerMode CurrentMode() const { return currentMode_; }
		uint32_t QuietFrames() const { return quietFrames_; }
		uint32_t TotalClutterOverflows() const { return totalClutterOverflows_; }
		const TrajectoryTracker& Trajectory() const { return trajectory_; }

	private:
		struct StageADiagRecord
		{
			uint64_t frameIndex = 0;
			uint32_t activeCells = 0;
			uint32_t blobCount = 0;
			uint32_t droppedBlobs = 0;
			uint32_t abstainReason = AbstainReason::None;
			bool clutterOverflow = false;
			bool scanSkippedDormant = false;
			float noiseSigmaEst = 0.0f;
			SchedulerMode mode = SchedulerMode::Off;
			bool valid = false;
		};

		bool CheckFastOffGate(const Config& cfg, FastObjectTrackSet* outTrackSet) const;
		void RecordStageADiag(const StageADiagRecord& rec);
		const StageADiagRecord* FindStageADiag(uint64_t frameIndex) const;

		ResidualWorkspace residualWs_;
		RefineWorkspace refineWs_;
		TrajectoryTracker trajectory_;

		SchedulerMode budgetTier_ = SchedulerMode::ActiveT1;
		SchedulerMode currentMode_ = SchedulerMode::Off;
		uint64_t lastResyncGeneration_ = 0;
		bool hasResyncGeneration_ = false;
		uint32_t quietFrames_ = 0;
		uint32_t dormantTick_ = 0;
		uint32_t totalClutterOverflows_ = 0;

		// Per-frame Stage A diagnostics ring (eliminates look-ahead diag skew across [t, t + W_a]).
		StageADiagRecord stageARing_[kWindowRingCapacity];
		int stageARingCount_ = 0;
		StageADiagRecord lastStageADiag_;
	};
}
