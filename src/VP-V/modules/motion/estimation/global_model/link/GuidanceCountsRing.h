// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K4-4): the 16 B guidance-counts readback (CameraGuidanceCounts(), 1x1 rgba32f) through a small ring of
// host-mapped buffers, in the shape of the field readback ring: pl_tex_download into the buffer, completion tested ONLY with
// pl_buf_poll(gpu, buf, 0); never blocks. A full ring drops the request (the pair is judged without counts: visible as an
// unjudged pair). Render thread only.
#pragma once

#include "GuidanceScheduler.h"
#include "NativeCopyReadback.h"

#include <libplacebo/gpu.h>

#include <vector>

namespace CameraLink
{
	class GuidanceCountsRing : public IGuidanceCountsSource
	{
	public:
		GuidanceCountsRing(pl_gpu gpu, int entries = 4, pl_log log = nullptr);
		~GuidanceCountsRing() override;
		GuidanceCountsRing(const GuidanceCountsRing&) = delete;
		GuidanceCountsRing& operator=(const GuidanceCountsRing&) = delete;

		bool Request(uint64_t pairSeq, pl_tex counts) override;
		size_t Poll(std::vector<Done>& out) override;
		uint64_t Dropped() const { return m_dropped; }

	private:
		struct Entry { pl_buf buf = nullptr; bool busy = false; bool native = false; uint64_t seq = 0; };
		pl_gpu m_gpu;
		pl_log m_log;
		std::unique_ptr<NativeCopyReadback> m_native;   // D3D11: no buffer transfers there (NativeCopyReadback.h)
		bool m_nativeTried = false;
		std::vector<Entry> m_entries;
		uint64_t m_dropped = 0;
	};
}
