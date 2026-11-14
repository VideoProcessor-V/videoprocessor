// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// L-B4 step (a): the motion producer's P1 reader over the skeleton's Acquire (refs/129 L-B4, refs/126 s5 B4).
// IP1Source (core/LaneInterfaces.h) is the producer's one P1 seam. This implementation gets frame `seq`'s P1 through
// Acquire(kP1HalfLuma, seq, Cpu) - the lane slot, the memo, else the owner fill (the lane's own CPU law) - and
// turns the published half plane into the engine's 8-bit view (VpLaneProducts::Engine8Half: top byte of the P1 half sample,
// exact for a 10-bit tap view). It NEVER recomputes P1 (no shared-decode fallback: that is P1Reader's live fill, a second
// computation); a miss reads false, and the producer keeps its own decode and shows it.
//
// The formats whose engine codes differ from P1's (VpFrameAnalysis::P1EngineViewDiffersFromProducer: RGB structure luma, raw
// full-range 4:2:x words) are refused here too: they stay on the producer's own decode until the owner has the registered
// FIX (lead8/FIX-B4-RGB.md) and decides.
//
// Any thread. The Acquire function is injected (the memo for the frame, from L-CORE's MemoFor); the result's pin keeps the
// bytes alive for the copy. One reader serves every producer thread (no scratch state).
#pragma once

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

namespace VpFrameAnalysis
{
	class P1AcquireSource : public VpSkeleton::IP1Source
	{
	public:
		// Acquire(kP1HalfLuma, seq, Cpu) on the frame's memo; an empty result (status Unavailable) when there is none.
		using AcquireFn = std::function<VpSkeleton::AcquireResult(uint64_t seq)>;

		explicit P1AcquireSource(AcquireFn acquire = AcquireFn()) : m_acquire(std::move(acquire)) {}
		P1AcquireSource(const P1AcquireSource&) = delete;
		P1AcquireSource& operator=(const P1AcquireSource&) = delete;

		bool ReadEngine8(uint64_t seq, const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh,
			VpSkeleton::P1ReadKind& how) override
		{
			how = VpSkeleton::P1ReadKind::None;
			if (!m_acquire || source.width <= 0 || source.height <= 0)
				return false;
			if (P1EngineViewDiffersFromProducer(source))
			{
				m_ownDecode.fetch_add(1, std::memory_order_relaxed);   // the named exception: the producer's own codes (FIX-B4-RGB)
				return false;
			}
			const VpSkeleton::Dims dims{ static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height) };
			const uint32_t w = VpLaneProducts::HalfDim(dims.width), h = VpLaneProducts::HalfDim(dims.height);
			const size_t halfBytes = VpLaneProducts::HalfLumaHalfBytes(dims);
			const VpSkeleton::AcquireResult r = m_acquire(seq);
			if (!r.Ok() || r.cpu == nullptr || r.cpuBytes < halfBytes)
			{
				m_unread.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			// The memo / slot bytes are byte-addressed: copy the half plane out aligned (the pin holds them only for this call).
			std::vector<uint16_t> half(static_cast<size_t>(w) * h);
			std::memcpy(half.data(), r.cpu, halfBytes);
			out.resize(half.size());
			VpLaneProducts::Engine8Half(half.data(), w, h, out.data());
			hw = static_cast<int>(w);
			hh = static_cast<int>(h);
			how = r.source == VpSkeleton::AcquireSource::LaneSlot ? VpSkeleton::P1ReadKind::LanePublished
				: VpSkeleton::P1ReadKind::LiveFill;
			m_acquired.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		uint64_t Acquired() const { return m_acquired.load(std::memory_order_relaxed); }
		uint64_t Unread() const { return m_unread.load(std::memory_order_relaxed); }
		uint64_t OwnDecode() const { return m_ownDecode.load(std::memory_order_relaxed); }   // refused: producer keeps its own codes

	private:
		AcquireFn m_acquire;
		std::atomic<uint64_t> m_acquired{ 0 }, m_unread{ 0 }, m_ownDecode{ 0 };
	};
}
