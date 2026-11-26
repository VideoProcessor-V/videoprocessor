// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "FieldReadbackRing.h"

#include <VP-V/core/VramUsers.h>

#include <algorithm>

namespace CameraLink
{
	namespace
	{
		size_t PlaneBytes(int gridW, int gridH)
		{
			return static_cast<size_t>(gridW) * static_cast<size_t>(gridH) * 4 * sizeof(float);
		}

		// MUST 3 (verify-gemini-mc-81ee082cf.md:81-86): choose fp16 staging ONLY when the source texture's format
		// is 16-bit float across every channel; never silently truncate rgba32f sources.
		bool IsFp16Source(pl_tex tex)
		{
			if (!tex || !tex->params.format)
				return false;
			const pl_fmt fmt = tex->params.format;
			if (fmt->type != PL_FMT_FLOAT || fmt->num_components <= 0)
				return false;
			for (int c = 0; c < fmt->num_components && c < 4; ++c)
				if (fmt->component_depth[c] != 16)
					return false;
			return true;
		}
	}

	FieldReadbackRing::FieldReadbackRing(pl_gpu gpu, int entries, pl_log log)
		: m_gpu(gpu), m_log(log), m_entries(static_cast<size_t>((std::max)(entries, 3)))
	{
		m_flying.reserve(m_entries.size());
	}

	FieldReadbackRing::~FieldReadbackRing()
	{
		Release();
	}

	void FieldReadbackRing::Release()
	{
		if (!m_gpu)
		{
			WithdrawVram();
			return;
		}
		bool any = !m_parked.empty() || (m_native && m_native->HasPendingFlush());
		for (const Entry& e : m_entries)
			any = any || e.buf || e.state == State::InFlight || e.state == State::Draining;
		if (any)
			pl_gpu_finish(m_gpu);   // teardown only: copies (including Draining slots) may be in flight and resources must outlive them
		if (m_native)
		{
			m_retiredFlushCount += m_native->FlushCount();
			m_native->Release();
			m_native.reset();
		}
		for (Entry& e : m_entries)
		{
			if (e.buf)
			{
				pl_buf_destroy(m_gpu, &e.buf);
				e.bufBytes = 0;
			}
			ResetEntry(e);
		}
		for (ParkedBuf& p : m_parked)
			if (p.buf)
				pl_buf_destroy(m_gpu, &p.buf);
		m_parked.clear();
		m_gpu = nullptr;
		WithdrawVram();
	}

	size_t FieldReadbackRing::DrainingSlots() const
	{
		size_t n = 0;
		for (const Entry& e : m_entries)
			if (e.state == State::Draining)
				++n;
		return n;
	}

	uint64_t FieldReadbackRing::AllocatedVramBytes() const
	{
		uint64_t bytes = m_native ? m_native->AllocatedBytes() : 0;
		for (const Entry& e : m_entries)
			if (e.buf)
				bytes += static_cast<uint64_t>(e.bufBytes);
		for (const ParkedBuf& p : m_parked)
			if (p.buf)
				bytes += static_cast<uint64_t>(p.bytes);
		return bytes;
	}

	void FieldReadbackRing::UpdateVramDeclaration()
	{
		const uint64_t held = AllocatedVramBytes();
		if (held == m_declaredVram)
			return;
		if (held > 0)
		{
			VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserCameraModel, true);
			VpSkeleton::VramUsers::Instance().DeclareFixed(this, VpSkeleton::kUserCameraModel, held);
		}
		else
		{
			VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserCameraModel);
		}
		m_declaredVram = held;
	}

	void FieldReadbackRing::WithdrawVram()
	{
		VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserCameraModel);
		m_declaredVram = 0;
	}

	void FieldReadbackRing::ResetEntry(Entry& e)
	{
		e.state = State::Free;
		e.fwd = nullptr;
		e.bwd = nullptr;
		e.mapped = false;
		e.haveBwd = false;
		e.native = false;
		e.seed = false;
	}

	void FieldReadbackRing::DrainParked()
	{
		if (!m_gpu)
			return;
		bool freed = false;
		for (size_t i = 0; i < m_parked.size();)
		{
			if (!pl_buf_poll(m_gpu, m_parked[i].buf, 0))
			{
				pl_buf_destroy(m_gpu, &m_parked[i].buf);
				m_parked.erase(m_parked.begin() + static_cast<std::ptrdiff_t>(i));
				freed = true;
			}
			else
				++i;
		}
		if (freed)
			UpdateVramDeclaration();
	}

	void FieldReadbackRing::DrainDraining()
	{
		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			Entry& e = m_entries[i];
			if (e.state != State::Draining)
				continue;
			if (!m_native)
			{
				ResetEntry(e);
				++m_drainedCount;
				++m_stats.drained;
				continue;
			}
			const int idx = static_cast<int>(i);
			const LedDrive::ReadState st = m_native->Poll(idx);
			if (st == LedDrive::ReadState::Pending)
				continue;   // GPU still holds the cleared copy: keep Draining, never reuse yet
			if (st == LedDrive::ReadState::Ready)
				m_native->Discard(idx);   // unmap without delivering: cleared copies are never returned as valid readbacks
			ResetEntry(e);
			++m_drainedCount;
			++m_stats.drained;
		}
	}

	bool FieldReadbackRing::TryNative(const Entry& e)
	{
		const int planes = 2;   // always a two-plane target: a field without a backward plane fills only the first half
		// MUST 3 (verify-gemini-mc-81ee082cf.md:81-86): stage fp16 ONLY when BOTH source planes (or the sole forward plane)
		// are 16-bit float; otherwise keep R32G32B32A32_FLOAT. Include fp16 in the reuse key so format switches recreate cleanly.
		const bool fp16 = IsFp16Source(e.fwd) && (!e.bwd || IsFp16Source(e.bwd));
		if (m_native && m_native->Active() && m_native->Width() == e.gridW && m_native->Height() == e.gridH
			&& m_native->Planes() == planes && m_native->IsFp16() == fp16)
			return true;
		if (m_nativeUnavailable)
			return false;
		for (const Entry& x : m_entries)
			if (&x != &e && (x.state == State::InFlight || x.state == State::Draining) && x.native)
				return false;   // a different geometry or format while copies are in flight or draining: wait until idle
		if (!m_native)
			m_native.reset(new NativeCopyReadback());
		if (!m_native->Create(m_log, m_gpu, static_cast<int>(m_entries.size()), e.gridW, e.gridH, planes, fp16))
		{
			m_retiredFlushCount += m_native->FlushCount();
			m_native.reset();
			UpdateVramDeclaration();
			if (!m_nativeEverOk)
				m_nativeUnavailable = true;   // the very first Create failed: not D3D11 (or no storable format): the pl_buf path for good
			else
				m_lastError = "native readback re-create failed after a geometry change; retried on the next request";
			return false;
		}
		m_nativeEverOk = true;
		UpdateVramDeclaration();
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
		if (m_nativeEverOk)
		{
			// D3D11 (native worked before): never fall to the pl_buf path (no buffer transfers there). The request fails, is
			// counted in Stats().failed and the next request retries.
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
				UpdateVramDeclaration();
				m_lastError = "cannot create a host-readable buffer";
				return false;
			}
			e.bufBytes = need;
			UpdateVramDeclaration();
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
				m_parked.push_back(ParkedBuf(e.buf, e.bufBytes));
				e.buf = nullptr;
				e.bufBytes = 0;
				return false;
			}
		}
		e.state = State::InFlight;
		return true;
	}

	FieldReadbackRing::Entry* FieldReadbackRing::FreeSlot()
	{
		DrainParked();
		DrainDraining();
		for (Entry& e : m_entries)
			if (e.state == State::Free)
				return &e;
		++m_stats.dropped;
		return nullptr;
	}

	bool FieldReadbackRing::Begin(Entry& e)
	{
		e.mapped = false;
		e.ticket = ++m_ticket;
		if (!Start(e))
		{
			ResetEntry(e);
			++m_stats.failed;
			return false;
		}
		return true;
	}

	bool FieldReadbackRing::Request(const VpMotion::SeedField& f)
	{
		++m_stats.requested;
		const int per = f.blockSize / VpMotion::SeedField::kSourceBlockSize;
		if (!m_gpu || !f.fwd || !f.bwd || f.gridW <= 0 || f.gridH <= 0 || per <= 0 ||
			per * VpMotion::SeedField::kSourceBlockSize != f.blockSize || f.sourceGridW <= 0 || f.sourceGridH <= 0)
		{
			++m_stats.failed;
			return false;
		}
		Entry* slot = FreeSlot();
		if (!slot)
			return false;
		Entry& e = *slot;
		e.fwd = f.fwd;
		e.bwd = f.bwd;
		e.haveBwd = true;
		e.seq = f.seq;
		e.pairFrom = f.pairFrom;
		e.pairTo = f.pairTo;
		e.gridW = f.gridW;
		e.gridH = f.gridH;
		e.blockSize = f.blockSize;
		e.scale = f.scaleToSourcePx;
		e.stamp = f.stamp;
		e.seed = true;
		e.layout = SeedLayout();
		e.layout.gridW = f.gridW;
		e.layout.gridH = f.gridH;
		e.layout.per = per;
		e.layout.scaleToSourcePx = f.scaleToSourcePx;
		e.layout.quantumSourcePx = f.quantumSourcePx;
		e.layout.sourceGridW = f.sourceGridW;
		e.layout.sourceGridH = f.sourceGridH;
		e.layout.sadTrustMax = f.sadTrustMax;
		e.layout.weightPenalty = f.weightPenalty;
		e.layout.consistencyPx = f.consistencyPx;
		return Begin(e);
	}

	bool FieldReadbackRing::Request(const VpMotion::VerifiedField& f)
	{
		++m_stats.requested;
		if (!m_gpu || !f.fwd || f.gridW <= 0 || f.gridH <= 0)
		{
			++m_stats.failed;
			return false;
		}
		Entry* slot = FreeSlot();
		if (!slot)
			return false;
		Entry& e = *slot;
		e.seed = false;
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
		return Begin(e);
	}

	size_t FieldReadbackRing::Poll(std::vector<std::shared_ptr<FieldCopy>>& ready)
	{
		DrainParked();
		DrainDraining();
		m_flying.clear();
		for (Entry& e : m_entries)
			if (e.state == State::InFlight && !e.mapped)
				m_flying.push_back(&e);
		std::sort(m_flying.begin(), m_flying.end(), [](const Entry* a, const Entry* b) { return a->ticket < b->ticket; });
		auto ensureScratch = [this](size_t count) -> float*
		{
			if (m_scratch.capacity() < count)
			{
				m_scratch.reserve(count);
				++m_scratchReallocs;
			}
			m_scratch.resize(count);
			return m_scratch.data();
		};
		size_t n = 0;
		for (Entry* ep : m_flying)
		{
			Entry& e = *ep;
			const size_t need = PlaneBytes(e.gridW, e.gridH) * (e.haveBwd ? 2 : 1);
			const float* base = nullptr;
			if (e.native)
			{
				const int idx = static_cast<int>(&e - m_entries.data());
				const LedDrive::ReadState state = m_native ? m_native->Poll(idx) : LedDrive::ReadState::Pending;
				if (state == LedDrive::ReadState::Pending)
					break;   // DXGI_ERROR_WAS_STILL_DRAWING: the copy is not done: never wait (and keep request order)
				float* dst = ensureScratch(PlaneBytes(e.gridW, e.gridH) * 2 / sizeof(float));
				if (state == LedDrive::ReadState::Failed || !m_native->Read(idx, dst))
				{
					// An error HRESULT, not "still drawing": freed and counted as failed (CameraLinkCore turns
					// it into a readback event), never an entry that stays in flight forever.
					m_lastError = "native readback copy reported a device error";
					ResetEntry(e);
					++m_stats.failed;
					continue;
				}
				base = dst;
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
				float* dst = ensureScratch(need / sizeof(float));
				if (!pl_buf_read(m_gpu, e.buf, 0, dst, need))
				{
					ResetEntry(e);
					++m_stats.failed;
					continue;
				}
				base = dst;
			}
			if (e.seed)
			{
				// F2: the seed onto the 16-px source grid (CPU only; the readback is the same two-plane copy).
				if (!SeedToSourceGrid(base, base + blocks * 4, e.layout, *c))
				{
					m_lastError = "the optical-flow seed's layout does not match its source grid";
					ResetEntry(e);
					++m_stats.failed;
					continue;
				}
			}
			else
			{
				c->origin = FieldOrigin::Live;
				UnpackRgba(base, blocks, e.scale, c->fwdX, c->fwdY, c->fwdTrust);
				if (e.haveBwd)
					UnpackRgba(base + blocks * 4, blocks, e.scale, c->bwdX, c->bwdY, c->bwdTrust);
			}
			ResetEntry(e);
			++m_stats.completed;
			ready.push_back(std::move(c));
			++n;
		}
		return n;
	}

	void FieldReadbackRing::OnFrameStart()
	{
		DrainParked();
		DrainDraining();
		FlushPending();
	}

	bool FieldReadbackRing::FlushPending()
	{
		return m_native && m_native->FlushPending();
	}

	void FieldReadbackRing::Clear()
	{
		for (Entry& e : m_entries)
		{
			if (e.state == State::Draining)
				continue;   // already draining on GPU: never reset to Free until Poll() != Pending
			if (e.state == State::InFlight && e.native && m_native)
			{
				// GEM-135 (AB1#3): native copy still in flight on GPU; enter Draining so slot is never
				// delivered and never reused or recreated until Poll() != Pending.
				e.state = State::Draining;
				e.fwd = nullptr;
				e.bwd = nullptr;
				e.mapped = false;
				e.haveBwd = false;
				e.seed = false;
				continue;
			}
			if (e.state == State::InFlight && e.buf && !e.native)
			{
				m_parked.push_back(ParkedBuf(e.buf, e.bufBytes));   // may still be written: destroyed by a later Request/Poll once idle
				e.buf = nullptr;
				e.bufBytes = 0;
			}
			ResetEntry(e);
		}
	}
}
