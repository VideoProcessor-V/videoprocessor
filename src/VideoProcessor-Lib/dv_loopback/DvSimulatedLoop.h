#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// DV tunnel loopback (2026-10-17): the output side and the simulated loop.
//
// IDvLoopOutput is where the harness sends each wire picture: the hardware
// backend (VPV through NVIDIA's experimental DV mode, src/VideoProcessor-
// DvLoopback) or DvSimulatedLoop, which connects the output buffer to the
// capture path in memory: it stores the RGB8 picture the way a capture card
// does (DvCaptureContainer.h), with a programmable fault model (latency,
// capture misses, output repeats, wire bit errors, a card that rescales
// range), and delivers VideoState + VideoFrame to a capture sink exactly as
// a capture device hands them to VP. Timestamps are virtual (frame n is sent
// at n periods), so the simulated runs are deterministic.
#include "DvCaptureContainer.h"
#include "DvLoopbackAnalyzer.h"
#include <DolbyVisionTunnelGate.h>
#include <BitDepth.h>
#include <HDRData.h>
#include <ColorSpace.h>
#include <deque>
#include <set>
#include <vector>

namespace DvLoopback
{
	class IDvLoopOutput
	{
	public:
		virtual ~IDvLoopOutput() = default;
		virtual void Begin(uint32_t width, uint32_t height, unsigned rateNumerator, unsigned rateDenominator) = 0;
		// sentQpc: when the picture left (QPC ticks, or virtual ticks).
		virtual void Send(const LoopFrame& frame, int64_t sentQpc) = 0;
		virtual void End() = 0;
	};

	struct WireBitError
	{
		uint32_t sentIndex = 0;
		size_t byte = 0;          // offset in the RGB8 picture (pixel * 3 + channel)
		uint8_t mask = 1;
	};

	struct LoopFaults
	{
		ContainerOptions container;            // how the card stores the RGB8 picture
		uint32_t latencyFrames = 0;            // pictures in flight before the first capture
		std::set<uint32_t> captureMisses;      // sent indices the capture never delivers (counter gap)
		std::set<uint32_t> outputRepeats;      // sent indices the output sends twice
		std::vector<WireBitError> bitErrors;
		// The capture state VP builds (DolbyVisionTunnelGate.h): the tunnel
		// switch and what the card reports. forceTunnelFlag bypasses the gate
		// (R10b/R10l, which the formatter accepts but the GUI gate does not).
		bool tunnelSwitch = true;
		bool forceTunnelFlag = false;
		BitDepth cardInputBitDepth = BitDepth::BITDEPTH_8BIT;
		EOTF cardEotf = EOTF::SDR;
		ColorSpace cardColorspace = ColorSpace::REC_709;
		bool cardHdrInfoFrame = false;
		// What a capture device that reports the Dolby VSIF would say
		// (DeckLink: Unknown). DV input fixes 2026-10-18.
		DolbyVisionVsif cardVsif = DolbyVisionVsif::Unknown;
	};

	class DvSimulatedLoop : public IDvLoopOutput
	{
	public:
		DvSimulatedLoop(IDvCaptureSink& sink, const LoopFaults& faults) : m_sink(sink), m_faults(faults) {}

		void Begin(uint32_t width, uint32_t height, unsigned rateNumerator, unsigned rateDenominator) override
		{
			m_width = width;
			m_height = height;
			m_state = new VideoState();
			m_state->valid = true;
			m_state->displayMode = std::make_shared<DisplayMode>(width, height, false, rateNumerator, rateDenominator);
			m_state->videoFrameEncoding = m_faults.container.encoding;
			m_state->eotf = m_faults.cardEotf;
			m_state->colorspace = m_faults.cardColorspace;
			if (m_faults.cardHdrInfoFrame)
				m_state->hdrData = std::make_shared<HDRData>();
			m_state->dolbyVisionVsif = m_faults.cardVsif;
			DolbyVisionTunnelGateInput gate;
			gate.tunnelSwitch = m_faults.tunnelSwitch;
			gate.libplaceboRenderer = true;
			gate.captureStateValid = true;
			gate.encoding = m_state->videoFrameEncoding;
			gate.eotf = m_state->eotf;
			m_state->dolbyVisionTunnel = m_faults.forceTunnelFlag || DolbyVisionTunnelGateOpen(gate);
			m_container = m_faults.container;
			m_container.rowBytes = m_state->BytesPerRow();
			m_sink.OnState(m_state);
		}

		void Send(const LoopFrame& frame, int64_t sentQpc) override
		{
			const int copies = m_faults.outputRepeats.count(frame.index) ? 2 : 1;
			for (int copy = 0; copy < copies; ++copy)
			{
				std::vector<uint8_t> rgb = frame.rgb8;
				for (const WireBitError& e : m_faults.bitErrors)
					if (e.sentIndex == frame.index && e.byte < rgb.size())
						rgb[e.byte] ^= e.mask;
				InFlight item;
				item.index = frame.index;
				item.sentQpc = sentQpc;
				WrapRgb8(rgb, m_width, m_height, m_container, item.bytes);
				m_queue.push_back(std::move(item));
				while (m_queue.size() > m_faults.latencyFrames)
					Deliver();
			}
		}

		void End() override
		{
			while (!m_queue.empty())
				Deliver();
		}

		const VideoStateComPtr& State() const { return m_state; }
		uint64_t Period() const { return m_periodTicks; }
		void SetPeriodTicks(uint64_t ticks) { m_periodTicks = ticks; }

	private:
		struct InFlight { uint32_t index = 0; int64_t sentQpc = 0; std::vector<uint8_t> bytes; };

		void Deliver()
		{
			InFlight item = std::move(m_queue.front());
			m_queue.pop_front();
			++m_counter;   // the card counts every picture on the wire, delivered or not
			const int64_t arrival = static_cast<int64_t>(m_wireSlot++) * static_cast<int64_t>(m_periodTicks) +
				static_cast<int64_t>(m_faults.latencyFrames) * static_cast<int64_t>(m_periodTicks) + 1;   // 0 = unknown
			if (m_faults.captureMisses.count(item.index))
				return;
			VideoFrame frame(item.bytes.data(), m_counter, arrival, nullptr, arrival);
			m_sink.OnFrame(frame, arrival);
		}

		IDvCaptureSink& m_sink;
		LoopFaults m_faults;
		ContainerOptions m_container;
		VideoStateComPtr m_state;
		uint32_t m_width = 0, m_height = 0;
		std::deque<InFlight> m_queue;
		uint64_t m_counter = 0, m_wireSlot = 0;
		uint64_t m_periodTicks = 1;
	};
}
