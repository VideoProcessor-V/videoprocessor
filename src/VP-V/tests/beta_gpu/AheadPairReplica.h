// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera AHEAD FIT measurement (test code only, the camera accuracy report): an OFFLINE REPRODUCTION of the MotionAhead producer's NVOF
// pair, because the Beta test host does not link the producer (MotionAheadProducer.cpp needs the renderer's pass-timer inbox, VRAM users
// and its own D3D11 device; adding it to the host would be a second runtime copy). Step for step it is the producer's Process():
//   * the half-size 8-bit luma by the shared half-luma unit (VpUnit::HalfLuma: Tap0 / Tap1, Box4, FinishEngine8 of the 10-bit codes),
//     the quarter (the 2 x 2 rounded mean of the half) when the de-judder searches at a quarter (lowRes 4K) - Producer::SetTargetSize;
//   * the same engine (NvOpticalFlow::Engine, the SLOW preset VectorEngine::PlanProducer starts it with, costs on), two luma textures in
//     ping-pong, the temporal hints only from the second pair on (continuous), both directions and both costs per pair;
//   * the same readback (staging copies, Map, row copies) and the same extras (MotionAhead::FlowGlobal both ways, MotionAhead::FlowExtras).
// Differences, stated in the report: the engine runs on the libplacebo device's immediate context (MotionWarp's own in-process engine does
// the same) instead of a device of its own, synchronously on the harness thread (the timing model in the report puts it on its own lane);
// the render's GPU load is absent, so the pair time is a lower bound of the live one.
#pragma once

#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/modules/motion/estimation/MotionAheadProducer.h>
#include <VP-V/modules/motion/estimation/NvOpticalFlow.h>

#include <libplacebo/d3d11.h>

#include <d3d11.h>
#include <atlbase.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace CameraAccuracyAhead
{
	struct PairTiming
	{
		double lumaMs = 0;       // half (and quarter) luma on the CPU
		double engineMs = 0;     // upload + engine (both directions) + staging copies, drained
		double readbackMs = 0;   // Map + row copies (the producer's CPU readback)
		double extrasMs = 0;     // FlowGlobal x 2 + FlowExtras
		double PairMs() const { return lumaMs + engineMs + readbackMs + extrasMs; }
	};

	class Replica
	{
	public:
		// srcW x srcH 16-bit luma (10-bit codes << 6); quarter = the de-judder searches at a quarter (lowRes).
		bool Open(pl_gpu gpu, int srcW, int srcH, bool quarter, std::string& why)
		{
			pl_d3d11 d3d = pl_d3d11_get(gpu);
			if (!d3d || !d3d->device)
			{
				why = "not a D3D11 device";
				return false;
			}
			m_device = d3d->device;
			m_device->GetImmediateContext(&m_context);
			m_srcW = srcW;
			m_srcH = srcH;
			m_halfW = static_cast<int>(VpUnit::HalfLuma::HalfDim(static_cast<uint32_t>(srcW)));
			m_halfH = static_cast<int>(VpUnit::HalfLuma::HalfDim(static_cast<uint32_t>(srcH)));
			m_quarter = quarter;
			m_w = quarter ? (m_halfW + 1) / 2 : m_halfW;
			m_h = quarter ? (m_halfH + 1) / 2 : m_halfH;
			const int gw = (m_w + 3) / 4, gh = (m_h + 3) / 4;
			auto make = [&](int tw, int th, DXGI_FORMAT format, bool staging, CComPtr<ID3D11Texture2D>& tex) {
				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = static_cast<UINT>(tw); desc.Height = static_cast<UINT>(th);
				desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = format; desc.SampleDesc.Count = 1;
				desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
				desc.BindFlags = staging ? 0 : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
				desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
				return SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &tex));
			};
			for (int i = 0; i < 2; ++i)
				if (!make(m_w, m_h, NvOpticalFlow::Engine::InputFormat(), false, m_luma[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), false, m_flow[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), false, m_cost[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::FlowFormat(), true, m_flowRead[i]) ||
					!make(gw, gh, NvOpticalFlow::Engine::CostFormat(), true, m_costRead[i]))
				{
					why = "cannot create the textures";
					return false;
				}
			m_engine.reset(new NvOpticalFlow::Engine());
			if (!m_engine->Open(m_device, m_context, m_w, m_h, NvOpticalFlow::Preset::Slow, true))
			{
				why = m_engine->LastError();
				m_engine.reset();
				return false;
			}
			m_have = false;
			return true;
		}

		int EngineW() const { return m_w; }
		int EngineH() const { return m_h; }

		// Frame by frame, oldest first. From the second frame on, `out` is the pair (previous -> this) and true.
		bool Push(const std::vector<uint16_t>& luma, MotionAhead::Flow& out, PairTiming& t)
		{
			t = PairTiming();
			auto now = []() { return std::chrono::steady_clock::now(); };
			auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
				return std::chrono::duration<double, std::milli>(b - a).count();
			};
			const auto t0 = now();
			HalfLuma8(luma);
			const auto t1 = now();
			t.lumaMs = ms(t0, t1);
			const int slot = m_have ? 1 - m_current : m_current;
			m_context->UpdateSubresource(m_luma[slot], 0, nullptr, m_bytes.data(), static_cast<UINT>(m_w), 0);
			bool produced = false;
			if (m_have)
			{
				if (!m_engine->Run(m_luma[m_current], m_luma[slot], m_flow[0], m_flow[1], m_cost[0], m_cost[1], nullptr, m_pairs > 0))
					throw std::runtime_error("camera accuracy (ahead): the engine failed: " + m_engine->LastError());
				for (int i = 0; i < 2; ++i)
				{
					m_context->CopyResource(m_flowRead[i], m_flow[i]);
					m_context->CopyResource(m_costRead[i], m_cost[i]);
				}
				out = MotionAhead::Flow();
				out.halfW = m_w;
				out.halfH = m_h;
				out.gridW = (m_w + 3) / 4;
				out.gridH = (m_h + 3) / 4;
				const size_t cells = static_cast<size_t>(out.gridW) * out.gridH;
				D3D11_MAPPED_SUBRESOURCE probe{};
				// The first Map waits for the engine and the copies: the engine time ends here, the CPU readback starts.
				if (FAILED(m_context->Map(m_flowRead[0], 0, D3D11_MAP_READ, 0, &probe)))
					throw std::runtime_error("camera accuracy (ahead): Map failed");
				m_context->Unmap(m_flowRead[0], 0);
				const auto t2 = now();
				t.engineMs = ms(t1, t2);
				for (int i = 0; i < 2; ++i)
				{
					std::vector<int16_t>& vectors = i == 0 ? out.forward : out.backward;
					std::vector<uint8_t>& costs = i == 0 ? out.forwardCost : out.backwardCost;
					vectors.resize(cells * 2);
					costs.resize(cells);
					D3D11_MAPPED_SUBRESOURCE map{};
					if (FAILED(m_context->Map(m_flowRead[i], 0, D3D11_MAP_READ, 0, &map)))
						throw std::runtime_error("camera accuracy (ahead): Map failed");
					for (int y = 0; y < out.gridH; ++y)
						std::memcpy(&vectors[static_cast<size_t>(y) * out.gridW * 2], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
							static_cast<size_t>(out.gridW) * 4);
					m_context->Unmap(m_flowRead[i], 0);
					if (FAILED(m_context->Map(m_costRead[i], 0, D3D11_MAP_READ, 0, &map)))
						throw std::runtime_error("camera accuracy (ahead): Map failed");
					for (int y = 0; y < out.gridH; ++y)
						std::memcpy(&costs[static_cast<size_t>(y) * out.gridW], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
							static_cast<size_t>(out.gridW));
					m_context->Unmap(m_costRead[i], 0);
				}
				const auto t3 = now();
				t.readbackMs = ms(t2, t3);
				MotionAhead::FlowGlobal(out.forward, out.forwardCost, out.gridW, out.gridH, out.global[0], out.second[0], true);
				MotionAhead::FlowGlobal(out.backward, out.backwardCost, out.gridW, out.gridH, out.global[1], out.second[1], true);
				MotionAhead::FlowExtras(out);
				t.extrasMs = ms(t3, now());
				++m_pairs;
				produced = true;
			}
			else
				t.engineMs = ms(t1, now());
			m_current = slot;
			m_have = true;
			return produced;
		}

	private:
		// The producer's HalfLuma8 for P010 (10-bit codes = words >> 6), then its quarter step.
		void HalfLuma8(const std::vector<uint16_t>& luma)
		{
			using namespace VpUnit::HalfLuma;
			std::vector<uint8_t> half(static_cast<size_t>(m_halfW) * m_halfH);
			for (int y = 0; y < m_halfH; ++y)
			{
				const uint16_t* r0 = &luma[static_cast<size_t>(Tap0(y, m_srcH)) * m_srcW];
				const uint16_t* r1 = &luma[static_cast<size_t>(Tap1(y, m_srcH)) * m_srcW];
				for (int x = 0; x < m_halfW; ++x)
				{
					const int x0 = Tap0(x, m_srcW), x1 = Tap1(x, m_srcW);
					half[static_cast<size_t>(y) * m_halfW + x] = FinishEngine8(Box4(r0[x0] >> 6, r0[x1] >> 6, r1[x0] >> 6, r1[x1] >> 6));
				}
			}
			if (!m_quarter)
			{
				m_bytes.swap(half);
				return;
			}
			m_bytes.resize(static_cast<size_t>(m_w) * m_h);
			for (int y = 0; y < m_h; ++y)
			{
				const uint8_t* q0 = &half[static_cast<size_t>((std::min)(2 * y, m_halfH - 1)) * m_halfW];
				const uint8_t* q1 = &half[static_cast<size_t>((std::min)(2 * y + 1, m_halfH - 1)) * m_halfW];
				for (int x = 0; x < m_w; ++x)
				{
					const int x0 = (std::min)(2 * x, m_halfW - 1), x1 = (std::min)(2 * x + 1, m_halfW - 1);
					m_bytes[static_cast<size_t>(y) * m_w + x] = static_cast<uint8_t>((q0[x0] + q0[x1] + q1[x0] + q1[x1] + 2) / 4);
				}
			}
		}

		CComPtr<ID3D11Device> m_device;
		CComPtr<ID3D11DeviceContext> m_context;
		std::unique_ptr<NvOpticalFlow::Engine> m_engine;
		CComPtr<ID3D11Texture2D> m_luma[2], m_flow[2], m_cost[2], m_flowRead[2], m_costRead[2];
		std::vector<uint8_t> m_bytes;
		int m_srcW = 0, m_srcH = 0, m_halfW = 0, m_halfH = 0, m_w = 0, m_h = 0;
		bool m_quarter = false, m_have = false;
		int m_current = 0;
		unsigned m_pairs = 0;
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
