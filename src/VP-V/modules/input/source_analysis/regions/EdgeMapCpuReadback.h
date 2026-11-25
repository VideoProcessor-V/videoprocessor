// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: demand-gated async GPU -> CPU readback state for kEdgeMap (S#3 / R3).
// Split into its own header (like EdgeMapDispatch.h) so EdgeMapFill.h (which defines the owner fill) never names
// pl_gpu or pl_dispatch (PrivateTextureScanTests::OwnerFillsTakeNoPrivateGpu). Header-only, C++14, render thread only.
//
// Invariants:
//   - Demand-gated: while Demanded() is false, ScheduleGpu / ScheduleExternal submit nothing, allocate no staging
//     textures, and declare 0 VRAM. Transitioning Demanded() true -> false calls Release(), freeing textures and
//     withdrawing the VRAM declaration.
//   - VRAM accounting: declares AllocatedBytes() (or external declaredVramBytes) to VramUsers under
//     ProductVramUserId(kEdgeMap) (kUserProductBase + kEdgeMap, Tier::T1), and withdraws in Release() and destructor.
//   - Non-blocking: uses LedDrive::NativeCopyReadbackDevice (R32F) + VpLaneProducts::ReadbackRing; PollAndTake reports
//     outPending = true while the frame is in flight (mapped to non-latched AcquireCause::ReadbackPending by the CPU fill).

#include <VP-V/core/Products.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/drive/led_measure/NativeCopyReadback.h>
#include <VP-V/modules/input/source_analysis/lane/LaneReadback.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Regions
	{
		// Derived retention and staging counts (Rule 1):
		//   kEdgeMapCpuKeepFrames = 2: keeps lag-1 (N - 1) and lag-2 (N - 2) past OnSeal(N) so lag-1 focus_map and its
		//                              lag-2 previous-frame delta both hit the memo without FrameReleased.
		//   kEdgeMapReadbackSlots = kEdgeMapCpuKeepFrames + 1 (3): 1 slot being written for frame N, 1 in-flight/ready
		//                              for N - 1, plus 1 latency margin slot (matching LaneReadback.h).
		static constexpr uint8_t kEdgeMapCpuKeepFrames = 2;
		static constexpr uint8_t kEdgeMapReadbackSlots = kEdgeMapCpuKeepFrames + 1;

		class EdgeMapCpuReadback
		{
		public:
			EdgeMapCpuReadback() = default;
			EdgeMapCpuReadback(const EdgeMapCpuReadback&) = delete;
			EdgeMapCpuReadback& operator=(const EdgeMapCpuReadback&) = delete;
			~EdgeMapCpuReadback() { Release(); }

			// Enables or disables CPU readback scheduling. Turning demand off releases staging textures and withdraws VRAM.
			void SetDemand(bool active)
			{
				const bool was = demanded_;
				demanded_ = active;
				if (was && !active)
					Release();
			}
			bool Demanded() const { return demanded_; }

			// Records the host's render pl_gpu from Lifecycle::GpuCreated (cleared by EdgeMapModule::Forget on GpuReleased/Shutdown).
			void SetHostGpu(pl_gpu gpu) { hostGpu_ = gpu; }

			// Attaches a caller-owned IReadbackDevice (e.g. a CPU unit test fake) driving the exact same ReadbackRing +
			// VramUsers contract. Declares declaredVramBytes under ProductVramUserId(kEdgeMap) when demanded_ && > 0.
			void AttachExternalDevice(VpSkeleton::IReadbackDevice* device, uint8_t slots = kEdgeMapReadbackSlots,
				uint64_t declaredVramBytes = 0)
			{
				if (ring_)
				{
					ring_->Reset();
					ring_.reset();
				}
				ReleaseOwnedGpu();
				WithdrawVram();
				externalDevice_ = device;
				externalSlots_ = slots > 0 ? slots : kEdgeMapReadbackSlots;
				externalVramBytes_ = declaredVramBytes;
				if (externalDevice_ != nullptr)
				{
					ring_.reset(new VpLaneProducts::ReadbackRing(externalDevice_, externalSlots_));
					if (demanded_ && externalVramBytes_ > 0)
						DeclareVram(externalVramBytes_);
				}
			}

			// Demand-gated GPU copy schedule: returns false with 0 submits / 0 allocation / 0 VRAM when !demanded_.
			bool ScheduleGpu(pl_log log, pl_gpu gpu, uint64_t seq, pl_tex edgeTex)
			{
				if (!demanded_)
					return false;
				if (gpu == nullptr || edgeTex == nullptr || edgeTex->params.w <= 0 || edgeTex->params.h <= 0)
					return false;
				const int w = edgeTex->params.w;
				const int h = edgeTex->params.h;
				if (!device_.Active() || deviceGpu_ != gpu || device_.Width() != w || device_.Height() != h)
				{
					if (ring_)
					{
						ring_->Reset();
						ring_.reset();
					}
					ReleaseOwnedGpu();
					WithdrawVram();
					externalDevice_ = nullptr;
					externalVramBytes_ = 0;
					auto resolver = [this](VpSkeleton::ProductId id, uint64_t s) -> pl_tex {
						return (id == VpSkeleton::kEdgeMap && s == pendingSeq_) ? pendingTex_ : nullptr;
					};
					if (!device_.Create(log, gpu, kEdgeMapReadbackSlots, w, h, LedDrive::CopyFormat::R32F, std::move(resolver)))
						return false;
					deviceGpu_ = gpu;
					ring_.reset(new VpLaneProducts::ReadbackRing(&device_, kEdgeMapReadbackSlots));
					DeclareVram(device_.AllocatedBytes());
				}
				ring_->Poll();
				if (ring_->HasPending(seq))
					return true;
				const uint64_t minKeep = seq > kEdgeMapCpuKeepFrames ? seq - kEdgeMapCpuKeepFrames : uint64_t(0);
				ring_->EvictReadyOlderThan(minKeep);
				if (!ring_->HasFreeSlot())
					ring_->EvictOldestReady();
				pendingSeq_ = seq;
				pendingTex_ = edgeTex;
				const bool submitted = ring_->Submit(VpSkeleton::kEdgeMap, seq);
				pendingTex_ = nullptr;
				return submitted;
			}

			// Convenience wrapper using the host pl_gpu recorded via SetHostGpu (keeps EdgeMapFill.h free of pl_gpu).
			bool ScheduleWithHostGpu(uint64_t seq, pl_tex edgeTex)
			{
				return ScheduleGpu(nullptr, hostGpu_, seq, edgeTex);
			}

			// Demand-gated schedule for an attached external IReadbackDevice.
			bool ScheduleExternal(uint64_t seq)
			{
				if (!demanded_ || externalDevice_ == nullptr)
					return false;
				if (!ring_)
					ring_.reset(new VpLaneProducts::ReadbackRing(externalDevice_, externalSlots_));
				if (externalVramBytes_ > 0 && declaredVramBytes_ != externalVramBytes_)
					DeclareVram(externalVramBytes_);
				ring_->Poll();
				if (ring_->HasPending(seq))
					return true;
				const uint64_t minKeep = seq > kEdgeMapCpuKeepFrames ? seq - kEdgeMapCpuKeepFrames : uint64_t(0);
				ring_->EvictReadyOlderThan(minKeep);
				if (!ring_->HasFreeSlot())
					ring_->EvictOldestReady();
				return ring_->Submit(VpSkeleton::kEdgeMap, seq);
			}

			// Non-blocking poll + take for the kEdgeMap CPU owner fill.
			bool PollAndTake(uint64_t seq, std::vector<uint8_t>& outBytes, bool& outPending, std::string& outReason)
			{
				outPending = false;
				outBytes.clear();
				if (!ring_)
				{
					outReason = demanded_ ? "edge_map CPU: readback device not ready" : "edge_map CPU: no active CPU demand";
					return false;
				}
				ring_->Poll();
				const bool taken = ring_->Take(seq, [&outBytes](const void* p, size_t n) {
					if (p != nullptr && n > 0)
					{
						const uint8_t* b = static_cast<const uint8_t*>(p);
						outBytes.assign(b, b + n);
					}
				}, kEdgeMapCpuKeepFrames);
				if (taken && !outBytes.empty())
					return true;
				if (ring_->HasInFlight(seq) || ring_->HasReady(seq))
				{
					outPending = true;
					outReason = "edge_map CPU: readback in flight";
					return false;
				}
				if (ring_->WasFailed(seq))
				{
					outReason = "edge_map CPU: readback device failed";
					return false;
				}
				outReason = demanded_ ? "edge_map CPU: readback not scheduled for frame" : "edge_map CPU: no active CPU demand";
				return false;
			}

			void Poll()
			{
				if (ring_)
					ring_->Poll();
			}

			// Releases owned GPU staging textures, resets ring state, and withdraws the VramUsers declaration.
			void Release()
			{
				if (ring_)
				{
					ring_->Reset();
					ring_.reset();
				}
				ReleaseOwnedGpu();
				WithdrawVram();
			}

			uint64_t DeclaredVramBytes() const { return declaredVramBytes_; }
			VpLaneProducts::ReadbackCounters Counters() const
			{
				return ring_ ? ring_->Counters() : VpLaneProducts::ReadbackCounters();
			}
			bool HasInFlight(uint64_t seq) const { return ring_ && ring_->HasInFlight(seq); }
			bool HasReady(uint64_t seq) const { return ring_ && ring_->HasReady(seq); }
			bool Active() const { return (externalDevice_ != nullptr && ring_ != nullptr) || device_.Active(); }
			int Width() const { return device_.Width(); }
			int Height() const { return device_.Height(); }
			HRESULT LastError() const { return device_.LastError(); }
			uint64_t FailedCount() const { return device_.FailedCount(); }

		private:
			void ReleaseOwnedGpu()
			{
				device_.Release();
				deviceGpu_ = nullptr;
				pendingTex_ = nullptr;
				pendingSeq_ = 0;
			}

			void DeclareVram(uint64_t bytes)
			{
				if (bytes == 0)
					return;
				VpSkeleton::VramUser u;
				u.id = VpSkeleton::ProductVramUserId(VpSkeleton::kEdgeMap);
				u.name = "edge_map CPU readback";
				u.tier = VpSkeleton::Tier::T1;
				u.lossless = false;
				u.levels = { bytes };
				u.levelText = { nullptr };
				VpSkeleton::VramUsers::Instance().Declare(this, u, bytes);
				declaredVramBytes_ = bytes;
			}

			void WithdrawVram()
			{
				if (declaredVramBytes_ != 0)
				{
					VpSkeleton::VramUsers::Instance().Withdraw(this, VpSkeleton::ProductVramUserId(VpSkeleton::kEdgeMap));
					declaredVramBytes_ = 0;
				}
			}

			bool demanded_ = false;
			pl_gpu hostGpu_ = nullptr;
			pl_gpu deviceGpu_ = nullptr;
			LedDrive::NativeCopyReadbackDevice device_;
			VpSkeleton::IReadbackDevice* externalDevice_ = nullptr;
			uint8_t externalSlots_ = kEdgeMapReadbackSlots;
			uint64_t externalVramBytes_ = 0;
			std::unique_ptr<VpLaneProducts::ReadbackRing> ring_;
			uint64_t pendingSeq_ = 0;
			pl_tex pendingTex_ = nullptr;
			uint64_t declaredVramBytes_ = 0;
		};
	}
}
