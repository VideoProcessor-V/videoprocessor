// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split: the owner fill of the shared grain split (kP10GrainSplit, GPU form) for a reader's VARIANT
// (size, scale, code unit), get-or-compute-and-publish through the skeleton memo (SkeletonAcquire.h G3). A reader whose
// plane the lane's product does not fit asks the memo; the owner computes the split ONCE per (frame, variant) and every
// further reader of that key reuses it. Readers never run a Splitter of their own (OD 11-14 04:30: "TD own split deleted").
//
// Variant axes served: size (the plane's width x height), scale (ScaleAxis: eighths of the law's Scale, 0 = 1.0) and code
// unit (the signal's colour depth in bits; the code unit itself is derived from the plane's bit encoding, CodeUnit()).
// The crop axis is NOT served on this base: a reader with an active-picture crop refuses visibly (VariantUndeclared)
// until the iface-L-CORE crop axis lands. The law is GrainSplit::DefaultParams(); the block edge is BlockFor(scale).
//
// The source plane is the P13 producer's (input/capture SourcePlanes.h): memo.Acquire(kP13SourceRing, seq, Gpu) -> the
// luma VIEW of the frame and its facts (the bit encoding). A frame P13 does not serve declines visibly, with P13's reason.
//
// Device (R2, coordinator ruling on gate C5 SET 4 item 4 MUST-2, option 1): the fill holds no device of its own. It takes the
// device, the dispatch and every texture through the request's GPU context (core/SkeletonAcquire.h OwnerGpuContext), via the
// unit's context entry (GrainSplitGpu.h Splitter::Compute(ctx, ...)). Memory: no cross-frame pool. Each fill's textures
// (outputs, intermediates, the level table) are the context's, accounted against the variant's reserve (ReserveFor: the
// unit's one size law, ContextShapesOf) and held by the memo spot (keepAlive); the skeleton's GPU device frees them when the
// memo releases the spot. Nothing here outlives the fill except that keep-alive.
#pragma once

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <libplacebo/colorspace.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GrainSplit
{
	namespace Gpu
	{
		// The unit that owns kP10GrainSplit in the catalog (ProductCatalog.h); the memo refuses any other.
		constexpr const char* kOwnerUnit = "repair/grain_split";

		// The scale axis: eighths of the law's Scale (1.0 = the default, key 0). A scale that is not a whole number of eighths
		// is not representable: the reader refuses visibly (never a silently rounded law).
		inline bool ScaleRepresentable(double scale)
		{
			const double e = scale * 8.0;
			return scale >= 1.0 && e <= 255.0 && e == std::floor(e);
		}
		// The axis value of a non-representable scale: 1 (an eighth, below every served axis: 1.0 is keyed 0, the next served
		// value is 9). VariantFor gives it, so the refusal is in the key itself, not in each caller's ScaleRepresentable check:
		// the owner's Fill refuses it visibly and never computes the default-scale law in its place.
		constexpr uint8_t kScaleAxisUnrepresentable = 1;
		inline uint8_t ScaleAxis(double scale)
		{
			if (scale == 1.0) return uint8_t(0);
			return ScaleRepresentable(scale) ? static_cast<uint8_t>(scale * 8.0) : kScaleAxisUnrepresentable;
		}
		// True for an axis value the owner serves (0 = 1.0, or 9..255 eighths).
		inline bool ScaleAxisServed(uint8_t axis) { return axis == 0 || axis > 8; }
		inline double ScaleOfAxis(uint8_t axis) { return axis == 0 ? 1.0 : axis / 8.0; }
		// The block edge of the law at a scale (Params::blockPx at <= 2160 lines, x Scale): the same law the readers use.
		inline int BlockFor(double scale) { const int b = static_cast<int>(std::floor(DefaultParams().blockPx * scale + 0.5)); return b < 1 ? 1 : b; }

		// The variant key of a luma split of w x h at `scale` for a signal of `colorDepth` bits. A scale that is not a whole
		// number of eighths >= 1 gets kScaleAxisUnrepresentable, which the owner refuses (a structural guard, rule 5).
		inline VpSkeleton::VariantKey VariantFor(int w, int h, double scale, int colorDepth)
		{
			VpSkeleton::VariantKey k;
			k.width = static_cast<uint32_t>(w);
			k.height = static_cast<uint32_t>(h);
			k.scale = ScaleAxis(scale);
			k.codeUnit = static_cast<uint8_t>(colorDepth > 0 && colorDepth < 256 ? colorDepth : 10);
			return k;
		}

		// The variant key of a chroma split (kP10cGrainSplitChroma) for chroma plane `plane` (1 = Cb, 2 = Cr; any other value
		// is rejected visibly by ChromaReserveFor / FillChroma). Negative values map to 255 so they never wrap into 1 or 2.
		inline VpSkeleton::VariantKey ChromaVariantFor(int plane, int w, int h, double scale, int colorDepth)
		{
			VpSkeleton::VariantKey k = VariantFor(w, h, scale, colorDepth);
			k.plane = static_cast<uint8_t>(plane >= 0 && plane < 256 ? plane : 255);
			return k;
		}

		// The owner of the kP10GrainSplit and kP10cGrainSplitChroma GPU variants. Its fill runs on the render thread (the memo's
		// GPU fill contract) with the request's GPU context; the memo must outlive Register .. Unregister.
		class VariantOwner
		{
		public:
			VariantOwner() = default;
			VariantOwner(const VariantOwner&) = delete;
			VariantOwner& operator=(const VariantOwner&) = delete;

			static uint32_t ServedAxes()
			{
				return VpSkeleton::VariantKey::kAxisSize | VpSkeleton::VariantKey::kAxisScale | VpSkeleton::VariantKey::kAxisCodeUnit;
			}

			static uint32_t ChromaServedAxes()
			{
				return VpSkeleton::VariantKey::kAxisPlane | VpSkeleton::VariantKey::kAxisSize
					| VpSkeleton::VariantKey::kAxisScale | VpSkeleton::VariantKey::kAxisCodeUnit;
			}

			// The reserve of a variant (OwnerFill::reserveFor): every texture its fill makes, by the unit's one size law
			// (ContextShapesOf at the variant's size and block edge, the law's DefaultParams). 0 for a variant the fill refuses
			// (nothing is made for it).
			static uint64_t ReserveFor(const VpSkeleton::VariantKey& v)
			{
				if (v.width == 0 || v.height == 0 || v.plane != 0 || !ScaleAxisServed(v.scale))
					return 0;
				return ContextShapesOf(static_cast<int>(v.width), static_cast<int>(v.height), BlockFor(ScaleOfAxis(v.scale)), DefaultParams()).Bytes();
			}

			// Reserve of a kP10cGrainSplitChroma variant: serves chroma planes 1 (Cb) and 2 (Cr) only; plane 0 or >= 3 returns 0
			// and is visibly declined by FillChroma.
			static uint64_t ChromaReserveFor(const VpSkeleton::VariantKey& v)
			{
				if (v.width == 0 || v.height == 0 || (v.plane != 1 && v.plane != 2) || !ScaleAxisServed(v.scale))
					return 0;
				return ContextShapesOf(static_cast<int>(v.width), static_cast<int>(v.height), BlockFor(ScaleOfAxis(v.scale)), DefaultParams()).Bytes();
			}

			// The owner fill as registered: Register() on a memo directly, or RegisterThrough() a module's skeleton external demand
			// (GrainSplitOwnerModule / GrainLevelModule). This owner must outlive the registration.
			// GF-P1 MUST-5 (GV AMEND): `maxWidth` x `maxHeight` = the largest observed source luma. reserveBytes is the ahead
			// planner's figure before a fill is measured (AheadNeedLocked); reserveFor stays the exact per-variant reserve.
			VpSkeleton::OwnerFill MakeOwnerFill(int maxWidth = 0, int maxHeight = 0)
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = ServedAxes();
				f.reserveBytes = maxWidth > 0 && maxHeight > 0 ? ReserveFor(VariantFor(maxWidth, maxHeight, 1.0, 10)) : uint64_t(0);
				f.reserveFor = &VariantOwner::ReserveFor;
				f.fill = [this](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& m, VpSkeleton::OwnerFillOutput& out) {
					return Fill(req, m, out);
				};
				return f;
			}

			VpSkeleton::OwnerFill MakeChromaOwnerFill(int maxChromaWidth = 0, int maxChromaHeight = 0)
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = ChromaServedAxes();
				f.reserveBytes = maxChromaWidth > 0 && maxChromaHeight > 0
					? ChromaReserveFor(ChromaVariantFor(1, maxChromaWidth, maxChromaHeight, 1.0, 10)) : uint64_t(0);
				f.reserveFor = &VariantOwner::ChromaReserveFor;
				f.fill = [this](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& m, VpSkeleton::OwnerFillOutput& out) {
					return FillChroma(req, m, out);
				};
				return f;
			}

			VpSkeleton::SkeletonMemo::Registration Register(VpSkeleton::SkeletonMemo& memo, int maxWidth = 0, int maxHeight = 0)
			{
				return memo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, MakeOwnerFill(maxWidth, maxHeight));
			}
			VpSkeleton::SkeletonMemo::Registration RegisterChroma(VpSkeleton::SkeletonMemo& memo, int maxChromaWidth = 0, int maxChromaHeight = 0)
			{
				return memo.RegisterOwnerFill(VpSkeleton::kP10cGrainSplitChroma, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, MakeChromaOwnerFill(maxChromaWidth, maxChromaHeight));
			}
			// The same registration through the skeleton's external demand (Skeleton::IExternalDemand, or any type with its
			// RegisterOwnerFill): the production path (GrainSplitOwnerModule). A template so this header needs no
			// skeleton-module include; the call stays here, in the P10/P10c owner's file (producer_chain.py attributes P10/P10c by it).
			template <class Demand>
			VpSkeleton::SkeletonMemo::Registration RegisterThrough(Demand& demand, int maxWidth = 0, int maxHeight = 0)
			{
				return demand.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, MakeOwnerFill(maxWidth, maxHeight));
			}
			template <class Demand>
			VpSkeleton::SkeletonMemo::Registration RegisterChromaThrough(Demand& demand, int maxChromaWidth = 0, int maxChromaHeight = 0)
			{
				return demand.RegisterOwnerFill(VpSkeleton::kP10cGrainSplitChroma, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, MakeChromaOwnerFill(maxChromaWidth, maxChromaHeight));
			}

			// Fills run so far (one per (frame, variant): the compute-once counters of this owner).
			uint64_t Computes() const { std::lock_guard<std::mutex> g(m_mutex); return m_computes; }
			uint64_t ChromaComputes() const { std::lock_guard<std::mutex> g(m_mutex); return m_chromaComputes; }
			// The last decline's reason (also the decline reason the memo shows with OwnerDeclined).
			std::string LastError() const { std::lock_guard<std::mutex> g(m_mutex); return m_error; }
			std::string LastChromaError() const { std::lock_guard<std::mutex> g(m_mutex); return m_chromaError; }

		private:
			// What the memo spot keeps (OwnerFillOutput::gpu.keepAlive): the published frame and the keep-alive of every texture
			// it names (the GPU device frees them when the spot goes).
			struct Held
			{
				Frame frame;
				ContextHold textures;
			};

			bool Decline(VpSkeleton::OwnerFillOutput& out, const std::string& why)
			{
				out.declineReason = why;
				std::lock_guard<std::mutex> g(m_mutex);
				m_error = why;
				return false;
			}

			bool DeclineChroma(VpSkeleton::OwnerFillOutput& out, const std::string& why)
			{
				out.declineReason = why;
				std::lock_guard<std::mutex> g(m_mutex);
				m_error = why;
				m_chromaError = why;
				return false;
			}

			static void ActiveRectForPlane(const Vpv::Capture::SourcePlaneSet& facts, pl_tex plane, int& ax0, int& ay0, int& ax1, int& ay1)
			{
				const int pw = plane ? plane->params.w : 0;
				const int ph = plane ? plane->params.h : 0;
				ax0 = 0; ay0 = 0; ax1 = pw; ay1 = ph;
				if (pw <= 0 || ph <= 0)
					return;
				const float cx0 = (std::min)(facts.crop.x0, facts.crop.x1);
				const float cx1 = (std::max)(facts.crop.x0, facts.crop.x1);
				const float cy0 = (std::min)(facts.crop.y0, facts.crop.y1);
				const float cy1 = (std::max)(facts.crop.y0, facts.crop.y1);
				if (!(cx1 > cx0) || !(cy1 > cy0))
					return;
				const int lw = (facts.tex[0] && facts.tex[0]->params.w > 0) ? facts.tex[0]->params.w : pw;
				const int lh = (facts.tex[0] && facts.tex[0]->params.h > 0) ? facts.tex[0]->params.h : ph;
				const double sx = static_cast<double>(pw) / static_cast<double>(lw);
				const double sy = static_cast<double>(ph) / static_cast<double>(lh);
				const int x0 = (std::max)(0, (std::min)(pw - 1, static_cast<int>(std::floor(static_cast<double>(cx0) * sx + 1e-6))));
				const int y0 = (std::max)(0, (std::min)(ph - 1, static_cast<int>(std::floor(static_cast<double>(cy0) * sy + 1e-6))));
				const int x1 = (std::max)(x0 + 1, (std::min)(pw, static_cast<int>(std::ceil(static_cast<double>(cx1) * sx - 1e-6))));
				const int y1 = (std::max)(y0 + 1, (std::min)(ph, static_cast<int>(std::ceil(static_cast<double>(cy1) * sy - 1e-6))));
				ax0 = x0; ay0 = y0; ax1 = x1; ay1 = y1;
			}

			bool Fill(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out)
			{
				const VpSkeleton::VariantKey& v = req.variant;
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
					return Decline(out, "grain split variant: no GPU context");
				if (v.width == 0 || v.height == 0 || v.plane != 0) return Decline(out, "grain split variant: a luma size is required");
				if (!ScaleAxisServed(v.scale)) return Decline(out, "grain split variant: Scale not a whole number of eighths (not served)");
				// The producer chain: the frame's luma plane from P13 (never a private source).
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);
				const Vpv::Capture::SourcePlaneSet* facts = Vpv::Capture::SourcePlanes::FactsOf(src);
				if (!src.Ok() || facts == nullptr || src.gpu.resource == nullptr)
					return Decline(out, "grain split variant: no source plane for the frame (P13: " + memo.LastCause(VpSkeleton::kP13SourceRing) + ")");
				const pl_tex plane = static_cast<pl_tex>(src.gpu.resource);
				if (static_cast<uint32_t>(plane->params.w) != v.width || static_cast<uint32_t>(plane->params.h) != v.height)
					return Decline(out, "grain split variant: the source plane is not the requested size (crop axis not served)");
				const double scale = ScaleOfAxis(v.scale);
				const int B = BlockFor(scale);
				// GF-P1 MUST-6 (Gemini's snippet, USE AS-IS): the law is depth-scaled to the variant's colour depth (GrainSplit.h
				// DepthScale contract), so GS_CODE_SCALE / GS_FLOOR and the luma bins follow the code unit the plane is read in.
				const int bits = v.codeUnit ? int(v.codeUnit) : DefaultParams().sourceBits;
				const double cu = VpvGpu::CodeUnit(plane, facts->repr.bits, bits);
				Params law = DefaultParams();
				law.sourceBits = bits;
				int ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
				ActiveRectForPlane(*facts, plane, ax0, ay0, ax1, ay1);
				const std::shared_ptr<Held> held = std::make_shared<Held>();
				Splitter splitter;   // holds no texture: every one is the context's (held->textures)
				if (!splitter.Compute(*req.gpu, plane, 0, cu, scale, B, law, req.seq, held->textures, ax0, ay0, ax1, ay1, false))
					return Decline(out, "grain split variant: " + splitter.LastError());
				held->frame = splitter.Out();
				out.gpu.resource = &held->frame;
				out.gpu.width = static_cast<uint32_t>(held->frame.w);
				out.gpu.height = static_cast<uint32_t>(held->frame.h);
				out.gpu.bytes = held->textures.bytes;
				out.gpu.keepAlive = held;
				{
					std::lock_guard<std::mutex> g(m_mutex);
					++m_computes;
				}
				return true;
			}

			bool FillChroma(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out)
			{
				const VpSkeleton::VariantKey& v = req.variant;
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
					return DeclineChroma(out, "grain split chroma variant: no GPU context");
				if (v.width == 0 || v.height == 0 || (v.plane != 1 && v.plane != 2))
					return DeclineChroma(out, "grain split chroma variant: chroma plane 1 (Cb) or 2 (Cr) and non-zero size are required");
				if (!ScaleAxisServed(v.scale))
					return DeclineChroma(out, "grain split chroma variant: Scale not a whole number of eighths (not served)");
				// Producer chain: source planes and encoding facts from P13 (never a private source).
				const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Gpu);
				const Vpv::Capture::SourcePlaneSet* facts = Vpv::Capture::SourcePlanes::FactsOf(src);
				if (!src.Ok() || facts == nullptr || src.gpu.resource == nullptr)
					return DeclineChroma(out, "grain split chroma variant: no source plane for the frame (P13: " + memo.LastCause(VpSkeleton::kP13SourceRing) + ")");
				if (facts->rgb)
					return DeclineChroma(out, "grain split chroma variant: RGB source has no chroma plane");
				// Resolve both planar (planes >= 3: tex[1]=Cb, tex[2]=Cr, comp 0) and semi-planar (planes == 2: tex[1]=UV, comp 0/1).
				pl_tex chromaTex = nullptr;
				int component = 0;
				if (v.plane == 1)
				{
					if (facts->planes >= 2 && facts->tex[1] != nullptr)
					{
						chromaTex = facts->tex[1];
						component = 0;
					}
				}
				else
				{
					if (facts->planes >= 3 && facts->tex[2] != nullptr)
					{
						chromaTex = facts->tex[2];
						component = 0;
					}
					else if (facts->planes == 2 && facts->tex[1] != nullptr && facts->components[1] >= 2)
					{
						chromaTex = facts->tex[1];
						component = 1;
					}
				}
				if (chromaTex == nullptr)
					return DeclineChroma(out, "grain split chroma variant: requested chroma plane absent in P13 source");
				if (chromaTex->params.format == nullptr)
					return DeclineChroma(out, "grain split chroma variant: chroma plane has no texture format");
				if (component < 0 || component >= chromaTex->params.format->num_components)
					return DeclineChroma(out, "grain split chroma variant: chroma component out of texture format range");
				if (static_cast<uint32_t>(chromaTex->params.w) != v.width || static_cast<uint32_t>(chromaTex->params.h) != v.height)
					return DeclineChroma(out, "grain split chroma variant: the source chroma plane is not the requested size (crop axis not served)");
				const double scale = ScaleOfAxis(v.scale);
				const int B = BlockFor(scale);
				const int bits = v.codeUnit ? int(v.codeUnit) : DefaultParams().sourceBits;
				const double cu = VpvGpu::CodeUnit(chromaTex, facts->repr.bits, bits);
				Params law = DefaultParams();
				law.sourceBits = bits;
				const std::shared_ptr<Held> held = std::make_shared<Held>();
				int ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
				ActiveRectForPlane(*facts, chromaTex, ax0, ay0, ax1, ay1);
				Splitter splitter;
				if (!splitter.Compute(*req.gpu, chromaTex, component, cu, scale, B, law, req.seq, held->textures, ax0, ay0, ax1, ay1, true))
					return DeclineChroma(out, "grain split chroma variant: " + splitter.LastError());
				held->frame = splitter.Out();
				out.gpu.resource = &held->frame;
				out.gpu.width = static_cast<uint32_t>(held->frame.w);
				out.gpu.height = static_cast<uint32_t>(held->frame.h);
				out.gpu.bytes = held->textures.bytes;
				out.gpu.keepAlive = held;
				{
					std::lock_guard<std::mutex> g(m_mutex);
					++m_chromaComputes;
				}
				return true;
			}

			mutable std::mutex m_mutex;
			uint64_t m_computes = 0;
			uint64_t m_chromaComputes = 0;
			std::string m_error;
			std::string m_chromaError;
		};

		// A reader's view of an Acquired variant (null when the result holds none).
		inline const Frame* FrameOf(const VpSkeleton::AcquireResult& r)
		{
			return r.Ok() && r.gpu.resource ? static_cast<const Frame*>(r.gpu.resource) : nullptr;
		}

		// Ahead-planner reserve helpers for GrainSplitOwnerModule (derived from VariantOwner::ReserveFor / ChromaReserveFor at scale 1.0).
		inline uint64_t LumaReserveBytes(VpSkeleton::Dims luma)
		{
			constexpr uint32_t kMaxDim = static_cast<uint32_t>(INT_MAX);
			if (luma.width == 0 || luma.height == 0 || luma.width > kMaxDim || luma.height > kMaxDim)
				return 0;
			return VariantOwner::ReserveFor(VariantFor(static_cast<int>(luma.width), static_cast<int>(luma.height), 1.0, 10));
		}

		inline uint64_t ChromaReserveBytes(VpSkeleton::Dims chroma)
		{
			constexpr uint32_t kMaxDim = static_cast<uint32_t>(INT_MAX);
			if (chroma.width == 0 || chroma.height == 0 || chroma.width > kMaxDim || chroma.height > kMaxDim)
				return 0;
			return VariantOwner::ChromaReserveFor(ChromaVariantFor(1, static_cast<int>(chroma.width), static_cast<int>(chroma.height), 1.0, 10));
		}

		constexpr const char* kP10UnregisteredId = "grain_split_owner_unregistered";
		constexpr const char* kP10cUnregisteredId = "grain_split_chroma_owner_unregistered";

		// 15b A1 production owner module: observes source luma + chroma dims at Entrance and registers BOTH
		// kP10GrainSplit and kP10cGrainSplitChroma owner fills through Skeleton::IExternalDemand, updating ahead-planner
		// reserves atomically via ReplaceOwnerFill on resize / post-detach re-baseline with zero unregistered gap and
		// visible StatusKind::Fallbacks reporting. Registered in HubBootstrap.cpp BEFORE RegisterGrainLevelModule().
		class GrainSplitOwnerModule : public Vpv::Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "repair.grain_split.variant_owner"; }
			void OnLifecycle(RendererExtension::LifecycleFacts& f) override
			{
				if (f.host && (f.kind == RendererExtension::Lifecycle::GpuReleased || f.kind == RendererExtension::Lifecycle::Shutdown))
					Forget(f.host);
			}
			bool OnFrame(RendererExtension::FrameContext& c) override
			{
				if (c.phase != RendererExtension::FramePhase::Entrance || c.image == nullptr || c.image->num_planes < 1
					|| c.image->planes[0].texture == nullptr || Vpv::Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					return false;
				VpSkeleton::Dims luma;
				luma.width = static_cast<uint32_t>(c.image->planes[0].texture->params.w);
				luma.height = static_cast<uint32_t>(c.image->planes[0].texture->params.h);
				VpSkeleton::Dims chroma;
				if (pl_color_system_is_ycbcr_like(c.image->repr.sys))
				{
					for (int p = 1; p < c.image->num_planes && p < 3; ++p)
					{
						const pl_tex t = c.image->planes[p].texture;
						if (t == nullptr)
							continue;
						const uint32_t tw = static_cast<uint32_t>(t->params.w);
						const uint32_t th = static_cast<uint32_t>(t->params.h);
						if (tw > chroma.width) chroma.width = tw;
						if (th > chroma.height) chroma.height = th;
					}
				}
				Observe(c.host, luma, chroma);
				return false;
			}
			void OnStatus(const RendererExtension::Context& ctx, RendererExtension::StatusKind kind, std::vector<std::string>& lines, const char*, const char*) override
			{
				if (kind != RendererExtension::StatusKind::Fallbacks || Vpv::Hub::ExtensionHub::Instance().OriginalPathway(ctx.host))
					return;
				const std::vector<std::string> f = FallbackLines();
				lines.insert(lines.end(), f.begin(), f.end());
			}
			std::vector<std::string> FallbackLines() const
			{
				std::string lumaProb, chromaProb;
				{
					std::lock_guard<std::mutex> g(mutex_);
					lumaProb = lumaSlot_.problem;
					chromaProb = chromaSlot_.problem;
				}
				std::vector<std::string> out;
				if (!lumaProb.empty())
					out.push_back(std::string(kP10UnregisteredId) + ": " + lumaProb);
				if (!chromaProb.empty())
					out.push_back(std::string(kP10cUnregisteredId) + ": " + chromaProb);
				return out;
			}
			bool Observe(const void* host, VpSkeleton::Dims luma, VpSkeleton::Dims chroma = VpSkeleton::Dims{}, Vpv::Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				bool found = false;
				for (HostSize& h : hosts_)
				{
					if (h.host == host)
					{
						if (h.luma != luma)
						{
							h.luma = luma;
							h.lumaBytes = LumaReserveBytes(luma);
						}
						if (h.chroma != chroma)
						{
							h.chroma = chroma;
							h.chromaBytes = ChromaReserveBytes(chroma);
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
					h.chroma = chroma;
					h.lumaBytes = LumaReserveBytes(luma);
					h.chromaBytes = ChromaReserveBytes(chroma);
					hosts_.push_back(h);
				}
				return UpdateLocked(demand);
			}
			void Forget(const void* host, Vpv::Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (size_t i = 0; i < hosts_.size(); ++i)
				{
					if (hosts_[i].host == host)
					{
						hosts_.erase(hosts_.begin() + static_cast<std::ptrdiff_t>(i));
						break;
					}
				}
				UpdateLocked(demand);
			}
			bool LumaRegistered() const { return lumaSlot_.registered.load(std::memory_order_acquire); }
			bool ChromaRegistered() const { return chromaSlot_.registered.load(std::memory_order_acquire); }
			bool LumaServedElsewhere() const { return lumaSlot_.elsewhere.load(std::memory_order_acquire); }
			bool ChromaServedElsewhere() const { return chromaSlot_.elsewhere.load(std::memory_order_acquire); }
			uint64_t LumaRegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return lumaSlot_.registered.load(std::memory_order_acquire) ? lumaSlot_.reserve : uint64_t(0);
			}
			uint64_t ChromaRegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return chromaSlot_.registered.load(std::memory_order_acquire) ? chromaSlot_.reserve : uint64_t(0);
			}
			const VariantOwner& Owner() const { return owner_; }

		private:
			static constexpr size_t kMaxHosts = 16;   // matches HostSlots renderer table capacity
			struct HostSize
			{
				const void* host = nullptr;
				VpSkeleton::Dims luma;
				VpSkeleton::Dims chroma;
				uint64_t lumaBytes = 0;
				uint64_t chromaBytes = 0;
			};
			struct SlotState
			{
				uint64_t reserve = 0;
				uint64_t triedReserve = 0;
				bool tried = false;
				bool detached = false;
				std::string problem;
				std::atomic<bool> registered{ false };
				std::atomic<bool> elsewhere{ false };
			};
			static std::string RegistrationText(VpSkeleton::SkeletonMemo::Registration r)
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
			bool UpdateSlotLocked(SlotState& s, VpSkeleton::ProductId id, const char* label,
				uint64_t wantBytes, VpSkeleton::Dims wantDims, bool isChroma, Vpv::Skeleton::IExternalDemand*& demand)
			{
				const bool ok = s.registered.load(std::memory_order_acquire);
				if (wantBytes == 0)
				{
					s.triedReserve = 0;
					s.problem.clear();
					if (ok)
						s.detached = true;
					else if (!s.elsewhere.load(std::memory_order_acquire))
						s.tried = false;
					return ok;
				}
				if (ok && !s.detached && wantBytes <= s.reserve)
				{
					s.problem.clear();
					s.triedReserve = s.reserve;
					return true;
				}
				if (ok && s.detached && wantBytes == s.reserve)
				{
					s.problem.clear();
					s.triedReserve = s.reserve;
					s.detached = false;
					return true;
				}
				const bool needInit = !s.tried;
				const bool needReplace = ok
					&& (wantBytes > s.reserve || (s.detached && wantBytes != s.reserve))
					&& wantBytes != s.triedReserve;
				if (!needInit && !needReplace)
					return ok || s.elsewhere.load(std::memory_order_acquire);
				if (demand == nullptr)
					demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
						Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
				{
					s.problem = "no skeleton to register with (the skeleton module is not registered yet); retried at the next frame";
					return ok || s.elsewhere.load(std::memory_order_acquire);
				}
				using R = VpSkeleton::SkeletonMemo::Registration;
				if (needInit)
				{
					s.tried = true;
					const R rs = isChroma
						? owner_.RegisterChromaThrough(*demand, static_cast<int>(wantDims.width), static_cast<int>(wantDims.height))
						: owner_.RegisterThrough(*demand, static_cast<int>(wantDims.width), static_cast<int>(wantDims.height));
					const bool reg = rs == R::Registered;
					s.reserve = reg ? wantBytes : uint64_t(0);
					s.triedReserve = reg ? wantBytes : uint64_t(0);
					s.detached = false;
					s.registered.store(reg, std::memory_order_release);
					s.elsewhere.store(rs == R::AlreadyRegistered, std::memory_order_release);
					s.problem = (reg || rs == R::AlreadyRegistered) ? std::string()
						: std::string(label) + " " + RegistrationText(rs) + " (variant Acquires stay NoOwnerFill; not retried)";
					return reg || rs == R::AlreadyRegistered;
				}
				s.triedReserve = wantBytes;
				VpSkeleton::OwnerFill fill = isChroma
					? owner_.MakeChromaOwnerFill(static_cast<int>(wantDims.width), static_cast<int>(wantDims.height))
					: owner_.MakeOwnerFill(static_cast<int>(wantDims.width), static_cast<int>(wantDims.height));
				const R rs = demand->ReplaceOwnerFill(id, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, std::move(fill));
				if (rs == R::Replaced || rs == R::Registered)
				{
					s.reserve = wantBytes;
					s.detached = false;
					s.problem.clear();
				}
				else
				{
					s.problem = std::string(label) + " " + RegistrationText(rs)
						+ " (reserve " + std::to_string(wantBytes) + " B; retried at the next size change)";
				}
				return true;
			}
			bool UpdateLocked(Vpv::Skeleton::IExternalDemand* demand)
			{
				uint64_t wantLuma = 0, wantChroma = 0;
				VpSkeleton::Dims wantLumaDims{}, wantChromaDims{};
				for (const HostSize& h : hosts_)
				{
					if (h.lumaBytes > wantLuma) { wantLuma = h.lumaBytes; wantLumaDims = h.luma; }
					if (h.chromaBytes > wantChroma) { wantChroma = h.chromaBytes; wantChromaDims = h.chroma; }
				}
				const bool lumaOk = UpdateSlotLocked(lumaSlot_, VpSkeleton::kP10GrainSplit, "P10 variant owner",
					wantLuma, wantLumaDims, false, demand);
				const bool chromaOk = UpdateSlotLocked(chromaSlot_, VpSkeleton::kP10cGrainSplitChroma, "P10c chroma variant owner",
					wantChroma, wantChromaDims, true, demand);
				return lumaOk && (wantChroma == 0 || chromaOk);
			}
			mutable std::mutex mutex_;
			std::vector<HostSize> hosts_;
			VariantOwner owner_;
			SlotState lumaSlot_;
			SlotState chromaSlot_;
		};

		// Start-up only (HubBootstrap), after the skeleton module and P13 source planes, BEFORE GrainLevelModule; idempotent.
		inline void RegisterGrainSplitOwnerModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Vpv::Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Vpv::Hub::ISkeletonModule>(new GrainSplitOwnerModule()));
		}
	}
}
