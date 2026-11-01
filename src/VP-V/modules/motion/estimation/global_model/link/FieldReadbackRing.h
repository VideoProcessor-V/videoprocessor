// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-4): the GPU -> CPU readback ring of the verified P3 field. Each entry is a host-mapped pl_buf the
// field textures are downloaded into (pl_tex_download with .buf, asynchronous); completion is tested ONLY with
// pl_buf_poll(gpu, buf, 0). The render thread never waits and never maps blockingly: a full ring drops the newest
// request (counted). Render thread only (libplacebo is not thread-safe); the FieldCopy results are plain data.
#pragma once

#include "FieldCopy.h"

#include <VP-V/core/LaneInterfaces.h>
#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <libplacebo/gpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace CameraLink
{
	class FieldReadbackRing : public VpSkeleton::IReadbackDevice
	{
	public:
		struct Stats
		{
			uint64_t requested = 0, completed = 0, dropped = 0, failed = 0;
		};

		// `entries` is clamped to >= 3.
		FieldReadbackRing(pl_gpu gpu, int entries);
		// Tolerates copies in flight: pl_gpu_finish (the only blocking wait, destructor only) then pl_buf_destroy, because
		// destroying a buffer the GPU still writes would be a use-after-free.
		~FieldReadbackRing() override;
		FieldReadbackRing(const FieldReadbackRing&) = delete;
		FieldReadbackRing& operator=(const FieldReadbackRing&) = delete;

		// Render thread. Starts the copy at once (startNow) or leaves the entry pending until CopyToStaging(seq). False
		// when the ring is full (newest request dropped, counted), the field is unusable, or the download failed.
		bool Request(const VpMotion::VerifiedField& f, bool startNow = true);

		// Appends every finished entry, in request order, as a new FieldCopy (source px) and frees it. Never blocks.
		size_t Poll(std::vector<std::shared_ptr<FieldCopy>>& ready);

		// Graph swap: forgets all entries. Buffers the GPU may still write are parked and destroyed by a later
		// Request/Poll once idle (never waited for).
		void Clear();

		struct Stats Stats() const { return m_stats; }
		int Entries() const { return static_cast<int>(m_entries.size()); }

		// IReadbackDevice. The staging index is the ring entry index. CopyToStaging starts the copy of the pending
		// request whose seq matches (`product` is unused: the ring carries the one field product).
		bool CopyToStaging(VpSkeleton::ProductId product, uint64_t seq, int stagingIndex) override;
		bool QueryDone(int stagingIndex) override;
		const void* MapNoWait(int stagingIndex, size_t* bytes) override;
		void Unmap(int stagingIndex) override;

	private:
		enum class State { Free, Pending, InFlight };
		struct Entry
		{
			State state = State::Free;
			pl_buf buf = nullptr;
			size_t bufBytes = 0;
			uint64_t ticket = 0;
			pl_tex fwd = nullptr, bwd = nullptr;
			uint64_t seq = 0, pairFrom = 0, pairTo = 0;
			int gridW = 0, gridH = 0, blockSize = 0;
			float scale = 1.0f;
			VpIndicator::Stamp stamp;
			bool haveBwd = false;
			bool mapped = false;
		};

		bool Start(Entry& e);
		void Release(Entry& e);
		void DrainParked();

		pl_gpu m_gpu;
		std::vector<Entry> m_entries;
		std::vector<pl_buf> m_parked;
		uint64_t m_ticket = 0;
		struct Stats m_stats;
	};
}
