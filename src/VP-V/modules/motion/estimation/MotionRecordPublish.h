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
// Pure CPU, header only, C++14.
#pragma once

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/vector_delivery/DejudderPolicy.h>

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
		inline FrameAnalysis::MotionSummary SummaryOf(bool pairValid, uint64_t from, uint64_t to, const float* global, int halvings)
		{
			if (!pairValid || !global || from == 0 || to <= from)
				return Absent();
			const VectorDemand::GlobalSample sample = VectorDemand::SampleGlobal(DejudderPolicy::EngineGlobalSourcePixels(
				static_cast<double>(global[0]), static_cast<double>(global[1]), halvings), global[2], global[3], 0);
			if (!sample.usable)
				return Absent();
			const double cell = DejudderPolicy::EngineCellSourcePixels(halvings);
			const double periods = static_cast<double>(to - from);
			FrameAnalysis::MotionSummary m;
			m.vectorsValid = true;
			m.pairFrom = from;
			m.globalX = cell * static_cast<double>(global[0]) / periods;
			m.globalY = cell * static_cast<double>(global[1]) / periods;
			m.source = FrameAnalysis::VectorSource::EngineAhead;
			return m;
		}

		// False = nothing was written (no store, or the record is gone / from an older epoch).
		inline bool Publish(FrameAnalysis::Store* store, uint64_t to, const FrameAnalysis::MotionSummary& summary)
		{
			return store && to != 0 && store->PublishMotion(to, summary);
		}
	}
}
