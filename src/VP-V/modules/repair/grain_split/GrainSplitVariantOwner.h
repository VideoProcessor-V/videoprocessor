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
// The source plane comes from SourcePlaneStandIn: a VP-V-side stand-in with the shape of the iface-L-CORE P13 source-plane
// provider (Acquire(kP13SourceRing, seq, Gpu, VariantKey{plane}) -> a pl_tex view valid until AfterRender). It has no
// Bill hook and no call site in the renderer; it is DELETED at the rebase onto r2/job1-core (the fill then asks P13).
//
// Memory: the computed splits live in a small pool of Splitters; a pooled Splitter is reused only once the memo has released
// every spot that holds it (no per-frame allocation in steady state, never an overwrite under a reader). Its bytes (outputs
// and private intermediates) are reported to the memo's VRAM ledger grant (OwnerFillOutput::gpu.bytes).
#pragma once

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/colorspace.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace GrainSplit
{
	namespace Gpu
	{
		// The unit that owns kP10GrainSplit in the catalog (ProductCatalog.h); the memo refuses any other.
		constexpr const char* kOwnerUnit = "repair/grain_split";
		// Pooled Splitters at most (frames held by the memo x variants); a fill beyond it declines (visible OwnerDeclined).
		constexpr size_t kVariantPoolMax = 8;

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

		// ---- STAND-IN for the iface-L-CORE P13 source-plane provider (deleted at the rebase onto r2/job1-core) ----
		struct SourcePlaneView
		{
			pl_tex tex = nullptr;
			pl_bit_encoding bits{};
			bool Valid() const { return tex != nullptr; }
		};
		class SourcePlaneStandIn
		{
		public:
			// The source plane of frame `seq` (plane 0 = luma), valid until AfterRender.
			void Publish(uint64_t seq, uint8_t plane, pl_tex tex, const pl_bit_encoding& bits)
			{
				std::lock_guard<std::mutex> g(m_mutex);
				for (Entry& e : m_entries)
					if (e.seq == seq && e.plane == plane) { e.view.tex = tex; e.view.bits = bits; return; }
				Entry e;
				e.seq = seq; e.plane = plane; e.view.tex = tex; e.view.bits = bits;
				m_entries.push_back(e);
			}
			void AfterRender()
			{
				std::lock_guard<std::mutex> g(m_mutex);
				m_entries.clear();
			}
			// P13's shape: (seq, plane) -> the view, or an invalid view (the owner fill then declines, visibly).
			SourcePlaneView Acquire(uint64_t seq, uint8_t plane) const
			{
				std::lock_guard<std::mutex> g(m_mutex);
				for (const Entry& e : m_entries)
					if (e.seq == seq && e.plane == plane) return e.view;
				return SourcePlaneView();
			}

		private:
			struct Entry { uint64_t seq = 0; uint8_t plane = 0; SourcePlaneView view; };
			mutable std::mutex m_mutex;
			std::vector<Entry> m_entries;
		};

		// The owner of the kP10GrainSplit GPU variants. Lives on the render thread with its device; the memo and the planes
		// must outlive Register .. Unregister.
		class VariantOwner
		{
		public:
			VariantOwner(pl_gpu gpu, pl_dispatch dispatch, const SourcePlaneStandIn* planes) : m_gpu(gpu), m_dispatch(dispatch), m_planes(planes) {}
			~VariantOwner() { ReleasePool(); }
			VariantOwner(const VariantOwner&) = delete;
			VariantOwner& operator=(const VariantOwner&) = delete;

			static uint32_t ServedAxes()
			{
				return VpSkeleton::VariantKey::kAxisSize | VpSkeleton::VariantKey::kAxisScale | VpSkeleton::VariantKey::kAxisCodeUnit;
			}

			VpSkeleton::SkeletonMemo::Registration Register(VpSkeleton::SkeletonMemo& memo)
			{
				VpSkeleton::OwnerFill f;
				f.variantAxes = ServedAxes();
				f.fill = [this](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					return Fill(req, out);
				};
				return memo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Gpu, kOwnerUnit, std::move(f));
			}

			// Fills run so far (one per (frame, variant): the compute-once counter of this owner).
			uint64_t Computes() const { std::lock_guard<std::mutex> g(m_mutex); return m_computes; }
			// m_error is written by Fill outside m_mutex and returned by reference: both run on the render thread that owns
			// this object and its device (the memo calls Fill there), so no lock is needed; do not read it from another thread.
			const std::string& LastError() const { return m_error; }
			size_t PoolSize() const { std::lock_guard<std::mutex> g(m_mutex); return m_pool.size(); }
			// Frees the pooled Splitters no memo spot holds any more (all of them once the memo is cleared).
			void ReleasePool()
			{
				std::lock_guard<std::mutex> g(m_mutex);
				std::vector<std::shared_ptr<Slot>> keep;
				for (std::shared_ptr<Slot>& s : m_pool)
					if (s.use_count() > 1) keep.push_back(s);
				m_pool.swap(keep);
			}

		private:
			struct Slot
			{
				pl_gpu gpu = nullptr;
				Splitter splitter;
				~Slot() { if (gpu) splitter.Release(gpu); }
			};

			bool Fill(const VpSkeleton::OwnerFillRequest& req, VpSkeleton::OwnerFillOutput& out)
			{
				const VpSkeleton::VariantKey& v = req.variant;
				if (!m_gpu || !m_dispatch || !m_planes) { m_error = "grain split variant: no device"; return false; }
				if (v.width == 0 || v.height == 0 || v.plane != 0) { m_error = "grain split variant: a luma size is required"; return false; }
				if (!ScaleAxisServed(v.scale)) { m_error = "grain split variant: Scale not a whole number of eighths (not served)"; return false; }
				const SourcePlaneView src = m_planes->Acquire(req.seq, 0);
				if (!src.Valid()) { m_error = "grain split variant: no source plane for the frame"; return false; }
				if (static_cast<uint32_t>(src.tex->params.w) != v.width || static_cast<uint32_t>(src.tex->params.h) != v.height)
				{
					m_error = "grain split variant: the source plane is not the requested size (crop axis not served)";
					return false;
				}
				const double scale = ScaleOfAxis(v.scale);
				const int B = BlockFor(scale);
				const double cu = VpvGpu::CodeUnit(src.tex, src.bits, v.codeUnit ? v.codeUnit : 10);
				std::shared_ptr<Slot> slot = TakeSlot();
				if (!slot) { m_error = "grain split variant: pool full (frames not released)"; return false; }
				Options o;
				o.seq = req.seq;
				if (!slot->splitter.Compute(m_gpu, m_dispatch, src.tex, 0, cu, scale, B, DefaultParams(), o))
				{
					m_error = "grain split variant: " + slot->splitter.LastError();
					return false;
				}
				const Frame& f = slot->splitter.Out();
				uint64_t bytes = uint64_t(f.w) * uint64_t(f.h) * 16u + uint64_t(f.bw) * uint64_t(f.bh) * 16u;
				for (const VpSkeleton::ArenaRequest& r : Splitter::ArenaRequests(m_gpu, f.w, f.h, f.B, Storage::Full))
					bytes += VpSkeleton::ShapeBytes(r.shape) * r.count;
				out.gpu.resource = const_cast<Frame*>(&f);
				out.gpu.width = static_cast<uint32_t>(f.w);
				out.gpu.height = static_cast<uint32_t>(f.h);
				out.gpu.bytes = bytes;
				out.gpu.keepAlive = slot;
				{
					std::lock_guard<std::mutex> g(m_mutex);
					++m_computes;
				}
				return true;
			}

			// A pooled Splitter no memo spot holds (use count 1: the pool's own), else a new one up to kVariantPoolMax.
			std::shared_ptr<Slot> TakeSlot()
			{
				std::lock_guard<std::mutex> g(m_mutex);
				for (std::shared_ptr<Slot>& s : m_pool)
					if (s.use_count() == 1) return s;
				if (m_pool.size() >= kVariantPoolMax) return nullptr;
				std::shared_ptr<Slot> s = std::make_shared<Slot>();
				s->gpu = m_gpu;
				m_pool.push_back(s);
				return s;
			}

			pl_gpu m_gpu = nullptr;
			pl_dispatch m_dispatch = nullptr;
			const SourcePlaneStandIn* m_planes = nullptr;
			mutable std::mutex m_mutex;
			std::vector<std::shared_ptr<Slot>> m_pool;
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
