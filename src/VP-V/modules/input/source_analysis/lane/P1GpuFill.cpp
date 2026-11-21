// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P1 GPU owner fill, matcher pyramid variant (BBP-B commit B2; plan lead18/coreplan-B-prereq-P2global-pyramid.md section (b)):
//   - BuildPyramidInto: THE one code copy of the matcher's half / pyramid law (today's MotionWarp::Downsample non-fade branch +
//     Pyramid), run through the caller's pass runner: the owner fill below, and the matcher's visible fallback
//     (row product.motion_warp, B3) in its own scratch textures.
//   - The OwnerFill of (kP1HalfLuma, Gpu): serves the variant { codeUnit 8, scale 1..6 } (P1GpuFill.h contract); every level
//     Acquires the level it is made from, so a level is never built twice per frame.
//   - P1GpuModule: registers that fill with the skeleton (reserve derived from the observed source luma size, like the edge map).
// Built without the precompiled header (libplacebo + VP-V headers only).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>

#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VariantShape.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace VpFrameAnalysis
{
namespace P1Gpu
{
	namespace
	{
		// The matcher's former pass names (DejudderWarp.cpp Downsample / Pyramid), unchanged.
		const char* const kHalfPassName = "VideoProcessor de-judder: half-resolution luma";
		const char* const kPyramid1PassName = "VideoProcessor de-judder: luma pyramid 1/8";
		const char* const kPyramid2PassName = "VideoProcessor de-judder: luma pyramid 1/32";
	}

	bool BuildPyramidInto(const PyramidPassRunner& run, const pl_tex_t* luma, float codes, const pl_tex_t* half, const pl_tex_t* l1,
		const pl_tex_t* l2, int from, int levels)
	{
		if (!run)
			return false;
		if (from < 0)
		{
			// Today's Downsample (non-fade): luma -> half, x codes, 2x2 law, work group 16.
			if (luma == nullptr || half == nullptr)
				return false;
			PyramidPass pass;
			pass.description = kHalfPassName;
			pass.header = VpUnit::HalfLuma::Glsl();
			pass.body = &MatcherHalfBody();
			pass.src = luma;
			pass.dst = half;
			pass.hasCodes = true;
			pass.codes = codes;
			pass.width = int(reinterpret_cast<pl_tex>(half)->params.w);
			pass.height = int(reinterpret_cast<pl_tex>(half)->params.h);
			pass.group = 16;
			if (!run(pass))
				return false;
			from = 0;
		}
		// Today's Pyramid(half, l1, l2, from, levels): the 4x4 law, work group 8, each level from the level above.
		if (from < 1 && levels >= 1)
		{
			if (half == nullptr || l1 == nullptr)
				return false;
			PyramidPass pass;
			pass.description = kPyramid1PassName;
			pass.body = &MatcherPyramidLevelBody();
			pass.src = half;
			pass.dst = l1;
			pass.width = int(reinterpret_cast<pl_tex>(l1)->params.w);
			pass.height = int(reinterpret_cast<pl_tex>(l1)->params.h);
			pass.group = 8;
			if (!run(pass))
				return false;
		}
		if (from < 2 && levels >= 2)
		{
			if (l1 == nullptr || l2 == nullptr)
				return false;
			PyramidPass pass;
			pass.description = kPyramid2PassName;
			pass.body = &MatcherPyramidLevelBody();
			pass.src = l1;
			pass.dst = l2;
			pass.width = int(reinterpret_cast<pl_tex>(l2)->params.w);
			pass.height = int(reinterpret_cast<pl_tex>(l2)->params.h);
			pass.group = 8;
			if (!run(pass))
				return false;
		}
		return true;
	}

	namespace
	{
		// One pass through libplacebo's dispatch (the matcher's Dispatch, without its test-only timing): descriptors `src`
		// (sampled, nearest, clamp) and `dst` (storage, write-only), the `codes` uniform on a 2x2 pass, one work group per
		// group x group dst texels. `why` names a failure.
		bool DispatchPass(pl_dispatch dispatch, const PyramidPass& p, std::string& why)
		{
			pl_shader_desc desc[2]{};
			desc[0].desc.name = "src";
			desc[0].desc.type = PL_DESC_SAMPLED_TEX;
			desc[0].binding.object = reinterpret_cast<pl_tex>(p.src);
			desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			desc[1].desc.name = "dst";
			desc[1].desc.type = PL_DESC_STORAGE_IMG;
			desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
			desc[1].binding.object = reinterpret_cast<pl_tex>(p.dst);
			float codes = p.codes;
			const pl_shader_var vars[1] = { { pl_var_float("codes"), &codes, true } };
			pl_custom_shader custom{};
			custom.description = p.description;
			custom.header = p.header.empty() ? nullptr : p.header.c_str();
			custom.body = p.body != nullptr ? p.body->c_str() : nullptr;
			custom.input = PL_SHADER_SIG_NONE;
			custom.output = PL_SHADER_SIG_NONE;
			custom.descriptors = desc;
			custom.num_descriptors = 2;
			custom.variables = vars;
			custom.num_variables = p.hasCodes ? 1 : 0;
			custom.compute = true;
			custom.compute_group_size[0] = p.group;
			custom.compute_group_size[1] = p.group;
			pl_shader sh = pl_dispatch_begin(dispatch);
			if (!pl_shader_custom(sh, &custom))
			{
				pl_dispatch_abort(dispatch, &sh);
				why = std::string("shader rejected: ") + p.description;
				return false;
			}
			pl_dispatch_compute_params compute{};
			compute.shader = &sh;
			compute.dispatch_size[0] = (p.width + p.group - 1) / p.group;
			compute.dispatch_size[1] = (p.height + p.group - 1) / p.group;
			compute.dispatch_size[2] = 1;
			if (!pl_dispatch_compute(dispatch, &compute))
			{
				why = std::string("dispatch failed: ") + p.description;
				return false;
			}
			return true;
		}

		// Levels 3 and 4 are the first 4x4 step (half -> 1/8, or 1/4 -> 1/16); 5 and 6 the second (1/8 -> 1/32, 1/16 -> 1/64).
		// The lowRes inner's levels 2 / 4 / 6 keep the outer's pass names, as today.
		bool IsFirstFourByFourStep(uint8_t level) { return level == 3 || level == 4; }

		VpSkeleton::OwnerFill MakeP1GpuOwnerFill(uint64_t reserveBytes)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = OwnerVariantAxes();
			f.defaults = VpSkeleton::VariantKey();   // stage 0, every axis default: codeUnit / scale stay in the canonical key
			f.reserveBytes = reserveBytes;
			f.pixelData = false;   // analysis data (block-match codes), never pixels: OD:670 (d), the R16F matcher variant
			f.keepFrames = 1;      // A = frame n - 1 stays readable while n renders (SkeletonAcquire.h keepFrames)
			f.historyFrames = 1;
			f.jobClass = VpSkeleton::JobClass::Core;
			// GF-P1 MUST-1: the matcher's consumers read two frames past the render seal (render pair B = N + 1, the EstimateAhead
			// pair C = N + 2; VectorEngineEstimate.inl offer, VectorEngine.cpp P13 / P1 acquires). Declared here, the memo-wide
			// window (DeriveMemoWindow's kAheadNeed term) computes aheadCompute >= 2 (bounded by the queue ahead), so neither is
			// refused NotPlannedAhead; beyond the queue NotQueued stays the honest answer, and the P13 views need no exemption.
			f.aheadDepth = 2;
			f.fill = [](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
				{
					out.declineReason = "p1_gpu: no GPU context";
					return false;
				}
				const uint8_t level = MatcherPyramidLevel(req.variant);
				MatcherLevelLaw law;
				if (level == 0 || !MatcherLevelLawOf(level, &law))
				{
					out.declineReason = "p1_gpu: only the matcher pyramid variant (codeUnit 8, scale 1..6) is served";
					return false;
				}
				// The source luma (P13): its size is `full` for every level (levels count halvings of the source), its plane set
				// carries the range for the range codes.
				// The offer table first (no memo entry): an Acquire of a seq not offered yet would cache its decline for the whole
				// render frame and refuse the later Entrance offer of that seq to every P13 reader of the frame.
				if (!Vpv::Capture::SourcePlanes::Find(req.host, req.seq))
				{
					out.declineReason = "p1_gpu: source luma not offered this frame";
					return false;
				}
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);
				if (!src.Ok() || !src.gpu.Valid() || src.gpu.resource == nullptr)
				{
					out.declineReason = "p1_gpu: no source luma view";
					return false;
				}
				const pl_tex lumaTex = static_cast<pl_tex>(src.gpu.resource);
				VpSkeleton::Dims full;
				full.width = uint32_t(lumaTex->params.w);
				full.height = uint32_t(lumaTex->params.h);
				pl_tex inTex = lumaTex;
				VpSkeleton::AcquireResult inLevel;   // keeps the level above alive while this one is made
				if (law.input != 0)
				{
					inLevel = memo.Acquire(VpSkeleton::kP1HalfLuma, req.seq, VpSkeleton::AcquireForm::Gpu, MatcherPyramidKey(law.input));
					if (!inLevel.Ok() || !inLevel.gpu.Valid() || inLevel.gpu.resource == nullptr)
					{
						out.declineReason = "p1_gpu: the level above is unavailable";
						return false;
					}
					inTex = static_cast<pl_tex>(inLevel.gpu.resource);
				}
				bool fullRange = false;
				if (law.rangeCodes)
				{
					const Vpv::Capture::SourcePlaneSet* facts = Vpv::Capture::SourcePlanes::FactsOf(src);
					if (facts == nullptr)
					{
						out.declineReason = "p1_gpu: no source range facts";
						return false;
					}
					fullRange = facts->fullRange;
				}
				VpSkeleton::TexShape shape;
				if (!VpSkeleton::VariantTexShape(req.variant, full, 1, MatcherStorageFormat(), &shape))
				{
					out.declineReason = "p1_gpu: no texture shape for this level";
					return false;
				}
				std::string why;
				if (!req.gpu->Allocate(shape, out.gpu, why))
				{
					out.declineReason = "p1_gpu: " + why;
					return false;
				}
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());
				std::string passError;
				const PyramidPassRunner run = [&](const PyramidPass& p) { return DispatchPass(dispatch, p, passError); };
				bool ok;
				if (law.twoByTwo)
				{
					// Level 1: luma -> half x range codes; level 2: half -> 1/4, codes 1 (the lowRes inner's lumaInCodes law).
					ok = BuildPyramidInto(run, reinterpret_cast<const pl_tex_t*>(inTex), MatcherCodes(!law.rangeCodes, fullRange),
						reinterpret_cast<const pl_tex_t*>(dst), nullptr, nullptr, -1, 0);
				}
				else if (IsFirstFourByFourStep(level))
				{
					ok = BuildPyramidInto(run, nullptr, 0.0f, reinterpret_cast<const pl_tex_t*>(inTex), reinterpret_cast<const pl_tex_t*>(dst),
						nullptr, 0, 1);
				}
				else
				{
					ok = BuildPyramidInto(run, nullptr, 0.0f, nullptr, reinterpret_cast<const pl_tex_t*>(inTex),
						reinterpret_cast<const pl_tex_t*>(dst), 1, 2);
				}
				if (!ok)
				{
					out.declineReason = "p1_gpu: " + (passError.empty() ? std::string("a pyramid pass failed") : passError);
					return false;
				}
				return true;
			};
			return f;
		}
	}

	VpSkeleton::OwnerFill MatcherPyramidOwnerFill(uint64_t reserveBytes) { return MakeP1GpuOwnerFill(reserveBytes); }
}
}

namespace Vpv
{
	namespace FrameAnalysis
	{
		namespace RE = RendererExtension;
		static constexpr const char* kP1GpuUnit = "input/source_analysis/lane";   // the catalog's kP1HalfLuma owner (ProductCatalog.h)

		// Registers the (kP1HalfLuma, Gpu) fill with the skeleton found by the hub. The reserve is DERIVED (rule 6) from the
		// LARGEST source luma size over the live hosts, observed at Entrance: one fill call allocates one level, the half (level 1)
		// is the largest, so reserve = VariantReserveBytes(level 1, luma). Registered on the first size and re-registered only when
		// that largest size changes (like EdgeMapModule); a host is forgotten at GpuReleased / Shutdown. Nothing is registered
		// before a size is known (an Acquire then is a visible NoOwnerFill). Cost per frame: one short lock and a lookup.
		class P1GpuModule;
		// The registered module (owned by the hub); null before registration and after the hub destroyed it (the destructor
		// clears it: a test reset of the hub's modules leaves no dangling pointer for NoteP1GpuSourceLuma).
		static std::atomic<P1GpuModule*>& RegisteredP1GpuModule()
		{
			static std::atomic<P1GpuModule*> module{ nullptr };
			return module;
		}

		class P1GpuModule : public Hub::ISkeletonModule
		{
		public:
			~P1GpuModule() override
			{
				P1GpuModule* self = this;
				RegisteredP1GpuModule().compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
			}
			const char* Name() const override { return "input.source_analysis.p1_gpu"; }
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
			// onlyIfUnknown: the matcher's offers (NoteP1GpuSourceLuma) only seed a host's size before its first Entrance; the
			// Entrance stays the one source of a host's size afterwards, so two notifiers can never alternate registrations.
			bool Observe(const void* host, VpSkeleton::Dims luma, Skeleton::IExternalDemand* demand = nullptr, bool onlyIfUnknown = false)
			{
				std::lock_guard<std::mutex> g(mutex_);
				bool found = false;
				for (auto& h : hosts_)
				{
					if (h.host == host)
					{
						if (!onlyIfUnknown && (h.luma.width != luma.width || h.luma.height != luma.height))
						{
							h.luma = luma;
							h.bytes = ReserveFor(luma);
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
					h.bytes = ReserveFor(luma);
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
			static uint64_t ReserveFor(VpSkeleton::Dims luma)
			{
				if (luma.width == 0 || luma.height == 0)
					return 0;
				return VpSkeleton::VariantReserveBytes(VpFrameAnalysis::P1Gpu::MatcherPyramidKey(1), luma, 1,
					VpFrameAnalysis::P1Gpu::MatcherStorageFormat());
			}
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
					demand->UnregisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu, kP1GpuUnit);
				const VpSkeleton::SkeletonMemo::Registration r = demand->RegisterOwnerFill(VpSkeleton::kP1HalfLuma,
					VpSkeleton::AcquireForm::Gpu, kP1GpuUnit, VpFrameAnalysis::P1Gpu::MakeP1GpuOwnerFill(want));
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

		// Start-up only (HubBootstrap), after the skeleton module; idempotent.
		void RegisterP1GpuModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			P1GpuModule* const module = new P1GpuModule();
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(module));
			RegisteredP1GpuModule().store(module, std::memory_order_release);
		}

		void NoteP1GpuSourceLuma(const void* host, uint32_t width, uint32_t height)
		{
			P1GpuModule* const module = RegisteredP1GpuModule().load(std::memory_order_acquire);
			if (module == nullptr || host == nullptr || width == 0 || height == 0
				|| Hub::ExtensionHub::Instance().OriginalPathway(host))
				return;
			VpSkeleton::Dims luma;
			luma.width = width;
			luma.height = height;
			module->Observe(host, luma, nullptr, true);
		}
	}
}
