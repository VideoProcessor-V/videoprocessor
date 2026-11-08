// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-2..4): camera-guided matching. ONE unit, BuildGuide(), makes the GuidanceFrame of pair N from P7(N-d), the
// CPU field copy of that pair and the scene flags of N; the camera worker (lane) and the render thread (live) both call it, so
// the two results are byte-equal. The scheduler:
//   * d is fixed per graph (SetLag, on a rebuild only): the matcher for pair N runs ahead of the render, so P7(N-1) would
//     serialise the lane; P7(N-d) is final by then. Guidance NEVER depends on lag-1 timing.
//   * REFUSES (guidance = nullptr: today's full search, byte-identical) with a counted, visible reason: all switches off,
//     record late, slip / ancient stamp, record not usable (D-K4 verdict gate), confidence below the plan floor, governor
//     tripped, no plan.
//   * implements IPreEstimateHook against the P5a header (VectorEngineInterface.h): BeforeEstimate binds the guidance,
//     AfterEstimate reads the 16 B counts a frame later (escalated = planned - accepted) into MeasurementDiagnostics and the
//     governor. The lane result is used only if it was published BEFORE the render asked; a later one is discarded.
// Render thread except PublishLane (camera worker). Deterministic.
#pragma once

#include "CameraLink.h"
#include "CameraUsable.h"
#include "GuidanceGate.h"
#include "GuidanceGovernor.h"
#include "GuidanceStatus.h"
#include "MotionBenchWriter.h"

#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace CameraLink
{
	// What the host knows about the pair about to be searched.
	struct PairContext
	{
		uint64_t recordSeq = 0;                 // the record the guide reads (the d-th previous): set by the scheduler BEFORE the provider runs
		bool spansSlip = false;                 // IVectorEngine::PairSpansSlip(recordSeq, N)
		bool ancientStamp = false;              // the stamp of the pair is ancient (Indicator::StampOf(...).ancient): counted as a slip
		bool cut = false, fade = false, flash = false;   // P4(N) scene flags
		double fadeGain = 1, fadeOffset = 0;
		double nextPairSeconds = 0;             // duration of the pair about to be searched; <= 0 unknown
		bool flowCpuResident = false;           // temporalSeed input available (engine's CPU flow of N)
		std::vector<float> nvofX, nvofY;        // that flow in source px (grid of the record), else empty
	};

	struct GuideInputs
	{
		const CameraModel::CameraMotion* record = nullptr;   // P7(N-d)
		const FieldCopy* prevField = nullptr;                // its field copy (prevFwd), may be null
		PairContext ctx;
	};

	struct GuideResult
	{
		std::shared_ptr<const CameraModel::GuidanceFrame> frame;   // null = refused
		Refusal refusal = Refusal::AllOff;
		CameraModel::MeasurementPlan plan;                         // counters + fallbackReason (diagnostics)
		bool fromLane = false;
		double confidence = 0;                                     // of the record used (bench / diagnostics)
		const char* reason = "";
	};

	// The ONE unit. Pure and deterministic: the same inputs give the same bytes, on any thread.
	GuideResult BuildGuide(const CameraModel::MeasurementSwitches& sw, const CameraModel::MeasurementConfig& cfg, const GuideInputs& in);

	// Deep equality of two guidance frames (test / lane-vs-live check).
	bool SameGuide(const CameraModel::GuidanceFrame& a, const CameraModel::GuidanceFrame& b);

	// The 16 B counts texture: r/g = narrow accepted fwd/bwd, b/a = E1 skipped fwd/bwd (-1 = not run).
	struct GuidanceOutcome
	{
		bool valid = false;
		uint32_t narrowPlanned = 0, narrowAccepted = 0, e1Eligible = 0, e1Accepted = 0;
		uint32_t escalated = 0;
	};
	GuidanceOutcome ParseGuidanceCounts(const float counts[4], uint32_t narrowBlocksPlanned, uint32_t e1EligibleBlocks);

	struct GuidanceSettings
	{
		CameraModel::MeasurementSwitches switches;
		CameraModel::MeasurementConfig config;
		GovernorConfig governor;
	};

	struct GuidanceCounters
	{
		std::array<uint64_t, size_t(Refusal::Count)> refusals{};
		uint64_t guided = 0, fromLane = 0, fromLive = 0, laneDiscarded = 0, lanePublished = 0, pairsJudged = 0;
		uint64_t narrowIdleEngine = 0, rejectedByMatcher = 0;   // from EstimateOutcome (narrow idle under the engine / per-block guidance refused)
	};

	// What the host read from the matcher after Estimate (MotionWarp::CameraGuidanceUsed / CameraGuidanceCounts / the pass timers).
	// The plain IPreEstimateHook::AfterEstimate carries only guidanceUsed and the counts texture; the rest rides here
	// (CameraGuidanceHook::AfterEstimateEx) so the diagnostics and the benchmark row are complete.
	struct EstimateOutcome
	{
		bool guidanceUsed = false;         // any guided effect ran (model global, static, skip, narrow, E1, fade)
		bool narrowPass = false, e1Pass = false;   // the passes that write the 16 B counts (the counts are stale otherwise)
		bool narrowDroppedEngine = false;  // narrow planned, the engine's field seeds / replaces the searches (never a dropped seed)
		const char* rejected = "";         // non-empty: the per-block guidance was not used (reason)
		const char* gpuNotApplied = "";    // plan parts the GPU matcher does not do yet
		double gpuMs = -1;                 // GPU ms of this Estimate (sum of the motion pass timers), < 0 = not measured
		pl_tex counts = nullptr;           // CameraGuidanceCounts() (read only when a pass that writes it ran)
	};

	// A 16 B counts readback (K1's readback ring shape, one texel). Poll never blocks.
	class IGuidanceCountsSource
	{
	public:
		virtual ~IGuidanceCountsSource() {}
		virtual bool Request(uint64_t pairSeq, pl_tex counts) = 0;
		struct Done { uint64_t pairSeq = 0; float v[4] = { -1, -1, -1, -1 }; };
		virtual size_t Poll(std::vector<Done>& out) = 0;
	};

	class GuidanceScheduler
	{
	public:
		GuidanceScheduler(ICameraLink& link, const GuidanceSettings& settings);

		void SetSettings(const GuidanceSettings& s);
		void SetLag(uint8_t d) { m_lag = d < kMinGuideLagLocal ? kMinGuideLagLocal : d; RefreshSnapshot(); }
		uint8_t Lag() const { return m_lag; }
		// Fills the PairContext of pair N (scene flags from P4, slip from the engine). False: no description possible.
		// The provider is also called from the camera worker (PublishLane): it must be thread-safe and read P4 of the pair from a
		// pinned / published slot, never from render-thread-only state.
		void SetContextProvider(std::function<bool(uint64_t pairSeq, PairContext&)> p) { m_context = std::move(p); RefreshSnapshot(); }
		// Re-checked when a STORED lane result is consumed (the slip can appear after the lane built it): (recordSeq, pairSeq) ->
		// true = the pair spans a slip now. Render thread. Null = no re-check.
		void SetSlipCheck(std::function<bool(uint64_t recordSeq, uint64_t pairSeq)> f) { m_slipCheck = std::move(f); }

		// Camera worker: right after Fit(N-d) once P4(N) is published. Stores the result unless the render already asked.
		// Published: stored for the render. NotReady: the record or P4(N) is not there yet (ask again later; nothing stored).
		// Discarded: the render asked first (counted) or the pair cannot be a lane pair. Off: no switch is on.
		enum class LaneOutcome : uint8_t { Published, NotReady, Discarded, Off };
		LaneOutcome TryPublishLane(uint64_t pairSeq);
		bool PublishLane(uint64_t pairSeq) { return TryPublishLane(pairSeq) == LaneOutcome::Published; }
		// The lag the lane job must use (the snapshot the worker reads; thread-safe, unlike Lag()).
		uint8_t SnapshotLag() const;
		// True when the worker-side snapshot has any switch on (the lane job adds nothing otherwise). Thread-safe.
		bool LaneWanted() const;
		// Render thread: the guidance for pair N (the lane result when it was there first, else the live computation).
		// pairSeconds = the measured duration of the pair (advances the governor's cool-down for every pair asked).
		GuideResult Guide(uint64_t pairSeq, double pairSeconds = 0);

		// The counts of a finished guided pair (from the readback): diagnostics + governor.
		void OnCounts(uint64_t pairSeq, const float counts[4], double pairSeconds);

		// A snapshot: the render thread's counters plus the lane counters the camera worker writes (atomics). Render thread.
		GuidanceCounters Counters() const
		{
			GuidanceCounters c = m_counters;
			c.lanePublished = m_lanePublished.load(std::memory_order_relaxed);
			c.laneDiscarded = m_laneDiscarded.load(std::memory_order_relaxed);
			return c;
		}
		const GuidanceGovernor& Governor() const { return m_governor; }
		// VP_MOTION_BENCH rows (null = none). Written for guided AND refused pairs.
		void SetBench(MotionBenchWriter* w) { m_bench = w; }
		const CameraModel::MeasurementDiagnostics& Diagnostics() const { return m_diag; }
		void ResetForDiscontinuity();   // drops stored lane results and pending plans (vectors-valid state)
		std::vector<std::string> StatusLines() const;
		// The skeleton's visible state (FallbackCatalog Decisions: camera.guidance.escalations_high / .refused). Render thread.
		std::vector<FallbackCatalog::Decision> CollectDecisions() const;
		// The GPU ms of the Estimate that ran pair N guided (the sum of the motion pass timers the host took after Estimate;
		// < 0 = not measured). Goes into the pair's VP_MOTION_BENCH row. Render thread, after BeforeEstimate.
		void NoteGpuMs(uint64_t pairSeq, double ms);
		// TEST SEAM ONLY: stores `result` as the lane result of pair N exactly as TryPublishLane would, but as if it had been built under
		// the thresholds `builtUnder` (the switches, lag and record are the current ones). It bypasses SetSettings' clear, so a test can
		// prove that the threshold key ALONE rejects a stored result. Never called by production code.
		void StoreLaneForTest(uint64_t pairSeq, const GuideResult& result, const CameraModel::MeasurementConfig& builtUnder);
		// What the matcher did with the guide (render thread, right after Estimate): visible diagnostics and the bench's gpu_ms.
		void OnEstimateOutcome(uint64_t pairSeq, const EstimateOutcome& o);

	private:
		struct Stored { uint64_t seq = ~uint64_t(0); uint32_t key = 0; uint32_t cfgKey = 0; uint64_t recSeq = 0; GuideResult result; double gpuMs = -1; };
		static constexpr uint8_t kMinGuideLagLocal = 2;
		GuideResult Compute(uint64_t pairSeq);
		void RefreshSnapshot();   // render thread: publishes the governed switches / lag / config / provider for PublishLane
		uint32_t KeyOf(const CameraModel::MeasurementSwitches& sw, uint8_t lag) const { return SwitchMask(sw) | (uint32_t(lag) << 16); }
		static uint32_t CfgKeyOf(const CameraModel::MeasurementConfig& c);   // every threshold that changes a plan

		ICameraLink& m_link;
		GuidanceSettings m_settings;
		GuidanceGovernor m_governor;
		CameraModel::MeasurementDiagnostics m_diag;
		GuidanceCounters m_counters;                // render thread only
		std::atomic<uint64_t> m_lanePublished{ 0 }, m_laneDiscarded{ 0 };   // the camera worker's counters
		uint64_t m_epoch = 0;                       // under m_mutex: bumped by a discontinuity / new settings; a lane result of an old epoch is never stored
		std::function<bool(uint64_t, uint64_t)> m_slipCheck;
		RefusalHold m_hold;
		double m_narrowIdleRemaining = 0;   // measured seconds the narrow-idle entry is still held
		std::string m_rejectedText, m_gpuNotAppliedText;
		uint8_t m_lag = kMinGuideLagLocal;
		MotionBenchWriter* m_bench = nullptr;
		std::function<bool(uint64_t, PairContext&)> m_context;
		// What the camera worker reads (under m_mutex): the same governed switches the render thread uses.
		CameraModel::MeasurementSwitches m_snapSwitches;
		CameraModel::MeasurementConfig m_snapConfig;
		uint8_t m_snapLag = kMinGuideLagLocal;
		std::function<bool(uint64_t, PairContext&)> m_snapContext;
		mutable std::mutex m_mutex;           // the lane store only
		std::vector<Stored> m_lane;           // ring by seq
		std::vector<Stored> m_pending;        // render-side: the plan awaiting its counts
		std::vector<uint64_t> m_asked;        // render asked for this seq already (a lane result after it is discarded)
	};

	// The hook the vector engine calls around Estimate.
	class CameraGuidanceHook : public VpMotion::IPreEstimateHook
	{
	public:
		CameraGuidanceHook(GuidanceScheduler& scheduler, IGuidanceCountsSource* counts, ICameraLink& link)
			: m_sched(scheduler), m_counts(counts), m_link(link) {}
		void BeforeEstimate(const VpMotion::EstimateContext& ctx, VpMotion::IGuidanceSink& sink) override;
		void AfterEstimate(uint64_t pairSeq, bool guidanceUsed, pl_tex countsTex) override;
		// SHIM: P5a's AfterEstimate carries only guidanceUsed and the counts texture; it forwards into EstimateOutcome here. The one
		// edit when M2 adds the plain-data outcome: an override that maps it onto EstimateOutcome and calls AfterEstimateEx.
		// The full form: everything the host read from the matcher (see EstimateOutcome). AfterEstimate forwards here.
		void AfterEstimateEx(uint64_t pairSeq, const EstimateOutcome& outcome);
		// Render thread, once per frame: finish the pairs whose counts arrived.
		void Poll();
		// The motion pass GPU ms of the Estimate of pair N (< 0 = not measured), for its bench row. Optional.
		void NoteEstimateGpuMs(uint64_t pairSeq, double ms) { m_sched.NoteGpuMs(pairSeq, ms); }
		uint64_t UsedPairs() const { return m_used; }
		// The 16 B counts readback (render thread; null = the counts are never read). Set by the adapter at Attach / Detach.
		void SetCountsSource(IGuidanceCountsSource* counts) { m_counts = counts; }

	private:
		GuidanceScheduler& m_sched;
		IGuidanceCountsSource* m_counts;
		ICameraLink& m_link;
		std::vector<std::pair<uint64_t, double>> m_waiting;   // pairSeq, pairSeconds
		uint64_t m_used = 0;
	};
}
