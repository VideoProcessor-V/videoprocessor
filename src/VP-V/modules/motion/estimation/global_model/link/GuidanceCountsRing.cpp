// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "GuidanceCountsRing.h"

#include <algorithm>
#include <cstring>

namespace CameraLink
{
	GuidanceCountsRing::GuidanceCountsRing(pl_gpu gpu, int entries, pl_log log)
		: m_gpu(gpu), m_log(log), m_entries(static_cast<size_t>((std::max)(entries, 2)))
	{
	}

	GuidanceCountsRing::~GuidanceCountsRing()
	{
		if (!m_gpu)
			return;
		if (m_native)
			m_native->Release();
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
		if (!m_nativeTried)
		{
			m_nativeTried = true;
			m_native.reset(new NativeCopyReadback());
			if (!m_native->Create(m_log, m_gpu, static_cast<int>(m_entries.size()), 1, 1, 1))
				m_native.reset();
		}
		if (m_native)
		{
			const int idx = static_cast<int>(slot - m_entries.data());
			if (!m_native->Start(idx, counts, nullptr))
				return false;
			slot->native = true;
			slot->busy = true;
			slot->seq = pairSeq;
			return true;
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
			{
				bp.host_mapped = false;   // no mapped memory on this device: read with pl_buf_read after the idle poll
				bp.memory_type = PL_BUF_MEM_AUTO;
				slot->buf = pl_buf_create(m_gpu, &bp);
			}
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
			Done d;
			d.pairSeq = e->seq;
			if (e->native)
			{
				const int idx = static_cast<int>(e - m_entries.data());
				if (!m_native || !m_native->Ready(idx) || !m_native->Read(idx, d.v))
					continue;   // not done: never wait
			}
			else if (pl_buf_poll(m_gpu, e->buf, 0))
				continue;   // still in flight
			else if (e->buf->data)
				std::memcpy(d.v, e->buf->data, sizeof(d.v));
			else if (!pl_buf_read(m_gpu, e->buf, 0, d.v, sizeof(d.v)))
			{
				e->busy = false;   // unreadable: drop (the pair is judged without counts)
				continue;
			}
			out.push_back(d);
			e->busy = false;
			++n;
		}
		return n;
	}
}
