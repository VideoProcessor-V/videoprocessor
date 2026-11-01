// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "FieldReadbackRing.h"

#include <algorithm>

namespace CameraLink
{
	namespace
	{
		size_t PlaneBytes(int gridW, int gridH)
		{
			return static_cast<size_t>(gridW) * static_cast<size_t>(gridH) * 4 * sizeof(float);
		}
	}

	FieldReadbackRing::FieldReadbackRing(pl_gpu gpu, int entries, pl_log log)
		: m_gpu(gpu), m_log(log), m_entries(static_cast<size_t>((std::max)(entries, 3)))
	{
	}

	FieldReadbackRing::~FieldReadbackRing()
	{
		if (!m_gpu)
			return;
		if (m_native)
			m_native->Release();
		bool any = !m_parked.empty();
		for (const Entry& e : m_entries)
			any = any || e.buf;
		if (any)
			pl_gpu_finish(m_gpu);   // destructor only: copies may be in flight and the buffers must outlive them
		for (Entry& e : m_entries)
			if (e.buf)
				pl_buf_destroy(m_gpu, &e.buf);
		for (pl_buf& b : m_parked)
			pl_buf_destroy(m_gpu, &b);
	}

	void FieldReadbackRing::Release(Entry& e)
	{
		e.state = State::Free;
		e.fwd = nullptr;
		e.bwd = nullptr;
		e.mapped = false;
		e.haveBwd = false;
	}

	void FieldReadbackRing::DrainParked()
	{
		for (size_t i = 0; i < m_parked.size();)
		{
			if (!pl_buf_poll(m_gpu, m_parked[i], 0))
			{
				pl_buf_destroy(m_gpu, &m_parked[i]);
				m_parked.erase(m_parked.begin() + static_cast<std::ptrdiff_t>(i));
			}
			else
				++i;
		}
	}

	bool FieldReadbackRing::TryNative(const Entry& e)
	{
		const int planes = 2;   // always a two-plane target: a field without a backward plane fills only the first half
		if (m_native && m_native->Active() && m_native->Width() == e.gridW && m_native->Height() == e.gridH && m_native->Planes() == planes)
			return true;
		if (m_nativeUnavailable)
			return false;
		for (const Entry& x : m_entries)
			if (&x != &e && x.state == State::InFlight && x.native)
				return false;   // a different geometry while copies are in flight: this request uses the buffer path / is dropped
		if (!m_native)
			m_native.reset(new NativeCopyReadback());
		if (!m_native->Create(m_log, m_gpu, static_cast<int>(m_entries.size()), e.gridW, e.gridH, planes))
		{
			m_native.reset();
			m_nativeUnavailable = true;   // not D3D11 (or no storable format): the pl_buf path
			return false;
		}
		return true;
	}

	bool FieldReadbackRing::Start(Entry& e)
	{
		if (TryNative(e))
		{
			const int idx = static_cast<int>(&e - m_entries.data());
			if (!m_native->Start(idx, e.fwd, e.bwd))
			{
				m_lastError = "native readback copy pass failed";
				return false;
			}
			e.native = true;
			e.state = State::InFlight;
			return true;
		}
		if (m_native && !m_nativeUnavailable)
		{
			m_lastError = "geometry changed while native copies are in flight";
			return false;
		}
		e.native = false;
		const size_t planeBytes = PlaneBytes(e.gridW, e.gridH);
		const size_t need = planeBytes * (e.haveBwd ? 2 : 1);
		if (!e.buf || e.bufBytes < need)
		{
			if (e.buf)
				pl_buf_destroy(m_gpu, &e.buf);   // a free/pending entry's buffer is idle (polled before it was freed)
			e.bufBytes = 0;
			pl_buf_params bp{};
			bp.size = need;
			bp.host_mapped = true;
			bp.host_readable = true;
			bp.memory_type = PL_BUF_MEM_HOST;
			e.buf = pl_buf_create(m_gpu, &bp);
			if (e.buf && !e.buf->data)
				pl_buf_destroy(m_gpu, &e.buf);
			if (!e.buf)
			{
				// No host-mapped memory on this device (e.g. the software test device): a host-readable buffer, read with
				// pl_buf_read AFTER the zero-timeout poll says it is idle (the copy is complete, so the read does not wait).
				bp.host_mapped = false;
				bp.memory_type = PL_BUF_MEM_AUTO;
				e.buf = pl_buf_create(m_gpu, &bp);
			}
			if (!e.buf)
			{
				m_lastError = "cannot create a host-readable buffer";
				return false;
			}
			e.bufBytes = need;
		}
		pl_tex_transfer_params d{};
		d.tex = e.fwd;
		d.buf = e.buf;
		d.buf_offset = 0;
		if (!pl_tex_download(m_gpu, &d))
		{
			m_lastError = "pl_tex_download into the buffer failed (texture needs host_readable and a readable format)";
			return false;
		}
		if (e.haveBwd)
		{
			pl_tex_transfer_params b{};
			b.tex = e.bwd;
			b.buf = e.buf;
			b.buf_offset = planeBytes;
			if (!pl_tex_download(m_gpu, &b))
			{
				// The forward copy is already queued: park the buffer so it is destroyed only once idle.
				m_parked.push_back(e.buf);
				e.buf = nullptr;
				e.bufBytes = 0;
				return false;
			}
		}
		e.state = State::InFlight;
		return true;
	}

	bool FieldReadbackRing::Request(const VpMotion::VerifiedField& f, bool startNow)
	{
		++m_stats.requested;
		if (!m_gpu || !f.fwd || f.gridW <= 0 || f.gridH <= 0)
		{
			++m_stats.failed;
			return false;
		}
		DrainParked();
		Entry* slot = nullptr;
		for (Entry& e : m_entries)
			if (e.state == State::Free) { slot = &e; break; }
		if (!slot)
		{
			++m_stats.dropped;
			return false;
		}
		Entry& e = *slot;
		e.fwd = f.fwd;
		e.bwd = f.bwd;
		e.haveBwd = f.bwd != nullptr;
		e.seq = f.seq;
		e.pairFrom = f.pairFrom;
		e.pairTo = f.pairTo;
		e.gridW = f.gridW;
		e.gridH = f.gridH;
		e.blockSize = f.blockSize;
		e.scale = f.scaleToSourcePx;
		e.stamp = f.stamp;
		e.mapped = false;
		e.ticket = ++m_ticket;
		if (!startNow)
		{
			e.state = State::Pending;
			return true;
		}
		if (!Start(e))
		{
			Release(e);
			++m_stats.failed;
			return false;
		}
		return true;
	}

	size_t FieldReadbackRing::Poll(std::vector<std::shared_ptr<FieldCopy>>& ready)
	{
		DrainParked();
		std::vector<Entry*> flying;
		for (Entry& e : m_entries)
			if (e.state == State::InFlight && !e.mapped)
				flying.push_back(&e);
		std::sort(flying.begin(), flying.end(), [](const Entry* a, const Entry* b) { return a->ticket < b->ticket; });
		size_t n = 0;
		for (Entry* ep : flying)
		{
			Entry& e = *ep;
			const size_t need = PlaneBytes(e.gridW, e.gridH) * (e.haveBwd ? 2 : 1);
			std::vector<float> tmp;
			const float* base = nullptr;
			if (e.native)
			{
				const int idx = static_cast<int>(&e - m_entries.data());
				if (!m_native || !m_native->Ready(idx))
					break;   // the event query says the copy is not done: never wait (and keep request order)
				tmp.resize(PlaneBytes(e.gridW, e.gridH) * 2 / sizeof(float));
				if (!m_native->Read(idx, tmp.data()))
					break;   // DXGI_ERROR_WAS_STILL_DRAWING: try again next poll
				base = tmp.data();
			}
			else if (pl_buf_poll(m_gpu, e.buf, 0))
				break;   // still busy; never wait (and keep request order)
			auto c = std::make_shared<FieldCopy>();
			c->seq = e.seq;
			c->pairFrom = e.pairFrom;
			c->pairTo = e.pairTo;
			c->gridW = e.gridW;
			c->gridH = e.gridH;
			c->blockSize = static_cast<double>(e.blockSize);
			c->stamp = e.stamp;
			const size_t blocks = static_cast<size_t>(e.gridW) * static_cast<size_t>(e.gridH);
			if (!base)
				base = reinterpret_cast<const float*>(e.buf->data);
			if (!base)
			{
				tmp.resize(need / sizeof(float));
				if (!pl_buf_read(m_gpu, e.buf, 0, tmp.data(), need))
				{
					Release(e);
					++m_stats.failed;
					continue;
				}
				base = tmp.data();
			}
			UnpackRgba(base, blocks, e.scale, c->fwdX, c->fwdY, c->fwdTrust);
			if (e.haveBwd)
				UnpackRgba(base + blocks * 4, blocks, e.scale, c->bwdX, c->bwdY, c->bwdTrust);
			Release(e);
			++m_stats.completed;
			ready.push_back(std::move(c));
			++n;
		}
		return n;
	}

	void FieldReadbackRing::Clear()
	{
		for (Entry& e : m_entries)
		{
			if (e.state == State::InFlight && e.buf && !e.native)
			{
				m_parked.push_back(e.buf);   // may still be written: destroyed by a later Request/Poll once idle
				e.buf = nullptr;
				e.bufBytes = 0;
			}
			Release(e);
		}
	}

	bool FieldReadbackRing::CopyToStaging(VpSkeleton::ProductId, uint64_t seq, int stagingIndex)
	{
		if (stagingIndex < 0 || static_cast<size_t>(stagingIndex) >= m_entries.size())
			return false;
		Entry& e = m_entries[static_cast<size_t>(stagingIndex)];
		if (e.state != State::Pending || e.seq != seq)
			return false;
		if (!Start(e))
		{
			Release(e);
			++m_stats.failed;
			return false;
		}
		return true;
	}

	bool FieldReadbackRing::QueryDone(int stagingIndex)
	{
		if (stagingIndex < 0 || static_cast<size_t>(stagingIndex) >= m_entries.size())
			return false;
		const Entry& e = m_entries[static_cast<size_t>(stagingIndex)];
		return e.state == State::InFlight && e.buf && !pl_buf_poll(m_gpu, e.buf, 0);
	}

	const void* FieldReadbackRing::MapNoWait(int stagingIndex, size_t* bytes)
	{
		if (!QueryDone(stagingIndex))
			return nullptr;
		Entry& e = m_entries[static_cast<size_t>(stagingIndex)];
		e.mapped = true;
		if (bytes)
			*bytes = PlaneBytes(e.gridW, e.gridH) * (e.haveBwd ? 2 : 1);
		return e.buf->data;
	}

	void FieldReadbackRing::Unmap(int stagingIndex)
	{
		if (stagingIndex < 0 || static_cast<size_t>(stagingIndex) >= m_entries.size())
			return;
		m_entries[static_cast<size_t>(stagingIndex)].mapped = false;
	}
}
