// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G1: the GPU kernels of the VP-owned lane products P10 (grain split), P11 (region block statistics) and P12
// (light level), as seen by the skeleton's render-thread dispatcher (modules/control/skeleton/gpu/
// SkeletonGpuDispatcher.h). One kernel object per product; the dispatcher owns the device side (its own pl_dispatch,
// the product textures, the tap plane) and calls a kernel's Step(i) for i = 0..Steps()-1, in order, for both the lane
// form (one step per lane chunk) and the live form (all steps at once): the same shaders, defines, uniforms and inputs,
// so lane == live by construction.
//
// The common input is the TAP plane: the lane SourceFrameView luma (10-bit studio codes, msbShift 6) uploaded as an
// R16 unorm texture of code << 6, so a texel is code * 64 / 65535 and floor(texel * 65535 + 0.5) / 64 recovers the code
// exactly; and the EXTENDED plane: the tap with the bars outside the picture rect replaced by the picture mirrored about
// its edge (RegionEvidence::ExtendPicture, same law, R16 unorm, exact copies of tap codes). Provenance: kTapProvenance,
// unit kTapFormUnitVersion (LaneGpuJobs.h).

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/input/source_analysis/lane/LaneGpuJobs.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace VpLaneProducts
{
	// The texture backend of the dispatcher's product textures: PlTexBackend, optionally host-readable (tests only).
	struct GpuTexBackend : VpSkeleton::PlTexBackend
	{
		explicit GpuTexBackend(pl_gpu g = nullptr, bool readable = false) : VpSkeleton::PlTexBackend(g), hostReadable(readable) {}
		Handle Create(const VpSkeleton::TexShape& s)
		{
			const pl_fmt fmt = FindFormat(gpu, s.fmt);
			if (!fmt)
				return nullptr;
			pl_tex_params tp{};
			tp.w = int(s.w);
			tp.h = int(s.h);
			tp.format = fmt;
			tp.sampleable = true;
			tp.storable = true;
			tp.host_readable = hostReadable;
			tp.blit_dst = hostReadable;
			return pl_tex_create(gpu, &tp);
		}
		bool hostReadable;
	};
	using GpuProductTextures = VpSkeleton::ProductTextures<GpuTexBackend>;

	// The picture rect of the full plane from product P4 (its rect is on the half plane, end exclusive); the whole frame
	// when P4 is absent, unreadable, or found no rect. CPU reference law: the lane and live forms both use it.
	inline RegionEvidence::Rect PictureFromStats(const uint8_t* p4, size_t bytes, int w, int h)
	{
		RegionEvidence::Rect r;
		r.left = 0;
		r.top = 0;
		r.right = w;
		r.bottom = h;
		SourceStatsRecord rec;
		if (p4 == nullptr || !ReadSourceStats(p4, bytes, &rec) || rec.rectFound == 0)
			return r;
		RegionEvidence::Rect q;
		q.left = int(rec.rectX0) * 2;
		q.top = int(rec.rectY0) * 2;
		q.right = int(rec.rectX1) * 2;
		q.bottom = int(rec.rectY1) * 2;
		return RegionEvidence::PictureOrFrame(q, w, h);
	}

	// One input of a kernel step: the input product's textures at the slot the job pinned (ring slot or live buffer),
	// or its CPU bytes (P4). `missing`: no valid content this frame (the kernel uses its declared missing-input form).
	struct GpuInput
	{
		VpSkeleton::ProductId id = VpSkeleton::kNoProduct;
		const GpuProductTextures* tex = nullptr;
		int ringSlot = -1;
		int live = -1;
		const uint8_t* cpu = nullptr;
		size_t cpuBytes = 0;
		bool missing = true;
		pl_tex Term(size_t t) const
		{
			if (missing || tex == nullptr)
				return nullptr;
			return live >= 0 ? tex->Live(size_t(live), t) : (ringSlot >= 0 ? tex->Ring(size_t(ringSlot), t) : nullptr);
		}
	};

	// What a kernel step sees. `out` = the target slot's textures in layout term order. `own` = the product's own
	// textures (P12 reads its previous frame through ownPrev*). The tap planes are the frame's (null when the kernel
	// does not need them). Render thread only.
	struct KernelContext
	{
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		VpSkeleton::ProductId id = VpSkeleton::kNoProduct;
		uint64_t seq = 0;
		VpSkeleton::Dims dims;
		bool live = false;
		pl_tex tap = nullptr;
		pl_tex extended = nullptr;
		RegionEvidence::Rect picture;
		std::array<pl_tex, 8> out{};
		size_t outCount = 0;
		std::array<GpuInput, VpSkeleton::kMaxInputs> in{};
		size_t inCount = 0;
		uint32_t missing = 0;          // bit k: input k missing
		const GpuProductTextures* own = nullptr;
		int ownPrevRingSlot = -1;      // the product's own frame seq-1 (history products), -1 = none
		int ownPrevLive = -1;
		pl_timer timer = nullptr;      // the dispatcher's timer for this product (may be null)
		std::string error;             // set by a failing step

		const GpuInput* Input(VpSkeleton::ProductId p) const
		{
			for (size_t k = 0; k < inCount; ++k)
				if (in[k].id == p)
					return &in[k];
			return nullptr;
		}
		pl_tex OwnPrev(size_t term) const
		{
			if (own == nullptr)
				return nullptr;
			return ownPrevLive >= 0 ? own->Live(size_t(ownPrevLive), term)
			                        : (ownPrevRingSlot >= 0 ? own->Ring(size_t(ownPrevRingSlot), term) : nullptr);
		}
	};

	// One product's kernel. All calls on the render thread, with the device the dispatcher was built on.
	class IProductKernel
	{
	public:
		virtual ~IProductKernel() {}
		virtual VpSkeleton::ProductId Product() const = 0;
		// The product's texture layout at `dims`: must match its declaration (ProductTextures refuses otherwise, visibly).
		virtual VpSkeleton::ProductTexLayout Layout(VpSkeleton::Dims dims) const = 0;
		// The kernel's own passes (the dispatcher adds P10's tap bands + extend in front: StepsFor in LaneGpuJobs.h).
		virtual uint32_t Steps(VpSkeleton::Dims dims) const = 0;
		// Whether the kernel reads the tap / extended planes (the dispatcher makes them for the frame first).
		virtual bool NeedsTap() const = 0;
		// Settings-generation or size change (render thread, GPU-legal phase): build LUTs, pre-warm shaders. False =
		// the product is unavailable (LastError, shown).
		virtual bool Prepare(pl_gpu gpu, pl_dispatch dispatch, VpSkeleton::Dims dims) { (void)gpu; (void)dispatch; (void)dims; return true; }
		// Record pass `step` (0..Steps()-1) of frame c.seq. Deterministic; false (c.error set) on failure.
		virtual bool Step(KernelContext& c, uint32_t step) = 0;
		virtual void Release(pl_gpu gpu) = 0;
		virtual std::string LastError() const { return std::string(); }
		// Private video memory the kernel holds outside the product textures (working textures the VRAM governor does not
		// see) at `dims`; `laneForm`: the lane form (a second working set) is wanted too. The live form's set is always counted.
		virtual uint64_t PrivateBytes(VpSkeleton::Dims dims, bool laneForm) const { (void)dims; (void)laneForm; return 0; }
		// The kernel's visible downgrades (storage refused, reduced forms, ...).
		virtual std::vector<FallbackCatalog::Decision> Fallbacks() const { return std::vector<FallbackCatalog::Decision>(); }
	};

	// ---- the tap and extend passes (G1) -------------------------------------------------------------------------------

	// Uploads rows [y0, y1) of `view` into the R16 unorm `tap` (code << msbShift). `scratch` is reused between bands.
	bool UploadTapRows(pl_gpu gpu, pl_tex tap, const VpSkeleton::SourceFrameView& view, uint32_t y0, uint32_t y1,
		std::vector<uint16_t>& scratch);

	// The extend pass: extended(x, y) = tap(ReflectInto(x, l, r), ReflectInto(y, t, b)) for the clipped picture rect
	// (RegionEvidence::ExtendPicture). One compute dispatch on `dispatch`.
	bool ExtendTap(pl_dispatch dispatch, pl_tex tap, pl_tex extended, const RegionEvidence::Rect& picture, pl_timer timer = nullptr);

	// The GLSL of the extend pass (pinned by the tests).
	const char* ExtendBodyGlsl();
}
