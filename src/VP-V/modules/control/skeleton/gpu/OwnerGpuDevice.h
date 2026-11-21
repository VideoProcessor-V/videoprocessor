// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// R2 (lead 12): the device side of THE sanctioned GPU context of non-view GPU owner fills (core/SkeletonAcquire.h OwnerGpuContext /
// IOwnerGpuDevice). One per renderer, held with the renderer's memo (SkeletonModule MemoHost), so it serves GPU fills with or
// without the analysis lane: the render pl_gpu (handed by GpuCreated), a pl_dispatch of its own (made on first use, render thread)
// and texture creation through the skeleton's texture backend (GpuProductTexBackend.h: sampleable + storable, the ledger's
// TexFormat). The memo accounts every texture against the fill's reserve before it reaches here.
//
// Lifetime: a texture's keep-alive (GpuMemoHandle::keepAlive, held by the memo spot) hands it back here when the spot is released
// (OnSeal at Begin, Clear, Discard ...); it is destroyed at the next FlushReleases (AfterRender, GPU-legal). Release() (device
// alive: GpuReleased / Shutdown) and Abandon() (device died) close the pool: the memo releases its GPU spots first
// (SkeletonMemo::ReleaseGpuSpots), and a texture still held after that (a reader's pin) is forgotten, never handed to a device that
// went away (counted: Stats().forgotten). The destructor calls no libplacebo function (Release() first while the device lives).
// Render thread, except the keep-alive (any thread: it only queues).

#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/control/skeleton/GpuProductTexBackend.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Skeleton
	{
		class OwnerGpuDevice : public VpSkeleton::IOwnerGpuDevice
		{
		public:
			struct Stats
			{
				uint64_t made = 0;        // textures created for owner fills
				uint64_t destroyed = 0;   // destroyed by FlushReleases / Release
				uint64_t forgotten = 0;   // handed back after the pool closed (device released or lost): not destroyed here
				uint64_t live = 0;        // held by memo spots now
				uint64_t liveBytes = 0;
			};

			explicit OwnerGpuDevice(pl_gpu gpu) : m_gpu(gpu), m_backend(gpu), m_pool(std::make_shared<Pool>()) {}
			// Audit S2: no libplacebo call here (as SkeletonModule's own teardown at process end, Teardown(.., gpuAlive false): the device
			// may already be gone). The lifecycle releases it first (GpuReleased / Shutdown -> DetachMemoGpu -> Release).
			~OwnerGpuDevice() override { Close(); }
			OwnerGpuDevice(const OwnerGpuDevice&) = delete;
			OwnerGpuDevice& operator=(const OwnerGpuDevice&) = delete;

			pl_gpu Gpu() const { return m_gpu; }

			// IOwnerGpuDevice (render thread).
			void* OwnerGpu() const override { return const_cast<void*>(static_cast<const void*>(m_gpu)); }
			void* OwnerDispatch() const override
			{
				if (m_dispatch == nullptr && m_gpu != nullptr)
					m_dispatch = pl_dispatch_create(m_gpu->log, m_gpu);
				return static_cast<void*>(m_dispatch);
			}
			uint64_t OwnerMaxSharedMemory() const override { return m_gpu != nullptr ? uint64_t(m_gpu->glsl.max_shmem_size) : 0u; }
			bool CreateOwnerTexture(const VpSkeleton::TexShape& shape, VpSkeleton::GpuMemoHandle& out, std::string& why) override
			{
				out = VpSkeleton::GpuMemoHandle();
				if (m_gpu == nullptr)
				{
					why = "the render device is gone";
					return false;
				}
				const pl_tex tex = m_backend.Create(shape);
				if (tex == nullptr)
				{
					why = m_backend.lastError.empty() ? std::string("the GPU refused an owner texture ") + std::to_string(shape.w) + "x" +
						std::to_string(shape.h) : m_backend.lastError;
					return false;
				}
				const uint64_t bytes = VpSkeleton::ShapeBytes(shape);
				const std::shared_ptr<Pool> pool = m_pool;
				{
					std::lock_guard<std::mutex> g(pool->mutex);
					++pool->stats.made;
					++pool->stats.live;
					pool->stats.liveBytes += bytes;
				}
				out.resource = const_cast<void*>(static_cast<const void*>(tex));
				out.width = shape.w;
				out.height = shape.h;
				out.format = uint32_t(shape.fmt);
				out.bytes = bytes;
				out.keepAlive = std::shared_ptr<void>(out.resource, [pool, bytes](void* p) { pool->Retire(static_cast<pl_tex>(p), bytes); });
				return true;
			}

			// Render thread, GPU-legal phase: destroy the textures the memo handed back.
			void FlushReleases()
			{
				std::vector<pl_tex> dead;
				{
					std::lock_guard<std::mutex> g(m_pool->mutex);
					dead.swap(m_pool->dead);
					m_pool->stats.destroyed += dead.size();
				}
				for (pl_tex& t : dead)
					if (m_gpu != nullptr)
						pl_tex_destroy(m_gpu, &t);
			}
			// The device is alive and goes (GpuReleased / Shutdown): everything handed back is destroyed, the dispatch too; the pool
			// closes (a later hand-back is forgotten). The memo released its GPU spots first.
			void Release()
			{
				FlushReleases();
				Close();
				if (m_dispatch != nullptr && m_gpu != nullptr)
					pl_dispatch_destroy(&m_dispatch);
				m_dispatch = nullptr;
				m_gpu = nullptr;
				m_backend.gpu = nullptr;
			}
			// The device died without GpuReleased: forget every device object WITHOUT calling libplacebo.
			void Abandon()
			{
				{
					std::lock_guard<std::mutex> g(m_pool->mutex);
					m_pool->stats.forgotten += m_pool->dead.size();
					m_pool->dead.clear();
				}
				Close();
				m_dispatch = nullptr;
				m_gpu = nullptr;
				m_backend.gpu = nullptr;
			}

			Stats GetStats() const
			{
				std::lock_guard<std::mutex> g(m_pool->mutex);
				return m_pool->stats;
			}

		private:
			struct Pool
			{
				std::mutex mutex;
				bool open = true;
				std::vector<pl_tex> dead;
				Stats stats;
				void Retire(pl_tex t, uint64_t bytes)
				{
					std::lock_guard<std::mutex> g(mutex);
					if (stats.live > 0)
						--stats.live;
					stats.liveBytes = stats.liveBytes >= bytes ? stats.liveBytes - bytes : 0;
					if (open)
						dead.push_back(t);
					else
						++stats.forgotten;   // the device went first: never destroyed through a device that may be gone
				}
			};
			void Close()
			{
				std::lock_guard<std::mutex> g(m_pool->mutex);
				m_pool->open = false;
			}

			pl_gpu m_gpu = nullptr;
			mutable pl_dispatch m_dispatch = nullptr;
			VpLaneProducts::GpuTexBackend m_backend;
			std::shared_ptr<Pool> m_pool;
		};
	}
}
