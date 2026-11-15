// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the OWNER FILL of kEdgeMap (lead8/iface-L-B5.md section C): per-pixel 3x3 Sobel |G| of the source
// luma, GPU, per-frame memo. Gets its device, dispatch and output texture ONLY from OwnerFillRequest::gpu (L-CORE), the source luma
// from the P13 ring view through the memo (get-or-compute-and-publish, no private upload). Header-only, C++14.
// Variant axes: none (default variant only; width / height = the source luma size). Output R32F in codes at the 10-bit scale
// (sampled value x RegionEvidence::kSourceTapCodesPerUnit, the source planes' one tap form, shared with the busy share): float32
// keeps a 12-bit source exact (rule 7), unlike the frozen r16f whose 11-bit mantissa does not.
// Reserve: DERIVED, never a fixed resolution (rule 6): kEdgeMap's declared size rule at the largest source luma size the Entrance
// image (the P13 view) shows on a live host; the module re-registers the fill when that size changes (EdgeMapModule::Observe).
// Declines (visible, with the cause): no GPU context, no luma view, texture refused / over the reserve, shader or dispatch failed.
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>

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
	namespace Regions
	{
		namespace RE = RendererExtension;
		static constexpr const char* kEdgeMapUnit = "input/source_analysis/regions";   // the catalog's kEdgeMap owner

		// Reserve per fill, DERIVED from the source (rule 6): kEdgeMap's DECLARED size rule (AppendEdgeMapProduct: one R32F sample per
		// source pixel) at the source luma size `luma`, so the ledger, the ahead cost and the fill agree on one figure (33 MB at a 4K
		// source, 8.3 MB at 1080p). 0 = no size known yet. The grant is checked BEFORE the fill; a larger frame declines visibly.
		inline uint64_t EdgeMapReserveBytes(VpSkeleton::Dims luma)
		{
			if (luma.width == 0 || luma.height == 0)
				return 0;
			std::vector<VpSkeleton::ProductDecl> decls;
			VpSkeleton::AppendEdgeMapProduct(decls);
			return decls.empty() ? uint64_t(0) : VpSkeleton::BytesOf(decls.back().size, luma);
		}

		inline VpSkeleton::OwnerFill MakeEdgeMapOwnerFill(uint64_t reserveBytes)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = 0;
			f.reserveBytes = reserveBytes;
			f.frameScoped = true;
			f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
				{
					out.declineReason = "edge_map: no GPU context";
					return false;
				}
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);
				if (!src.Ok() || !src.gpu.Valid() || src.gpu.resource == nullptr)
				{
					out.declineReason = "edge_map: no source luma view";
					return false;
				}
				const pl_tex in = static_cast<pl_tex>(src.gpu.resource);
				VpSkeleton::TexShape shape;
				shape.w = uint32_t(in->params.w);
				shape.h = uint32_t(in->params.h);
				shape.fmt = VpSkeleton::TexFormat::R32F;
				std::string why;
				if (!req.gpu->Allocate(shape, out.gpu, why))
				{
					out.declineReason = "edge_map: " + why;
					return false;
				}
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());
				pl_shader_desc desc[2]{};
				desc[0].desc.name = "em_src";
				desc[0].desc.type = PL_DESC_SAMPLED_TEX;
				desc[0].binding.object = in;
				desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
				desc[1].desc.name = "em_out";
				desc[1].desc.type = PL_DESC_STORAGE_IMG;
				desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
				desc[1].binding.object = dst;
				const float scale = RegionEvidence::kSourceTapCodesPerUnit;   // normalised sample -> 10-bit codes (the one tap form)
				const pl_shader_var vars[1] = { { pl_var_float("em_scale"), &scale, true } };
				const std::string header = RegionEvidence::EdgeMapHeaderGlsl();
				const std::string body = RegionEvidence::EdgeMapBodyGlsl();
				pl_custom_shader custom{};
				custom.description = "VP-V edge map";
				custom.header = header.c_str();
				custom.body = body.c_str();
				custom.input = PL_SHADER_SIG_NONE;
				custom.output = PL_SHADER_SIG_NONE;
				custom.descriptors = desc;
				custom.num_descriptors = 2;
				custom.variables = vars;
				custom.num_variables = 1;
				custom.compute = true;
				custom.compute_group_size[0] = 16;
				custom.compute_group_size[1] = 16;
				pl_shader sh = pl_dispatch_begin(dispatch);
				if (!pl_shader_custom(sh, &custom))
				{
					pl_dispatch_abort(dispatch, &sh);
					out.declineReason = "edge_map: shader rejected";
					return false;
				}
				pl_dispatch_compute_params compute{};
				compute.shader = &sh;
				compute.dispatch_size[0] = (in->params.w + 15) / 16;
				compute.dispatch_size[1] = (in->params.h + 15) / 16;
				compute.dispatch_size[2] = 1;
				if (!pl_dispatch_compute(dispatch, &compute))
				{
					out.declineReason = "edge_map: dispatch failed";
					return false;
				}
				return true;
			};
			return f;
		}

		// Registers the kEdgeMap fill with the skeleton found by the hub (tests pass their own demand). The reserve is derived
		// (EdgeMapReserveBytes) from the LARGEST source luma size over the live hosts, observed at Entrance (the image the P13 view
		// serves, before this frame's readers run); the fill is registered on the first size and re-registered (unregister, then
		// register) only when that largest size changes, so one figure serves every host's memo (module-wide registration). A host
		// is forgotten at GpuReleased / Shutdown. Nothing is registered before a size is known (an Acquire then is a visible
		// NoOwnerFill). Cost per frame: one short lock and a lookup in at most sixteen entries.
		class EdgeMapModule : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.source_analysis.edge_map"; }
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
			// Records `host`'s source luma size and (re-)registers the fill when the derived reserve changes. True = a fill with the
			// current reserve is registered.
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
							h.bytes = EdgeMapReserveBytes(luma);   // only when the size changes (no per-frame work beyond the lookup)
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
					h.bytes = EdgeMapReserveBytes(luma);
					hosts_.push_back(h);
				}
				return UpdateLocked(demand);
			}
			// Forgets `host`; a smaller largest size re-registers the fill with the smaller reserve (none left: registration kept).
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
			// The reserve of the registered fill (0 = none registered).
			uint64_t RegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return registered_.load(std::memory_order_acquire) ? reserve_ : uint64_t(0);
			}
		private:
			static constexpr size_t kMaxHosts = 16;   // the skeleton's renderer table (HostSlots)
			struct HostSize
			{
				const void* host = nullptr;
				VpSkeleton::Dims luma;
				uint64_t bytes = 0;   // EdgeMapReserveBytes(luma)
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
					return ok;   // no skeleton yet: retried at the next Entrance
				tried_ = want;   // a refused registration is retried at the next size change, not every frame
				if (ok)
					demand->UnregisterOwnerFill(VpSkeleton::kEdgeMap, VpSkeleton::AcquireForm::Gpu, kEdgeMapUnit);
				const VpSkeleton::SkeletonMemo::Registration r =
					demand->RegisterOwnerFill(VpSkeleton::kEdgeMap, VpSkeleton::AcquireForm::Gpu, kEdgeMapUnit, MakeEdgeMapOwnerFill(want));
				const bool now = r == VpSkeleton::SkeletonMemo::Registration::Registered;
				reserve_ = now ? want : uint64_t(0);
				registered_.store(now, std::memory_order_release);
				return now;
			}
			mutable std::mutex mutex_;
			std::vector<HostSize> hosts_;
			uint64_t reserve_ = 0;   // the registered fill's reserve
			uint64_t tried_ = 0;     // the last reserve a registration was attempted with
			std::atomic<bool> registered_{ false };
		};

		// Start-up only (HubBootstrap), after the skeleton module; idempotent.
		inline void RegisterEdgeMapModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new EdgeMapModule()));
		}
	}
}
