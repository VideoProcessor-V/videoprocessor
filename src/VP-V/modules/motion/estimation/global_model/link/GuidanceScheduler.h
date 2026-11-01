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
#include "GuidanceGovernor.h"
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
	enum class Refusal : uint8_t { None, AllOff, NoRecord, Slip, NotUsable, LowConfidence, Governor, NoPlan, Count };

	inline const char* Text(Refusal r)
	{
		switch (r)
		{
		case Refusal::None: return "guided";
		case Refusal::AllOff: return "all camera guidance switches off";
		case Refusal::NoRecord: return "camera record late";
		case Refusal::Slip: return "pair spans a slip";
		case Refusal::NotUsable: return "no dominant motion";
		case Refusal::LowConfidence: return "camera confidence too low";
		case Refusal::Governor: return "escalations high";
		case Refusal::NoPlan: return "plan empty";
		default: return "?";
		}
	}

	// What the host knows about the pair about to be searched.
	struct PairContext
	{
		bool spansSlip = false;                 // IVectorEngine::PairSpansSlip(N-d, N) or an ancient stamp
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

		// Camera worker: right after Fit(N-d) once P4(N) is published. Stores the result unless the render already asked.
		bool PublishLane(uint64_t pairSeq);
		// Render thread: the guidance for pair N (the lane result when it was there first, else the live computation).
		// pairSeconds = the measured duration of the pair (advances the governor's cool-down for every pair asked).
		GuideResult Guide(uint64_t pairSeq, double pairSeconds = 0);

		// The counts of a finished guided pair (from the readback): diagnostics + governor.
		void OnCounts(uint64_t pairSeq, const float counts[4], double pairSeconds);

		const GuidanceCounters& Counters() const { return m_counters; }
		const GuidanceGovernor& Governor() const { return m_governor; }
		// VP_MOTION_BENCH rows (null = none). Written for guided AND refused pairs.
		void SetBench(MotionBenchWriter* w) { m_bench = w; }
		const CameraModel::MeasurementDiagnostics& Diagnostics() const { return m_diag; }
		void ResetForDiscontinuity();   // drops stored lane results and pending plans (vectors-valid state)
		std::vector<std::string> StatusLines() const;

	private:
		struct Stored { uint64_t seq = ~uint64_t(0); uint32_t key = 0; GuideResult result; };
		static constexpr uint8_t kMinGuideLagLocal = 2;
		GuideResult Compute(uint64_t pairSeq);
		void RefreshSnapshot();   // render thread: publishes the governed switches / lag / config / provider for PublishLane
		uint32_t KeyOf(const CameraModel::MeasurementSwitches& sw, uint8_t lag) const { return SwitchMask(sw) | (uint32_t(lag) << 16); }

		ICameraLink& m_link;
		GuidanceSettings m_settings;
		GuidanceGovernor m_governor;
		CameraModel::MeasurementDiagnostics m_diag;
		GuidanceCounters m_counters;
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
		// Render thread, once per frame: finish the pairs whose counts arrived.
		void Poll();
		uint64_t UsedPairs() const { return m_used; }

	private:
		GuidanceScheduler& m_sched;
		IGuidanceCountsSource* m_counts;
		ICameraLink& m_link;
		std::vector<std::pair<uint64_t, double>> m_waiting;   // pairSeq, pairSeconds
		uint64_t m_used = 0;
	};
}
