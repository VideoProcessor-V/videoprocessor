// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// input.source_analysis/regions: the OWNER FILL of kEdgeMap (lead8/iface-L-B5.md section C): per-pixel 3x3 Sobel |G| of the source
// luma, GPU, per-frame memo. Gets its device, dispatch and output texture ONLY from OwnerFillRequest::gpu (L-CORE), the source luma
// from the P13 ring view through the memo (get-or-compute-and-publish, no private upload). Header-only, C++14.
// DEFAULT variant (VariantKey{}; width / height = the source luma size): output R32F in codes at the 10-bit scale (sampled value x
// RegionEvidence::kSourceTapCodesPerUnit, the source planes' one tap form, shared with the busy share): float32 keeps a 12-bit
// source exact (rule 7), unlike the frozen r16f whose 11-bit mantissa does not.
// STAGE variant (CB slice 18 S2b, design-EDGEMAP-DOMAIN-AXIS rev 2 s2a; EdgeMapStageKey): VariantKey{stage, width x height,
// codeUnit 8, param1 = picture ordinal} on the P13 stage picture of that key (SourcePlanes::OfferStage): RG32F at the picture
// size, R = Sobel |G| and G = the measure luma, both in 8-bit reference codes of the picture's measure domain (regions/
// MeasureDomain.h), taps clamped to the whole picture (no crop axis: a reader's 3 x 3 neighbourhood at its crop border reads the
// picture beyond it, as X5's former private Sobel did). G is there because X5's grain sums (Laplacian, energy) read the same
// luma; one law, one body for both variants (BusyEdgeGlsl.h).
// Reserve: DERIVED, never a fixed resolution (rule 6): EdgeMapFillReserveBytes (kEdgeMap's declared size rule, 8 B/px) at the largest
// source luma size the Entrance image (the P13 view) shows on a live host; the module re-registers the fill when that size changes
// (EdgeMapModule::Observe).
// Declines (visible, with the cause): no GPU context, no luma view / no stage picture (P13's cause), a stage key without its size
// or not in code unit 8, a size / code unit / ordinal without a stage, texture refused / over the reserve, shader or dispatch failed.
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdgeGlsl.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapCpuReadback.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapDispatch.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomain.h>
#include <VP-V/modules/input/source_analysis/regions/MeasureDomainGlsl.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <atomic>
#include <cstddef>
#include <functional>
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

		// Reserve per fill, DERIVED from the source (rule 6): kEdgeMap's DECLARED size rule (AppendEdgeMapProduct: 8 B per source
		// pixel, the largest variant's RG32F) at the source luma size `luma`, so the ledger, the ahead cost and the fill agree on one
		// figure (66 MB at a 4K source, 16.6 MB at 1080p). 0 = no size known yet. The grant is checked BEFORE the fill; a larger frame
		// declines visibly.
		inline uint64_t EdgeMapReserveBytes(VpSkeleton::Dims luma)
		{
			if (luma.width == 0 || luma.height == 0)
				return 0;
			std::vector<VpSkeleton::ProductDecl> decls;
			VpSkeleton::AppendEdgeMapProduct(decls);
			// C3 composition: the fixed id table pads the reserved ids AFTER edge_map (32..34), so the declaration is found by its id, not taken as the last entry.
			return decls.size() > size_t(VpSkeleton::kEdgeMap) && decls[VpSkeleton::kEdgeMap].id == VpSkeleton::kEdgeMap
				? VpSkeleton::BytesOf(decls[VpSkeleton::kEdgeMap].size, luma) : uint64_t(0);
		}

		// The registered fill's reserve (CB slice 18 S2b, owner Q2; SHOULD-2): the DECLARED size itself, which is the largest variant
		// either form serves: the default R32F at the source luma size (4 B/px) or the stage variant RG32F (8 B/px) at a stage picture
		// of the source luma size (X5's measured picture; its ratio-2 half picture needs a quarter). A larger stage picture declines
		// visibly (Allocate, "...reserve"). 265 MB at an 8K source, 66 MB at 4K. The ahead budget counts it once per fill one
		// render frame holds (SkeletonMemo::AheadNeedLocked: one per stage ordinal held at once; a cadence repeat does not add).
		inline uint64_t EdgeMapFillReserveBytes(VpSkeleton::Dims luma)
		{
			return EdgeMapReserveBytes(luma);
		}

		// The stage variant (design-EDGEMAP-DOMAIN-AXIS rev 2 s2a; coreplan-CB-18 S2b): the code unit of its measure luma, 8-bit
		// reference codes (VpMeasure::MeasureLumaCode). The default variant's code unit is 10 (the source tap form).
		static constexpr uint8_t kEdgeMapDefaultCodeUnit = 10;
		static constexpr uint8_t kEdgeMapStageCodeUnit = 8;
		static constexpr uint32_t kEdgeMapAxes = VpSkeleton::VariantKey::kAxisStage | VpSkeleton::VariantKey::kAxisSize
			| VpSkeleton::VariantKey::kAxisCodeUnit | VpSkeleton::VariantKey::kAxisParam1;

		// The key of the stage variant on stage picture (stage, width x height, ordinal): the P13 stage key plus code unit 8.
		inline VpSkeleton::VariantKey EdgeMapStageKey(uint8_t stage, uint32_t width, uint32_t height, uint16_t ordinal)
		{
			VpSkeleton::VariantKey k = Capture::SourcePlanes::StageKey(stage, width, height, ordinal);
			k.codeUnit = kEdgeMapStageCodeUnit;
			return k;
		}

		// The edge-map law's dispatch (DispatchEdgeMap) and the stage variant's law on one picture (DispatchEdgeMapStage) live in
		// EdgeMapDispatch.h (shared with X5's no-memo fallback); the fills below hand them the context's dispatch, the cast itself.

		// The DEFAULT variant: Sobel |G| of the source luma (P13 ring view), R32F at the luma size, 10-bit codes.
		inline bool FillEdgeMapDefault(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo,
			VpSkeleton::OwnerFillOutput& out)
		{
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
			pl_shader_desc desc[2]{};
			desc[0].desc.name = "em_src";
			desc[0].desc.type = PL_DESC_SAMPLED_TEX;
			desc[0].binding.object = in;
			desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			desc[1].desc.name = "em_out";
			desc[1].desc.type = PL_DESC_STORAGE_IMG;
			desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc[1].binding.object = static_cast<pl_tex>(out.gpu.resource);
			const float scale = RegionEvidence::kSourceTapCodesPerUnit;   // normalised sample -> 10-bit codes (the one tap form)
			const pl_shader_var vars[1] = { { pl_var_float("em_scale"), &scale, true } };
			return DispatchEdgeMap(static_cast<pl_dispatch>(req.gpu->Dispatch()), RegionEvidence::EdgeMapHeaderGlsl(), desc, 2, vars,
				1, in->params.w, in->params.h, out.declineReason);
		}

		// The STAGE variant: Sobel |G| of the measure luma of stage picture {stage, size, ordinal} (P13 stage view), RG32F at the
		// picture size, 8-bit reference codes (R = |G|, G = the measure luma). The measure domain comes from the picture's own colour
		// facts (VpMeasure::MeasureDomainFor), so the offering stage and this fill measure in one domain. Whole texture, no crop.
		inline bool FillEdgeMapStage(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo,
			VpSkeleton::OwnerFillOutput& out)
		{
			const VpSkeleton::VariantKey& v = req.variant;
			if (v.codeUnit != kEdgeMapStageCodeUnit)
			{
				out.declineReason = "edge_map: a stage variant is served in 8-bit measure codes only (code unit 8)";
				return false;
			}
			if (v.width == 0 || v.height == 0)
			{
				out.declineReason = "edge_map: a stage variant needs its picture size";
				return false;
			}
			const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu,
				Capture::SourcePlanes::StageKey(v.stage, v.width, v.height, v.param1));
			const Capture::StagePicture* pic = Capture::SourcePlanes::FactsOfStage(src);
			if (pic == nullptr || pic->tex == nullptr || !src.gpu.Valid())
			{
				const std::string cause = memo.LastCause(VpSkeleton::kP13SourceRing);
				out.declineReason = "edge_map: no stage picture (" + (cause.empty() ? std::string("not served") : cause) + ")";
				return false;
			}
			VpSkeleton::TexShape shape;
			shape.w = uint32_t(pic->tex->params.w);
			shape.h = uint32_t(pic->tex->params.h);
			shape.fmt = VpSkeleton::TexFormat::RG32F;
			std::string why;
			if (!req.gpu->Allocate(shape, out.gpu, why))
			{
				out.declineReason = "edge_map: " + why;
				return false;
			}
			return DispatchEdgeMapStage(static_cast<pl_dispatch>(req.gpu->Dispatch()), pic->tex, pic->color, pic->system, pic->primaries,
				static_cast<pl_tex>(out.gpu.resource), out.declineReason);
		}

		inline VpSkeleton::OwnerFill MakeEdgeMapOwnerFill(uint64_t reserveBytes)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = kEdgeMapAxes;
			f.defaults.codeUnit = kEdgeMapDefaultCodeUnit;
			f.reserveBytes = reserveBytes;
			f.frameScoped = true;
			f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
				{
					out.declineReason = "edge_map: no GPU context";
					return false;
				}
				if (req.variant.stage != 0)
					return FillEdgeMapStage(req, memo, out);
				if (req.variant.Axes() != 0u)
				{
					out.declineReason = "edge_map: size, code unit and ordinal are axes of a stage variant only";
					return false;
				}
				return FillEdgeMapDefault(req, memo, out);
			};
			return f;
		}

		// CPU owner fill of kEdgeMap (S#3 / R3): non-blocking readback from the host's EdgeMapCpuReadback ring.
		// Keeps kEdgeMapCpuKeepFrames (2) frames behind the seal so lag-1 focus_map and its lag-2 delta both hit memo.
		inline VpSkeleton::OwnerFill MakeEdgeMapCpuOwnerFill(std::function<EdgeMapCpuReadback*(const void* host)> lookup)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = 0;
			f.keepFrames = kEdgeMapCpuKeepFrames;
			f.frameScoped = false;
			f.renderThreadOnly = true;
			f.fill = [lookup](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
				EdgeMapCpuReadback* rb = lookup ? lookup(req.host) : nullptr;
				if (rb == nullptr)
				{
					out.declineReason = "edge_map CPU: no readback state for host";
					return false;
				}
				return rb->PollAndTake(req.seq, out.bytes, out.pending, out.declineReason);
			};
			return f;
		}

		inline VpSkeleton::OwnerFill MakeEdgeMapCpuOwnerFill(EdgeMapCpuReadback* direct)
		{
			return MakeEdgeMapCpuOwnerFill([direct](const void*) { return direct; });
		}

		// Registers the kEdgeMap fill with the skeleton found by the hub (tests pass their own demand). The reserve is derived
		// (EdgeMapFillReserveBytes) from the LARGEST source luma size over the live hosts, observed at Entrance (the image the P13 view
		// serves, before this frame's readers run); the fill is registered on the first size and re-registered (unregister, then
		// register) only when that largest size changes, so one figure serves every host's memo (module-wide registration). A host
		// is forgotten at GpuReleased / Shutdown. Nothing is registered before a size is known (an Acquire then is a visible
		// NoOwnerFill). Cost per frame: one short lock and a lookup in at most sixteen entries.
		class EdgeMapModule : public Hub::ISkeletonModule
		{
		public:
			~EdgeMapModule() override
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (HostSize& h : hosts_)
				{
					if (h.readback)
					{
						h.readback->Release();
						h.readback->SetHostGpu(nullptr);
					}
				}
				hosts_.clear();
			}
			const char* Name() const override { return "input.source_analysis.edge_map"; }
			void OnLifecycle(RE::LifecycleFacts& f) override
			{
				if (!f.host)
					return;
				if (f.kind == RE::Lifecycle::GpuCreated)
				{
					std::lock_guard<std::mutex> g(mutex_);
					if (HostSize* h = FindOrAddHostLocked(f.host))
						h->readback->SetHostGpu(f.gpu);
				}
				else if (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown)
				{
					Forget(f.host);
				}
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
				if (c.seq >= 0 && !c.cadenceRepeat && HasCpuDemand(c.host))
				{
					Skeleton::IExternalDemand* demand =
						static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
					if (demand != nullptr)
					{
						{
							std::lock_guard<std::mutex> g(mutex_);
							RegisterCpuFillLocked(demand);
						}
						const uint64_t seq = uint64_t(c.seq);
						const Skeleton::FrameMemo fm = demand->MemoFor(c.host, seq);
						if (fm.memo != nullptr)
						{
							const VpSkeleton::AcquireResult res =
								fm.memo->Acquire(VpSkeleton::kEdgeMap, seq, VpSkeleton::AcquireForm::Gpu);
							if (res.Ok() && res.gpu.Valid() && res.gpu.resource != nullptr)
							{
								std::shared_ptr<EdgeMapCpuReadback> rb = ReadbackSharedFor(c.host);
								if (rb)
									rb->ScheduleWithHostGpu(seq, static_cast<pl_tex>(res.gpu.resource));
							}
						}
					}
				}
				return false;
			}
			// Enables or disables demand-gated kEdgeMap CPU readback for `host`. Disabling releases staging textures and withdraws VRAM.
			void SetCpuDemand(const void* host, bool active, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				if (host == nullptr)
					return;
				HostSize* h = FindOrAddHostLocked(host);
				if (h && h->readback)
					h->readback->SetDemand(active);
				if (active)
					RegisterCpuFillLocked(demand);
			}
			bool HasCpuDemand(const void* host) const
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (const HostSize& h : hosts_)
					if (h.host == host)
						return h.readback && h.readback->Demanded();
				return false;
			}
			EdgeMapCpuReadback* ReadbackFor(const void* host)
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (HostSize& h : hosts_)
					if (h.host == host)
						return h.readback.get();
				return nullptr;
			}
			// Records `host`'s source luma size and (re-)registers the fill when the derived reserve changes. True = a fill with the
			// current reserve is registered.
			bool Observe(const void* host, VpSkeleton::Dims luma, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				if (HostSize* h = FindOrAddHostLocked(host))
				{
					if (h->luma.width != luma.width || h->luma.height != luma.height)
					{
						h->luma = luma;
						h->bytes = EdgeMapFillReserveBytes(luma);   // only when the size changes (no per-frame work beyond the lookup)
					}
				}
				return UpdateLocked(demand);
			}
			// Forgets `host`; releases its CPU readback / withdraws VRAM; a smaller largest size re-registers the fill with the smaller reserve (none left: registration kept).
			void Forget(const void* host, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (size_t i = 0; i < hosts_.size(); ++i)
				{
					if (hosts_[i].host == host)
					{
						if (hosts_[i].readback)
						{
							hosts_[i].readback->Release();
							hosts_[i].readback->SetHostGpu(nullptr);
						}
						hosts_.erase(hosts_.begin() + std::ptrdiff_t(i));
						break;
					}
				}
				UpdateLocked(demand);
			}
			bool Registered() const { return registered_.load(std::memory_order_acquire); }
			bool CpuRegistered() const { return cpuRegistered_.load(std::memory_order_acquire); }
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
				uint64_t bytes = 0;   // EdgeMapFillReserveBytes(luma)
				std::shared_ptr<EdgeMapCpuReadback> readback = std::make_shared<EdgeMapCpuReadback>();
			};
			HostSize* FindOrAddHostLocked(const void* host)
			{
				if (host == nullptr)
					return nullptr;
				for (HostSize& h : hosts_)
					if (h.host == host)
						return &h;
				if (hosts_.size() >= kMaxHosts)
					return nullptr;
				HostSize h;
				h.host = host;
				hosts_.push_back(std::move(h));
				return &hosts_.back();
			}
			std::shared_ptr<EdgeMapCpuReadback> ReadbackSharedFor(const void* host) const
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (const HostSize& h : hosts_)
					if (h.host == host)
						return h.readback;
				return nullptr;
			}
			bool AnyCpuDemandLocked() const
			{
				for (const HostSize& h : hosts_)
					if (h.readback && h.readback->Demanded())
						return true;
				return false;
			}
			void RegisterCpuFillLocked(Skeleton::IExternalDemand* demand)
			{
				if (cpuRegistered_.load(std::memory_order_acquire))
					return;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
					return;
				const VpSkeleton::SkeletonMemo::Registration cr =
					demand->RegisterOwnerFill(VpSkeleton::kEdgeMap, VpSkeleton::AcquireForm::Cpu, kEdgeMapUnit,
						MakeEdgeMapCpuOwnerFill([this](const void* host) { return ReadbackFor(host); }));
				if (cr == VpSkeleton::SkeletonMemo::Registration::Registered || cr == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered)
					cpuRegistered_.store(true, std::memory_order_release);
			}
			bool UpdateLocked(Skeleton::IExternalDemand* demand)
			{
				if (AnyCpuDemandLocked())
					RegisterCpuFillLocked(demand);
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
			std::atomic<bool> cpuRegistered_{ false };
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
