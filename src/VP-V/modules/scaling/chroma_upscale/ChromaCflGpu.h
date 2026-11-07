// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale on the GPU: the CfL upsampler of ChromaCfl.h as libplacebo compute passes that mirror the CPU model
// step by step (grain guard, luma at the chroma sites, regression, guided-filter averaging, Catmull-Rom upsample with the
// clamp and the grain add-back; bars by the plain bar-side Catmull-Rom).
//
// Source layouts: plane 0 = luma (one component, mapped to Y); chroma either plane 1 with two components (CB and CR in any
// order) or planes 1 and 2 with one component each. The chroma siting is read from the chroma plane's shift_x / shift_y. The
// output frame is the source with 2 planes: the luma plane unchanged and a full-size two-component CbCr plane with zero
// shifts, so libplacebo must not (and will not) upsample again.
//
// Passes (all compute, nothing read back, no atomics, no barriers):
//   CROP    luma rect, chroma rect(s) into own textures, bit-exact (only when the active picture is not the whole plane)
//   SPLIT   repair.grain_split x3 (luma, Cb, Cr)           (GrainSplitGpu)
//   LO      chroma grid: tent luma at the sites; clean chroma; chroma grain          -> rgba32f (Ylo, CcB, CcR), rg32f (GcB, GcR)
//   STATS   chroma grid: regression, rho2, neighbour range                            -> rgba32f (aB, bB, aR, bR), rg32f, rgba32f
//   AVG     chroma grid: box average of a and b                                      -> rgba32f
//   UP      every luma pixel: Catmull-Rom + CfL (picture) / bar-side Catmull-Rom (bars) -> the output texture
//
// Off: the output is the source frame itself and no pass runs. On any failure Apply returns false with LastError and the
// output untouched: the caller renders the original frame and shows "Chroma upscale: libplacebo (<reason>)".
//
// Memory (SA7): every texture between the passes is a per-frame temporary (the module keeps no state from frame to frame
// but its output). With Params::arena the six chroma-grid temporaries (lo, gc, ab, rho, lohi, avg) and the own grain
// splits' intermediates come from the shared scratch arena (null = private textures, byte-identical to before). An arena
// shortage is never silent: Apply fails for that frame (ScratchShort(), "scratch memory short (...)"), the caller shows the
// libplacebo fallback line, and the arena's FrameDecision() is the catalog entry "scratch_arena"; the next frame retries.
#pragma once

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/scaling/chroma_upscale/ChromaCfl.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/repair/grain_split/GrainNode.h>
#include <VP-V/modules/repair/grain_split/GrainSplitGpu.h>

#include <libplacebo/renderer.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ChromaUpscale
{
	namespace Gpu
	{
		// Scratch-arena scope of one Apply (one render stage = one scope; GpuAllocator.h) and the owner name its temporaries
		// are requested under. The own grain splits open their own scope (GrainSplit::Gpu::kArenaScope) BEFORE this one.
		constexpr uint16_t kArenaScope = 0x0501;
		constexpr const char* kArenaOwner = "chroma_cfl";

		struct Params
		{
			Settings settings;
			int colorDepth = 10;           // codes of the signal (the model's units)
			double lumaLines = 0.0;        // the picture's luma lines (sets the grain split's Scale); 0 = the luma plane's height
			ActiveRect active;             // the active picture in luma pixels (empty = whole plane); bars get the plain upsample
			CflParams cfl;
			// The shared grain.split products (one producer, many readers): luma and the two chroma planes, each on its own
			// active-picture crop with the module's scale and block. Null or not fitting: the module computes its own (status).
			// Read through the product's one API (RegionEvidence::GrainView: GPU form per plane, Luma / Cb / Cr).
			RegionEvidence::GrainView grain;
			// The source frame the shared product must be for (GrainSplit::Gpu::Frame::seq); kAnySeq = not checked. A
			// product of another frame or of another unit version is STALE: visible fallback, recovery on the next fit.
			uint64_t grainSeq = GrainSplit::Gpu::kAnySeq;
			// The governor dropped the shared product (GrainNode::Shed()): the module uses its own split and says so (Shed).
			bool grainShed = false;
			// SA7: the render thread's scratch arena (the owner thread; BeginFrame per frame is the host's). Non-null: the chroma-grid
			// temporaries and the own splits' intermediates come from it (the module destroys its private copies). Null: private
			// textures, exactly as before.
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;
		};

		class CflModule
		{
		public:
			CflModule() = default;
			~CflModule();
			CflModule(const CflModule&) = delete;
			CflModule& operator=(const CflModule&) = delete;

			// `source` with its chroma upsampled to luma size (2 planes: luma, CbCr; shifts 0). Off, or a 4:4:4 frame:
			// `out` = `source`, no pass. False (and `out` untouched) on any failure.
			// The output frame's chroma texture belongs to the module and stays valid until the next Apply (a host that keeps
			// frames, e.g. render-ahead or frame mixing, copies it).
			bool Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& out);
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }
			// off, ok, "4:4:4: nothing to upsample", or the failure reason.
			const std::string& Status() const { return m_status; }
			// Where the grain splits of the last frame came from (recomputed every frame, so the module returns to the shared
			// products the first frame they fit again): Ring = all three planes read the shared products; Missing = at least one
			// plane had no product (absent / invalid view) and none was misfit; WrongSize = a product exists but does not fit
			// its plane (size, block, scale, code unit, component). A plane that does not fit uses its own split. Shed is the
			// node's (never set here). Off / grain guard off: Ring (nothing read, nothing to show).
			GrainSplit::GrainStatus LastGrainStatus() const { return m_grainStatus; }
			// The visible fallback ("grain split (chroma-from-luma)"): engaged while any plane runs its own grain split.
			FallbackCatalog::Decision GrainFallbackDecision() const { return GrainSplit::ReaderFallback("chroma-from-luma", m_grainStatus); }
			unsigned long long Failures() const { return m_failures; }
			// True when the LAST Apply failed because the scratch arena had no texture (Params::arena set).
			bool ScratchShort() const { return m_scratchShort; }

			// The scratch-arena requests of one Apply for an active picture of rw x rh luma pixels whose chroma crop is pcw x pch
			// (the module's working sizes; lumaLines / chromaLines = the picture's lines in each plane, as Params::lumaLines and
			// lumaLines / the vertical subsampling): scope kArenaScope, owner kArenaOwner, tier T1 (an optional upsampler: shed
			// before the T0 temporaries): four rgba32f and two rg32f pcw x pch textures; plus, when ownSplit, the three own
			// splits' intermediates (GrainSplit::Gpu::Splitter::ArenaRequests, T0; one request per distinct shape because the
			// splits run one after the other). Empty when the formats do not map to arena formats.
			static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(pl_gpu gpu, int rw, int rh, int pcw, int pch, double lumaLines,
				double chromaLines, bool ownSplit = true);

			// VRAM of the module's textures for a W x H luma plane and a cw x ch chroma plane (bytes; with the three splits').
			// tempsInArena: the chroma-grid temporaries and the splits' bands are the arena's (counted in its plan, not here):
			// 16 bytes per luma pixel and 112 per chroma pixel less. The default is the private-texture form, unchanged.
			static uint64_t TextureBytes(int W, int H, int cw, int ch, bool tempsInArena = false);

		private:
			struct Impl;
			Impl* m_impl = nullptr;
			std::string m_error, m_status = "off";
			GrainSplit::GrainStatus m_grainStatus = GrainSplit::GrainStatus::Ring;
			unsigned long long m_failures = 0;
			bool m_scratchShort = false;
		};
	}
}
