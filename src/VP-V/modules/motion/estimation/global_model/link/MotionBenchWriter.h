// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-5): the benchmark hooks. VP_MOTION_BENCH=<csv> writes one row per guided/refused pair (format in
// tools/eval/motion_bench/README.md): pair,seq,switch_mask,d,confidence,reason,narrow,full,escalated,skipped,governor_state,gpu_ms.
// Off (variable unset) = the writer is closed and costs one branch. Also the PassTimers row names the camera link feeds.
// Header-only, C++14, thread-safe (one mutex), deterministic text.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace CameraLink
{
	// PassTimers row names (core/PassTimers.h keys rows by name): the benchmark reads these.
	constexpr const char* kTimerFit = "camera.fit";                  // CPU, the fit on the camera worker
	constexpr const char* kTimerReadback = "camera.readback";        // GPU, the field copy
	constexpr const char* kTimerLabels = "camera.labels";            // GPU, P8
	constexpr const char* kTimerEstimate = "motion.estimate";
	constexpr const char* kTimerGuidedPass = "motion.guided_pass";
	constexpr const char* kTimerBlockSearch = "motion.block_search";
	constexpr const char* kTimerD1 = "motion.d1";

	constexpr const char* kBenchHeader = "pair,seq,switch_mask,d,confidence,reason,narrow,full,escalated,skipped,governor_state,gpu_ms";

	inline uint32_t SwitchMask(const CameraModel::MeasurementSwitches& s)
	{
		return (s.modelGlobal ? 1u : 0u) | (s.guidedCandidate ? 2u : 0u) | (s.narrowWindow ? 4u : 0u) | (s.e1Skip ? 8u : 0u) |
			(s.staticGate ? 16u : 0u) | (s.cutSkip ? 32u : 0u) | (s.fadeNormalised ? 64u : 0u) | (s.flashBridge ? 128u : 0u) |
			(s.temporalSeed ? 256u : 0u) | (s.tinyMoverRoi ? 512u : 0u) | (s.objects ? 1024u : 0u);
	}

	struct BenchRow
	{
		uint64_t pair = 0, seq = 0;
		uint32_t switchMask = 0;
		int d = 0;
		double confidence = 0;
		const char* reason = "";
		uint32_t narrow = 0, full = 0, escalated = 0, skipped = 0;
		int governorState = 0;   // 0 normal, 1 tripped
		double gpuMs = -1;       // < 0: not measured (empty cell)
	};

	class MotionBenchWriter
	{
	public:
		// Opens the file named by VP_MOTION_BENCH (or `path`); closed when neither is set.
		void Open(const char* path = nullptr)
		{
			std::string p = path ? path : "";
			if (p.empty())
			{
#ifdef _WIN32
				char* v = nullptr;
				size_t n = 0;
				if (_dupenv_s(&v, &n, "VP_MOTION_BENCH") == 0 && v)
				{
					p = v;
					std::free(v);
				}
#else
				if (const char* v = std::getenv("VP_MOTION_BENCH"))
					p = v;
#endif
			}
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_out.is_open())
				m_out.close();
			if (p.empty())
				return;
			m_out.open(p.c_str(), std::ios::out | std::ios::trunc);
			if (m_out.is_open())
				m_out << kBenchHeader << "\n";
		}
		bool IsOpen() const { return m_open(); }

		void Write(const BenchRow& r)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_out.is_open())
				return;
			char b[320];
			char gpu[32] = "";
			if (r.gpuMs >= 0)
				std::snprintf(gpu, sizeof(gpu), "%.4f", r.gpuMs);
			std::snprintf(b, sizeof(b), "%llu,%llu,%u,%d,%.6g,%s,%u,%u,%u,%u,%d,%s\n", (unsigned long long)r.pair,
				(unsigned long long)r.seq, r.switchMask, r.d, r.confidence, r.reason, r.narrow, r.full, r.escalated, r.skipped,
				r.governorState, gpu);
			m_out << b;
		}

		void Close()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_out.is_open())
				m_out.close();
		}

	private:
		bool m_open() const { return const_cast<MotionBenchWriter*>(this)->OpenState(); }
		bool OpenState()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_out.is_open();
		}
		std::mutex m_mutex;
		std::ofstream m_out;
	};
}
