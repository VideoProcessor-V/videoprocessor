// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GuidanceCountsRing.h"

#include <algorithm>
#include <cstring>

namespace CameraLink
{
	GuidanceCountsRing::GuidanceCountsRing(pl_gpu gpu, int entries)
		: m_gpu(gpu), m_entries(static_cast<size_t>(std::max(entries, 2)))
	{
	}

	GuidanceCountsRing::~GuidanceCountsRing()
	{
		if (!m_gpu)
			return;
		bool any = false;
		for (const Entry& e : m_entries)
			any = any || e.busy;
		if (any)
			pl_gpu_finish(m_gpu);   // destructor only: a copy may be in flight
		for (Entry& e : m_entries)
			if (e.buf)
				pl_buf_destroy(m_gpu, &e.buf);
	}

	bool GuidanceCountsRing::Request(uint64_t pairSeq, pl_tex counts)
	{
		if (!m_gpu || !counts)
			return false;
		Entry* slot = nullptr;
		for (Entry& e : m_entries)
			if (!e.busy) { slot = &e; break; }
		if (!slot)
		{
			++m_dropped;
			return false;
		}
		if (!slot->buf)
		{
			pl_buf_params bp{};
			bp.size = 16;
			bp.host_mapped = true;
			bp.host_readable = true;
			bp.memory_type = PL_BUF_MEM_HOST;
			slot->buf = pl_buf_create(m_gpu, &bp);
			if (slot->buf && !slot->buf->data)
				pl_buf_destroy(m_gpu, &slot->buf);
			if (!slot->buf)
				return false;
		}
		pl_tex_transfer_params d{};
		d.tex = counts;
		d.buf = slot->buf;
		d.buf_offset = 0;
		if (!pl_tex_download(m_gpu, &d))
			return false;
		slot->busy = true;
		slot->seq = pairSeq;
		return true;
	}

	size_t GuidanceCountsRing::Poll(std::vector<Done>& out)
	{
		size_t n = 0;
		// Oldest request first (deterministic order).
		std::vector<Entry*> order;
		for (Entry& e : m_entries)
			if (e.busy)
				order.push_back(&e);
		std::sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) { return a->seq < b->seq; });
		for (Entry* e : order)
		{
			if (pl_buf_poll(m_gpu, e->buf, 0))
				continue;   // still in flight
			Done d;
			d.pairSeq = e->seq;
			std::memcpy(d.v, e->buf->data, sizeof(d.v));
			out.push_back(d);
			e->busy = false;
			++n;
		}
		return n;
	}
}
