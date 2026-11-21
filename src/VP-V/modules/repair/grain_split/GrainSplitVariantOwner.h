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

#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

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

		// The owner of the kP10GrainSplit GPU variants. Its fill runs on the render thread (the memo's GPU fill contract) with the
		// request's GPU context; the memo must outlive Register .. Unregister.
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

			// The reserve of a variant (OwnerFill::reserveFor): every texture its fill makes, by the unit's one size law
			// (ContextShapesOf at the variant's size and block edge, the law's DefaultParams). 0 for a variant the fill refuses
			// (nothing is made for it).
			static uint64_t ReserveFor(const VpSkeleton::VariantKey& v)
			{
				if (v.width == 0 || v.height == 0 || v.plane != 0 || !ScaleAxisServed(v.scale))
					return 0;
				return ContextShapesOf(static_cast<int>(v.width), static_cast<int>(v.height), BlockFor(ScaleOfAxis(v.scale)), DefaultParams()).Bytes();
			}

			// GF-P1 MUST-5 (GV AMEND): `maxWidth` x `maxHeight` = the largest observed source luma. reserveBytes is the ahead
			// planner's figure before a fill is measured (AheadNeedLocked): left 0, the memo's need was 0, the grant was never
			// made and every Acquire failed NoLedgerGrant before a fill could run. reserveFor stays the exact per-variant reserve.
			VpSkeleton::SkeletonMemo::Registration Register(VpSkeleton::SkeletonMemo& memo, int maxWidth, int maxHeight)
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = ServedAxes();
				f.reserveBytes = maxWidth > 0 && maxHeight > 0 ? ReserveFor(VariantFor(maxWidth, maxHeight, 1.0, 10)) : uint64_t(0);
				f.reserveFor = &VariantOwner::ReserveFor;
				f.fill = [this](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& m, VpSkeleton::OwnerFillOutput& out) {
					return Fill(req, m, out);
				};
				return memo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, std::move(f));
			}

			// Fills run so far (one per (frame, variant): the compute-once counter of this owner).
			uint64_t Computes() const { std::lock_guard<std::mutex> g(m_mutex); return m_computes; }
			// The last decline's reason (also the decline reason the memo shows with OwnerDeclined).
			std::string LastError() const { std::lock_guard<std::mutex> g(m_mutex); return m_error; }

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
				const std::shared_ptr<Held> held = std::make_shared<Held>();
				Splitter splitter;   // holds no texture: every one is the context's (held->textures)
				if (!splitter.Compute(*req.gpu, plane, 0, cu, scale, B, law, req.seq, held->textures))
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

			mutable std::mutex m_mutex;
			uint64_t m_computes = 0;
			std::string m_error;
		};

		// A reader's view of an Acquired variant (null when the result holds none).
		inline const Frame* FrameOf(const VpSkeleton::AcquireResult& r)
		{
			return r.Ok() && r.gpu.resource ? static_cast<const Frame*>(r.gpu.resource) : nullptr;
		}
	}
}
