// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-4): the GPU -> CPU readback ring of the verified P3 field. Each entry is a host-mapped pl_buf the
// field textures are downloaded into (pl_tex_download with .buf, asynchronous); completion is tested ONLY with
// pl_buf_poll(gpu, buf, 0). The render thread never waits and never maps blockingly: a full ring drops the newest
// request (counted). Render thread only (libplacebo is not thread-safe); the FieldCopy results are plain data.
#pragma once

#include "FieldCopy.h"
#include "NativeCopyReadback.h"

#include <VP-V/modules/motion/estimation/VectorEngineInterface.h>

#include <libplacebo/gpu.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace CameraLink
{
	class FieldReadbackRing
	{
	public:
		struct Stats
		{
			uint64_t requested = 0, completed = 0, dropped = 0, failed = 0;
		};

		// `entries` is clamped to >= 3.
		// `log` may be null. On D3D11 the NativeCopyReadback path is used (no buffer transfers there); elsewhere pl_buf.
		FieldReadbackRing(pl_gpu gpu, int entries, pl_log log = nullptr);
		// Tolerates copies in flight: pl_gpu_finish (the only blocking wait, destructor only) then pl_buf_destroy, because
		// destroying a buffer the GPU still writes would be a use-after-free.
		~FieldReadbackRing();
		FieldReadbackRing(const FieldReadbackRing&) = delete;
		FieldReadbackRing& operator=(const FieldReadbackRing&) = delete;

		// Render thread. Starts the copy. False
		// when the ring is full (newest request dropped, counted), the field is unusable, or the download failed.
		bool Request(const VpMotion::VerifiedField& f);

		// Appends every finished entry, in request order, as a new FieldCopy (source px) and frees it. Never blocks.
		size_t Poll(std::vector<std::shared_ptr<FieldCopy>>& ready);

		// Graph swap: forgets all entries. Buffers the GPU may still write are parked and destroyed by a later
		// Request/Poll once idle (never waited for).
		void Clear();

		struct Stats Stats() const { return m_stats; }
		const char* LastError() const { return m_lastError; }
		int Entries() const { return static_cast<int>(m_entries.size()); }

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
			bool native = false;   // read through NativeCopyReadback (slot = the entry index)
		};

		bool Start(Entry& e);
		void Release(Entry& e);
		void DrainParked();
		const char* m_lastError = "";

		bool TryNative(const Entry& e);
		pl_gpu m_gpu;
		pl_log m_log;
		std::unique_ptr<NativeCopyReadback> m_native;
		bool m_nativeEverOk = false;        // the first Create worked (D3D11): later failures retry, they do not demote
		bool m_nativeUnavailable = false;   // the very FIRST Create failed (not D3D11): the pl_buf path for good
		std::vector<Entry> m_entries;
		std::vector<pl_buf> m_parked;
		uint64_t m_ticket = 0;
		struct Stats m_stats;
	};
}
