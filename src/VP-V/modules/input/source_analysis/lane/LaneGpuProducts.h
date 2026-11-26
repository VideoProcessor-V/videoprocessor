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
// The common input is the TAP plane: the lane SourceFrameView luma (the Q6 analysis axis: 10-bit studio codes at msbShift 6, a 12-bit
// DV tap at msbShift 4, both MsbAlign to the same 16-bit lane scale; codeUnit 64 / 65535 for every tap) uploaded as an
// R16 unorm texture of the MsbAlign value (the Q6 axis value), so a texel is Q6 / 65535 and floor(texel * 65535 + 0.5) recovers
// Q6 exactly: / 64 is the 10-bit code (a 10-bit tap, an integer), while a 12-bit DV tap keeps two fraction bits (Q6 = code12 * 16);
// and the EXTENDED plane: the tap with the bars outside the picture rect replaced by the picture mirrored about
// its edge (RegionEvidence::ExtendPicture, same law, R16 unorm, exact copies of tap codes). Provenance: kTapProvenance,
// unit kTapFormUnitVersion (LaneGpuJobs.h).

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/modules/control/skeleton/GpuProductTexBackend.h>
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

	// The product store's small textures: constants a kernel binds but does not own (stand-ins for unused inputs, a LUT).
	// The store (the dispatcher) creates and destroys them, so kernels never create textures themselves (the skeleton's
	// private-texture rule: product textures and the arena are the only creators). Render thread only.
	class IStaticTextures
	{
	public:
		virtual ~IStaticTextures() {}
		// A sampleable rgba32f texture w x h, filled with `rgba` (w * h * 4 floats), named by (owner, slot). An existing slot is
		// returned as it is unless `replace` (then it is re-created with the new data). Null on failure (LastError()).
		virtual pl_tex Rgba32f(const void* owner, int slot, int w, int h, const float* rgba, bool replace) = 0;
		// Destroys every texture of `owner` (the kernel was released); the kernel forgets its pointers.
		virtual void Forget(const void* owner) = 0;
		virtual std::string LastError() const = 0;
	};

	// One product's kernel. All calls on the render thread, with the device the dispatcher was built on.
	class IProductKernel
	{
	public:
		virtual ~IProductKernel() {}
		// The dispatcher hands every kernel its texture store when the kernel is added (before any Prepare).
		virtual void SetStaticTextures(IStaticTextures* store) { (void)store; }
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
		// W23-SK-H1: private video memory this kernel CURRENTLY holds allocated at `dims` (0 when lazy/unallocated or
		// allocated at different dimensions), so the VRAM governor charges unallocated delta on upshifts and lazy allocs.
		virtual uint64_t ActualPrivateBytes(VpSkeleton::Dims dims) const { (void)dims; return 0; }
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

	// ---- W21-4: P13 GPU tap passes (zero CPU upload) ------------------------------------------------------------------

	// Unpacks a P13 raw DV tunnel rgba8 texture directly into the R16 unorm lane tap (code12 << 4 == Q6) in one GPU
	// compute dispatch via the shared DvTunnelGlsl::kHeader (HDMI 1.4a 6.5.4). `bitShift` is the container shift
	// (DolbyVisionTunnelUnpackReference::ContainerShift: 0..4). Returns false visibly on invalid handles, format,
	// capability, dimension mismatch, aliasing, or out-of-range shift.
	bool BuildDvTapFromP13Raw(pl_dispatch dispatch, pl_tex p13RawRgba8, pl_tex tapR16, bool bigEndian, uint32_t bitShift,
		pl_timer timer = nullptr);

	// Copies or MSB-aligns an unpacked P13 R16 unorm luma plane into the R16 unorm lane tap in one GPU compute dispatch.
	// `msbShift` follows SourceFrameView::MsbAlign (`(raw << msbShift) & 0xFFFF`, valid 0..15): 0 when P13 is already
	// MSB-aligned (SourceUnpackGpu output: DV 12-bit code12 << 4 == Q6, exact 0-texel-diff identity copy), or 1..15 to
	// left-shift unshifted codes into the Q6 lane axis. Returns false visibly on invalid inputs or aliasing.
	bool BuildTapFromP13Luma(pl_dispatch dispatch, pl_tex p13LumaR16, pl_tex tapR16, uint32_t msbShift,
		pl_timer timer = nullptr);

	// The GLSL bodies of the P13 GPU tap passes (pinned by the tests).
	const char* DvTapFromP13RawBodyGlsl();
	const char* TapFromP13LumaBodyGlsl();
}
