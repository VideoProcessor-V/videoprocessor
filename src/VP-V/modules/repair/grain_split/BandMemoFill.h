// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair/grain_split: the OWNER FILL of kBandMemo (P30 "band_memo"; C8 slice 8, coreplan-C8-8-U2, shared unit U2), GPU form.
// One separable Gaussian band per (frame, BandKey): two compute passes (across into an owner scratch R32F, down into the published R32F),
// the taps and the GLSL text from BandMemo.h (the shared emitter, BandFromBase::GlslFromTaps). Gets its device, dispatch and both textures
// ONLY from OwnerFillRequest::gpu (R2); the input plane through the memo (nested Acquire, no private upload).
// Served keys: plane 0, pipeline stage 0, Replicate edge, Law::Taps or Law::Low. Full resolution reads the P13 source luma view; Half reads
// P1 (kP1HalfLuma) as a GPU form. Not served, each a VISIBLE decline with its cause (product.band_memo_unavailable): a key outside the codec,
// a Reflect edge (CPU-only form), plane > 0 or a pipeline stage > 0 (REBASE-ON-CA: the slot planes come with C6/C7), and a Half key while P1
// has no GPU owner fill (the B0 lane). The CPU form is not registered (the declared backend is GpuCompute).
// Output units: the sampled value of the input view (normalised), exactly what the private blurs of the consumers produced.
// Reserve: two R32F textures of the largest luma size the P13 view shows (BandMemoReserveBytes; rule 6, derived, never fixed).
// Registration: BandMemoModule below (observer of Entrance, as EdgeMapModule); NOT wired into HubBootstrap in this slice (hook-line delta
// 0 rule): the lead decides the one call site (see the inbox).
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace GrainBand
	{
		namespace RE = RendererExtension;
		static constexpr const char* kBandMemoUnit = "repair/grain_split";   // the catalog's kBandMemo owner

		// Two R32F planes (scratch + output) at the source luma size; 0 = no size known yet.
		inline uint64_t BandMemoReserveBytes(VpSkeleton::Dims luma)
		{
			if (luma.width == 0 || luma.height == 0)
				return 0;
			return uint64_t(luma.width) * uint64_t(luma.height) * 4u * 2u;
		}

		namespace Detail
		{
			// One compute pass: `body` over `in` (sampled, clamp, nearest) into `dst` (storage r32f).
			inline bool BandPass(pl_dispatch dispatch, pl_tex in, pl_tex dst, const std::string& header, const char* body, const char* name)
			{
				pl_shader_desc desc[2]{};
				desc[0].desc.name = "bm_src";
				desc[0].desc.type = PL_DESC_SAMPLED_TEX;
				desc[0].binding.object = in;
				desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
				desc[1].desc.name = "bm_out";
				desc[1].desc.type = PL_DESC_STORAGE_IMG;
				desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
				desc[1].binding.object = dst;
				pl_custom_shader custom{};
				custom.description = name;
				custom.header = header.c_str();
				custom.body = body;
				custom.input = PL_SHADER_SIG_NONE;
				custom.output = PL_SHADER_SIG_NONE;
				custom.descriptors = desc;
				custom.num_descriptors = 2;
				custom.compute = true;
				custom.compute_group_size[0] = 16;
				custom.compute_group_size[1] = 16;
				pl_shader sh = pl_dispatch_begin(dispatch);
				if (!pl_shader_custom(sh, &custom))
				{
					pl_dispatch_abort(dispatch, &sh);
					return false;
				}
				pl_dispatch_compute_params compute{};
				compute.shader = &sh;
				compute.dispatch_size[0] = (dst->params.w + 15) / 16;
				compute.dispatch_size[1] = (dst->params.h + 15) / 16;
				compute.dispatch_size[2] = 1;
				return pl_dispatch_compute(dispatch, &compute);
			}
		}

		inline VpSkeleton::OwnerFill MakeBandMemoOwnerFill(uint64_t reserveBytes)
		{
			using VK = VpSkeleton::VariantKey;
			VpSkeleton::OwnerFill f;
			f.variantAxes = uint32_t(VK::kAxisPlane) | uint32_t(VK::kAxisSize) | uint32_t(VK::kAxisScale) | uint32_t(VK::kAxisSigma)
				| uint32_t(VK::kAxisStage) | uint32_t(VK::kAxisParam0) | uint32_t(VK::kAxisParam1);
			f.reserveBytes = reserveBytes;
			f.frameScoped = true;
			f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				VpUnit::BandMemo::BandKey key;
				if (!VpUnit::BandMemo::FromVariant(req.variant, &key))
				{
					out.declineReason = "band_memo: the key is not a band key (outside the codec)";
					return false;
				}
				if (key.edge != VpUnit::BandFromBase::EdgeMode::Replicate)
				{
					out.declineReason = "band_memo: a Reflect edge has no GPU form";
					return false;
				}
				if (key.plane != 0 || key.pipelineStage != 0)
				{
					out.declineReason = "band_memo: only the source luma plane is served (slot planes and chroma wait for the C6/C7 planes)";
					return false;
				}
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
				{
					out.declineReason = "band_memo: no GPU context";
					return false;
				}
				const bool half = key.resolution == VpUnit::BandMemo::Stage::Half;
				const VpSkeleton::AcquireResult src = memo.Acquire(half ? VpSkeleton::kP1HalfLuma : VpSkeleton::kP13SourceRing, req.seq,
				                                                   VpSkeleton::AcquireForm::Gpu);
				if (!src.Ok() || !src.gpu.Valid() || src.gpu.resource == nullptr)
				{
					out.declineReason = half ? "band_memo: no GPU half luma plane (P1 has no GPU form yet)" : "band_memo: no source luma view";
					return false;
				}
				const pl_tex in = static_cast<pl_tex>(src.gpu.resource);
				VpSkeleton::TexShape shape;
				shape.w = uint32_t(in->params.w);
				shape.h = uint32_t(in->params.h);
				shape.fmt = VpSkeleton::TexFormat::R32F;
				VpSkeleton::GpuMemoHandle scratch;
				std::string why;
				if (!req.gpu->Allocate(shape, scratch, why))
				{
					out.declineReason = "band_memo: " + why;
					return false;
				}
				if (!req.gpu->Allocate(shape, out.gpu, why))
				{
					out.declineReason = "band_memo: " + why;
					return false;
				}
				const pl_tex mid = static_cast<pl_tex>(scratch.resource);
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());
				const std::string header = VpUnit::BandMemo::GlslOf(key);
				if (!Detail::BandPass(dispatch, in, mid, header, VpUnit::BandMemo::AcrossBodyGlsl(), "VP-V band memo across")
					|| !Detail::BandPass(dispatch, mid, dst, header, VpUnit::BandMemo::DownBodyGlsl(), "VP-V band memo down"))
				{
					out.gpu = VpSkeleton::GpuMemoHandle();
					out.declineReason = "band_memo: shader or dispatch failed";
					return false;
				}
				return true;   // `scratch` goes out of scope: its keepAlive releases it at the device's next GPU-legal phase
			};
			return f;
		}

		// Registers the kBandMemo GPU fill with the skeleton found by the hub; the reserve follows the largest source luma size seen at
		// Entrance (as EdgeMapModule; re-registered only when it changes). Cost per frame: one short lock and a lookup.
		class BandMemoModule : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "repair.grain_split.band_memo"; }
			void OnLifecycle(RE::LifecycleFacts& f) override
			{
				if (f.host && (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown))
					Forget(f.host);
			}
			bool OnFrame(RE::FrameContext& c) override
			{
				if (c.phase != RE::FramePhase::Entrance || c.image == nullptr || c.image->num_planes < 1
					|| c.image->planes[0].texture == nullptr || Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					return false;
				VpSkeleton::Dims luma;
				luma.width = uint32_t(c.image->planes[0].texture->params.w);
				luma.height = uint32_t(c.image->planes[0].texture->params.h);
				Observe(c.host, luma);
				return false;
			}
			bool Observe(const void* host, VpSkeleton::Dims luma, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				bool found = false;
				for (auto& h : hosts_)
				{
					if (h.host == host)
					{
						if (h.luma.width != luma.width || h.luma.height != luma.height)
						{
							h.luma = luma;
							h.bytes = BandMemoReserveBytes(luma);
						}
						found = true;
						break;
					}
				}
				if (!found && host != nullptr && hosts_.size() < kMaxHosts)
				{
					HostSize h;
					h.host = host;
					h.luma = luma;
					h.bytes = BandMemoReserveBytes(luma);
					hosts_.push_back(h);
				}
				return UpdateLocked(demand);
			}
			void Forget(const void* host, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (size_t i = 0; i < hosts_.size(); ++i)
				{
					if (hosts_[i].host == host)
					{
						hosts_.erase(hosts_.begin() + std::ptrdiff_t(i));
						break;
					}
				}
				UpdateLocked(demand);
			}
			bool Registered() const { return registered_.load(std::memory_order_acquire); }
		private:
			static constexpr size_t kMaxHosts = 16;   // the skeleton's renderer table (HostSlots)
			struct HostSize
			{
				const void* host = nullptr;
				VpSkeleton::Dims luma;
				uint64_t bytes = 0;
			};
			bool UpdateLocked(Skeleton::IExternalDemand* demand)
			{
				uint64_t want = 0;
				for (const HostSize& h : hosts_)
					if (h.bytes > want)
						want = h.bytes;
				const bool ok = registered_.load(std::memory_order_acquire);
				if (want == 0 || (ok && want == reserve_) || want == tried_)
					return ok;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
					return ok;
				tried_ = want;
				if (ok)
					demand->UnregisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu, kBandMemoUnit);
				const VpSkeleton::SkeletonMemo::Registration r =
					demand->RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu, kBandMemoUnit, MakeBandMemoOwnerFill(want));
				const bool now = r == VpSkeleton::SkeletonMemo::Registration::Registered;
				reserve_ = now ? want : uint64_t(0);
				registered_.store(now, std::memory_order_release);
				return now;
			}
			mutable std::mutex mutex_;
			std::vector<HostSize> hosts_;
			uint64_t reserve_ = 0;
			uint64_t tried_ = 0;
			std::atomic<bool> registered_{ false };
		};

		// Start-up only; idempotent. Not called in this slice (see the header note).
		inline void RegisterBandMemoModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new BandMemoModule()));
		}
	}
}
