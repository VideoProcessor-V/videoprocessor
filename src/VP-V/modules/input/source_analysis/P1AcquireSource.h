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
// MAP-RGB-LUMA-PROXY (OD 11-15 11:11 (3)): every format is served. The former named exception (RGB structure luma, raw
// full-range 4:2:x words, VpFrameAnalysis::P1EngineViewDiffersFromProducer) is retired: the producer no longer has a private
// luma law, so its codes are the shared decoder's codes, the same ones P1 is built from.
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
		// Optional source offer hook invoked right before Acquire(seq) so the no-lane P1 CPU owner fill can compute
		// from the producer's frame without waiting on the render thread (default empty: backwards-compatible).
		using OfferFn = std::function<void(uint64_t seq, const AnalysisLumaSource&)>;

		// The one P1 tap shift Engine8Half is exact for: the lane's 10-bit tap views (SourceViewOf, codes << 6).
		static constexpr uint8_t kExactP1TapMsbShift = 6;

		// `p1TapMsbShift`: the msbShift of the tap view the P1 behind `acquire` is built from. The P1 bytes carry no shift, so
		// the owner states it; any shift other than 6 (e.g. SourceViewOf12's 4 for a 12-bit lane) gives other P1 codes and
		// Engine8Half is then not the producer's law: such a reader refuses every read (counted OwnDecode, no Acquire made).
		explicit P1AcquireSource(AcquireFn acquire = AcquireFn(), uint8_t p1TapMsbShift = kExactP1TapMsbShift,
			OfferFn offer = OfferFn())
			: m_acquire(std::move(acquire)), m_p1TapMsbShift(p1TapMsbShift), m_offer(std::move(offer)) {}
		P1AcquireSource(const P1AcquireSource&) = delete;
		P1AcquireSource& operator=(const P1AcquireSource&) = delete;

		// PRECISION-OK 119#5: the motion engine's design 8-bit input view (IP1Source::ReadEngine8, Engine8Half = HalfLuma.h law; P1 itself stays 16-bit)
		bool ReadEngine8(uint64_t seq, const AnalysisLumaSource& source, std::vector<uint8_t>& out, int& hw, int& hh,
			VpSkeleton::P1ReadKind& how) override
		{
			how = VpSkeleton::P1ReadKind::None;
			if (!m_acquire || source.width <= 0 || source.height <= 0)
				return false;
			if (m_p1TapMsbShift != kExactP1TapMsbShift)
			{
				// Another tap depth (Engine8Half not exact). RGB / full-range formats are served like every other (MAP-RGB-LUMA-PROXY).
				m_ownDecode.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const VpSkeleton::Dims dims{ static_cast<uint32_t>(source.width), static_cast<uint32_t>(source.height) };
			const uint32_t w = VpLaneProducts::HalfDim(dims.width), h = VpLaneProducts::HalfDim(dims.height);
			const size_t halfBytes = VpLaneProducts::HalfLumaHalfBytes(dims);
			if (m_offer)
				m_offer(seq, source);
			const VpSkeleton::AcquireResult r = m_acquire(seq);
			// Exact size: the P1 product of THIS source's dims is half + quarter (HalfLumaBytes), as the lane's own published
			// read requires (AnalysisLane.h ReadPublishedBytes); any other size is another frame geometry, never reinterpreted.
			if (!r.Ok() || r.cpu == nullptr || r.cpuBytes != VpLaneProducts::HalfLumaBytes(dims))
			{
				m_unread.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			// Bit depth: guarded above (m_p1TapMsbShift); an RGB / full-range view never reaches here either (OwnDecode).
			// The memo / slot bytes are byte-addressed: copy the half plane out aligned (the pin holds them only for this call).
			std::vector<uint16_t> half(static_cast<size_t>(w) * h);
			std::memcpy(half.data(), r.cpu, halfBytes);
			out.resize(half.size());
			VpLaneProducts::Engine8Half(half.data(), w, h, out.data());
			hw = static_cast<int>(w);
			hh = static_cast<int>(h);
			// P1ReadKind has no memo value: a lane-slot hit is LanePublished; a memo result (built by the owner fill at read, or
			// a later reader reusing that one build) is labelled LiveFill, because it was filled for the frame at read time,
			// not published ahead by the lane. The two memo cases are told apart by the FilledAtRead / MemoReused counters.
			how = r.source == VpSkeleton::AcquireSource::LaneSlot ? VpSkeleton::P1ReadKind::LanePublished
				: VpSkeleton::P1ReadKind::LiveFill;
			if (r.source == VpSkeleton::AcquireSource::Memo)
				(r.status == VpSkeleton::AcquireStatus::FilledAtRead ? m_filledAtRead : m_memoReused)
					.fetch_add(1, std::memory_order_relaxed);
			m_acquired.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// Threading + lifetime: ReadEngine8 runs on the producer thread; the AcquireFn the owner supplies (VectorEngine) captures
		// only the renderer host key (host_), which outlives the producer (VectorEngine owns it and stops the producer first).
		// The counters' consumer: VectorEngine::ProducerStatusSegment (Config status / Ctrl+Alt+D) and a DiagnosticHistory
		// decision record when a count changes (L-B4-P1-COUNTERS-NO-CONSUMER).
		uint64_t Acquired() const { return m_acquired.load(std::memory_order_relaxed); }
		uint64_t Unread() const { return m_unread.load(std::memory_order_relaxed); }
		uint64_t OwnDecode() const { return m_ownDecode.load(std::memory_order_relaxed); }   // refused: producer keeps its own codes
		uint64_t FilledAtRead() const { return m_filledAtRead.load(std::memory_order_relaxed); }   // memo: this read ran the fill
		uint64_t MemoReused() const { return m_memoReused.load(std::memory_order_relaxed); }       // memo: reused an earlier fill

	private:
		AcquireFn m_acquire;
		uint8_t m_p1TapMsbShift = kExactP1TapMsbShift;
		OfferFn m_offer;
		std::atomic<uint64_t> m_acquired{ 0 }, m_unread{ 0 }, m_ownDecode{ 0 }, m_filledAtRead{ 0 }, m_memoReused{ 0 };
	};
}
