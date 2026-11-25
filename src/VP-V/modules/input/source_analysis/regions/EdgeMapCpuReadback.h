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

#include <VP-V/core/GpuAttributionPl.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/drive/led_measure/NativeCopyReadback.h>
#include <VP-V/modules/input/source_analysis/lane/LaneReadback.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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
		static constexpr uint32_t kEdgeBlockSumsMagic = 0x454D4253u;   // 'EMBS' (matches FocusMap::kEdgeBlockSumsMagic)

		// GPU per-block (sum2, sum4) reduction readback device for kEdgeMap (HIGH-2 fix):
		// Reduces full-res R32F |G| on GPU to one (sum2, sum4) pair per P11 block strictly inside the active picture
		// rect, cutting CPU readback payload by ~510x (65 KB vs 33 MB at 4K) and staging VRAM by ~254x (783 KB vs 199 MB).
		// Uses LedDrive::NativeTexReadback (zero pl_tex_create/pl_tex_recreate calls here). Render thread only.
		class EdgeBlockSumsReadbackDevice : public VpSkeleton::IReadbackDevice
		{
		public:
			using SourceResolver = std::function<pl_tex(VpSkeleton::ProductId, uint64_t, int&, int&, int&, int&)>;

			EdgeBlockSumsReadbackDevice() = default;
			EdgeBlockSumsReadbackDevice(const EdgeBlockSumsReadbackDevice&) = delete;
			EdgeBlockSumsReadbackDevice& operator=(const EdgeBlockSumsReadbackDevice&) = delete;
			~EdgeBlockSumsReadbackDevice() override { Release(); }

			bool Create(pl_log log, pl_gpu gpu, int slots, int srcW, int srcH, SourceResolver resolver)
			{
				Release();
				if (!gpu || slots <= 0 || srcW <= 0 || srcH <= 0)
					return false;
				const int block = RegionEvidence::BlockSizeFor(srcH);
				const int gw = (srcW + block - 1) / block;
				const int gh = (srcH + block - 1) / block;
				if (!native_.Create(gpu, slots, gw, gh, DXGI_FORMAT_R32G32B32A32_FLOAT))
					return false;
				dispatch_ = pl_dispatch_create(log, gpu);
				if (!dispatch_)
				{
					native_.Release(gpu);
					return false;
				}
				GpuAttribution::Watch(dispatch_, "edge_map_block_reduce");
				gpu_ = gpu;
				srcW_ = srcW;
				srcH_ = srcH;
				block_ = block;
				gw_ = gw;
				gh_ = gh;
				slots_ = slots;
				resolver_ = std::move(resolver);
				const size_t payloadBytes = 16u + static_cast<size_t>(gw_) * static_cast<size_t>(gh_) * 2u * sizeof(float);
				slotBufs_.assign(static_cast<size_t>(slots_), std::vector<uint8_t>(payloadBytes, 0u));
				pendingFlush_ = false;
				lastError_ = S_OK;
				failedCount_ = 0;
				return true;
			}

			void Release()
			{
				if (dispatch_)
					pl_dispatch_destroy(&dispatch_);
				if (gpu_)
					native_.Release(gpu_);
				gpu_ = nullptr;
				srcW_ = srcH_ = block_ = gw_ = gh_ = slots_ = 0;
				resolver_ = nullptr;
				slotBufs_.clear();
				pendingFlush_ = false;
			}

			bool Active() const { return gpu_ != nullptr && dispatch_ != nullptr && native_.Active(); }
			int SrcWidth() const { return srcW_; }
			int SrcHeight() const { return srcH_; }
			int GridWidth() const { return gw_; }
			int GridHeight() const { return gh_; }
			int BlockSize() const { return block_; }
			uint64_t AllocatedBytes() const
			{
				if (!Active()) return 0u;
				// Default + staging RGBA32F (16 B/texel) across all slots.
				return static_cast<uint64_t>(slots_) * static_cast<uint64_t>(gw_) * static_cast<uint64_t>(gh_) * 16u * 2u;
			}
			HRESULT LastError() const { return lastError_; }
			uint64_t FailedCount() const { return failedCount_; }

			bool CopyToStaging(VpSkeleton::ProductId id, uint64_t seq, int stagingIndex) override
			{
				if (!Active() || !resolver_ || stagingIndex < 0 || stagingIndex >= slots_)
					return false;
				int ax0 = 0, ay0 = 0, ax1 = srcW_, ay1 = srcH_;
				pl_tex src = resolver_(id, seq, ax0, ay0, ax1, ay1);
				if (!src || src->params.w != srcW_ || src->params.h != srcH_)
					return false;
				pl_tex dst = native_.Target(stagingIndex);
				if (!dst)
					return false;
				if (ax1 <= ax0 || ay1 <= ay0)
				{
					ax0 = 0; ay0 = 0; ax1 = srcW_; ay1 = srcH_;
				}
				else
				{
					ax0 = (std::max)(0, (std::min)(srcW_, ax0));
					ay0 = (std::max)(0, (std::min)(srcH_, ay0));
					ax1 = (std::max)(ax0, (std::min)(srcW_, ax1));
					ay1 = (std::max)(ay0, (std::min)(srcH_, ay1));
				}

				static const char* const kHeader = R"GLSL(
shared float eb_s2[256];
shared float eb_s4[256];
)GLSL";
				static const char* const kBody = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 blk = ivec2(gl_WorkGroupID.xy);
int lx = int(gl_LocalInvocationID.x);
int ly = int(gl_LocalInvocationID.y);
int x0 = blk.x * eb_block;
int y0 = blk.y * eb_block;
int x1 = min(x0 + eb_block, eb_w);
int y1 = min(y0 + eb_block, eb_h);
int rx0 = max(x0, eb_ax0);
int ry0 = max(y0, eb_ay0);
int rx1 = min(x1, eb_ax1);
int ry1 = min(y1, eb_ay1);
float a2 = 0.0;
float a4 = 0.0;
for (int y = y0 + ly; y < y1; y += 16) {
    if (y < ry0 || y >= ry1) continue;
    for (int x = x0 + lx; x < x1; x += 16) {
        if (x < rx0 || x >= rx1) continue;
        float v = texelFetch(em_src, ivec2(x, y), 0).r;
        float v2 = v * v;
        a2 += v2;
        a4 += v2 * v2;
    }
}
eb_s2[li] = a2;
eb_s4[li] = a4;
barrier();
for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (li < stride) {
        eb_s2[li] += eb_s2[li + stride];
        eb_s4[li] += eb_s4[li + stride];
    }
    barrier();
}
if (li == 0u)
    imageStore(eb_dst, blk, vec4(eb_s2[0], eb_s4[0], 0.0, 0.0));
)GLSL";
				pl_shader_desc desc[2]{};
				desc[0].desc.name = "em_src";
				desc[0].desc.type = PL_DESC_SAMPLED_TEX;
				desc[0].binding.object = src;
				desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
				desc[1].desc.name = "eb_dst";
				desc[1].desc.type = PL_DESC_STORAGE_IMG;
				desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
				desc[1].binding.object = dst;

				const pl_shader_var vars[7] = {
					{ pl_var_int("eb_block"), &block_, true },
					{ pl_var_int("eb_w"), &srcW_, true },
					{ pl_var_int("eb_h"), &srcH_, true },
					{ pl_var_int("eb_ax0"), &ax0, true },
					{ pl_var_int("eb_ay0"), &ay0, true },
					{ pl_var_int("eb_ax1"), &ax1, true },
					{ pl_var_int("eb_ay1"), &ay1, true },
				};
				pl_custom_shader custom{};
				custom.description = "VP-V edge_map per-block (sum2, sum4) reduction";
				custom.header = kHeader;
				custom.body = kBody;
				custom.input = PL_SHADER_SIG_NONE;
				custom.output = PL_SHADER_SIG_NONE;
				custom.descriptors = desc;
				custom.num_descriptors = 2;
				custom.variables = vars;
				custom.num_variables = 7;
				custom.compute = true;
				custom.compute_shmem = sizeof(float) * 256u * 2u;
				custom.compute_group_size[0] = 16;
				custom.compute_group_size[1] = 16;

				pl_shader sh = pl_dispatch_begin(dispatch_);
				if (!pl_shader_custom(sh, &custom))
				{
					pl_dispatch_abort(dispatch_, &sh);
					return false;
				}
				pl_dispatch_compute_params compute{};
				compute.shader = &sh;
				compute.dispatch_size[0] = gw_;
				compute.dispatch_size[1] = gh_;
				compute.dispatch_size[2] = 1;
				if (!pl_dispatch_compute(dispatch_, &compute))
					return false;
				native_.Queue(stagingIndex);
				pendingFlush_ = true;
				return true;
			}

			bool QueryDone(int stagingIndex) override
			{
				if (!Active() || stagingIndex < 0 || stagingIndex >= slots_)
					return false;
				if (pendingFlush_)
				{
					pl_gpu_flush(gpu_);
					pendingFlush_ = false;
				}
				const LedDrive::ReadState st = native_.Poll(stagingIndex);
				if (st == LedDrive::ReadState::Failed)
				{
					lastError_ = native_.LastError();
					++failedCount_;
					return false;
				}
				return st == LedDrive::ReadState::Ready;
			}

			bool QueryFailed(int stagingIndex) override
			{
				if (!Active() || stagingIndex < 0 || stagingIndex >= slots_)
					return false;
				if (pendingFlush_)
				{
					pl_gpu_flush(gpu_);
					pendingFlush_ = false;
				}
				const LedDrive::ReadState st = native_.Poll(stagingIndex);
				if (st == LedDrive::ReadState::Failed)
				{
					lastError_ = native_.LastError();
					++failedCount_;
					return true;
				}
				return false;
			}

			void Discard(int stagingIndex) override
			{
				if (Active() && stagingIndex >= 0 && stagingIndex < slots_)
					native_.Discard(stagingIndex);
			}

			const void* MapNoWait(int stagingIndex, size_t* bytes) override
			{
				if (bytes) *bytes = 0u;
				if (!Active() || stagingIndex < 0 || stagingIndex >= slots_)
					return nullptr;
				if (pendingFlush_)
				{
					pl_gpu_flush(gpu_);
					pendingFlush_ = false;
				}
				std::vector<uint8_t>& buf = slotBufs_[static_cast<size_t>(stagingIndex)];
				const uint32_t hdr[4] = {
					kEdgeBlockSumsMagic,
					static_cast<uint32_t>(gw_),
					static_cast<uint32_t>(gh_),
					static_cast<uint32_t>(block_)
				};
				std::memcpy(buf.data(), hdr, sizeof(hdr));
				uint8_t* dst = buf.data() + sizeof(hdr);
				const int gw = gw_;
				const bool ok = native_.ReadRows(stagingIndex, gh_, [dst, gw](const unsigned char* src, size_t pitch, int nRows) {
					for (int r = 0; r < nRows; ++r)
					{
						const float* rowSrc = reinterpret_cast<const float*>(src + static_cast<size_t>(r) * pitch);
						uint8_t* rowDst = dst + static_cast<size_t>(r) * static_cast<size_t>(gw) * 2u * sizeof(float);
						for (int c = 0; c < gw; ++c)
						{
							const float pair[2] = { rowSrc[c * 4 + 0], rowSrc[c * 4 + 1] };
							std::memcpy(rowDst + static_cast<size_t>(c) * sizeof(pair), pair, sizeof(pair));
						}
					}
				});
				if (!ok)
				{
					if (native_.Failed())
					{
						lastError_ = native_.LastError();
						++failedCount_;
					}
					return nullptr;
				}
				if (bytes) *bytes = buf.size();
				return buf.data();
			}

			void Unmap(int) override {}

		private:
			LedDrive::NativeTexReadback native_;
			pl_gpu gpu_ = nullptr;
			pl_dispatch dispatch_ = nullptr;
			int srcW_ = 0, srcH_ = 0, block_ = 0, gw_ = 0, gh_ = 0, slots_ = 0;
			SourceResolver resolver_;
			std::vector<std::vector<uint8_t>> slotBufs_;
			bool pendingFlush_ = false;
			HRESULT lastError_ = S_OK;
			uint64_t failedCount_ = 0;
		};

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

			// Production demand-gated GPU per-block (sum2, sum4) reduction readback (HIGH-2 fix):
			// Reduces |G| to one (sum2, sum4) pair per P11 block on GPU strictly inside [activeX0, activeX1) x [activeY0, activeY1),
			// ~510x smaller payload and ~254x less staging VRAM than full-res R32F readback.
			bool ScheduleBlockSumsGpu(pl_log log, pl_gpu gpu, uint64_t seq, pl_tex edgeTex,
				int activeX0 = 0, int activeY0 = 0, int activeX1 = -1, int activeY1 = -1)
			{
				if (!demanded_)
					return false;
				if (gpu == nullptr || edgeTex == nullptr || edgeTex->params.w <= 0 || edgeTex->params.h <= 0)
					return false;
				const int w = edgeTex->params.w;
				const int h = edgeTex->params.h;
				if (!blockSumsDevice_.Active() || deviceGpu_ != gpu || blockSumsDevice_.SrcWidth() != w || blockSumsDevice_.SrcHeight() != h)
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
					auto resolver = [this](VpSkeleton::ProductId id, uint64_t s, int& ax0, int& ay0, int& ax1, int& ay1) -> pl_tex {
						if (id != VpSkeleton::kEdgeMap || s != pendingSeq_)
							return nullptr;
						ax0 = pendingAx0_; ay0 = pendingAy0_; ax1 = pendingAx1_; ay1 = pendingAy1_;
						return pendingTex_;
					};
					if (!blockSumsDevice_.Create(log, gpu, kEdgeMapReadbackSlots, w, h, std::move(resolver)))
						return false;
					deviceGpu_ = gpu;
					ring_.reset(new VpLaneProducts::ReadbackRing(&blockSumsDevice_, kEdgeMapReadbackSlots));
					DeclareVram(blockSumsDevice_.AllocatedBytes());
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
				pendingAx0_ = activeX0;
				pendingAy0_ = activeY0;
				pendingAx1_ = activeX1;
				pendingAy1_ = activeY1;
				const bool submitted = ring_->Submit(VpSkeleton::kEdgeMap, seq);
				pendingTex_ = nullptr;
				return submitted;
			}

			// Production convenience wrapper using the host pl_gpu recorded via SetHostGpu (keeps EdgeMapFill.h free of pl_gpu).
			bool ScheduleBlockSumsWithHostGpu(uint64_t seq, pl_tex edgeTex,
				int activeX0 = 0, int activeY0 = 0, int activeX1 = -1, int activeY1 = -1)
			{
				return ScheduleBlockSumsGpu(nullptr, hostGpu_, seq, edgeTex, activeX0, activeY0, activeX1, activeY1);
			}

			// Convenience wrapper using the host pl_gpu recorded via SetHostGpu (keeps EdgeMapFill.h free of pl_gpu).
			bool ScheduleWithHostGpu(uint64_t seq, pl_tex edgeTex)
			{
				return ScheduleBlockSumsGpu(nullptr, hostGpu_, seq, edgeTex);
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
			bool Active() const { return (externalDevice_ != nullptr && ring_ != nullptr) || blockSumsDevice_.Active() || device_.Active(); }
			int Width() const { return blockSumsDevice_.Active() ? blockSumsDevice_.GridWidth() : device_.Width(); }
			int Height() const { return blockSumsDevice_.Active() ? blockSumsDevice_.GridHeight() : device_.Height(); }
			HRESULT LastError() const { return blockSumsDevice_.Active() ? blockSumsDevice_.LastError() : device_.LastError(); }
			uint64_t FailedCount() const { return blockSumsDevice_.Active() ? blockSumsDevice_.FailedCount() : device_.FailedCount(); }

		private:
			void ReleaseOwnedGpu()
			{
				blockSumsDevice_.Release();
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
			EdgeBlockSumsReadbackDevice blockSumsDevice_;
			VpSkeleton::IReadbackDevice* externalDevice_ = nullptr;
			uint8_t externalSlots_ = kEdgeMapReadbackSlots;
			uint64_t externalVramBytes_ = 0;
			std::unique_ptr<VpLaneProducts::ReadbackRing> ring_;
			uint64_t pendingSeq_ = 0;
			pl_tex pendingTex_ = nullptr;
			int pendingAx0_ = 0, pendingAy0_ = 0, pendingAx1_ = -1, pendingAy1_ = -1;
			uint64_t declaredVramBytes_ = 0;
		};
	}
}
