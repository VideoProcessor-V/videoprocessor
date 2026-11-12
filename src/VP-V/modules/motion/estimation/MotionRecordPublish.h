// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV motion record publish (motion bypass scan row 24). The shared motion engine's whole-frame (pan) result for a pair, written
// into the frame-analysis record's motion group (FrameAnalysis::Store::PublishMotion) so the record carries the ONE shared global
// instead of an empty group. Additive: no consumer reads the motion group today (CameraExtension reads identity / scene / cadence
// only), so nothing changes behaviour; the group is there for the consumers that come after.
//
// What is published, and when:
//   - The pair's global motion as the engine reports it (flow->global, D1: 1/8-size texels, z = ambiguity, w = validity), in source
//     pixels PER FRAME (divided by the periods the pair spans, so an adjacent pair is the engine's EngineGlobal x / y exactly), under
//     the record of the pair's later frame, with pairFrom and source = EngineAhead. magnitudeP50 / P95 stay 0 (no block statistics
//     exist at the call site; a field-wide magnitude is not computed for this).
//   - ABSENT (a default MotionSummary: vectorsValid false, zeros, source None) when the P5a vectors-valid state (MotionValidity, the
//     one state) does not hold the pair, when there is no engine pair, or when the engine's global is not usable (ambiguity at or
//     above the threshold VectorDemand::SampleGlobal applies to every other consumer). Never a stale or last-known value.
//   - Nothing at all when there is no store (Original pathway, frame_analysis_store off) or the record is gone / from before a
//     discontinuity (PublishMotion refuses it: a pair never spans a discontinuity).
// Threads: the render thread (ServeStatistics); the Store has its own mutex.
//
// W1-C4: ONE engine-global sample per pair (SampleOf: the only VectorDemand::SampleGlobal of a pair on the statistics and the
// estimate paths), read by the record (SummaryOf) and by the ONE feed of the source-statistics consumers (Feed: source monitor
// fast-pan, motion intent). The two paths that reach Feed are exclusive on a frame (ServeStatistics requires !vectorDemand.estimate
// and no legacy estimate consumer); both gate with the demand's Serves while vector_demand is on (EstimateGate, WAIVER-C4-01).
//
// Pure CPU, header only, C++14.
#pragma once

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/consumers/DejudderPolicy.h>

#include <atomic>
#include <cstdint>

namespace VpMotion
{
	namespace MotionRecord
	{
		inline FrameAnalysis::MotionSummary Absent()
		{
			return FrameAnalysis::MotionSummary();
		}

		// global: the engine's D1 whole-frame motion (x, y, z, w) or null (no engine pair); halvings: the search halvings the engine
		// size was made with (DejudderPolicy::EngineCellSourcePixels). pairValid: MotionValidity::PairValid for (from, to), asked by
		// the caller of the facade at the moment of the publish.
		// W1-C4: one pair's engine global, sampled once (VectorDemand::SampleGlobal). pictureHeight: the active picture's height for
		// the intent's heights per frame (0 = none; usable and sourcePixels do not depend on it).
		struct PairSample
		{
			bool present = false;          // an engine global exists for the pair (else every reader sees "absent")
			uint64_t from = 0, to = 0;
			float gx = 0.0f, gy = 0.0f;    // the engine's D1 global x / y (1/8-size texels), for the record
			int halvings = 0;
			VectorDemand::GlobalSample global;
		};

		// W1-C4 (audit MUST 1): how many engine globals were sampled (VectorDemand::SampleGlobal runs) since the start, any thread
		// (relaxed). Read by the tests to pin ONE sample per pair; one relaxed add per pair, no other cost.
		inline std::atomic<uint64_t>& SamplesTaken()
		{
			static std::atomic<uint64_t> taken(0);
			return taken;
		}

		inline PairSample SampleOf(uint64_t from, uint64_t to, const float* global, int halvings, int pictureHeight)
		{
			PairSample s;
			s.from = from;
			s.to = to;
			s.halvings = halvings;
			if (!global)
				return s;
			SamplesTaken().fetch_add(1, std::memory_order_relaxed);
			s.present = true;
			s.gx = global[0];
			s.gy = global[1];
			s.global = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(global[0]), static_cast<double>(global[1]), halvings), global[2], global[3], pictureHeight);
			return s;
		}

		// The record's motion group from the pair's one sample.
		inline FrameAnalysis::MotionSummary SummaryOf(bool pairValid, const PairSample& s)
		{
			if (!pairValid || !s.present || s.from == 0 || s.to <= s.from || !s.global.usable)
				return Absent();
			const double cell = DejudderPolicy::EngineCellSourcePixels(s.halvings);
			const double periods = static_cast<double>(s.to - s.from);
			FrameAnalysis::MotionSummary m;
			m.vectorsValid = true;
			m.pairFrom = s.from;
			m.globalX = cell * static_cast<double>(s.gx) / periods;
			m.globalY = cell * static_cast<double>(s.gy) / periods;
			m.source = FrameAnalysis::VectorSource::EngineAhead;
			return m;
		}

		inline FrameAnalysis::MotionSummary SummaryOf(bool pairValid, uint64_t from, uint64_t to, const float* global, int halvings)
		{
			if (!pairValid || !global || from == 0 || to <= from)
				return Absent();
			return SummaryOf(pairValid, SampleOf(from, to, global, halvings, 0));
		}

		// W1-C4: the ONE feed of the source-statistics consumers from a pair's sample, in the order both paths always used: the
		// source monitor's fast-pan motion (a usable global), then the intent's vectors (the intent active). The gates are the
		// caller's: ServeStatistics passes VectorDemand::Serves(SourceMonitorPan) / Serves(MotionIntent); the render-time estimate
		// passes EstimateGate (the same Serves while the demand decides; true / true, its old ungated feed, with vector_demand off).
		inline void Feed(const PairSample& s, bool monitorGate, bool intentGate, IVectorEngineHost& host)
		{
			if (!s.present)
				return;
			if (s.global.usable && monitorGate)
				host.NoteSourceMonitorMotion(s.global.sourcePixels, s.to);
			if (intentGate && host.IntentActive())
			{
				IntentInterpreter::VectorSample vectors;
				vectors.from = s.from;
				vectors.to = s.to;
				vectors.valid = s.global.intentValid;
				vectors.heightsPerFrame = s.global.heightsPerFrame;
				host.NoteIntentVectors(vectors);
			}
		}

		// W1-C4 (audit MUST 2, WAIVER-C4-01): the render-time estimate's gate of one source-statistics consumer. While the demand
		// decides (vector_demand on) the gate is its Serves, as ServeStatistics passes; with vector_demand off the demand decides
		// nothing (Reason::Disabled, served 0) and the legacy estimate's ungated feed is kept, so that mode is unchanged.
		inline bool EstimateGate(const VectorDemand::Decision& demand, VectorDemand::Consumer c)
		{
			return demand.reason == VectorDemand::Reason::Disabled || demand.Serves(c);
		}

		// W1-C4 (audit MUST 1 / 2): the render-time estimate's whole engine-global step for the pair (from, to): ONE sample, read by the
		// engine's CPU record (VectorEngine::NoteEngineGlobal, usable from the sample) and by the one feed with the demand's gates.
		// global null = no producer pair: the record says so and nothing is fed (as before).
		inline void EstimatePair(VectorEngine& engine, const VectorDemand::Decision& demand, uint64_t from, uint64_t to,
			const float* global, int halvings, int pictureHeight, IVectorEngineHost& host)
		{
			const PairSample s = SampleOf(from, to, global, halvings, pictureHeight);
			engine.NoteEngineGlobal(from, to, global, halvings, s.present && s.global.usable);
			Feed(s, EstimateGate(demand, VectorDemand::Consumer::SourceMonitorPan), EstimateGate(demand, VectorDemand::Consumer::MotionIntent),
				host);
		}

		// False = nothing was written (no store, or the record is gone / from an older epoch).
		inline bool Publish(FrameAnalysis::Store* store, uint64_t to, const FrameAnalysis::MotionSummary& summary)
		{
			return store && to != 0 && store->PublishMotion(to, summary);
		}
	}
}
