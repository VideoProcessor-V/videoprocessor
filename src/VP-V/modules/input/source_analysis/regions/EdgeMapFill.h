// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the OWNER FILL of kEdgeMap (lead8/iface-L-B5.md section C): per-pixel 3x3 Sobel |G| of the source
// luma, GPU, per-frame memo. Gets its device, dispatch and output texture ONLY from OwnerFillRequest::gpu (L-CORE), the source luma
// from the P13 ring view through the memo (get-or-compute-and-publish, no private upload). Header-only, C++14.
// Variant axes: none (default variant only; width / height = the source luma size). Output R32F in codes at the 10-bit scale
// (sampled value x 1023): float32 keeps a 12-bit source exact (rule 7), unlike the frozen r16f whose 11-bit mantissa does not.
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
#include <memory>
#include <string>

namespace Vpv
{
	namespace Regions
	{
		namespace RE = RendererExtension;
		static constexpr const char* kEdgeMapUnit = "input/source_analysis/regions";   // the catalog's kEdgeMap owner

		// Reserve per fill: one R32F plane at 8K (7680 x 4320) = 132 MB; the ledger grant is checked BEFORE the fill and a larger frame declines.
		static constexpr uint64_t kEdgeMapReserveBytes = uint64_t(7680) * 4320u * 4u;

		inline VpSkeleton::OwnerFill MakeEdgeMapOwnerFill()
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = 0;
			f.reserveBytes = kEdgeMapReserveBytes;
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
				const float scale = 1023.0f;   // normalised sample -> 10-bit codes
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

		// Registers the kEdgeMap fill with the skeleton found by the hub (tests pass their own demand); idempotent.
		class EdgeMapModule : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "input.source_analysis.edge_map"; }
			bool OnFrame(RE::FrameContext& c) override
			{
				if (c.phase == RE::FramePhase::Begin)
					EnsureRegistered();
				return false;
			}
			bool EnsureRegistered(Skeleton::IExternalDemand* demand = nullptr)
			{
				if (registered_.load(std::memory_order_acquire))
					return true;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
					return false;
				const VpSkeleton::SkeletonMemo::Registration r =
					demand->RegisterOwnerFill(VpSkeleton::kEdgeMap, VpSkeleton::AcquireForm::Gpu, kEdgeMapUnit, MakeEdgeMapOwnerFill());
				const bool ok = r == VpSkeleton::SkeletonMemo::Registration::Registered
					|| r == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
				registered_.store(ok, std::memory_order_release);
				return ok;
			}
		private:
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
