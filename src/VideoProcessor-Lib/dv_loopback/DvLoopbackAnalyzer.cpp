/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include <pch.h>
#include "DvLoopbackAnalyzer.h"
#include "DvCaptureContainer.h"
#include <DolbyVisionTunnelPresentation.h>
#include <DebugLog.h>
#include <VP-V/modules/input/signal/DvTunnelMetadataExtension.h>   // WP-14: the formatter hook

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace DvLoopback
{
	namespace
	{
		std::mutex g_logMutex;
		std::vector<std::string>* g_logTarget = nullptr;

		void __cdecl CollectLog(const char* message)
		{
			std::lock_guard<std::mutex> guard(g_logMutex);
			if (g_logTarget && message)
				g_logTarget->emplace_back(message);
		}

		template <typename T>
		T MedianOf(std::vector<T> values)
		{
			std::sort(values.begin(), values.end());
			return values[values.size() / 2];
		}
	}

	const char* ToString(Shown shown)
	{
		switch (shown)
		{
		case Shown::Picture: return "DV picture";
		case Shown::Sdr: return "SDR passthrough";
		case Shown::Black: return "black";
		case Shown::Rejected: return "rejected";
		case Shown::TunnelOff: return "tunnel off";
		case Shown::Handoff: return "normal path (handed off)";
		}
		return "?";
	}

	bool LoopReport::Logged(const std::string& text) const
	{
		for (const std::string& line : log)
			if (line.find(text) != std::string::npos)
				return true;
		return false;
	}

	std::string LoopReport::Summary() const
	{
		char text[1024];
		snprintf(text, sizeof(text),
			"captured=%llu identified=%llu unidentified=%llu | shown: DV=%llu SDR=%llu black=%llu rejected=%llu tunnel_off=%llu handed_off=%llu (handoffs=%llu take_backs=%llu) | "
			"dropped=%llu repeated=%llu out_of_order=%llu | offset=%lld frames latency=%.2f ms (%.2f frames; min %.2f max %.2f) | "
			"pixel_mismatch=%llu frames/%llu samples transport_errors=%llu frames/%llu bytes metadata_mismatch=%llu "
			"reference_mismatch=%llu wrong_kind=%llu black_for_DV=%llu black_for_SDR=%llu | %s",
			(unsigned long long)captured, (unsigned long long)identified, (unsigned long long)unidentified,
			(unsigned long long)pictures, (unsigned long long)sdr, (unsigned long long)black,
			(unsigned long long)rejected, (unsigned long long)tunnelOff,
			(unsigned long long)handedOff, (unsigned long long)handoffs, (unsigned long long)takeBacks,
			(unsigned long long)dropped, (unsigned long long)repeated, (unsigned long long)outOfOrder,
			(long long)frameOffset, latencyMsMedian, latencyFrames, latencyMsMin, latencyMsMax,
			(unsigned long long)pixelMismatchFrames, (unsigned long long)pixelMismatchSamples,
			(unsigned long long)transportErrorFrames, (unsigned long long)transportErrorBytes,
			(unsigned long long)metadataMismatchFrames, (unsigned long long)referenceMismatchFrames,
			(unsigned long long)wrongKindFrames, (unsigned long long)blackWhereDvExpected,
			(unsigned long long)blackWhereSdrExpected, BitExact() ? "BIT-EXACT" : "NOT bit-exact");
		std::string out = text;
		for (const std::string& issue : issues)
			out += "\n  " + issue;
		return out;
	}

	DvLoopbackAnalyzer::DvLoopbackAnalyzer(const AnalyzerOptions& options) : m_options(options)
	{
		if (m_options.collectLog)
		{
			std::lock_guard<std::mutex> guard(g_logMutex);
			g_logTarget = &m_report.log;
			DebugLog::SetExternalSink(CollectLog);
			m_logHooked = true;
		}
	}

	DvLoopbackAnalyzer::~DvLoopbackAnalyzer()
	{
		if (m_logHooked)
		{
			DebugLog::SetExternalSink(nullptr);
			std::lock_guard<std::mutex> guard(g_logMutex);
			g_logTarget = nullptr;
		}
	}

	void DvLoopbackAnalyzer::Issue(const std::string& text)
	{
		if (m_report.issues.size() < m_options.maxIssues)
			m_report.issues.push_back(text);
	}

	void DvLoopbackAnalyzer::OnState(const VideoStateComPtr& state)
	{
		m_state = state;
		if (!state || !state->valid || !state->dolbyVisionTunnel)
			return;
		// The GUI's hand-off probe restarts with every capture state.
		DolbyVisionTunnelHandoffSignal signal;
		signal.colorspace = state->colorspace;
		signal.hdrInfoFrame = state->hdrData != nullptr;
		signal.vsif = state->dolbyVisionVsif;
		if (state->displayMode)
			signal.frameRate = state->displayMode->RefreshRateHz();
		m_handoff.Reset(signal);
		if (!m_formatter)
		{
			m_formatter.reset(new CR210DolbyVisionTunnelFormatter());
			// WP-14: VP's DM additions (L9/L10, the bounded hold) through the formatter's hook.
			m_formatter->SetMetadataExtension(std::make_shared<DvTunnelMetadataExtension>());
		}
		m_formatter->SetBugHuntFixes(m_options.bugHuntFixes);
		if (m_options.scalarUnpack)
			m_formatter->SetConversionMethod(CR210DolbyVisionTunnelFormatter::ConversionMethod::SCALAR);
		try
		{
			VideoStateComPtr copy = state;
			m_formatter->OnVideoState(copy);
			m_output.assign(static_cast<size_t>(m_formatter->GetOutFrameSize()), 0);
		}
		catch (const std::exception& e)
		{
			Issue(std::string("formatter refused the video state: ") + e.what());
			m_formatter.reset();
		}
	}

	void DvLoopbackAnalyzer::OnFrame(const VideoFrame& frame, int64_t arrivalQpc)
	{
		FrameRecord rec;
		rec.ordinal = m_ordinal++;
		rec.counter = frame.GetCounter();
		rec.timestamp = frame.GetTimingTimestamp();
		rec.arrivalQpc = arrivalQpc;
		++m_report.captured;
		if (!m_state || !m_state->valid || !m_state->displayMode || !frame.GetData())
		{
			rec.shown = Shown::Rejected;
			++m_report.rejected;
			m_report.frames.push_back(rec);
			return;
		}
		const uint32_t width = m_state->displayMode->FrameWidth();
		const uint32_t height = m_state->displayMode->FrameHeight();
		const uint32_t stride = m_state->BytesPerRow();
		const VideoFrameEncoding encoding = m_state->videoFrameEncoding;
		const auto* data = static_cast<const uint8_t*>(frame.GetData());

		// 1. Identify the frame from the wire bytes alone (both placements).
		bool high8 = false;
		for (const bool candidate : { false, true })
		{
			if (!UnwrapRgb8(data, width, height, stride, encoding, candidate, m_rgb))
				break;
			if (ReadStampFromRgb8(m_rgb, width, height, m_options.sequenceId, rec.sentIndex))
			{
				rec.identified = true;
				high8 = candidate;
				break;
			}
			if (!IsTenBitRgb(encoding))
				break;
		}

		// 2. VP's decode and the renderer's presentation decision.
		DolbyVisionTunnelFrameMetadata md;
		CR210DolbyVisionTunnelFormatter::RasterView raster;
		raster.data = data;
		raster.width = width;
		raster.height = height;
		raster.stride = stride;
		raster.encoding = encoding;
		const bool probeHandoff = m_options.handoff && m_state->dolbyVisionTunnel && m_formatter != nullptr;
		// While handed off, the GUI probes before the renderer: the picture
		// with the first CRC-valid header is held back (never shown through the
		// normal path) and the tunnel formatter starts fresh.
		bool probedEarly = false, heldBack = false;
		if (probeHandoff && m_handoff.HandedOff())
		{
			probedEarly = true;
			if (m_handoff.OnFrame(raster, rec.counter) == DolbyVisionTunnelHandoffEvent::TakeBack)
			{
				heldBack = true;
				++m_report.takeBacks;
				DebugLog::Log("Dolby Vision RGB tunnel hand-off: signal taken back by the tunnel at picture %llu: %s",
					static_cast<unsigned long long>(rec.counter), m_handoff.Reason());
				VideoStateComPtr copy = m_state;
				try { m_formatter->OnVideoState(copy); }
				catch (const std::exception& e) { Issue(std::string("formatter refused the video state: ") + e.what()); }
			}
		}
		if (!m_state->dolbyVisionTunnel || !m_formatter)
		{
			rec.shown = Shown::TunnelOff;
			++m_report.tunnelOff;
		}
		else if (heldBack)
		{
			rec.shown = Shown::Rejected;
			++m_report.rejected;
		}
		else if (m_options.handoff && m_handoff.HandedOff())
		{
			// The GUI closed the gate for this signal: the normal r210 path
			// shows it (and newlldv may infer PQ). The probe keeps watching
			// for a CRC-valid header that takes the signal back.
			rec.shown = Shown::Handoff;
			++m_report.handedOff;
		}
		else if (!m_formatter->FormatVideoFrame(frame, m_output.data()))
		{
			rec.shown = Shown::Rejected;
			++m_report.rejected;
		}
		else
		{
			md = m_formatter->GetDolbyVisionTunnelMetadata();
			switch (DecideDolbyVisionTunnelPresentation(encoding, md))
			{
			case DolbyVisionTunnelPresentation::Picture: rec.shown = Shown::Picture; ++m_report.pictures; break;
			case DolbyVisionTunnelPresentation::SdrPassthrough: rec.shown = Shown::Sdr; ++m_report.sdr; break;
			default: rec.shown = Shown::Black; ++m_report.black; break;
			}
			if (rec.shown == Shown::Picture)
			{
				m_planes.assign(reinterpret_cast<const uint16_t*>(m_output.data()),
					reinterpret_cast<const uint16_t*>(m_output.data()) + m_output.size() / 2);
				if (m_options.decodedFault)
					m_options.decodedFault(m_planes, width, height);
				uint32_t decodedIndex = 0;
				rec.decodedStampAgrees = ReadStampFromPlanes(m_planes, width, height, m_options.sequenceId, decodedIndex) &&
					rec.identified && decodedIndex == rec.sentIndex;
			}
		}
		m_lastMetadata = md;
		m_lastShown = rec.shown;
		// The GUI runs the probe after the renderer has the frame; its decision
		// applies from the next picture (DolbyVisionTunnelHandoff.h).
		if (probeHandoff && !probedEarly &&
			m_handoff.OnFrame(raster, rec.counter) == DolbyVisionTunnelHandoffEvent::HandOff)
		{
			++m_report.handoffs;
			DebugLog::Log("Dolby Vision RGB tunnel hand-off: signal handed to the normal path at picture %llu: %s",
				static_cast<unsigned long long>(rec.counter), m_handoff.Reason());
		}

		// 3. Compare with what was sent.
		LoopFrame sent;
		char line[512];
		if (rec.identified)
		{
			++m_report.identified;
			if (m_options.expected && m_options.expected(rec.sentIndex, sent))
			{
				// Wire bytes (placement as identified).
				if (sent.rgb8.size() == m_rgb.size())
				{
					size_t first = SIZE_MAX;
					for (size_t k = 0; k < m_rgb.size(); ++k)
						if (m_rgb[k] != sent.rgb8[k])
						{
							if (first == SIZE_MAX) first = k;
							++rec.transportErrors;
						}
					if (rec.transportErrors)
					{
						++m_report.transportErrorFrames;
						m_report.transportErrorBytes += rec.transportErrors;
						snprintf(line, sizeof(line), "frame %u (capture %llu): %llu wire bytes differ, first at pixel %zu (x=%zu y=%zu) channel %zu: sent %u got %u",
							rec.sentIndex, (unsigned long long)rec.ordinal, (unsigned long long)rec.transportErrors,
							first / 3, (first / 3) % width, (first / 3) / width, first % 3,
							sent.rgb8[first], m_rgb[first]);
						Issue(line);
					}
				}
				if (sent.sdr)
				{
					if (rec.shown == Shown::Picture)
					{
						rec.wrongKind = true;
						snprintf(line, sizeof(line), "frame %u: SDR frame decoded as a DV picture", rec.sentIndex);
						Issue(line);
					}
					else if (rec.shown == Shown::Black)
						++m_report.blackWhereSdrExpected;
				}
				else
				{
					if (rec.shown == Shown::Sdr || rec.shown == Shown::Handoff)
					{
						rec.wrongKind = true;
						snprintf(line, sizeof(line), "frame %u: DV tunnel frame shown %s (packed pixels)", rec.sentIndex,
							rec.shown == Shown::Sdr ? "as SDR passthrough" : "through the normal path (handed off)");
						Issue(line);
					}
					else if (rec.shown == Shown::Black)
						++m_report.blackWhereDvExpected;
					else if (rec.shown == Shown::Picture)
					{
						std::vector<uint16_t> want;
						FormatterPlanes(sent.embedded, want);
						const size_t n = (std::min)(want.size(), m_planes.size());
						size_t first = SIZE_MAX;
						for (size_t k = 0; k < n; ++k)
							if (want[k] != m_planes[k])
							{
								if (first == SIZE_MAX) first = k;
								++rec.pixelMismatches;
							}
						if (want.size() != m_planes.size())
							rec.pixelMismatches += 1;
						if (rec.pixelMismatches)
						{
							++m_report.pixelMismatchFrames;
							m_report.pixelMismatchSamples += rec.pixelMismatches;
							const size_t plane = first == SIZE_MAX ? 0 : first / (static_cast<size_t>(width) * height);
							const size_t p = first == SIZE_MAX ? 0 : first % (static_cast<size_t>(width) * height);
							snprintf(line, sizeof(line), "frame %u: %llu decoded samples differ, first %s at x=%zu y=%zu: want %u got %u (12-bit codes)",
								rec.sentIndex, (unsigned long long)rec.pixelMismatches, plane == 0 ? "I" : (p % width) % 2 ? "Cp" : "Ct",
								p % width, p / width, first == SIZE_MAX ? 0U : static_cast<unsigned>(want[first] >> 4),
								first == SIZE_MAX ? 0U : static_cast<unsigned>(m_planes[first] >> 4));
							Issue(line);
						}
						if (sent.hasDm)
						{
							bool accepted = false;
							const FlatMetadata want = ExpectedDecode(sent.effectiveDm, accepted);
							const std::vector<std::string> diff = DiffMetadata(want, Flatten(md));
							rec.metadataMismatches = diff.size();
							if (!diff.empty())
							{
								++m_report.metadataMismatchFrames;
								std::string text = "frame " + std::to_string(rec.sentIndex) + ": metadata differs:";
								for (size_t k = 0; k < diff.size() && k < 6; ++k)
									text += " " + diff[k] + ";";
								Issue(text);
							}
						}
					}
				}
				if (m_options.reference)
				{
					const std::string error = m_options.reference(rec.sentIndex, m_rgb);
					if (!error.empty())
					{
						rec.referenceMismatch = true;
						++m_report.referenceMismatchFrames;
						Issue("frame " + std::to_string(rec.sentIndex) + ": independent reference: " + error);
					}
				}
				if (rec.wrongKind)
					++m_report.wrongKindFrames;
			}
			if (m_options.sentQpc && m_options.qpcFrequency > 0 && arrivalQpc)
			{
				const int64_t sentAt = m_options.sentQpc(rec.sentIndex);
				if (sentAt)
					rec.latencyMs = static_cast<double>(arrivalQpc - sentAt) * 1000.0 /
						static_cast<double>(m_options.qpcFrequency);
			}
		}
		else
			++m_report.unidentified;
		(void)high8;
		if (m_options.keepFrames)
			m_report.frames.push_back(rec);
		else
		{
			// Keep only what Finish() needs.
			FrameRecord slim;
			slim.ordinal = rec.ordinal; slim.identified = rec.identified; slim.sentIndex = rec.sentIndex;
			slim.latencyMs = rec.latencyMs; slim.shown = rec.shown;
			m_report.frames.push_back(slim);
		}
	}

	LoopReport DvLoopbackAnalyzer::Finish()
	{
		LoopReport& r = m_report;
		r.dropped = r.repeated = r.outOfOrder = 0;
		r.droppedIndices.clear();
		std::vector<int64_t> offsets;
		std::vector<double> latencies;
		bool havePrevious = false;
		uint32_t previous = 0, lowest = UINT32_MAX, highest = 0;
		std::vector<uint32_t> seen;
		for (const FrameRecord& f : r.frames)
		{
			if (!f.identified)
				continue;
			if (havePrevious)
			{
				if (f.sentIndex == previous) ++r.repeated;
				else if (f.sentIndex < previous) ++r.outOfOrder;
			}
			havePrevious = true;
			previous = f.sentIndex;
			lowest = (std::min)(lowest, f.sentIndex);
			highest = (std::max)(highest, f.sentIndex);
			seen.push_back(f.sentIndex);
			offsets.push_back(static_cast<int64_t>(f.ordinal) - static_cast<int64_t>(f.sentIndex));
			if (f.latencyMs >= 0)
				latencies.push_back(f.latencyMs);
		}
		if (!seen.empty())
		{
			std::sort(seen.begin(), seen.end());
			seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
			for (uint32_t i = lowest, k = 0; i <= highest; ++i)
			{
				while (k < seen.size() && seen[k] < i) ++k;
				if (k >= seen.size() || seen[k] != i)
				{
					++r.dropped;
					if (r.droppedIndices.size() < 64)
						r.droppedIndices.push_back(i);
				}
				if (i == UINT32_MAX) break;
			}
			r.frameOffset = MedianOf(offsets);
		}
		if (!latencies.empty())
		{
			r.latencyMsMedian = MedianOf(latencies);
			r.latencyMsMin = *std::min_element(latencies.begin(), latencies.end());
			r.latencyMsMax = *std::max_element(latencies.begin(), latencies.end());
			const double rate = m_state && m_state->displayMode ? m_state->displayMode->RefreshRateHz() : 0.0;
			if (rate > 0)
				r.latencyFrames = r.latencyMsMedian * rate / 1000.0;
		}
		if (m_logHooked)
		{
			DebugLog::SetExternalSink(nullptr);
			std::lock_guard<std::mutex> guard(g_logMutex);
			g_logTarget = nullptr;
			m_logHooked = false;
		}
		return r;
	}
}
