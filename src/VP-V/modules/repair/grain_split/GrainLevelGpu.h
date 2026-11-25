// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair.grain_split: the OWNER FILL of kP10gGrainLevel ("grain.level", P10g, refs/120 T3; coreplan-C10-15b-PC-grain.md:19), GPU,
// per-frame memo, get-or-compute-and-publish (SkeletonAcquire.h G3). Header-only, C++14.
//
// What it publishes: one RGBA32F texel per 16-px block of the source luma (the declared size rule, AppendGrainLevelProduct):
//   x = N_b    the block grain level (noise variance, codes^2 of D; the P10 split's blocks.x),
//   y = g      the temporal coherence gate (0: no temporal evidence),
//   z = f'^2 N the retained grain share (block mean of the Wiener share squared, f' = f + g (1 - f) = f while g = 0, times N_b),
//   w = rho1   the lag-1 temporal correlation (-1 = not measured).
// Until the temporal passes are costed (GrainLevel.h: no GPU kernel without a measured 4K cost; lead14\design-grain-chain-cost.md
// rows are ESTIMATES for every temporal step) this fill runs the SINGLE-FRAME law, which equals the unit-4 law exactly
// (GrainLevel.h: "Fallback is the unit-4 law bit for bit"; blocks.N is already N_unit4). It says so visibly on the Fallbacks route
// (StatusKind::Fallbacks -> GetFallbacksStatus: Ctrl+I, Config status, Ctrl+Alt+D; never log-only): "grain_level_temporal" once a
// reader has had a fill, "grain_level_declined" while the most recent fill declined (with the last reason and the cumulative
// count; released when a fill publishes again), and
// "grain_level_unregistered" / "grain_split_owner_unregistered" while a size has been seen but a registration is missing.
//
// T5 GATES (lead A20-CB, review-cb-p10g-r1 SHOULD-3; open until the T5 run closes them, no 102b row of their own):
//   1. NOT COMPILED: this file and tests/GrainLevelFillTests.cpp were written CODE ONLY; the T5 build is their first compile.
//   2. GPU PARITY: the fill's texels vs GrainSplit's CPU blocks.N and the CPU f^2 block mean (GPU tier, VP_TEST_GPU=hardware).
//   3. 4K COST: the single-frame pass's measured 4K time on the laptop (and the 3090 when available).
//   4. refRenderMs: Products.h AppendGrainLevelProduct's 0.05 ms is a PLACEHOLDER; replace it with gate 3's measured cost.
//
// Inputs (the producer chain, never a private source): the frame's luma facts from P13 (SourcePlanes::FactsOf: size, bit
// encoding) and the P10 grain split of the FULL luma at scale 1.0 through the memo's variant Acquire (GrainSplitVariantOwner.h),
// read ONLY through the unit's one read accessor (ViewGlsl / BindView). Device, dispatch and the output texture come ONLY from
// OwnerFillRequest::gpu (L-CORE R2): nothing here holds a device or a texture across frames.
//
// Reserve: DERIVED (rule 6): the declared size rule at the LARGEST source luma size the Entrance image shows on a live host
// (as EdgeMapFill.h); the module re-registers the fill when that size changes. The nested P10 variant has its own reserve
// (VariantOwner::ReserveFor).
//
// Declines (visible, OwnerDeclined with the cause): no GPU context, no source facts, an RGB source (no luma plane), the P10
// variant refused or not fitting, the texture refused / over the reserve, shader or dispatch failed.
//
// The module also registers the P10 variant owner's fill in production (it had no production registration: every variant
// Acquire was a visible NoOwnerFill; this also means TD's P10 variant Acquires are now served). Both registrations go through
// the skeleton's external demand (SkeletonModule::Query answers whenever the skeleton module is registered, on or off), so nothing
// is registered before the skeleton module is, and nothing in the Original pathway.
// MERGE NOTE (review-cb-p10g-r1 SHOULD-1): 15b A1 is the SINGLE P10 owner registration. Until it lands this module registers
// it; an AlreadyRegistered answer (A1, or anyone, registered first) is benign: the P10 owner is served by that registration
// and nothing is shown. When A1 lands, delete the split registration here (splitOwner_, splitTried_, the RegisterThrough call).
// The module-wide RegisterOwnerFill already is the atomic "has an owner fill" check (it answers AlreadyRegistered under the
// skeleton's owner-fill lock, before any memo is touched); a separate HasOwnerFill pre-check would need a memo (MemoFor
// creates one) and could race with A1's registration, so there is none.
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>
#include <VP-V/modules/repair/grain_split/GrainSplitVariantOwner.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/dispatch.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GrainSplit
{
	namespace LevelGpu
	{
		namespace RE = RendererExtension;
		// The hub and the skeleton's external demand live in Vpv (ExtensionHub.h, SkeletonFacts.h); this namespace is outside it.
		namespace Hub = Vpv::Hub;
		namespace Skeleton = Vpv::Skeleton;
		constexpr const char* kOwnerUnit = Gpu::kOwnerUnit;                // the catalog's kP10gGrainLevel owner (ProductCatalog.h)
		constexpr const char* kFallbackId = "grain_level_temporal";        // the Fallbacks id (refs/120 fallback visibility)
		constexpr int kGroupEdge = 16;                                       // workgroup edge: one workgroup per block

		// Reserve per fill, DERIVED from the source (rule 6): kP10gGrainLevel's DECLARED size rule at the source luma size. 0 = no size.
		inline uint64_t ReserveBytes(VpSkeleton::Dims luma)
		{
			if (luma.width == 0 || luma.height == 0)
				return 0;
			std::vector<VpSkeleton::ProductDecl> decls;
			VpSkeleton::AppendGrainLevelProduct(decls);
			return decls.size() > size_t(VpSkeleton::kP10gGrainLevel) && decls[VpSkeleton::kP10gGrainLevel].id == VpSkeleton::kP10gGrainLevel
				? VpSkeleton::BytesOf(decls[VpSkeleton::kP10gGrainLevel].size, luma) : uint64_t(0);
		}

		// Ahead-planner reserve for the nested P10 variant owner (S2-M4), DERIVED from VariantOwner::ReserveFor at scale 1.0
		// (ContextShapesOf, default 10-bit variant key: the exact formula VariantOwner::MakeOwnerFill uses). 0 = no valid size.
		inline uint64_t SplitReserveBytes(VpSkeleton::Dims luma)
		{
			constexpr uint32_t kMaxDim = static_cast<uint32_t>(INT_MAX);   // domain bound of VariantOwner::MakeOwnerFill(int, int)
			if (luma.width == 0 || luma.height == 0 || luma.width > kMaxDim || luma.height > kMaxDim)
				return 0;
			return Gpu::VariantOwner::ReserveFor(
				Gpu::VariantFor(static_cast<int>(luma.width), static_cast<int>(luma.height), 1.0, 10));
		}

		// The signal's colour depth from the frame's bit encoding (the P10 variant key's code-unit axis): color_depth, else
		// sample_depth, else 10 (the house default of VariantFor).
		inline int ColorDepthOf(const pl_bit_encoding& bits)
		{
			if (bits.color_depth > 0)
				return bits.color_depth;
			if (bits.sample_depth > 0)
				return bits.sample_depth;
			return 10;
		}

		// What the module shows (status / Fallbacks): fills that published, fills that declined, the last decline, and whether the
		// most recent fill declined (lastDeclined: set by Decline, cleared by NoteFill; the declined line is shown only while set,
		// so a transient decline releases once a fill publishes again; review-cb-p10g-r2 SHOULD-1).
		struct Counters
		{
			std::atomic<uint64_t> fills{ 0 };
			std::atomic<uint64_t> declines{ 0 };
			std::atomic<bool> lastDeclined{ false };
			mutable std::mutex mutex;
			std::string lastDecline;
			std::string LastDecline() const { std::lock_guard<std::mutex> g(mutex); return lastDecline; }
		};

		// The pass: one workgroup per block. Each invocation sums f^2 of its pixels of the block (gs_fetch .z = the Wiener share),
		// the group reduces in shared memory and invocation 0 writes the block texel. GV_B / GV_W / GV_H are the frame's block edge
		// and size (defines, so the text is fixed per geometry).
		inline std::string HeaderGlsl(const Gpu::Frame& f)
		{
			return Gpu::ViewGlsl(f.storage, f.codeUnit) + "#define GV_B " + std::to_string(f.B) + "\n#define GV_W " + std::to_string(f.w) +
				"\n#define GV_H " + std::to_string(f.h) + "\nshared vec2 gv_sum[256];\n";
		}
		constexpr const char* kBodyGlsl = R"GLSL(
ivec2 gv_blk = ivec2(gl_WorkGroupID.xy);
ivec2 gv_org = gv_blk * GV_B;
uint gv_li = gl_LocalInvocationIndex;
vec2 gv_acc = vec2(0.0);
for (int y = int(gl_LocalInvocationID.y); y < GV_B; y += 16)
    for (int x = int(gl_LocalInvocationID.x); x < GV_B; x += 16)
    {
        ivec2 p = gv_org + ivec2(x, y);
        if (p.x < GV_W && p.y < GV_H)
        {
            float f = gs_fetch(p).z;
            gv_acc += vec2(f * f, 1.0);
        }
    }
gv_sum[gv_li] = gv_acc;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u)
{
    if (gv_li < s)
        gv_sum[gv_li] += gv_sum[gv_li + s];
    barrier();
}
if (gv_li == 0u)
{
    vec2 t = gv_sum[0];
    float n = gs_blocks(gv_blk).x;
    float mf2 = t.y > 0.0 ? t.x / t.y : 0.0;
    imageStore(gv_out, gv_blk, vec4(n, 0.0, mf2 * n, -1.0));
}
)GLSL";

		inline bool Decline(VpSkeleton::OwnerFillOutput& out, Counters* c, const std::string& why)
		{
			out.declineReason = "grain level: " + why;
			if (c)
			{
				c->declines.fetch_add(1, std::memory_order_relaxed);
				{
					std::lock_guard<std::mutex> g(c->mutex);
					c->lastDecline = out.declineReason;
				}
				c->lastDeclined.store(true, std::memory_order_release);
			}
			return false;
		}

		// A fill published: counted, and the declined line releases (the cumulative count and the last reason are kept).
		inline void NoteFill(Counters* c)
		{
			if (c == nullptr)
				return;
			c->fills.fetch_add(1, std::memory_order_relaxed);
			c->lastDeclined.store(false, std::memory_order_release);
		}

		inline VpSkeleton::OwnerFill MakeOwnerFill(uint64_t reserveBytes, std::shared_ptr<Counters> counters)
		{
			VpSkeleton::OwnerFill f;
			f.variantAxes = 0;
			f.reserveBytes = reserveBytes;
			f.frameScoped = true;
			f.jobClass = VpSkeleton::JobClass::Derived;   // LaneSpareWork.h StaticClassOf: Derived, never a job
			f.fill = [counters](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				Counters* c = counters.get();
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
					return Decline(out, c, "no GPU context");
				// The frame's luma facts (size, bit encoding) from P13: the same plane the P10 variant fill splits.
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);
				const Vpv::Capture::SourcePlaneSet* facts = Vpv::Capture::SourcePlanes::FactsOf(src);
				if (!src.Ok() || facts == nullptr || src.gpu.resource == nullptr)
					return Decline(out, c, "no source plane for the frame (P13: " + memo.LastCause(VpSkeleton::kP13SourceRing) + ")");
				if (facts->rgb)
					return Decline(out, c, "RGB source (no luma plane to split)");
				const pl_tex plane = static_cast<pl_tex>(src.gpu.resource);
				const int w = plane->params.w, h = plane->params.h;
				const int depth = ColorDepthOf(facts->repr.bits);
				const int B = Gpu::BlockFor(1.0);
				// The P10 split of this frame's full luma at scale 1.0 (variant Acquire: the owner computes it once per frame and key).
				const VpSkeleton::AcquireResult split = memo.Acquire(VpSkeleton::kP10GrainSplit, req.seq, VpSkeleton::AcquireForm::Gpu,
					Gpu::VariantFor(w, h, 1.0, depth));
				if (!split.Ok())
					return Decline(out, c, std::string("grain split unavailable: ") + VpSkeleton::Token(split.cause));
				// The code unit reads the plane's texture format: only the fit test needs it, and a plane without one declines.
				if (plane->params.format == nullptr)
					return Decline(out, c, "the source plane has no texture format");
				const double cu = VpvGpu::CodeUnit(plane, facts->repr.bits, depth);
				const Gpu::Frame* gs = Gpu::FrameOf(split);
				if (Gpu::CheckFit(gs, w, h, B, 1.0, cu, 0, req.seq) != Gpu::FitResult::Fits)
					return Decline(out, c, "the grain split variant does not fit the frame's luma");
				VpSkeleton::TexShape shape;
				shape.w = uint32_t(gs->bw);
				shape.h = uint32_t(gs->bh);
				shape.fmt = VpSkeleton::TexFormat::RGBA32F;
				std::string why;
				if (!req.gpu->Allocate(shape, out.gpu, why))
					return Decline(out, c, why);
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				std::vector<pl_shader_desc> desc;
				Gpu::BindView(*gs, desc);   // THE ONE READ ACCESSOR's descriptors
				desc.push_back(VpvGpu::Storage("gv_out", dst));
				const std::string header = HeaderGlsl(*gs);
				const size_t shmem = size_t(kGroupEdge) * size_t(kGroupEdge) * 2u * sizeof(float);
				if (!VpvGpu::Pass(static_cast<pl_dispatch>(req.gpu->Dispatch()), "VP-V grain level (single-frame)", header, kBodyGlsl,
						desc.data(), int(desc.size()), nullptr, 0, gs->bw, gs->bh, shmem, kGroupEdge, kGroupEdge))
					return Decline(out, c, "shader rejected or dispatch failed");
				NoteFill(c);
				return true;
			};
			return f;
		}

		constexpr const char* kDeclinedId = "grain_level_declined";                     // Fallbacks id: a fill declined
		constexpr const char* kUnregisteredId = "grain_level_unregistered";             // Fallbacks id: no P10g owner fill of ours
		constexpr const char* kSplitUnregisteredId = "grain_split_owner_unregistered";  // Fallbacks id: the P10 owner refused

		// A registration answer in words (the Fallbacks lines quote it).
		inline std::string RegistrationText(VpSkeleton::SkeletonMemo::Registration r)
		{
			using R = VpSkeleton::SkeletonMemo::Registration;
			switch (r)
			{
			case R::Registered: return "registered";
			case R::Replaced: return "replaced";
			case R::NotOwner: return "refused: not the catalog's owning unit";
			case R::NotInCatalog: return "refused: not in the catalog";
			case R::AlreadyRegistered: return "refused: another fill is already registered";
			case R::BadDefaults: return "refused: bad defaults";
			}
			return "refused";
		}

		// The Fallbacks texts (pure: tested on the CPU). All go to StatusKind::Fallbacks, the route that reaches the screen.
		// Declines: while the most recent fill declined (`active`), the last reason and the cumulative count; released (empty) once
		// a fill publishes again (Engage/Release, FallbackStatus.h).
		inline std::string DeclineLine(uint64_t declines, const std::string& lastDecline, bool active = true)
		{
			if (declines == 0 || !active)
				return std::string();
			return std::string(kDeclinedId) + ": " + (lastDecline.empty() ? std::string("a fill declined") : lastDecline) + " (" +
				std::to_string(declines) + (declines == 1 ? " decline)" : " declines)");
		}
		// The declined line from the counters now (FallbackLines shows exactly this).
		inline std::string DeclineLineOf(const Counters& c)
		{
			return DeclineLine(c.declines.load(std::memory_order_relaxed), c.LastDecline(), c.lastDeclined.load(std::memory_order_acquire));
		}
		// A missing registration after a source size was seen (`problem` empty = none): `id` is kUnregisteredId (P10g) or
		// kSplitUnregisteredId (the P10 variant owner).
		inline std::string UnregisteredLine(const char* id, const std::string& problem)
		{
			if (problem.empty())
				return std::string();
			return std::string(id) + ": " + problem;
		}

		// The single-frame law in use: once a reader has had a fill.
		inline std::string FallbackLine(uint64_t fills)
		{
			if (fills == 0)
				return std::string();
			return std::string(kFallbackId) + ": grain level runs the single-frame law (temporal grain passes not built yet)";
		}

		// Registers, through the skeleton found by the hub (tests pass their own demand): the P10 variant owner's fill (registered
		// initially via RegisterThrough and atomically replaced via ReplaceOwnerFill when the largest observed source luma reserve
		// grows or a new session re-attaches after full detach; S2-M4) and the P10g fill with a reserve derived from the LARGEST
		// source luma size over the live hosts, observed at Entrance; the P10g fill is re-registered only when that size changes
		// (as EdgeMapModule). A host is forgotten at GpuReleased / Shutdown. Cost per frame: one short lock and a lookup in at most
		// sixteen entries.
		class GrainLevelModule : public Hub::ISkeletonModule
		{
		public:
			GrainLevelModule() : counters_(std::make_shared<Counters>()) {}
			const char* Name() const override { return "repair.grain_split.grain_level"; }
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
			void OnStatus(const RE::Context&, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*) override
			{
				if (kind != RE::StatusKind::Fallbacks)
					return;
				const std::vector<std::string> f = FallbackLines();
				lines.insert(lines.end(), f.begin(), f.end());
			}
			// The module's Fallbacks lines now (OnStatus pushes exactly these): the single-frame law once a fill ran, the declines,
			// and each missing registration. Empty when none applies.
			std::vector<std::string> FallbackLines() const
			{
				std::string level, split;
				{
					std::lock_guard<std::mutex> g(mutex_);
					level = levelProblem_;
					split = splitProblem_;
				}
				std::vector<std::string> out;
				const std::string texts[4] = { FallbackLine(counters_->fills.load(std::memory_order_relaxed)),
					DeclineLineOf(*counters_),
					UnregisteredLine(kUnregisteredId, level), UnregisteredLine(kSplitUnregisteredId, split) };
				for (const std::string& t : texts)
					if (!t.empty())
						out.push_back(t);
				return out;
			}
			// Records `host`'s source luma size and (re-)registers when the derived reserve changes. True = the P10g fill is
			// registered with the current reserve.
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
							h.bytes = ReserveBytes(luma);
							h.splitBytes = SplitReserveBytes(luma);
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
					h.bytes = ReserveBytes(luma);
					h.splitBytes = SplitReserveBytes(luma);
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
			bool SplitOwnerRegistered() const { return splitRegistered_.load(std::memory_order_acquire); }
			// The P10 owner was already registered by someone else (15b A1): benign, P10 is served by that registration.
			bool SplitOwnerServedElsewhere() const { return splitElsewhere_.load(std::memory_order_acquire); }
			uint64_t RegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return registered_.load(std::memory_order_acquire) ? reserve_ : uint64_t(0);
			}
			uint64_t SplitRegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return splitRegistered_.load(std::memory_order_acquire) ? splitReserve_ : uint64_t(0);
			}
			const Counters& Stats() const { return *counters_; }
		private:
			static constexpr size_t kMaxHosts = 16;   // the skeleton's renderer table (HostSlots)
			struct HostSize
			{
				const void* host = nullptr;
				VpSkeleton::Dims luma;
				uint64_t bytes = 0;        // ReserveBytes(luma)
				uint64_t splitBytes = 0;   // SplitReserveBytes(luma)
			};
			bool UpdateLocked(Skeleton::IExternalDemand* demand)
			{
				uint64_t want = 0;
				uint64_t wantSplit = 0;
				VpSkeleton::Dims wantSplitLuma{};
				for (const HostSize& h : hosts_)
				{
					if (h.bytes > want)
						want = h.bytes;
					if (h.splitBytes > wantSplit)
					{
						wantSplit = h.splitBytes;
						wantSplitLuma = h.luma;
					}
				}
				const bool ok = registered_.load(std::memory_order_acquire);
				const bool splitOk = splitRegistered_.load(std::memory_order_acquire);
				const bool levelDone = (ok && want == reserve_) || want == tried_;
				if (want == 0)
				{
					// No live size: nothing is owed. Reset tried_ and splitTriedReserve_ so a refused size is retried (and shown
					// again) when it returns (review-cb-p10g-r2 MUST-1); an active registration stays short-circuited on exact
					// match via ok && want == reserve_ (and splitReserve_ for P10), while splitDetached_ lets a new session at a
					// different resolution re-baseline P10's ahead-planner reserve cleanly (S2-M4).
					levelProblem_.clear();
					tried_ = 0;
					splitTriedReserve_ = 0;
					if (splitOk)
					{
						splitProblem_.clear();
						splitDetached_ = true;
					}
					return ok;
				}
				// P10 variant owner ahead-planner reserve tracking (S2-M4):
				//   - initial registration (!splitTried_): atomic RegisterThrough (preserves AlreadyRegistered for 15b A1);
				//   - when owned by us (splitOk): atomic ReplaceOwnerFill whenever wantSplit grows above splitReserve_
				//     (or differs from splitReserve_ on the first size after a full detach), skipping repeated retries of an
				//     already-refused reserve until the requested size changes or detaches.
				if (splitOk && wantSplit > 0 && !splitDetached_ && wantSplit <= splitReserve_)
				{
					splitProblem_.clear();
					splitTriedReserve_ = splitReserve_;
				}
				else if (splitOk && wantSplit > 0 && splitDetached_ && wantSplit == splitReserve_)
				{
					splitProblem_.clear();
					splitTriedReserve_ = splitReserve_;
					splitDetached_ = false;
				}
				const bool splitNeedInit = !splitTried_ && wantSplit > 0;
				const bool splitNeedReplace = splitOk && wantSplit > 0
					&& (wantSplit > splitReserve_ || (splitDetached_ && wantSplit != splitReserve_))
					&& wantSplit != splitTriedReserve_;
				const bool splitNeedWork = splitNeedInit || splitNeedReplace;
				if (levelDone && !splitNeedWork)
					return ok;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
				{
					// No skeleton yet: retried at the next Entrance, shown meanwhile (a size has been seen).
					if (!levelDone)
						levelProblem_ = "no skeleton to register with (the skeleton module is not registered yet); retried at the next frame";
					if (splitNeedWork && levelDone)
						splitProblem_ = "no skeleton to register with (the skeleton module is not registered yet); retried at the next frame";
					return ok;
				}
				if (splitNeedInit)
				{
					splitTried_ = true;   // initial P10 owner registration: an initial refusal / AlreadyRegistered is not retried
					using R = VpSkeleton::SkeletonMemo::Registration;
					const R rs = splitOwner_.RegisterThrough(*demand, static_cast<int>(wantSplitLuma.width), static_cast<int>(wantSplitLuma.height));
					const bool reg = rs == R::Registered;
					splitReserve_ = reg ? wantSplit : uint64_t(0);
					splitTriedReserve_ = reg ? wantSplit : uint64_t(0);
					splitDetached_ = false;
					splitRegistered_.store(reg, std::memory_order_release);
					splitElsewhere_.store(rs == R::AlreadyRegistered, std::memory_order_release);   // benign: see MERGE NOTE
					splitProblem_ = reg || rs == R::AlreadyRegistered ? std::string()
						: "P10 variant owner " + RegistrationText(rs) + " (P10 variant Acquires stay NoOwnerFill; not retried)";
				}
				else if (splitNeedReplace)
				{
					// S2-M4: atomic ReplaceOwnerFill keeps the ahead-planner reserve tracking the largest observed luma
					// with zero unregistered gap. A failed replace preserves the active registration and reports visibly.
					splitTriedReserve_ = wantSplit;
					using R = VpSkeleton::SkeletonMemo::Registration;
					const R rs = demand->ReplaceOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, kOwnerUnit,
						splitOwner_.MakeOwnerFill(static_cast<int>(wantSplitLuma.width), static_cast<int>(wantSplitLuma.height)));
					if (rs == R::Replaced || rs == R::Registered)
					{
						splitReserve_ = wantSplit;
						splitDetached_ = false;
						splitProblem_.clear();
					}
					else
					{
						splitProblem_ = "P10 variant owner " + RegistrationText(rs)
							+ " (reserve " + std::to_string(wantSplit) + " B; retried at the next size change)";
					}
				}
				if (levelDone)
					return ok;
				tried_ = want;   // a refused registration is retried at the next size change, not every frame
				if (ok)
					demand->UnregisterOwnerFill(VpSkeleton::kP10gGrainLevel, VpSkeleton::AcquireForm::Gpu, kOwnerUnit);
				const VpSkeleton::SkeletonMemo::Registration r =
					demand->RegisterOwnerFill(VpSkeleton::kP10gGrainLevel, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, MakeOwnerFill(want, counters_));
				const bool now = r == VpSkeleton::SkeletonMemo::Registration::Registered;
				reserve_ = now ? want : uint64_t(0);
				registered_.store(now, std::memory_order_release);
				levelProblem_ = now ? std::string()
					: "P10g owner fill " + RegistrationText(r) + " (reserve " + std::to_string(want) + " B; retried at the next size change)";
				return now;
			}
			mutable std::mutex mutex_;
			std::vector<HostSize> hosts_;
			Gpu::VariantOwner splitOwner_;   // the P10 variant owner (outlives its registration: the module lives for the process)
			std::shared_ptr<Counters> counters_;
			uint64_t reserve_ = 0;
			uint64_t tried_ = 0;
			uint64_t splitReserve_ = 0;        // ahead-planner reserveBytes of our registered kP10GrainSplit fill
			uint64_t splitTriedReserve_ = 0;   // last kP10GrainSplit reserve attempted (avoids per-frame retry on refused replace)
			bool splitTried_ = false;
			bool splitDetached_ = false;       // set when all hosts detach (want == 0) so the next session can re-baseline splitReserve_
			std::string levelProblem_;   // the P10g registration's missing state in words (empty = none); guarded by mutex_
			std::string splitProblem_;   // the P10 owner's refusal in words (empty = registered or served elsewhere); guarded by mutex_
			std::atomic<bool> registered_{ false };
			std::atomic<bool> splitRegistered_{ false };
			std::atomic<bool> splitElsewhere_{ false };
		};

		// Start-up only (HubBootstrap), after the skeleton module and the P13 source planes; idempotent.
		inline void RegisterGrainLevelModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new GrainLevelModule()));
		}
	}
}
