// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5):
// Libplacebo Compute Dispatch Wrapper & WARP-Testable GPU Engine (FP32 end-to-end).
//   - Mirrors TdV2ReferenceEngine (TD-S3), TdV2BlendMath (TD-S1), and TdV2Chroma apply (TD-S4)
//     across 4 fused GPU compute stages ("td_v2_p1_*", "td_v2_p2_*", "td_v2_p3_*", "td_v2_p4_*")
//     plus Chroma GPU apply ("td_v2_chroma_*").
//   - Mode::Off: ZERO GPU dispatches (dispatchCount = 0), byte-identical outputY = receivedY.
//   - Shared ScratchArena (kArenaScope = 0x0402, kArenaOwner = "temporal_detail_v2", Tier::T1):
//     acquires ALL per-frame intermediate GPU scratch textures via RAII ArenaScopeGuard.
//     Any arena shortage fails visibly (scratchShort = true, StandInKind::GpuScratchShortage,
//     carry reset, succeeded = false — ZERO silent fallback to private textures!).
//   - VRAM governor declaration via MakeVramUser() under VpSkeleton::kUserTemporalDetail.
//   - Persistent GPU carryCleanTex_ (strictly grain-free — NEVER receives Layer-4 add-back),
//     reset cleanly on cut / span-invalid / resolution change / scratch shortage / Release(gpu).
//
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <VP-V/core/ScratchArena.h>
#include <VP-V/core/VramGovernor.h>
#include <VP-V/core/VramUsers.h>
#include <VP-V/modules/control/skeleton/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Shaders.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TdV2
{
namespace Gpu
{
	// Scratch-arena scope and owner name for TD v2 GPU passes (distinct from legacy v1 0x0401 / "temporal_detail").
	constexpr uint16_t kArenaScope = 0x0402;
	constexpr const char* kArenaOwner = "temporal_detail_v2";

	// Maximum simultaneous references fused on GPU: prevPeak, nextPeak, recurrent carryClean.
	constexpr int kMaxGpuRefs = 3;

	// ---- GPU Frame Process Result (extends FrameProcessResult with GPU dispatch/arena/error status) ----
	struct GpuProcessResult
	{
		TdV2Plane outputY;
		TdV2Plane outputClean;
		TdV2Plane tdDelta;
		std::vector<TdMapsView::Block> tdMaps;
		int cellsW = 0;
		int cellsH = 0;
		size_t activeRefCount = 0;
		int dispatchCount = 0;
		size_t guardViolationCells = 0;
		size_t focusVetoCells = 0;
		size_t nonFiniteCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		bool carryUsed = false;
		bool carryReset = false;
		bool scratchShort = false;
		bool succeeded = false;
		std::string lastError;
	};

	// ---- Chroma GPU Delta Apply Input / Output (for direct parity verification against TD-S4) ----
	struct ChromaGpuApplyInput
	{
		int lumaW = 0;
		int lumaH = 0;
		DeliveredChromaFormat deliveredFormat = DeliveredChromaFormat::Yuv420;
		ChromaSubsampling codedSubsampling = ChromaSubsampling::Yuv420;
		SitingSpec siting = SitingSpec::Left();
		ActiveRect activeRect;
		YCbCrMatrixSpec matrix = YCbCrMatrixSpec::Bt709();

		// YUV delivery inputs (used when deliveredFormat != Rgb444).
		TdV2Plane deliveredCb;
		TdV2Plane deliveredCr;

		// RGB delivery inputs (used when deliveredFormat == Rgb444).
		TdV2Plane deliveredR;
		TdV2Plane deliveredG;
		TdV2Plane deliveredB;

		// Signed coded-grid chroma deltas (cW x cH).
		TdV2Plane codedDeltaCb;
		TdV2Plane codedDeltaCr;
	};

	struct ChromaGpuApplyResult
	{
		TdV2Plane outCb;
		TdV2Plane outCr;
		TdV2Plane outR;
		TdV2Plane outG;
		TdV2Plane outB;
		TdV2Plane deliveredDeltaCb;
		TdV2Plane deliveredDeltaCr;
		int dispatchCount = 0;
		bool succeeded = false;
		std::string lastError;
	};

	namespace Detail
	{
		// RAII scope guard for ScratchArena: closes scope on every exit path if and only if BeginScope succeeded.
		struct ArenaScopeGuard
		{
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;
			bool opened = false;

			ArenaScopeGuard(VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* a, uint16_t scope)
				: arena(a)
			{
				if (arena != nullptr)
					opened = arena->BeginScope(scope);
			}

			~ArenaScopeGuard()
			{
				if (arena != nullptr && opened)
					arena->EndScope();
			}

			ArenaScopeGuard(const ArenaScopeGuard&) = delete;
			ArenaScopeGuard& operator=(const ArenaScopeGuard&) = delete;
		};

		inline bool EnsureTex(pl_gpu gpu, pl_tex& tex, int w, int h, pl_fmt fmt,
		                      bool sampleable, bool storable, bool hostWritable, bool hostReadable)
		{
			if (!gpu || !fmt || w <= 0 || h <= 0)
				return false;
			pl_tex_params tp{};
			tp.w = w;
			tp.h = h;
			tp.format = fmt;
			tp.sampleable = sampleable;
			tp.storable = storable;
			tp.host_writable = hostWritable;
			tp.host_readable = hostReadable;
			return pl_tex_recreate(gpu, &tex, &tp);
		}

		inline bool UploadFloats(pl_gpu gpu, pl_tex tex, const float* data)
		{
			if (!gpu || !tex || !data)
				return false;
			pl_tex_transfer_params tp{};
			tp.tex = tex;
			tp.ptr = const_cast<float*>(data);
			return pl_tex_upload(gpu, &tp);
		}

		inline bool DownloadFloats(pl_gpu gpu, pl_tex tex, float* outData)
		{
			if (!gpu || !tex || !outData)
				return false;
			pl_tex_transfer_params tp{};
			tp.tex = tex;
			tp.ptr = outData;
			return pl_tex_download(gpu, &tp);
		}

		inline bool UploadPlaneR32F(pl_gpu gpu, pl_tex tex, const TdV2Plane& plane)
		{
			if (plane.Empty())
				return false;
			std::vector<float> buf(plane.v.size());
			for (size_t i = 0; i < plane.v.size(); ++i)
				buf[i] = static_cast<float>(plane.v[i]);
			return UploadFloats(gpu, tex, buf.data());
		}

		inline bool DownloadPlaneR32F(pl_gpu gpu, pl_tex tex, int w, int h, TdV2Plane& outPlane)
		{
			if (w <= 0 || h <= 0)
				return false;
			std::vector<float> buf(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
			if (!DownloadFloats(gpu, tex, buf.data()))
				return false;
			outPlane = TdV2Plane(w, h, 0.0);
			for (size_t i = 0; i < buf.size(); ++i)
				outPlane.v[i] = static_cast<double>(buf[i]);
			return true;
		}

		inline int GroupsFor(int dim, int groupSize)
		{
			return (std::max)(1, (dim + groupSize - 1) / groupSize);
		}

		inline std::string CommonGeomDefines(int w, int h, int cw, int ch, int cellPx, const ActiveRect& ar)
		{
			std::string s;
			s += VpvGpu::DefineInt("TD_W", w);
			s += VpvGpu::DefineInt("TD_H", h);
			s += VpvGpu::DefineInt("TD_CW", cw);
			s += VpvGpu::DefineInt("TD_CH", ch);
			s += VpvGpu::DefineInt("TD_CELL_PX", cellPx);
			s += VpvGpu::DefineInt("TD_AX0", ar.x0);
			s += VpvGpu::DefineInt("TD_AY0", ar.y0);
			s += VpvGpu::DefineInt("TD_AX1", ar.x1);
			s += VpvGpu::DefineInt("TD_AY1", ar.y1);
			s += VpvGpu::Define("TD_NUM_FLOOR", kEngineNumFloor);
			return s;
		}
	} // namespace Detail

	// ---- Libplacebo Compute GPU Engine (Stage TD-S5) ----
	class TdV2GpuEngine
	{
	public:
		explicit TdV2GpuEngine(const Config& cfg = Config{})
			: cfg_(cfg)
		{
		}

		~TdV2GpuEngine() = default;
		TdV2GpuEngine(const TdV2GpuEngine&) = delete;
		TdV2GpuEngine& operator=(const TdV2GpuEngine&) = delete;

		void SetConfig(const Config& cfg) { cfg_ = cfg; }
		const Config& GetConfig() const { return cfg_; }

		void ResetCarry(pl_gpu gpu = nullptr)
		{
			hasCarry_ = false;
			carryW_ = 0;
			carryH_ = 0;
			if (gpu != nullptr && carryCleanTex_ != nullptr)
				pl_tex_destroy(gpu, &carryCleanTex_);
		}

		void Release(pl_gpu gpu)
		{
			ResetCarry(gpu);
			if (gpu != nullptr)
			{
				DestroyPrivateScratch(gpu);
				for (pl_tex* t : {
					&inRecYTex_, &inCleanYTex_, &inOwnGrainTex_, &inRefCleanTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_,
					&outYTex_, &outCleanTex_, &outDeltaTex_, &outTdMapsTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_ })
				{
					if (*t != nullptr)
						pl_tex_destroy(gpu, t);
				}
			}
			else
			{
				for (pl_tex* t : {
					&privCurBands_, &privRefBands_[0], &privRefBands_[1], &privRefBands_[2], &privBandTmpH_,
					&privWarped_[0], &privWarped_[1], &privWarped_[2],
					&privDeltaScratch_, &privCandOutScratch_,
					&privMotionGate_, &privCellWeights_, &privCellDiagC_, &privCellFocus_,
					&carryCleanTex_,
					&inRecYTex_, &inCleanYTex_, &inOwnGrainTex_, &inRefCleanTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_,
					&outYTex_, &outCleanTex_, &outDeltaTex_, &outTdMapsTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_ })
				{
					*t = nullptr;
				}
			}
		}

		bool HasCarry() const { return hasCarry_; }
		const std::string& LastError() const { return lastError_; }
		uint64_t Failures() const { return failures_; }
		int LastDispatchCount() const { return lastDispatchCount_; }
		bool ScratchShort() const { return scratchShort_; }

		// Scratch-arena requests for one active frame at w x h (scoped under kArenaScope, kArenaOwner, Tier::T1).
		// All formats are strictly FP32 (R32F / RG32F / RGBA32F) — zero FP16 moment cancellation.
		static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(
			pl_gpu gpu, int w, int h, int cellPx = kCanonicalBlockPx)
		{
			std::vector<VpSkeleton::ArenaRequest> reqs;
			if (!gpu || w <= 0 || h <= 0)
				return reqs;
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return reqs;
			}
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const uint32_t uw = static_cast<uint32_t>(w);
			const uint32_t uh = static_cast<uint32_t>(h);
			const uint32_t cw = static_cast<uint32_t>((w + effCellPx - 1) / effCellPx);
			const uint32_t ch = static_cast<uint32_t>((h + effCellPx - 1) / effCellPx);

			auto pushReq = [&](uint32_t rw, uint32_t rh, VpSkeleton::TexFormat fmt, uint8_t count)
			{
				VpSkeleton::ArenaRequest r;
				r.scope = kArenaScope;
				r.owner = kArenaOwner;
				r.tier = VpSkeleton::Tier::T1;
				r.shape = VpSkeleton::TexShape{ rw, rh, fmt };
				r.count = count;
				reqs.push_back(r);
			};

			pushReq(uw, uh, VpSkeleton::TexFormat::RGBA32F, 5);      // curBands, refBands[0..2], bandTmpH
			pushReq(uw, uh, VpSkeleton::TexFormat::RG32F, 3);        // warpedRef[0..2]
			pushReq(uw, uh, VpSkeleton::TexFormat::R32F, 2);         // deltaScratch, candOutScratch
			pushReq(3u * cw, ch, VpSkeleton::TexFormat::RGBA32F, 3); // motionGate, cellWeights, cellDiagC
			pushReq(cw, ch, VpSkeleton::TexFormat::RG32F, 1);        // cellFocus
			return reqs;
		}

		// Exact VRAM working-set estimate in bytes at w x h (persistent carry + arena scratch + IO/output textures).
		static uint64_t EstimatedVramBytes(int w, int h, int cellPx = kCanonicalBlockPx)
		{
			if (w <= 0 || h <= 0)
				return 0;
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const uint64_t px = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
			const uint64_t cw = static_cast<uint64_t>((w + effCellPx - 1) / effCellPx);
			const uint64_t ch = static_cast<uint64_t>((h + effCellPx - 1) / effCellPx);
			const uint64_t cells = cw * ch;

			const uint64_t bR32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::R32F);
			const uint64_t bRG32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RG32F);
			const uint64_t bRGBA32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RGBA32F);

			uint64_t total = 0;
			// Persistent carryCleanTex_ (1 x R32F)
			total += 1u * px * bR32;
			// Scratch set (matching ArenaRequests)
			total += 5u * px * bRGBA32;
			total += 3u * px * bRG32;
			total += 2u * px * bR32;
			total += 3u * (3u * cells) * bRGBA32;
			total += 1u * cells * bRG32;
			// IO & readback textures (7 full-res R32F + motion/prior/tdMaps/diag/guard/warpDiag cell grids)
			total += 7u * px * bR32;
			total += (3u * cells) * (bRGBA32 + bRG32);
			total += (3u * cells) * bRGBA32; // tdMaps (cw x 3*ch RGBA32F)
			total += cells * bRGBA32;        // diagFlags
			total += 3u * cells * bRG32;     // cellPrior, guardCell, warpDiag
			return total;
		}

		// VRAM governor user declaration under VpSkeleton::kUserTemporalDetail (sheddable Tier T1).
		static VpSkeleton::VramUser MakeVramUser(int w, int h, int cellPx = kCanonicalBlockPx)
		{
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserTemporalDetail;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserTemporalDetail);
			u.tier = VpSkeleton::Tier::T1;
			u.shedOrder = 0;
			u.lossless = false;
			const uint64_t bytes = EstimatedVramBytes(w, h, cellPx);
			u.levels.push_back(bytes);
			u.levelText.push_back(nullptr);
			if (bytes > 0)
			{
				u.levels.push_back(0);
				u.levelText.push_back("off (GPU memory shed)");
			}
			return u;
		}

		// Execute full TD v2 4-pass compute pipeline on `gpu` + `dispatch`.
		GpuProcessResult ProcessFrame(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const FrameProcessInput& in,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			StandInRegistry* standIns = nullptr)
		{
			GpuProcessResult res;
			lastDispatchCount_ = 0;
			scratchShort_ = false;
			lastError_.clear();

			const int w = in.receivedY.w;
			const int h = in.receivedY.h;
			res.outputY = in.receivedY;

			// Mode::Off -> ZERO GPU dispatches, byte-identical passthrough (even if gpu/dispatch/arena null).
			if (!cfg_.IsEnabled())
			{
				if (w > 0 && h > 0 && !in.receivedY.Empty())
				{
					const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
					res.cellsW = (w + cellPx - 1) / cellPx;
					res.cellsH = (h + cellPx - 1) / cellPx;
					res.tdMaps.resize(static_cast<size_t>(res.cellsW) * static_cast<size_t>(res.cellsH));
					res.tdDelta = TdV2Plane(w, h, 0.0);
					res.outputClean = in.receivedY;
				}
				res.succeeded = true;
				return res;
			}

			auto failVisible = [&](const std::string& msg, StandInKind kind) -> GpuProcessResult
			{
				lastError_ = msg;
				res.lastError = msg;
				res.succeeded = false;
				++failures_;
				if (standIns != nullptr)
					standIns->Engage(kind, msg.c_str());
				return res;
			};

			if (w <= 0 || h <= 0 || in.receivedY.Empty())
				return failVisible("TdV2GpuEngine empty receivedY input", StandInKind::GpuDispatchFailed);

			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			const int grainCellPx = cfg_.grainCellPx > 0 ? cfg_.grainCellPx : cellPx;
			res.cellsW = (w + cellPx - 1) / cellPx;
			res.cellsH = (h + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(res.cellsW) * static_cast<size_t>(res.cellsH);
			res.tdMaps.resize(numCells);
			res.tdDelta = TdV2Plane(w, h, 0.0);

			if (gpu == nullptr || dispatch == nullptr)
				return failVisible("TdV2GpuEngine null pl_gpu or pl_dispatch", StandInKind::GpuDispatchFailed);

			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0))
			{
				ResetCarry(gpu);
				res.carryReset = true;
				res.nonFiniteCells = numCells;
				return failVisible("TdV2GpuEngine invalid sourceBits", StandInKind::NonFiniteClosure);
			}

			if (cfg_.enableLongAnchor && standIns != nullptr)
				standIns->Engage(StandInKind::LongPairDrift, "enableLongAnchor requested; 1-session long pair pending");

			// Cut / invalid span or resolution change resets recurrent carry cleanly.
			if (in.isCutOrSpanInvalid || (hasCarry_ && (carryW_ != w || carryH_ != h)))
			{
				ResetCarry(gpu);
				res.carryReset = true;
			}

			// Validate receivedY finite upfront (matches TdV2ReferenceEngine fail-closed behavior).
			for (double val : in.receivedY.v)
			{
				if (!std::isfinite(val))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine non-finite receivedY");
					ResetCarry(gpu);
					res.carryReset = true;
					res.nonFiniteCells = numCells;
					res.succeeded = true;
					return res;
				}
			}

			const ActiveRect active = in.activeRect.Empty()
				? ActiveRect::Full(w, h)
				: in.activeRect.ClampedTo(w, h);

			// Layer 1: Grain-free clean & own-grain separation.
			TdV2Plane curClean(w, h, 0.0);
			TdV2Plane ownGrain(w, h, 0.0);
			if (in.hasCleanSplit && !in.cleanY.Empty() && in.cleanY.w == w && in.cleanY.h == h)
			{
				for (size_t i = 0; i < in.receivedY.v.size(); ++i)
				{
					const double cv = in.cleanY.v[i];
					if (!std::isfinite(cv))
					{
						if (standIns != nullptr)
							standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine non-finite cleanY");
						ResetCarry(gpu);
						res.carryReset = true;
						res.nonFiniteCells = numCells;
						res.succeeded = true;
						return res;
					}
					curClean.v[i] = cv;
					ownGrain.v[i] = in.receivedY.v[i] - cv;
				}
			}
			else
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator, "no upstream P10g split supplied");
				curClean = in.receivedY;
			}
			res.outputClean = curClean;

			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR32 || !fmtRG32 || !fmtRGBA32)
				return failVisible("TdV2GpuEngine required FP32 texture formats unsupported on pl_gpu", StandInKind::GpuDispatchFailed);

			// Acquire per-frame scratch textures via RAII ArenaScopeGuard (or private fallback ONLY when arena == nullptr).
			Detail::ArenaScopeGuard scopeGuard(arena, kArenaScope);
			ScratchSet sc{};
			if (arena != nullptr)
			{
				// Never hold redundant private scratch when running on a shared ScratchArena.
				DestroyPrivateScratch(gpu);

				const uint32_t uw = static_cast<uint32_t>(w);
				const uint32_t uh = static_cast<uint32_t>(h);
				const uint32_t ucw = static_cast<uint32_t>(res.cellsW);
				const uint32_t uch = static_cast<uint32_t>(res.cellsH);

				const VpSkeleton::TexShape shFullRGBA{ uw, uh, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shFullRG{ uw, uh, VpSkeleton::TexFormat::RG32F };
				const VpSkeleton::TexShape shFullR{ uw, uh, VpSkeleton::TexFormat::R32F };
				const VpSkeleton::TexShape shCell3RGBA{ 3u * ucw, uch, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shCellRG{ ucw, uch, VpSkeleton::TexFormat::RG32F };

				bool ok = scopeGuard.opened;
				if (ok)
				{
					sc.curBands = arena->Acquire(kArenaOwner, shFullRGBA);
					for (int i = 0; i < kMaxGpuRefs; ++i)
						sc.refBands[i] = arena->Acquire(kArenaOwner, shFullRGBA);
					sc.bandTmpH = arena->Acquire(kArenaOwner, shFullRGBA);
					for (int i = 0; i < kMaxGpuRefs; ++i)
						sc.warped[i] = arena->Acquire(kArenaOwner, shFullRG);
					sc.deltaScratch = arena->Acquire(kArenaOwner, shFullR);
					sc.candOutScratch = arena->Acquire(kArenaOwner, shFullR);
					sc.motionGate = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellWeights = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellDiagC = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellFocus = arena->Acquire(kArenaOwner, shCellRG);
					ok = sc.AllNonNull();
				}
				if (!ok)
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = hadCarry;
					scratchShort_ = true;
					res.scratchShort = true;
					return failVisible("GPU scratch arena shortage (frame skipped visibly)", StandInKind::GpuScratchShortage);
				}
			}
			else
			{
				if (!EnsurePrivateScratch(gpu, w, h, res.cellsW, res.cellsH, fmtR32, fmtRG32, fmtRGBA32, sc))
					return failVisible("TdV2GpuEngine private scratch texture allocation failed", StandInKind::GpuDispatchFailed);
			}

			// Ensure persistent carryCleanTex_ and IO/output staging textures.
			if (!Detail::EnsureTex(gpu, carryCleanTex_, w, h, fmtR32, true, true, true, false) ||
			    !Detail::EnsureTex(gpu, inRecYTex_, w, h, fmtR32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inCleanYTex_, w, h, fmtR32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inOwnGrainTex_, w, h, fmtR32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inRefCleanTex_, w, h, fmtR32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inMotionVecTex_, 3 * res.cellsW, res.cellsH, fmtRGBA32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inMotionMetaTex_, 3 * res.cellsW, res.cellsH, fmtRG32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, inCellPriorTex_, res.cellsW, res.cellsH, fmtRG32, true, false, true, false) ||
			    !Detail::EnsureTex(gpu, outYTex_, w, h, fmtR32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outCleanTex_, w, h, fmtR32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outDeltaTex_, w, h, fmtR32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outTdMapsTex_, res.cellsW, 3 * res.cellsH, fmtRGBA32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outDiagFlagsTex_, res.cellsW, res.cellsH, fmtRGBA32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outGuardCellTex_, res.cellsW, res.cellsH, fmtRG32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outWarpDiagTex_, res.cellsW, res.cellsH, fmtRG32, true, true, false, true))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine IO/output texture allocation failed", StandInKind::GpuDispatchFailed);
			}

			// Assemble active references (up to 3: prevPeak, nextPeak, persistent GPU carryCleanTex_).
			struct ActiveRefSpec
			{
				const TdV2Plane* cpuClean = nullptr; // null when using persistent GPU carryCleanTex_
				bool useGpuCarry = false;
				CellMotionField motion;
				bool isExactCopy = false;
			};
			std::vector<ActiveRefSpec> refSpecs;
			refSpecs.reserve(kMaxGpuRefs);

			auto tryAddRefSpec = [&](const TdV2Plane* refClean, bool useCarry, const CellMotionField* mf, bool exactCopy) -> bool
			{
				if (!useCarry)
				{
					if (refClean == nullptr || refClean->Empty() || refClean->w != w || refClean->h != h)
						return false;
				}
				else if (!hasCarry_ || carryW_ != w || carryH_ != h || carryCleanTex_ == nullptr)
				{
					return false;
				}

				ActiveRefSpec spec;
				spec.cpuClean = refClean;
				spec.useGpuCarry = useCarry;
				spec.isExactCopy = exactCopy;

				if (mf == nullptr)
				{
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "null motion field; assuming Still");
					spec.motion = CellMotionField::Uniform(res.cellsW, res.cellsH, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				}
				else if (!mf->IsWellFormed(res.cellsW, res.cellsH))
				{
					++res.motionStandInRefs;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "dimension-mismatched motion field; ref rejected");
					return false;
				}
				else
				{
					spec.motion = *mf;
				}
				refSpecs.push_back(std::move(spec));
				return true;
			};

			if (!in.isCutOrSpanInvalid)
			{
				if (in.prevPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.prevPeak.cleanPlane, false, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent);
				if (in.nextPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.nextPeak.cleanPlane, false, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent);
				if (cfg_.enableCarryForward && hasCarry_ && carryW_ == w && carryH_ == h)
				{
					if (tryAddRefSpec(nullptr, true, in.hasCarryMotion ? &in.carryMotion : nullptr, false))
						res.carryUsed = true;
				}
			}

			res.activeRefCount = refSpecs.size();

			// First frame / cut frame with no references: seed persistent GPU carryCleanTex_ directly.
			if (refSpecs.empty())
			{
				if (!Detail::UploadPlaneR32F(gpu, carryCleanTex_, curClean))
				{
					ResetCarry(gpu);
					return failVisible("TdV2GpuEngine carry seed upload failed", StandInKind::GpuDispatchFailed);
				}
				hasCarry_ = true;
				carryW_ = w;
				carryH_ = h;
				res.succeeded = true;
				return res;
			}

			// Upload current frame inputs (receivedY, curClean, ownGrain).
			if (!Detail::UploadPlaneR32F(gpu, inRecYTex_, in.receivedY) ||
			    !Detail::UploadPlaneR32F(gpu, inCleanYTex_, curClean) ||
			    !Detail::UploadPlaneR32F(gpu, inOwnGrainTex_, ownGrain))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine current frame upload failed", StandInKind::GpuDispatchFailed);
			}

			// Upload motion vectors and metadata for active references (preserving NaN/Inf for GPU shader closure).
			const int K = static_cast<int>(refSpecs.size());
			std::vector<float> motVecBuf(static_cast<size_t>(3 * res.cellsW) * static_cast<size_t>(res.cellsH) * 4u, 0.0f);
			std::vector<float> motMetaBuf(static_cast<size_t>(3 * res.cellsW) * static_cast<size_t>(res.cellsH) * 2u, 0.0f);
			for (int k = 0; k < K; ++k)
			{
				const CellMotionField& mf = refSpecs[static_cast<size_t>(k)].motion;
				for (int cy = 0; cy < res.cellsH; ++cy)
				{
					for (int cx = 0; cx < res.cellsW; ++cx)
					{
						const size_t srcIdx = static_cast<size_t>(cy) * res.cellsW + cx;
						const size_t dstTexel = static_cast<size_t>(cy) * (3 * res.cellsW) + static_cast<size_t>(k * res.cellsW + cx);
						motVecBuf[dstTexel * 4u + 0u] = static_cast<float>(mf.vx[srcIdx]);
						motVecBuf[dstTexel * 4u + 1u] = static_cast<float>(mf.vy[srcIdx]);
						motVecBuf[dstTexel * 4u + 2u] = static_cast<float>(mf.camVx[srcIdx]);
						motVecBuf[dstTexel * 4u + 3u] = static_cast<float>(mf.camVy[srcIdx]);
						motMetaBuf[dstTexel * 2u + 0u] = static_cast<float>(static_cast<int>(mf.cls[srcIdx]));
						motMetaBuf[dstTexel * 2u + 1u] = mf.valid[srcIdx] ? 1.0f : 0.0f;
					}
				}
			}
			if (!Detail::UploadFloats(gpu, inMotionVecTex_, motVecBuf.data()) ||
			    !Detail::UploadFloats(gpu, inMotionMetaTex_, motMetaBuf.data()))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine motion upload failed", StandInKind::GpuDispatchFailed);
			}

			// Upload per-cell retainedGrainVar (mapped via P10-D6 MapCellToGrainGrid; NaN/neg preserved for GPU) + explicitFocusChange.
			const int gW = (w + grainCellPx - 1) / grainCellPx;
			std::vector<float> cellPriorBuf(numCells * 2u, 0.0f);
			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					float gv = 0.0f;
					if (!in.retainedGrainVar.empty())
					{
						const int gcx = MapCellToGrainGrid(cx, cellPx, grainCellPx);
						const int gcy = MapCellToGrainGrid(cy, cellPx, grainCellPx);
						const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
						if (gIdx < in.retainedGrainVar.size())
							gv = static_cast<float>(in.retainedGrainVar[gIdx]);
					}
					float expFc = 0.0f;
					if (in.explicitFocusChangeCells != nullptr && cIdx < in.explicitFocusChangeCells->size() &&
					    (*in.explicitFocusChangeCells)[cIdx] != 0u)
					{
						expFc = 1.0f;
					}
					cellPriorBuf[cIdx * 2u + 0u] = gv;
					cellPriorBuf[cIdx * 2u + 1u] = expFc;
				}
			}
			if (!Detail::UploadFloats(gpu, inCellPriorTex_, cellPriorBuf.data()))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine cell prior upload failed", StandInKind::GpuDispatchFailed);
			}

			// Derived physics & coding step prior (exact CPU Rule-1 derivations passed to GLSL via #define).
			double effStepDelta = in.estimatedStepDelta;
			double effZeroFrac = in.zeroCoeffFraction;
			if (!in.hasStepPrior || !std::isfinite(effStepDelta) || effStepDelta <= 0.0 || !std::isfinite(effZeroFrac))
			{
				effStepDelta = DepthScale(cfg_.sourceBits);
				effZeroFrac = 0.0;
				if (standIns != nullptr)
					standIns->Engage(StandInKind::StepPriorDefault);
			}
			const RoundingCellStats a2Stats = ComputeRoundingCellStats(effStepDelta, in.deadZonePhi, effZeroFrac);
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double depthScale = DepthScale(cfg_.sourceBits);
			const double gainMD3 = gains.mid + gains.d3;
			const double stepNoiseVar = a2Stats.valid ? (std::max)(0.0, a2Stats.totalVar) : 0.0;
			const double structureFloor = (std::max)(depthScale * depthScale, 2.0 * gainMD3 * stepNoiseVar);
			const double fSharp = AnalyticalEdgeFocusRatio(0.0);
			const double fDefocus = AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);
			const double fShiftDelta = fSharp - fDefocus;

			const std::string geomDefs = Detail::CommonGeomDefines(w, h, res.cellsW, res.cellsH, cellPx, active);
			const std::string bandTapsHdr = Shaders::BuildBandTapsHeader();

			int dispatches = 0;
			auto runPass = [&](const char* desc, const std::string& hdr, const char* body,
			                   const pl_shader_desc* descs, int nDescs,
			                   int gx, int gy, int gw = 16, int gh = 16, size_t shmem = 0) -> bool
			{
				if (!VpvGpu::Pass(dispatch, desc, hdr, body, descs, nDescs, nullptr, 0, gx, gy, shmem, gw, gh))
					return false;
				++dispatches;
				return true;
			};

			const int fullGx = Detail::GroupsFor(w, 16);
			const int fullGy = Detail::GroupsFor(h, 16);
			const int cellGx = Detail::GroupsFor(res.cellsW, 8);
			const int cellGy = Detail::GroupsFor(res.cellsH, 8);

			// ---- PASS 1A: Camera-Relative Motion Gate + Karis 3x3 Silhouette Dilation ----
			{
				std::string hdr = geomDefs;
				hdr += VpvGpu::DefineInt("TD_K", K);
				hdr += VpvGpu::Define("TD_STILL_SPEED", kStillObjectSpeedPx);
				hdr += VpvGpu::Define("TD_FAST_CUTOFF", kFastObjectCutoffPx);
				hdr += Shaders::kCoreHelpers;
				hdr += Shaders::kPass1MotionGateHeader;
				pl_shader_desc d[3] = {
					VpvGpu::Sampled("motion_vec", inMotionVecTex_),
					VpvGpu::Sampled("motion_meta", inMotionMetaTex_),
					VpvGpu::Storage("motion_gate", sc.motionGate)
				};
				if (!runPass("td_v2_p1_motion_gate", hdr, Shaders::kPass1MotionGate, d, 3,
				             Detail::GroupsFor(K * res.cellsW, 16), Detail::GroupsFor(res.cellsH, 16), 16, 16))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p1_motion_gate dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// Helper: Separable H + V 4-band decomposition (U2 BandFromBase taps).
			if (standIns != nullptr)
				standIns->Engage(StandInKind::BandMemoW2);
			auto runBandDecomp = [&](pl_tex srcTex, pl_tex dstBands) -> bool
			{
				const std::string hdr = geomDefs + bandTapsHdr;
				pl_shader_desc dh[2] = {
					VpvGpu::Sampled("src_tex", srcTex),
					VpvGpu::Storage("band_tmp_h", sc.bandTmpH)
				};
				if (!runPass("td_v2_p1_band_h", hdr, Shaders::kPass1BandH, dh, 2, fullGx, fullGy))
					return false;
				pl_shader_desc dv[2] = {
					VpvGpu::Sampled("band_tmp_h", sc.bandTmpH),
					VpvGpu::Storage("bands_out", dstBands)
				};
				return runPass("td_v2_p1_band_v", hdr, Shaders::kPass1BandV, dv, 2, fullGx, fullGy);
			};

			// Decompose current frame grain-free clean into curBands (L, M, H, D3).
			if (!runBandDecomp(inCleanYTex_, sc.curBands))
			{
				ResetCarry(gpu);
				return failVisible("td_v2_p1_band current clean dispatch failed", StandInKind::GpuDispatchFailed);
			}

			// ---- PASS 1B + 1C: Warp each active reference + Decompose into refBands[k] ----
			for (int k = 0; k < K; ++k)
			{
				const ActiveRefSpec& rSpec = refSpecs[static_cast<size_t>(k)];
				pl_tex refSrcTex = carryCleanTex_;
				if (!rSpec.useGpuCarry)
				{
					if (!Detail::UploadPlaneR32F(gpu, inRefCleanTex_, *rSpec.cpuClean))
					{
						ResetCarry(gpu);
						return failVisible("TdV2GpuEngine ref clean upload failed", StandInKind::GpuDispatchFailed);
					}
					refSrcTex = inRefCleanTex_;
				}

				std::string wHdr = geomDefs;
				wHdr += VpvGpu::DefineInt("TD_REF_IDX", k);
				wHdr += Shaders::kCoreHelpers;
				wHdr += Shaders::kPass1WarpHeader;
				pl_shader_desc dw[4] = {
					VpvGpu::Sampled("ref_clean", refSrcTex),
					VpvGpu::Sampled("cur_clean", inCleanYTex_),
					VpvGpu::Sampled("motion_vec", inMotionVecTex_),
					VpvGpu::Storage("warped_out", sc.warped[k])
				};
				if (!runPass("td_v2_p1_warp", wHdr, Shaders::kPass1WarpAndGate, dw, 4, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p1_warp dispatch failed", StandInKind::GpuDispatchFailed);
				}

				if (!runBandDecomp(sc.warped[k], sc.refBands[k]))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p1_band ref dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 2A: Noise-Debiased Focus Guard + Parseval Focal-Plane Shift ----
			if (standIns != nullptr)
				standIns->Engage(StandInKind::FocusMapK32);
			{
				std::string fHdr = geomDefs;
				fHdr += VpvGpu::DefineInt("TD_K", K);
				fHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				fHdr += VpvGpu::Define("TD_GAIN_D3", gains.d3);
				fHdr += VpvGpu::Define("TD_STEP_NOISE_VAR", stepNoiseVar);
				fHdr += VpvGpu::Define("TD_STRUCTURE_FLOOR", structureFloor);
				fHdr += VpvGpu::Define("TD_F_SHARP", fSharp);
				fHdr += VpvGpu::Define("TD_F_DEFOCUS", fDefocus);
				fHdr += VpvGpu::Define("TD_F_SHIFT_DELTA", fShiftDelta);
				fHdr += Shaders::kPass2FocusHeader;
				pl_shader_desc df[10] = {
					VpvGpu::Sampled("cur_bands", sc.curBands),
					VpvGpu::Sampled("ref_bands_0", sc.refBands[0]),
					VpvGpu::Sampled("ref_bands_1", sc.refBands[1]),
					VpvGpu::Sampled("ref_bands_2", sc.refBands[2]),
					VpvGpu::Sampled("warped_0", sc.warped[0]),
					VpvGpu::Sampled("warped_1", sc.warped[1]),
					VpvGpu::Sampled("warped_2", sc.warped[2]),
					VpvGpu::Sampled("cell_prior", inCellPriorTex_),
					VpvGpu::Storage("cell_focus", sc.cellFocus),
					VpvGpu::Storage("warp_diag", outWarpDiagTex_)
				};
				if (!runPass("td_v2_p2_focus", fHdr, Shaders::kPass2Focus, df, 10, cellGx, cellGy, 8, 8))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p2_focus dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 2B: FP32 Shifted-Origin Moments + Blind A1/A2/A3 + KxK Solve + TdMaps ----
			{
				const int poolR = cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0;
				std::string sHdr = geomDefs;
				sHdr += VpvGpu::DefineInt("TD_K", K);
				sHdr += VpvGpu::DefineInt("TD_POOL_R", poolR);
				sHdr += VpvGpu::DefineInt("TD_ESTIMATOR", static_cast<int>(cfg_.estimator));
				sHdr += VpvGpu::DefineInt("TD_BOKEH_L_ONLY", cfg_.bokehMode == BokehMode::LowBandCellOnly ? 1 : 0);
				sHdr += VpvGpu::DefineInt("TD_EXACT_COPY_0", (K > 0 && refSpecs[0].isExactCopy) ? 1 : 0);
				sHdr += VpvGpu::DefineInt("TD_EXACT_COPY_1", (K > 1 && refSpecs[1].isExactCopy) ? 1 : 0);
				sHdr += VpvGpu::DefineInt("TD_EXACT_COPY_2", (K > 2 && refSpecs[2].isExactCopy) ? 1 : 0);
				sHdr += VpvGpu::DefineInt("TD_A2_VALID", a2Stats.valid ? 1 : 0);
				sHdr += VpvGpu::Define("TD_A2_SIGMA_SENT", a2Stats.valid ? a2Stats.sigmaSentSq : 0.0);
				sHdr += VpvGpu::Define("TD_A2_SIGMA_ZERO", a2Stats.valid ? a2Stats.sigmaZeroSq : 0.0);
				sHdr += VpvGpu::Define("TD_A2_TOTAL_VAR", a2Stats.valid ? a2Stats.totalVar : 0.0);
				sHdr += VpvGpu::Define("TD_GAIN_LOW", gains.low);
				sHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				sHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				sHdr += VpvGpu::Define("TD_TRACE_REG", kDefaultTraceRegScale);
				sHdr += Shaders::kCoreHelpers;
				sHdr += Shaders::kPass2SolveHeader;
				pl_shader_desc ds[14] = {
					VpvGpu::Sampled("cur_bands", sc.curBands),
					VpvGpu::Sampled("ref_bands_0", sc.refBands[0]),
					VpvGpu::Sampled("ref_bands_1", sc.refBands[1]),
					VpvGpu::Sampled("ref_bands_2", sc.refBands[2]),
					VpvGpu::Sampled("warped_0", sc.warped[0]),
					VpvGpu::Sampled("warped_1", sc.warped[1]),
					VpvGpu::Sampled("warped_2", sc.warped[2]),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("cell_focus", sc.cellFocus),
					VpvGpu::Sampled("cell_prior", inCellPriorTex_),
					VpvGpu::Storage("cell_weights", sc.cellWeights),
					VpvGpu::Storage("cell_diag_c", sc.cellDiagC),
					VpvGpu::Storage("td_maps", outTdMapsTex_),
					VpvGpu::Storage("cell_diag_flags", outDiagFlagsTex_)
				};
				if (!runPass("td_v2_p2_moments_solve", sHdr, Shaders::kPass2MomentsAndSolve, ds, 14, cellGx, cellGy, 8, 8))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p2_moments_solve dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 3A: Spatial-Only Weight Interpolation + 3-Sigma Huber Taper + Pixel Bound ----
			const int bxStart = ((active.x0 + 7) / 8) * 8;
			const int byStart = ((active.y0 + 7) / 8) * 8;
			const int bxEnd = (active.x1 / 8) * 8;
			const int byEnd = (active.y1 / 8) * 8;
			const bool runDctPocs = in.transformIdentified8x8 && (effStepDelta > 0.0) && (bxStart < bxEnd) && (byStart < byEnd);

			{
				std::string bHdr = geomDefs;
				bHdr += VpvGpu::DefineInt("TD_K", K);
				bHdr += VpvGpu::DefineInt("TD_XFORM_8X8", in.transformIdentified8x8 ? 1 : 0);
				bHdr += VpvGpu::DefineInt("TD_BX0", bxStart);
				bHdr += VpvGpu::DefineInt("TD_BY0", byStart);
				bHdr += VpvGpu::DefineInt("TD_BX1", bxEnd);
				bHdr += VpvGpu::DefineInt("TD_BY1", byEnd);
				bHdr += VpvGpu::DefineInt("TD_A2_VALID", a2Stats.valid ? 1 : 0);
				bHdr += VpvGpu::Define("TD_A2_TOTAL_VAR", a2Stats.valid ? a2Stats.totalVar : 0.0);
				bHdr += VpvGpu::Define("TD_GAIN_LOW", gains.low);
				bHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				bHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				bHdr += VpvGpu::Define("TD_HUBER_KNEE_SQ", kHuberKneeSq);
				bHdr += VpvGpu::Define("TD_PIXEL_BOUND_SIGMA", kPixelBoundSigma);
				bHdr += Shaders::kCoreHelpers;
				bHdr += Shaders::kPass3BlendHeader;
				pl_shader_desc db[10] = {
					VpvGpu::Sampled("cur_bands", sc.curBands),
					VpvGpu::Sampled("ref_bands_0", sc.refBands[0]),
					VpvGpu::Sampled("ref_bands_1", sc.refBands[1]),
					VpvGpu::Sampled("ref_bands_2", sc.refBands[2]),
					VpvGpu::Sampled("warped_0", sc.warped[0]),
					VpvGpu::Sampled("warped_1", sc.warped[1]),
					VpvGpu::Sampled("warped_2", sc.warped[2]),
					VpvGpu::Sampled("cell_weights", sc.cellWeights),
					VpvGpu::Sampled("cell_diag_c", sc.cellDiagC),
					VpvGpu::Storage("delta_out", runDctPocs ? sc.deltaScratch : outDeltaTex_)
				};
				if (!runPass("td_v2_p3_blend_bound", bHdr, Shaders::kPass3BlendAndBound, db, 10, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p3_blend_bound dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 3B: Optional Codec-Grid-Aligned 8x8 DCT-II STRICT/CELL POCS ----
			if (runDctPocs)
			{
				const double peakCode = PeakCodeForBits(cfg_.sourceBits);
				const int iters = (std::max)(1, (std::min)(2, cfg_.cellProjIterations));
				std::string pHdr = geomDefs;
				pHdr += VpvGpu::DefineInt("TD_BX0", bxStart);
				pHdr += VpvGpu::DefineInt("TD_BY0", byStart);
				pHdr += VpvGpu::DefineInt("TD_BX1", bxEnd);
				pHdr += VpvGpu::DefineInt("TD_BY1", byEnd);
				pHdr += VpvGpu::DefineInt("TD_POCS_ITERS", iters);
				pHdr += VpvGpu::DefineInt("TD_RESIDUAL_MODE", cfg_.residualMode == ResidualMode::Strict ? 1 : 0);
				pHdr += VpvGpu::Define("TD_STEP_DELTA", effStepDelta);
				pHdr += VpvGpu::Define("TD_DEADZONE_PHI", in.deadZonePhi);
				pHdr += VpvGpu::Define("TD_PEAK_CODE", peakCode);
				pHdr += Shaders::BuildDctBasisHeader();
				pHdr += Shaders::kCoreHelpers;
				pHdr += Shaders::kPass3bDctHeader;
				pl_shader_desc dp[4] = {
					VpvGpu::Sampled("delta_in", sc.deltaScratch),
					VpvGpu::Sampled("cur_clean", inCleanYTex_),
					VpvGpu::Sampled("pred_clean", sc.warped[0]),
					VpvGpu::Storage("delta_out", outDeltaTex_)
				};
				const size_t shmemBytes = (5u * 64u) * sizeof(float);
				if (!runPass("td_v2_p3_dct_pocs", pHdr, Shaders::kPass3bDctPocs, dp, 4,
				             Detail::GroupsFor(w, 8), Detail::GroupsFor(h, 8), 8, 8, shmemBytes))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p3_dct_pocs dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 4A: Recompose (candClean -> outClean & carryCleanTex_; candOut -> candOutScratch) ----
			{
				pl_shader_desc dr[7] = {
					VpvGpu::Sampled("rec_y", inRecYTex_),
					VpvGpu::Sampled("cur_clean", inCleanYTex_),
					VpvGpu::Sampled("own_grain", inOwnGrainTex_),
					VpvGpu::Sampled("delta_tex", outDeltaTex_),
					VpvGpu::Storage("out_clean", outCleanTex_),
					VpvGpu::Storage("carry_clean", carryCleanTex_),
					VpvGpu::Storage("cand_out", sc.candOutScratch)
				};
				if (!runPass("td_v2_p4_recompose", geomDefs, Shaders::kPass4Recompose, dr, 7, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p4_recompose dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 4B: Layer-4 TdGuardPort Fine-Band Extraction (reusing bandTmpH & refBands[0]) ----
			{
				const std::string gFineHdr = geomDefs + Shaders::BuildFineTapHeader();
				pl_shader_desc dgh[4] = {
					VpvGpu::Sampled("rec_y", inRecYTex_),
					VpvGpu::Sampled("cand_out", sc.candOutScratch),
					VpvGpu::Sampled("own_grain", inOwnGrainTex_),
					VpvGpu::Storage("band_tmp_h", sc.bandTmpH)
				};
				if (!runPass("td_v2_p4_guard_h", gFineHdr, Shaders::kPass4GuardFineH, dgh, 4, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p4_guard_h dispatch failed", StandInKind::GpuDispatchFailed);
				}
				pl_shader_desc dgv[5] = {
					VpvGpu::Sampled("band_tmp_h", sc.bandTmpH),
					VpvGpu::Sampled("rec_y", inRecYTex_),
					VpvGpu::Sampled("cand_out", sc.candOutScratch),
					VpvGpu::Sampled("own_grain", inOwnGrainTex_),
					VpvGpu::Storage("guard_bands", sc.refBands[0])
				};
				if (!runPass("td_v2_p4_guard_v", gFineHdr, Shaders::kPass4GuardFineV, dgv, 5, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p4_guard_v dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 4C: Layer-4 TdGuardPort Per-Cell Evaluation ----
			{
				std::string gcHdr = geomDefs;
				gcHdr += VpvGpu::DefineInt("TD_HAS_CLEAN_SPLIT", in.hasCleanSplit ? 1 : 0);
				gcHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				gcHdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				gcHdr += Shaders::kCoreHelpers;
				pl_shader_desc dgc[3] = {
					VpvGpu::Sampled("guard_bands", sc.refBands[0]),
					VpvGpu::Sampled("cell_prior", inCellPriorTex_),
					VpvGpu::Storage("guard_cell", outGuardCellTex_)
				};
				if (!runPass("td_v2_p4_guard_cell", gcHdr, Shaders::kPass4GuardCell, dgc, 3, cellGx, cellGy, 8, 8))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p4_guard_cell dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 4D: Final Apply + Layer-4 Guard Add-Back (outputY ONLY; Shadow preserves receivedY) ----
			{
				std::string aHdr = geomDefs;
				aHdr += VpvGpu::DefineInt("TD_SHADOW_MODE", cfg_.ModifiesPixels() ? 0 : 1);
				pl_shader_desc da[5] = {
					VpvGpu::Sampled("rec_y", inRecYTex_),
					VpvGpu::Sampled("cand_out", sc.candOutScratch),
					VpvGpu::Sampled("guard_bands", sc.refBands[0]),
					VpvGpu::Sampled("guard_cell", outGuardCellTex_),
					VpvGpu::Storage("out_y", outYTex_)
				};
				if (!runPass("td_v2_p4_apply_guard", aHdr, Shaders::kPass4ApplyAndGuard, da, 5, fullGx, fullGy))
				{
					ResetCarry(gpu);
					return failVisible("td_v2_p4_apply_guard dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// Update persistent carry tracking (strictly grain-free carryCleanTex_).
			hasCarry_ = true;
			carryW_ = w;
			carryH_ = h;
			res.dispatchCount = dispatches;
			lastDispatchCount_ = dispatches;

			// Read back outputs and cell diagnostics.
			TdV2Plane gpuOutY;
			if (!Detail::DownloadPlaneR32F(gpu, outYTex_, w, h, gpuOutY) ||
			    !Detail::DownloadPlaneR32F(gpu, outCleanTex_, w, h, res.outputClean) ||
			    !Detail::DownloadPlaneR32F(gpu, outDeltaTex_, w, h, res.tdDelta))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine output plane download failed", StandInKind::GpuDispatchFailed);
			}
			// Shadow mode preserves receivedY bit-exact (while GPU pass also wrote exact rec_y).
			if (cfg_.ModifiesPixels())
				res.outputY = std::move(gpuOutY);

			// Download U19 TdMaps (cw x 3*ch RGBA32F).
			std::vector<float> mapsBuf(numCells * 3u * 4u, 0.0f);
			std::vector<float> diagBuf(numCells * 4u, 0.0f);
			std::vector<float> guardBuf(numCells * 2u, 0.0f);
			std::vector<float> warpDiagBuf(numCells * 2u, 0.0f);
			if (!Detail::DownloadFloats(gpu, outTdMapsTex_, mapsBuf.data()) ||
			    !Detail::DownloadFloats(gpu, outDiagFlagsTex_, diagBuf.data()) ||
			    !Detail::DownloadFloats(gpu, outGuardCellTex_, guardBuf.data()) ||
			    !Detail::DownloadFloats(gpu, outWarpDiagTex_, warpDiagBuf.data()))
			{
				ResetCarry(gpu);
				return failVisible("TdV2GpuEngine diagnostic download failed", StandInKind::GpuDispatchFailed);
			}

			bool anyGuardEvaluated = false;
			size_t totalWarpNonFinite = 0;
			for (int cy = 0; cy < res.cellsH; ++cy)
			{
				for (int cx = 0; cx < res.cellsW; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * res.cellsW + cx;
					float packed[TdMapsView::kPlanes][4] = {};
					for (int p = 0; p < TdMapsView::kPlanes; ++p)
					{
						const size_t row = static_cast<size_t>(p * res.cellsH + cy);
						const size_t texel = row * static_cast<size_t>(res.cellsW) + static_cast<size_t>(cx);
						for (int c = 0; c < 4; ++c)
							packed[p][c] = mapsBuf[texel * 4u + static_cast<size_t>(c)];
					}
					res.tdMaps[cIdx] = TdMapsView::Unpack(packed);

					if (diagBuf[cIdx * 4u + 0u] > 0.5f)
						++res.focusVetoCells;
					res.nonFiniteCells += static_cast<size_t>(std::llround(static_cast<double>(diagBuf[cIdx * 4u + 1u])));
					res.singularCells += static_cast<size_t>(std::llround(static_cast<double>(diagBuf[cIdx * 4u + 2u])));
					res.singleRefA2Cells += static_cast<size_t>(std::llround(static_cast<double>(diagBuf[cIdx * 4u + 3u])));

					const int gFlags = static_cast<int>(std::llround(static_cast<double>(guardBuf[cIdx * 2u + 1u])));
					if ((gFlags & 2) != 0)
						anyGuardEvaluated = true;
					if ((gFlags & 1) != 0)
						++res.guardViolationCells;

					res.borderBilinearPixels += static_cast<size_t>(std::llround(static_cast<double>(warpDiagBuf[cIdx * 2u + 0u])));
					totalWarpNonFinite += static_cast<size_t>(std::llround(static_cast<double>(warpDiagBuf[cIdx * 2u + 1u])));
				}
			}
			if (totalWarpNonFinite > 0)
				++res.nonFiniteCells;

			// Mirror all visible StandInRegistry engagements identically to TdV2ReferenceEngine.
			if (standIns != nullptr)
			{
				if (res.borderBilinearPixels > 0)
					standIns->Engage(StandInKind::BorderBilinearFallback);
				if (res.nonFiniteCells > 0)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine GPU non-finite closure");
				if (res.singularCells > 0)
					standIns->Engage(StandInKind::SingularSolveClosure, "TdV2GpuEngine singular covariance solve");
				if (res.singleRefA2Cells > 0)
					standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
				if (anyGuardEvaluated)
					standIns->Engage(StandInKind::GrainGuardW4c);

				// Engage TransformUnknownBound whenever any pixel outside an interior 8x8 DCT block had non-zero delta.
				bool anyBoundApplied = false;
				for (int y = active.y0; y < active.y1 && !anyBoundApplied; ++y)
				{
					for (int x = active.x0; x < active.x1; ++x)
					{
						const bool inFull = runDctPocs && (x >= bxStart && x < bxEnd && y >= byStart && y < byEnd);
						if (!inFull && std::fabs(res.tdDelta.at(x, y)) > 0.0)
						{
							anyBoundApplied = true;
							break;
						}
					}
				}
				if (anyBoundApplied)
					standIns->Engage(StandInKind::TransformUnknownBound);
			}

			res.succeeded = true;
			return res;
		}

		// Execute Chroma GPU Delta Apply Pass (YUV sited upsample OR unclipped RGB matrix via CPU-passed LumaCoeff).
		ChromaGpuApplyResult ApplyChromaDeltaGpu(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const ChromaGpuApplyInput& in,
			StandInRegistry* standIns = nullptr)
		{
			ChromaGpuApplyResult res;
			auto failChroma = [&](const std::string& msg, StandInKind kind) -> ChromaGpuApplyResult
			{
				lastError_ = msg;
				res.lastError = msg;
				res.succeeded = false;
				++failures_;
				if (standIns != nullptr)
					standIns->Engage(kind, msg.c_str());
				return res;
			};

			if (!gpu || !dispatch)
				return failChroma("ApplyChromaDeltaGpu null pl_gpu or pl_dispatch", StandInKind::GpuDispatchFailed);
			if (in.lumaW <= 0 || in.lumaH <= 0)
				return failChroma("ApplyChromaDeltaGpu invalid luma dimensions", StandInKind::GpuDispatchFailed);

			const bool isRgb = (in.deliveredFormat == DeliveredChromaFormat::Rgb444);
			const ChromaSubsampling delivSub = DeliveredSubsampling(in.deliveredFormat);
			const int dSubX = SubsampleX(delivSub);
			const int dSubY = SubsampleY(delivSub);
			const int cSubX = SubsampleX(in.codedSubsampling);
			const int cSubY = SubsampleY(in.codedSubsampling);
			const int dW = ChromaDetail::SubsampledDim(in.lumaW, dSubX);
			const int dH = ChromaDetail::SubsampledDim(in.lumaH, dSubY);
			const int cW = ChromaDetail::SubsampledDim(in.lumaW, cSubX);
			const int cH = ChromaDetail::SubsampledDim(in.lumaH, cSubY);

			if (in.codedDeltaCb.w != cW || in.codedDeltaCb.h != cH ||
			    in.codedDeltaCr.w != cW || in.codedDeltaCr.h != cH)
			{
				return failChroma("ApplyChromaDeltaGpu codedDelta dimension mismatch", StandInKind::GpuDispatchFailed);
			}

			if (isRgb)
			{
				if (in.deliveredR.w != dW || in.deliveredR.h != dH ||
				    in.deliveredG.w != dW || in.deliveredG.h != dH ||
				    in.deliveredB.w != dW || in.deliveredB.h != dH)
				{
					return failChroma("ApplyChromaDeltaGpu RGB dimension mismatch", StandInKind::GpuDispatchFailed);
				}
				if (!std::isfinite(in.matrix.kr) || !std::isfinite(in.matrix.kg) || !std::isfinite(in.matrix.kb) ||
				    !(in.matrix.kg > kEngineNumFloor))
				{
					return failChroma("ApplyChromaDeltaGpu invalid YCbCr matrix", StandInKind::NonFiniteClosure);
				}
			}
			else
			{
				if (in.deliveredCb.w != dW || in.deliveredCb.h != dH ||
				    in.deliveredCr.w != dW || in.deliveredCr.h != dH)
				{
					return failChroma("ApplyChromaDeltaGpu YUV dimension mismatch", StandInKind::GpuDispatchFailed);
				}
			}

			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtRG32 || !fmtRGBA32)
				return failChroma("ApplyChromaDeltaGpu required FP32 formats unsupported", StandInKind::GpuDispatchFailed);

			const ActiveRect lumaActive = in.activeRect.Empty()
				? ActiveRect::Full(in.lumaW, in.lumaH)
				: in.activeRect.ClampedTo(in.lumaW, in.lumaH);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, cSubX, cSubY, cW, cH);
			const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, dSubX, dSubY, dW, dH);
			const bool identityGrid = (in.codedSubsampling == delivSub && cW == dW && cH == dH);

			// Pack coded_delta (RG32F).
			pl_tex codedDeltaTex = nullptr;
			if (!Detail::EnsureTex(gpu, codedDeltaTex, cW, cH, fmtRG32, true, false, true, false))
				return failChroma("ApplyChromaDeltaGpu codedDelta texture alloc failed", StandInKind::GpuDispatchFailed);

			std::vector<float> codedBuf(static_cast<size_t>(cW) * static_cast<size_t>(cH) * 2u, 0.0f);
			for (size_t i = 0; i < in.codedDeltaCb.v.size(); ++i)
			{
				codedBuf[i * 2u + 0u] = static_cast<float>(in.codedDeltaCb.v[i]);
				codedBuf[i * 2u + 1u] = static_cast<float>(in.codedDeltaCr.v[i]);
			}
			if (!Detail::UploadFloats(gpu, codedDeltaTex, codedBuf.data()))
			{
				pl_tex_destroy(gpu, &codedDeltaTex);
				return failChroma("ApplyChromaDeltaGpu codedDelta upload failed", StandInKind::GpuDispatchFailed);
			}

			std::string hdr;
			hdr += VpvGpu::DefineInt("TD_DW", dW);
			hdr += VpvGpu::DefineInt("TD_DH", dH);
			hdr += VpvGpu::DefineInt("TD_DSUB_X", dSubX);
			hdr += VpvGpu::DefineInt("TD_DSUB_Y", dSubY);
			hdr += VpvGpu::DefineInt("TD_CSUB_X", cSubX);
			hdr += VpvGpu::DefineInt("TD_CSUB_Y", cSubY);
			hdr += VpvGpu::Define("TD_DSHIFT_X", dSubX > 1 ? static_cast<double>(in.siting.shiftX) : 0.0);
			hdr += VpvGpu::Define("TD_DSHIFT_Y", dSubY > 1 ? static_cast<double>(in.siting.shiftY) : 0.0);
			hdr += VpvGpu::Define("TD_CSHIFT_X", cSubX > 1 ? static_cast<double>(in.siting.shiftX) : 0.0);
			hdr += VpvGpu::Define("TD_CSHIFT_Y", cSubY > 1 ? static_cast<double>(in.siting.shiftY) : 0.0);
			hdr += VpvGpu::DefineInt("TD_CAX0", cActive.x0);
			hdr += VpvGpu::DefineInt("TD_CAY0", cActive.y0);
			hdr += VpvGpu::DefineInt("TD_CAX1", cActive.x1);
			hdr += VpvGpu::DefineInt("TD_CAY1", cActive.y1);
			hdr += VpvGpu::DefineInt("TD_DAX0", dActive.x0);
			hdr += VpvGpu::DefineInt("TD_DAY0", dActive.y0);
			hdr += VpvGpu::DefineInt("TD_DAX1", dActive.x1);
			hdr += VpvGpu::DefineInt("TD_DAY1", dActive.y1);
			hdr += VpvGpu::DefineInt("TD_IDENTITY_GRID", identityGrid ? 1 : 0);
			hdr += VpvGpu::Define("TD_NUM_FLOOR", kEngineNumFloor);
			hdr += VpvGpu::Define("TD_MAT_KR", in.matrix.kr);
			hdr += VpvGpu::Define("TD_MAT_KG", in.matrix.kg);
			hdr += VpvGpu::Define("TD_MAT_KB", in.matrix.kb);
			hdr += Shaders::kCoreHelpers;
			hdr += Shaders::kPassChromaDeltaHeader;

			const int gx = Detail::GroupsFor(dW, 16);
			const int gy = Detail::GroupsFor(dH, 16);

			if (!isRgb)
			{
				pl_tex delivUvTex = nullptr;
				pl_tex outUvDeltaTex = nullptr;
				bool ok = Detail::EnsureTex(gpu, delivUvTex, dW, dH, fmtRG32, true, false, true, false) &&
				          Detail::EnsureTex(gpu, outUvDeltaTex, dW, dH, fmtRGBA32, true, true, false, true);
				if (ok)
				{
					std::vector<float> uvBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
					for (size_t i = 0; i < in.deliveredCb.v.size(); ++i)
					{
						uvBuf[i * 2u + 0u] = static_cast<float>(in.deliveredCb.v[i]);
						uvBuf[i * 2u + 1u] = static_cast<float>(in.deliveredCr.v[i]);
					}
					ok = Detail::UploadFloats(gpu, delivUvTex, uvBuf.data());
				}
				if (ok)
				{
					pl_shader_desc d[3] = {
						VpvGpu::Sampled("deliv_uv", delivUvTex),
						VpvGpu::Sampled("coded_delta", codedDeltaTex),
						VpvGpu::Storage("out_uv_delta", outUvDeltaTex)
					};
					ok = VpvGpu::Pass(dispatch, "td_v2_chroma_apply_yuv", hdr, Shaders::kPassChromaApplyYuv,
					                  d, 3, nullptr, 0, gx, gy, 0, 16, 16);
					if (ok)
						res.dispatchCount = 1;
				}
				if (ok)
				{
					std::vector<float> outBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
					ok = Detail::DownloadFloats(gpu, outUvDeltaTex, outBuf.data());
					if (ok)
					{
						res.outCb = TdV2Plane(dW, dH, 0.0);
						res.outCr = TdV2Plane(dW, dH, 0.0);
						res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
						res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
						for (size_t i = 0; i < res.outCb.v.size(); ++i)
						{
							res.outCb.v[i] = static_cast<double>(outBuf[i * 4u + 0u]);
							res.outCr.v[i] = static_cast<double>(outBuf[i * 4u + 1u]);
							res.deliveredDeltaCb.v[i] = static_cast<double>(outBuf[i * 4u + 2u]);
							res.deliveredDeltaCr.v[i] = static_cast<double>(outBuf[i * 4u + 3u]);
						}
					}
				}
				if (delivUvTex) pl_tex_destroy(gpu, &delivUvTex);
				if (outUvDeltaTex) pl_tex_destroy(gpu, &outUvDeltaTex);
				pl_tex_destroy(gpu, &codedDeltaTex);
				if (!ok)
					return failChroma("td_v2_chroma_apply_yuv dispatch or transfer failed", StandInKind::GpuDispatchFailed);
				res.succeeded = true;
				return res;
			}

			// Unclipped RGB delivery path.
			pl_tex delivRgbTex = nullptr;
			pl_tex outRgbTex = nullptr;
			pl_tex outDeltaTex = nullptr;
			bool ok = Detail::EnsureTex(gpu, delivRgbTex, dW, dH, fmtRGBA32, true, false, true, false) &&
			          Detail::EnsureTex(gpu, outRgbTex, dW, dH, fmtRGBA32, true, true, false, true) &&
			          Detail::EnsureTex(gpu, outDeltaTex, dW, dH, fmtRG32, true, true, false, true);
			if (ok)
			{
				std::vector<float> rgbBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
				for (size_t i = 0; i < in.deliveredR.v.size(); ++i)
				{
					rgbBuf[i * 4u + 0u] = static_cast<float>(in.deliveredR.v[i]);
					rgbBuf[i * 4u + 1u] = static_cast<float>(in.deliveredG.v[i]);
					rgbBuf[i * 4u + 2u] = static_cast<float>(in.deliveredB.v[i]);
					rgbBuf[i * 4u + 3u] = 0.0f;
				}
				ok = Detail::UploadFloats(gpu, delivRgbTex, rgbBuf.data());
			}
			if (ok)
			{
				pl_shader_desc d[4] = {
					VpvGpu::Sampled("deliv_rgb", delivRgbTex),
					VpvGpu::Sampled("coded_delta", codedDeltaTex),
					VpvGpu::Storage("out_rgb", outRgbTex),
					VpvGpu::Storage("out_delta", outDeltaTex)
				};
				ok = VpvGpu::Pass(dispatch, "td_v2_chroma_apply_rgb", hdr, Shaders::kPassChromaApplyRgb,
				                  d, 4, nullptr, 0, gx, gy, 0, 16, 16);
				if (ok)
					res.dispatchCount = 1;
			}
			if (ok)
			{
				std::vector<float> rgbOutBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
				std::vector<float> delOutBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
				ok = Detail::DownloadFloats(gpu, outRgbTex, rgbOutBuf.data()) &&
				     Detail::DownloadFloats(gpu, outDeltaTex, delOutBuf.data());
				if (ok)
				{
					res.outR = TdV2Plane(dW, dH, 0.0);
					res.outG = TdV2Plane(dW, dH, 0.0);
					res.outB = TdV2Plane(dW, dH, 0.0);
					res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
					res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
					for (size_t i = 0; i < res.outR.v.size(); ++i)
					{
						res.outR.v[i] = static_cast<double>(rgbOutBuf[i * 4u + 0u]);
						res.outG.v[i] = static_cast<double>(rgbOutBuf[i * 4u + 1u]);
						res.outB.v[i] = static_cast<double>(rgbOutBuf[i * 4u + 2u]);
						res.deliveredDeltaCb.v[i] = static_cast<double>(delOutBuf[i * 2u + 0u]);
						res.deliveredDeltaCr.v[i] = static_cast<double>(delOutBuf[i * 2u + 1u]);
					}
				}
			}
			if (delivRgbTex) pl_tex_destroy(gpu, &delivRgbTex);
			if (outRgbTex) pl_tex_destroy(gpu, &outRgbTex);
			if (outDeltaTex) pl_tex_destroy(gpu, &outDeltaTex);
			pl_tex_destroy(gpu, &codedDeltaTex);
			if (!ok)
				return failChroma("td_v2_chroma_apply_rgb dispatch or transfer failed", StandInKind::GpuDispatchFailed);
			res.succeeded = true;
			return res;
		}

	private:
		struct ScratchSet
		{
			pl_tex curBands = nullptr;
			pl_tex refBands[kMaxGpuRefs] = {};
			pl_tex bandTmpH = nullptr;
			pl_tex warped[kMaxGpuRefs] = {};
			pl_tex deltaScratch = nullptr;
			pl_tex candOutScratch = nullptr;
			pl_tex motionGate = nullptr;
			pl_tex cellWeights = nullptr;
			pl_tex cellDiagC = nullptr;
			pl_tex cellFocus = nullptr;

			bool AllNonNull() const
			{
				if (!curBands || !bandTmpH || !deltaScratch || !candOutScratch ||
				    !motionGate || !cellWeights || !cellDiagC || !cellFocus)
				{
					return false;
				}
				for (int i = 0; i < kMaxGpuRefs; ++i)
				{
					if (!refBands[i] || !warped[i])
						return false;
				}
				return true;
			}
		};

		bool EnsurePrivateScratch(pl_gpu gpu, int w, int h, int cw, int ch,
		                          pl_fmt fmtR32, pl_fmt fmtRG32, pl_fmt fmtRGBA32, ScratchSet& out)
		{
			bool ok = Detail::EnsureTex(gpu, privCurBands_, w, h, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privBandTmpH_, w, h, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privDeltaScratch_, w, h, fmtR32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCandOutScratch_, w, h, fmtR32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privMotionGate_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellWeights_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellDiagC_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellFocus_, cw, ch, fmtRG32, true, true, false, false);
			for (int i = 0; i < kMaxGpuRefs && ok; ++i)
			{
				ok = Detail::EnsureTex(gpu, privRefBands_[i], w, h, fmtRGBA32, true, true, false, false) &&
				     Detail::EnsureTex(gpu, privWarped_[i], w, h, fmtRG32, true, true, false, false);
			}
			if (!ok)
				return false;
			out.curBands = privCurBands_;
			for (int i = 0; i < kMaxGpuRefs; ++i)
			{
				out.refBands[i] = privRefBands_[i];
				out.warped[i] = privWarped_[i];
			}
			out.bandTmpH = privBandTmpH_;
			out.deltaScratch = privDeltaScratch_;
			out.candOutScratch = privCandOutScratch_;
			out.motionGate = privMotionGate_;
			out.cellWeights = privCellWeights_;
			out.cellDiagC = privCellDiagC_;
			out.cellFocus = privCellFocus_;
			return true;
		}

		void DestroyPrivateScratch(pl_gpu gpu)
		{
			if (!gpu)
				return;
			for (pl_tex* t : {
				&privCurBands_, &privRefBands_[0], &privRefBands_[1], &privRefBands_[2], &privBandTmpH_,
				&privWarped_[0], &privWarped_[1], &privWarped_[2],
				&privDeltaScratch_, &privCandOutScratch_,
				&privMotionGate_, &privCellWeights_, &privCellDiagC_, &privCellFocus_ })
			{
				if (*t != nullptr)
					pl_tex_destroy(gpu, t);
			}
		}

		Config cfg_;
		bool hasCarry_ = false;
		int carryW_ = 0;
		int carryH_ = 0;
		pl_tex carryCleanTex_ = nullptr;

		// Owned private scratch textures (used ONLY when arena == nullptr).
		pl_tex privCurBands_ = nullptr;
		pl_tex privRefBands_[kMaxGpuRefs] = {};
		pl_tex privBandTmpH_ = nullptr;
		pl_tex privWarped_[kMaxGpuRefs] = {};
		pl_tex privDeltaScratch_ = nullptr;
		pl_tex privCandOutScratch_ = nullptr;
		pl_tex privMotionGate_ = nullptr;
		pl_tex privCellWeights_ = nullptr;
		pl_tex privCellDiagC_ = nullptr;
		pl_tex privCellFocus_ = nullptr;

		// Owned IO staging and readback textures.
		pl_tex inRecYTex_ = nullptr;
		pl_tex inCleanYTex_ = nullptr;
		pl_tex inOwnGrainTex_ = nullptr;
		pl_tex inRefCleanTex_ = nullptr;
		pl_tex inMotionVecTex_ = nullptr;
		pl_tex inMotionMetaTex_ = nullptr;
		pl_tex inCellPriorTex_ = nullptr;
		pl_tex outYTex_ = nullptr;
		pl_tex outCleanTex_ = nullptr;
		pl_tex outDeltaTex_ = nullptr;
		pl_tex outTdMapsTex_ = nullptr;
		pl_tex outDiagFlagsTex_ = nullptr;
		pl_tex outGuardCellTex_ = nullptr;
		pl_tex outWarpDiagTex_ = nullptr;

		std::string lastError_;
		uint64_t failures_ = 0;
		int lastDispatchCount_ = 0;
		bool scratchShort_ = false;
	};

} // namespace Gpu
} // namespace TdV2
