// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera accuracy report (test code only): the REAL MotionAhead producer for the Beta test host (WAIVER-MV-04). After motion unification
// (SK-MV, owner decision 2026-11-08: ONE motion source) the matcher opens no NVOF engine; the only optical-flow input is the pair the
// MotionAhead::Producer publishes (P2). This host used to have no producer, so the camera accuracy report could not run its baseline
// variants ("the optical flow engine did not run") and reproduced the ahead pair with a private engine copy. Now the host links the
// production MotionAheadProducer.cpp and drives MotionAhead::Producer itself (Start on the render device's adapter, Submit each frame as a
// P010 AnalysisLumaSource, Take the pair from its P2 slot), and hands the same pair to the de-judder through SetExternalFlow, exactly as the
// renderer does through the vector engine's P2View. No private motion estimation lives in the test (rule 21).
// Differences from live, stated in the report: frames are submitted one at a time and each pair is waited for (no look-ahead queue; the
// timing model of the report places the producer on its own lane), and the render's GPU load is absent, so the pair time is a lower bound
// of the live one.
#pragma once

#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>
#include <AnalysisLumaSource.h>

#include <libplacebo/d3d11.h>

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace CameraAccuracyAhead
{
	struct PairTiming
	{
		double lumaMs = 0;       // half (and quarter) luma on the CPU (the producer's lumaMs)
		double engineMs = 0;     // upload + engine (both directions) + staging copies + readback (the producer's lastPairMs less luma and extras)
		double readbackMs = 0;   // included in engineMs: the real producer reports one pair time
		double extrasMs = 0;     // FlowGlobal x 2 + FlowExtras (the producer's extrasMs)
		double PairMs() const { return lumaMs + engineMs + readbackMs + extrasMs; }
	};

	// Runs the production producer over one scene's frames and keeps its published pairs.
	class RealProducer
	{
	public:
		// quarter = the de-judder searches at a quarter (lowRes 4K): the producer's SetTargetSize, as the vector engine sets it.
		bool Open(pl_gpu gpu, int srcW, int srcH, bool quarter, std::string& why)
		{
			pl_d3d11 d3d = pl_d3d11_get(gpu);
			if (!d3d || !d3d->device)
			{
				why = "not a D3D11 device";
				return false;
			}
			m_srcW = srcW;
			m_srcH = srcH;
			const int halfW = (srcW + 1) / 2, halfH = (srcH + 1) / 2;
			if (quarter)
				m_producer.SetTargetSize((halfW + 1) / 2, (halfH + 1) / 2);
			if (!m_producer.Start(d3d->device, NvOpticalFlow::Preset::Slow))
			{
				why = "the producer did not start: " + m_producer.Error();
				return false;
			}
			return true;
		}

		MotionAhead::Producer& Producer() { return m_producer; }

		// Submits frame f (luma = 10-bit codes << 6, srcW x srcH; chroma = the interleaved 4:2:0 plane, srcW x srcH/2 words) and waits for the
		// pair (f - 1 -> f). On true, `flow` is that pair (null for f = 0) and `t` the producer's own timing of it.
		bool Push(uint64_t f, const std::vector<uint16_t>& luma, const std::vector<uint16_t>& chroma, MotionAhead::FlowPtr& flow, PairTiming& t,
			std::string& why)
		{
			flow.reset();
			t = PairTiming();
			std::vector<uint8_t>& buf = m_buffer[f & 1];
			const size_t lumaBytes = static_cast<size_t>(m_srcW) * m_srcH * 2, chromaBytes = chroma.size() * 2;
			buf.resize(lumaBytes + chromaBytes);   // P010: the luma plane, then the interleaved chroma plane, rows of srcW words
			std::memcpy(buf.data(), luma.data(), lumaBytes);
			std::memcpy(buf.data() + lumaBytes, chroma.data(), chromaBytes);
			AnalysisLumaSource source;
			source.data = buf.data();
			source.dataBytes = buf.size();
			source.width = m_srcW;
			source.height = m_srcH;
			source.rowBytes = static_cast<size_t>(m_srcW) * 2;
			source.chromaRowBytes = static_cast<size_t>(m_srcW) * 2;
			source.format = AnalysisLumaFormat::P010;
			m_producer.Submit(f, 1, source);
			// Frame 0 opens the engine (first use of the device); a pair is ready within seconds of it.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(f == 0 ? 120 : 60);
			while (std::chrono::steady_clock::now() < deadline)
			{
				const MotionAhead::Producer::Stats st = m_producer.GetStats();
				if (st.frames >= f + 1)
				{
					if (f == 0)
						return true;
					flow = m_producer.Take(f - 1, f);
					if (!flow)
					{
						why = "the producer published no pair for frame " + std::to_string(f) + ": " + m_producer.Error();
						return false;
					}
					t.lumaMs = st.lumaMs;
					t.extrasMs = st.extrasMs;
					t.engineMs = (std::max)(0.0, st.lastPairMs - st.lumaMs - st.extrasMs);
					return true;
				}
				if (!m_producer.Running())
				{
					why = "the producer stopped: " + m_producer.Error();
					return false;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			why = "the producer made no pair for frame " + std::to_string(f) + " in time: " + m_producer.Error();
			return false;
		}

		void Stop() { m_producer.Stop(); }

	private:
		MotionAhead::Producer m_producer;
		std::vector<uint8_t> m_buffer[2];
		int m_srcW = 0, m_srcH = 0;
	};

	// The FlowView VectorEngine::FlowOf serves for this pair (SK-MV, r1s3/mv-fu): the raw payload, units and cell size, so the harness feeds
	// the production CopyAhead exactly the shape the live path does. sourceW = the source width the engine size derives from.
	inline VpMotion::FlowView ViewOf(const MotionAhead::Flow& flow, uint64_t from, uint64_t to, int sourceW)
	{
		VpMotion::FlowView v;
		const double scale = static_cast<double>(sourceW) / flow.halfW;
		v.gridW = flow.gridW;
		v.gridH = flow.gridH;
		v.blockSize = 4.0 * scale;
		v.from = from;
		v.to = to;
		v.rawForward = flow.forward.data();
		v.rawBackward = flow.backward.empty() ? nullptr : flow.backward.data();
		v.forwardCost = flow.forwardCost.empty() ? nullptr : flow.forwardCost.data();
		v.backwardCost = flow.backwardCost.empty() ? nullptr : flow.backwardCost.data();
		v.rawUnitsPerEnginePx = 32.0;
		v.enginePxToSource = scale;
		v.cellEnginePx = 4;
		v.engineW = flow.halfW;
		v.engineH = flow.halfH;
		return v;
	}
}
