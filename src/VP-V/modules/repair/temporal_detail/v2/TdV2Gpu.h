// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S5 + TD-S5-OPT (2026-11-23_TD-consolidated-plan.md s3.2 TD-S5):
// Optimized libplacebo Compute Dispatch Wrapper & WARP-Testable GPU Engine:
//   - Mirrors TdV2ReferenceEngine (TD-S3), TdV2BlendMath (TD-S1), and TdV2Chroma apply (TD-S4)
//     across 4 optimized compute stages ("td_v2_p1_*", "td_v2_p2a_*", "td_v2_p2b_*", "td_v2_p3_*", "td_v2_p4_*")
//     plus cached Chroma GPU apply ("td_v2_chroma_*").
//   - Optimizations (1)-(7):
//       1. LDS-tiled single-loop separable blurs (kPass1BandH / kPass1BandV, FP32)
//       2. 2-stage coalesced LDS cell reduction + fused Focus Guard (kPass2CellStats -> kPass2MergeAndSolve, Chan-Pebay merge, FP32)
//       3. Compact Preset::RealTime band storage (SC-05 trade-off: half-res 2x2 area-reduced FP32 RG32F (L,D3) via U2 HalfSigma
//          + full-res FP32 RG32F (M,H); ALL band storage & moment inputs stay strictly FP32 per plan L60 — never FP16!
//          Preset::Reference retains full-res RGBA32F parity <= 1e-3 10-bit codes)
//       4. Single-dispatch fused Pass 4 (kPass4FusedApplyGuard: 5 dispatches -> 1, eliminates ownGrain/candOut/guardFine scratch;
//          Mode::Shadow skips out_y imageStore completely)
//       5. Reused 1x warpedVal (R32F, reverse k=K-1..0 loop leaves refs[0].clean ready for Pass 3B!) + packed 3-ref x 3-bit flags (R16)
//       6. 20x20 LDS-tiled bicubic warp (kPass1WarpAndGate, cell-constant MV per 16x16 WG, explicit fixed motion slots prev=0/next=1/carry=2)
//       7. Zero-copy ProcessFrameGpu(pl_tex...) with strict I/O alias & dimension validation + cached ApplyChromaDeltaGpu textures +
//          all-zero finite 0-dispatch skip + per-frame float UBOs + empty-block DCT fetch skip + Pass-1 face mask support
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
		size_t faceMaskedCells = 0;
		size_t nonFiniteCells = 0;
		size_t a2InvalidCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		bool carryUsed = false;
		bool carryReset = false;
		bool scratchShort = false;
		bool usedExternalBandMemo = false;       // True when caller-supplied kBandMemo curLowBandTex skipped current-frame low-band blur
		bool externalBandMemoRejected = false;   // True when non-null curLowBandTex failed dimension/format validation (visible fail-closed)
		bool succeeded = false;
		std::string lastError;
	};

	// ---- Zero-Copy GPU-Resident Frame Input / Output (Speedup 7: ProcessFrameGpu) ----
	struct GpuReferenceInput
	{
		pl_tex cleanTex = nullptr;               // Grain-free reference plane (w x h R32F, sampleable)
		const CellMotionField* motion = nullptr; // Optional CPU motion field (used when motionVecTex/MetaTex null)
		bool isExactCopyOfCurrent = false;
		Homography3x3 cameraHomography;          // Optional P7 3x3 camera homography (used strictly on RegionMotionClass::Camera cells when valid)
	};

	struct GpuFrameInput
	{
		uint64_t seq = 0;
		int w = 0;
		int h = 0;
		pl_tex receivedYTex = nullptr;           // Required when enabled: w x h R32F (sampleable)
		pl_tex cleanYTex = nullptr;              // Optional: w x h R32F grain-free clean (null => uses receivedYTex)
		bool hasCleanSplit = false;              // True when cleanYTex is upstream P10g grain-free clean
		ActiveRect activeRect;

		// Up to 3 external/recurrent references (kMaxGpuRefs == 3): prevPeak, nextPeak, optional prevPeak2, carryCleanTex_.
		GpuReferenceInput prevPeak;
		GpuReferenceInput nextPeak;
		GpuReferenceInput prevPeak2;

		// Optional Field B CodecModel prediction P_B (w x h R32F, sampleable; null => uses refs[0].clean stand-in visibly).
		pl_tex codecModelPbTex = nullptr;
		// Optional kBandMemo precomputed BandFromBase low band L = G(kSigmaBase) of curCleanY (SOTA #10):
		//   w x h sampleable float (Reference or RealTime), or wHalf x hHalf sampleable float (RealTime).
		//   When valid, skips current-frame coarse low-band H+V blur passes in a single fused shader pass; default nullptr = bit-identical baseline.
		pl_tex curLowBandTex = nullptr;

		// Optional pre-packed GPU motion, cell prior, and Pass-1 face/non-rigid mask textures (ZERO CPU upload!):
		//   motionVecTex:  3*cw x ch RGBA32F (fixed slots: 0=prevPeak, 1=nextPeak, 2=carry; vx, vy, camVx, camVy)
		//   motionMetaTex: 3*cw x ch RG32F   (fixed slots: 0=prevPeak, 1=nextPeak, 2=carry; cls, valid)
		//   cellPriorTex:  cw x ch RG32F or RGBA32F (retainedGrainVar, explicitFocusBits[bit0=focusChange, bit1=defocused/bokeh],
		//                  optional .z = cellStepScale [defaults to 1.0 when RG32F], .w = reserved)
		//   faceMaskTex:   cw x ch R32F/R8   (Pass-1 non-rigid/face mask: > 0.5 => strict delta=0 passthrough)
		pl_tex motionVecTex = nullptr;
		pl_tex motionMetaTex = nullptr;
		pl_tex cellPriorTex = nullptr;
		pl_tex faceMaskTex = nullptr;

		// Optional CPU fallback motion/prior/mask fields (used only when corresponding GPU texture above is null).
		CellMotionField carryMotion;
		bool hasCarryMotion = false;
		std::vector<double> retainedGrainVar;
		std::vector<double> cellStepScale;
		const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
		const std::vector<uint8_t>* explicitDefocusedCells = nullptr;
		const std::vector<uint8_t>* faceMask = nullptr;

		// Coding step prior (same contract as FrameProcessInput).
		bool hasStepPrior = false;
		double estimatedStepDelta = 0.0;
		double deadZonePhi = kDeadZonePhiInter;
		double zeroCoeffFraction = 0.0;
		bool transformIdentified8x8 = false;

		bool isCutOrSpanInvalid = false;
		double fadeKeep = 1.0;                   // U7 CutFadeJudge gradual fade keep in [0, 1] (default 1.0 = bit-identical; <=0 or NaN resets carry)
		PsdShape psdShape;                       // Optional P10g grain PSD shape multipliers (default 1.0 white-noise identity)
	};

	struct GpuFrameOutput
	{
		// Caller-provided GPU output targets (zero readback!).
		// In Mode::Shadow, outputYTex is NOT written (caller retains receivedYTex bit-exact).
		pl_tex outputYTex = nullptr;             // w x h R32F (storable)
		pl_tex outputCleanTex = nullptr;         // Optional: w x h R32F (storable)
		pl_tex tdDeltaTex = nullptr;             // Optional: w x h RG32F (storable)
		pl_tex tdMapsTex = nullptr;              // Optional: cw x 3*ch RGBA32F (storable)
		pl_tex diagFlagsTex = nullptr;           // Optional: cw x ch RGBA32F (storable)
		pl_tex guardCellTex = nullptr;           // Optional: cw x ch RG32F (storable)
		pl_tex warpDiagTex = nullptr;            // Optional: cw x ch RG32F (storable)

		// When true, reads back tiny cell-grid diagnostics to populate counters, tdMaps, and StandInRegistry.
		// When false (zero-copy realtime render loop), performs ZERO GPU->CPU downloads!
		bool readbackCellDiagnostics = false;

		// Metadata & counters (cell counters populated when readbackCellDiagnostics == true).
		int cellsW = 0;
		int cellsH = 0;
		size_t activeRefCount = 0;
		int dispatchCount = 0;
		size_t guardViolationCells = 0;
		size_t focusVetoCells = 0;
		size_t faceMaskedCells = 0;
		size_t nonFiniteCells = 0;
		size_t a2InvalidCells = 0;
		size_t singularCells = 0;
		size_t singleRefA2Cells = 0;
		size_t motionStandInRefs = 0;
		size_t borderBilinearPixels = 0;
		bool carryUsed = false;
		bool carryReset = false;
		bool scratchShort = false;
		bool usedExternalBandMemo = false;       // True when caller-supplied kBandMemo curLowBandTex skipped current-frame low-band blur
		bool externalBandMemoRejected = false;   // True when non-null curLowBandTex failed dimension/format validation (visible fail-closed)
		bool succeeded = false;
		std::string lastError;
		std::vector<TdMapsView::Block> tdMaps;
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
		size_t nonFinitePixels = 0;
		size_t borderBilinearPixels = 0;
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

		inline bool DownloadDeltaRG32F(pl_gpu gpu, pl_tex tex, int w, int h, TdV2Plane& outDelta, bool& outAnyBoundEngaged)
		{
			outAnyBoundEngaged = false;
			if (w <= 0 || h <= 0)
				return false;
			const size_t pxCount = static_cast<size_t>(w) * static_cast<size_t>(h);
			std::vector<float> buf(pxCount * 2u, 0.0f);
			if (!DownloadFloats(gpu, tex, buf.data()))
				return false;
			outDelta = TdV2Plane(w, h, 0.0);
			for (size_t i = 0; i < pxCount; ++i)
			{
				outDelta.v[i] = static_cast<double>(buf[i * 2u + 0u]);
				const float flRaw = buf[i * 2u + 1u];
				if (std::isfinite(flRaw) && flRaw > 0.5f &&
				    (static_cast<unsigned>(std::llround(static_cast<double>(flRaw))) & 1u) != 0u)
				{
					outAnyBoundEngaged = true;
				}
			}
			return true;
		}

		inline size_t NonNegRoundToSize(float v)
		{
			if (!std::isfinite(v) || v <= 0.0f)
				return 0u;
			return static_cast<size_t>((std::max)(0LL, std::llround(static_cast<double>(v))));
		}

		inline int GroupsFor(int dim, int groupSize)
		{
			return (std::max)(1, (dim + groupSize - 1) / groupSize);
		}

		// Static compile-time geometry defines ONLY (per-frame activeRect/flags/modes live in VpvGpu::Var float UBOs!).
		inline std::string StaticGeomDefines(int w, int h, int cw, int ch, int cellPx)
		{
			std::string s;
			s += VpvGpu::DefineInt("TD_W", w);
			s += VpvGpu::DefineInt("TD_H", h);
			s += VpvGpu::DefineInt("TD_HALF_W", (w + 1) / 2);
			s += VpvGpu::DefineInt("TD_HALF_H", (h + 1) / 2);
			s += VpvGpu::DefineInt("TD_CW", cw);
			s += VpvGpu::DefineInt("TD_CH", ch);
			s += VpvGpu::DefineInt("TD_CELL_PX", cellPx);
			s += VpvGpu::Define("TD_NUM_FLOOR", kEngineNumFloor);
			return s;
		}

		// C++14 SFINAE helpers to extract an optional face/non-rigid mask from FrameProcessInput regardless of field spelling.
		template <int N> struct PriorityTag : PriorityTag<N - 1> {};
		template <> struct PriorityTag<0> {};

		inline const std::vector<uint8_t>* MaskAsPtr(const std::vector<uint8_t>* p) { return p; }
		inline const std::vector<uint8_t>* MaskAsPtr(const std::vector<uint8_t>& v) { return v.empty() ? nullptr : &v; }

		template <typename T>
		inline auto TryGetFaceMaskImpl(const T& x, PriorityTag<3>) -> decltype(MaskAsPtr(x.faceMask))
		{
			return MaskAsPtr(x.faceMask);
		}
		template <typename T>
		inline auto TryGetFaceMaskImpl(const T& x, PriorityTag<2>) -> decltype(MaskAsPtr(x.faceMaskCells))
		{
			return MaskAsPtr(x.faceMaskCells);
		}
		template <typename T>
		inline auto TryGetFaceMaskImpl(const T& x, PriorityTag<1>) -> decltype(MaskAsPtr(x.nonRigidFaceMask))
		{
			return MaskAsPtr(x.nonRigidFaceMask);
		}
		template <typename T>
		inline const std::vector<uint8_t>* TryGetFaceMaskImpl(const T&, PriorityTag<0>)
		{
			return nullptr;
		}
		template <typename T>
		inline const std::vector<uint8_t>* TryGetFaceMask(const T& x)
		{
			return TryGetFaceMaskImpl(x, PriorityTag<3>{});
		}

		template <typename T>
		inline auto TryGetExplicitDefocusedImpl(const T& x, PriorityTag<1>) -> decltype(MaskAsPtr(x.explicitDefocusedCells))
		{
			return MaskAsPtr(x.explicitDefocusedCells);
		}
		template <typename T>
		inline const std::vector<uint8_t>* TryGetExplicitDefocusedImpl(const T&, PriorityTag<0>)
		{
			return nullptr;
		}
		template <typename T>
		inline const std::vector<uint8_t>* TryGetExplicitDefocusedCells(const T& x)
		{
			return TryGetExplicitDefocusedImpl(x, PriorityTag<1>{});
		}

		// Coding step prior resolution mirroring TdV2ReferenceEngine.h 1:1 (H3 / Rule 4: zero silent fallback on invalid explicit prior).
		struct ResolvedA2StepPrior
		{
			double effStepDelta = 0.0;
			double effPhi = 0.0;
			double effZeroFrac = 0.0;
			bool inputNonFinite = false;
			RoundingCellStats stats{};
		};

		inline ResolvedA2StepPrior ResolveA2StepPrior(
			bool hasStepPrior, double estimatedStepDelta, double deadZonePhi, double zeroCoeffFraction,
			StandInRegistry* standIns)
		{
			ResolvedA2StepPrior r{};
			r.effStepDelta = estimatedStepDelta;
			r.effPhi = deadZonePhi;
			r.effZeroFrac = zeroCoeffFraction;
			r.inputNonFinite = !std::isfinite(r.effPhi) || !std::isfinite(r.effZeroFrac);
			if (!hasStepPrior)
			{
				if (!r.inputNonFinite)
				{
					r.effStepDelta = kDefaultStepPriorLsb; // True 1-LSB quantization step in native integer code units
					r.effPhi = kUniformRoundingPhi;        // Unbiased uniform output rounding (phi = 0.5 -> V1 = 1/12)
					r.effZeroFrac = 0.0;                   // Uniform rounding cell without invented pi0
					if (standIns != nullptr)
						standIns->Engage(StandInKind::StepPriorDefault);
				}
				else
				{
					r.effStepDelta = -1.0;
				}
			}
			else
			{
				if (!std::isfinite(r.effStepDelta))
					r.inputNonFinite = true;
				if (r.inputNonFinite || r.effStepDelta <= 0.0 || r.effPhi <= 0.0 || r.effPhi >= 1.0 ||
				    r.effZeroFrac < 0.0 || r.effZeroFrac > 1.0)
				{
					r.effStepDelta = -1.0;
				}
			}
			r.stats = ComputeRoundingCellStats(r.effStepDelta, r.effPhi, r.effZeroFrac);
			return r;
		}

		// Canonical per-frame scratch-arena request list (single source of truth shared by ArenaRequests
		// and EstimatedScratchVramBytes so VRAM accounting can NEVER drift from arena allocations).
		inline std::vector<VpSkeleton::ArenaRequest> BuildArenaRequests(
			int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference)
		{
			std::vector<VpSkeleton::ArenaRequest> reqs;
			if (w <= 0 || h <= 0)
				return reqs;

			const bool compact = (preset == Preset::RealTime);
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const uint32_t uw = static_cast<uint32_t>(w);
			const uint32_t uh = static_cast<uint32_t>(h);
			const uint32_t wHalf = static_cast<uint32_t>((w + 1) / 2);
			const uint32_t hHalf = static_cast<uint32_t>((h + 1) / 2);
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

			if (compact)
			{
				pushReq(uw, uh, VpSkeleton::TexFormat::RG32F, 5);        // curBandsMH, refBandsMH[0..2], deltaScratch (strictly FP32 per plan L60)
				pushReq(wHalf, hHalf, VpSkeleton::TexFormat::RG32F, 5);  // curBandsLD3, refBandsLD3[0..2], bandHalfTmp (FP32)
			}
			else
			{
				pushReq(uw, uh, VpSkeleton::TexFormat::RGBA32F, 5);      // curBands, refBands[0..2], bandTmpH
				pushReq(uw, uh, VpSkeleton::TexFormat::RG32F, 1);        // deltaScratch
			}
			pushReq(uw, uh, VpSkeleton::TexFormat::R32F, 1);             // warpedVal (reused across k=K-1..0)
			pushReq(uw, uh, VpSkeleton::TexFormat::R16, 2);              // refFlags[0..1] (ping-pong packed 3-ref x 3-bit)
			pushReq(3u * cw, ch, VpSkeleton::TexFormat::RGBA32F, 3);     // motionGate, cellWeights, cellDiagC
			pushReq(3u * cw, 3u * ch, VpSkeleton::TexFormat::RGBA32F, 1);// cellStats (Chan-Pebay 10 floats/band/cell)
			pushReq(cw, ch, VpSkeleton::TexFormat::RGBA32F, 1);          // cellFocus (focusFlags[bit0=focusChange,bit1=isDefocused], cellOwnGrainVar, spatialRms, isFace)
			return reqs;
		}
	} // namespace Detail

	// ---- Libplacebo Compute GPU Engine (Stage TD-S5 + TD-S5-OPT) ----
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

		void SetConfig(const Config& cfg)
		{
			if (!cfg.IsEnabled() && hasCarry_)
				ResetCarry();
			cfg_ = cfg;
		}
		const Config& GetConfig() const { return cfg_; }

		void ResetCarry(pl_gpu gpu = nullptr)
		{
			hasCarry_ = false;
			carryW_ = 0;
			carryH_ = 0;
			hasLastSeq_ = false;
			lastSeq_ = 0;
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
					&inRecYTex_, &inCleanYTex_, &inRefCleanTex_, &inCodecModelPbTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_, &inFaceMaskTex_,
					&outYTex_, &outCleanTex_, &outDeltaTex_, &outTdMapsTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_,
					&chromaCodedDeltaTex_, &chromaDelivUvTex_, &chromaDelivRgbTex_,
					&chromaOutUvTex_, &chromaOutRgbTex_, &chromaOutDeltaTex_ })
				{
					if (*t != nullptr)
						pl_tex_destroy(gpu, t);
				}
			}
			else
			{
				for (pl_tex* t : {
					&privCurBands_, &privRefBands_[0], &privRefBands_[1], &privRefBands_[2], &privBandTmpH_,
					&privCurBandsMH_, &privRefBandsMH_[0], &privRefBandsMH_[1], &privRefBandsMH_[2],
					&privCurBandsLD3_, &privRefBandsLD3_[0], &privRefBandsLD3_[1], &privRefBandsLD3_[2], &privBandHalfTmp_,
					&privWarpedVal_, &privRefFlags_[0], &privRefFlags_[1],
					&privDeltaScratch_,
					&privMotionGate_, &privCellWeights_, &privCellDiagC_, &privCellStats_, &privCellFocus_,
					&carryCleanTex_,
					&inRecYTex_, &inCleanYTex_, &inRefCleanTex_, &inCodecModelPbTex_,
					&inMotionVecTex_, &inMotionMetaTex_, &inCellPriorTex_, &inFaceMaskTex_,
					&outYTex_, &outCleanTex_, &outDeltaTex_, &outTdMapsTex_,
					&outDiagFlagsTex_, &outGuardCellTex_, &outWarpDiagTex_,
					&chromaCodedDeltaTex_, &chromaDelivUvTex_, &chromaDelivRgbTex_,
					&chromaOutUvTex_, &chromaOutRgbTex_, &chromaOutDeltaTex_ })
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
		//   Preset::Reference: full-res RGBA32F bands (strict <= 1e-3 10-bit CPU parity).
		//   Preset::RealTime:  compact full-res FP32 RG32F (M,H) + half-res FP32 RG32F (L,D3) (SC-05; strictly FP32 per plan L60!).
		static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(
			pl_gpu gpu, int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference)
		{
			if (!gpu || w <= 0 || h <= 0)
				return {};
			if (!VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F) ||
			    !VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F))
			{
				return {};
			}
			return Detail::BuildArenaRequests(w, h, cellPx, preset);
		}

		// Exact scratch-arena VRAM bytes derived directly from Detail::BuildArenaRequests (single source of truth — zero drift!).
		static uint64_t EstimatedScratchVramBytes(
			int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference)
		{
			uint64_t total = 0;
			for (const VpSkeleton::ArenaRequest& r : Detail::BuildArenaRequests(w, h, cellPx, preset))
				total += VpSkeleton::ShapeBytes(r.shape) * static_cast<uint64_t>(r.count);
			return total;
		}

		// Exact worst-case persistent VRAM bytes for all 6 cached chroma textures allocated by ApplyChromaDeltaGpu
		// at full resolution (w x h) when both YUV and RGB delivery paths are cached on the same engine:
		//   RG32F (3):   chromaCodedDeltaTex_, chromaDelivUvTex_, chromaOutDeltaTex_
		//   RGBA32F (3): chromaOutUvTex_, chromaDelivRgbTex_, chromaOutRgbTex_
		static uint64_t EstimatedChromaVramBytes(int w, int h)
		{
			if (w <= 0 || h <= 0)
				return 0;
			const uint64_t px = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
			const uint64_t bRG32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RG32F);
			const uint64_t bRGBA32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RGBA32F);
			return 3u * px * bRG32 + 3u * px * bRGBA32;
		}

		// Format-aware persistent VRAM bytes for ApplyChromaDeltaGpu on ONE active delivery path at exact coded/delivered
		// dimensions (NEVER discarding true incoming chroma: 4:2:0 budgets 1/4 luma pixels; 4:2:2 budgets 1/2; 4:4:4/RGB
		// budgets full luma resolution for the active path only instead of simultaneous worst-case YUV + RGB 4K!).
		static uint64_t EstimatedChromaVramBytesFor(
			int lumaW, int lumaH, DeliveredChromaFormat deliv, ChromaSubsampling coded)
		{
			if (lumaW <= 0 || lumaH <= 0)
				return 0;
			const int dSubX = DeliveredSubsampleX(deliv);
			const int dSubY = DeliveredSubsampleY(deliv);
			const int cSubX = SubsampleX(coded);
			const int cSubY = SubsampleY(coded);
			const uint64_t dPx = static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaW, dSubX)) *
			                     static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaH, dSubY));
			const uint64_t cPx = static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaW, cSubX)) *
			                     static_cast<uint64_t>(ChromaDetail::SubsampledDim(lumaH, cSubY));
			const uint64_t bRG32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RG32F);
			const uint64_t bRGBA32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RGBA32F);
			// Common coded delta (cW x cH RG32F) + delivered output delta (dW x dH RG32F):
			uint64_t total = cPx * bRG32 + dPx * bRG32;
			if (deliv == DeliveredChromaFormat::Rgb444)
			{
				// Active RGB path: chromaDelivRgbTex_ + chromaOutRgbTex_ (2 x dW x dH RGBA32F).
				total += 2u * dPx * bRGBA32;
			}
			else
			{
				// Active YUV path: chromaDelivUvTex_ (dW x dH RG32F) + chromaOutUvTex_ (dW x dH RGBA32F).
				total += dPx * (bRG32 + bRGBA32);
			}
			return total;
		}

		// Persistent TD-owned VRAM bytes at w x h (persistent carryCleanTex_ + all 8 persistent cell-grid IO/output
		// textures + Reference CPU staging/readback or RealTime fallback internal outputs + optional cached chroma).
		// Strictly excludes ArenaRequests scratch textures (budgeted separately under kUserScratchArena /
		// kUserScratchArenaExtra — zero double-counting!).
		// Note (LOW-1): Preset::RealTime targets zero-copy ProcessFrameGpu; callers invoking CPU-staging ProcessFrame
		// under Preset::RealTime pass includeCpuStagingInputs = true to budget engine-owned in*Tex_ staging planes too.
		static uint64_t EstimatedPersistentVramBytes(
			int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference,
			bool includeChroma = false, bool includeRealTimeInternalOutputs = false,
			bool includeCpuStagingInputs = false)
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

			const bool compact = (preset == Preset::RealTime);
			uint64_t total = 0;
			// Persistent carryCleanTex_ (1 x R32F)
			total += 1u * px * bR32;
			// All 8 cell-grid persistent IO/output textures:
			//   inMotionVecTex_ (3*cells RGBA32F), inMotionMetaTex_ (3*cells RGBA32F: cls, valid, confidence, 0),
			//   outTdMapsTex_ (3*cells RGBA32F), outDiagFlagsTex_ / inCellPriorTex_ (2 * cells RGBA32F),
			//   outGuardCellTex_ / outWarpDiagTex_ (2 * cells RG32F),
			//   inFaceMaskTex_ (cells R32F).
			total += (3u * cells) * (2u * bRGBA32);
			total += (3u * cells) * bRGBA32;
			total += 2u * cells * bRGBA32;
			total += 2u * cells * bRG32;
			total += cells * bR32;
			// Reference preset (or CPU-staging ProcessFrame under RealTime) budgets all 6 full-res R32F staging/output
			// textures (inRecYTex_, inCleanYTex_, inRefCleanTex_, inCodecModelPbTex_, outYTex_, outCleanTex_) +
			// 1 full-res RG32F (outDeltaTex_). Zero-copy RealTime ProcessFrameGpu uses caller-supplied outputs unless
			// includeRealTimeInternalOutputs is true (budgets engine-owned fallback outYTex_, outCleanTex_, outDeltaTex_).
			if (!compact || includeCpuStagingInputs)
				total += 6u * px * bR32 + 1u * px * bRG32;
			else if (includeRealTimeInternalOutputs)
				total += 2u * px * bR32 + 1u * px * bRG32;
			if (includeChroma)
				total += EstimatedChromaVramBytes(w, h);
			return total;
		}

		// Format-aware VRAM governor user declaration budgeting the active chroma delivery/coded format exactly.
		static VpSkeleton::VramUser MakeVramUserFor(
			int lumaW, int lumaH, DeliveredChromaFormat deliv, ChromaSubsampling coded,
			int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference,
			bool includeRealTimeInternalOutputs = false, bool includeCpuStagingInputs = false)
		{
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserTemporalDetail;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserTemporalDetail);
			u.tier = VpSkeleton::Tier::T1;
			u.shedOrder = 0;
			u.lossless = false;
			const uint64_t lumaBytes = EstimatedPersistentVramBytes(
				lumaW, lumaH, cellPx, preset, false, includeRealTimeInternalOutputs, includeCpuStagingInputs);
			const uint64_t chromaBytes = EstimatedChromaVramBytesFor(lumaW, lumaH, deliv, coded);
			if (chromaBytes > 0)
			{
				u.levels.push_back(lumaBytes + chromaBytes);
				u.levelText.push_back(nullptr);
				if (lumaBytes > 0)
				{
					u.levels.push_back(lumaBytes);
					u.levelText.push_back("luma only (GPU chroma shed)");
				}
				u.levels.push_back(0);
				u.levelText.push_back("off (GPU memory shed)");
			}
			else
			{
				u.levels.push_back(lumaBytes);
				u.levelText.push_back(nullptr);
				if (lumaBytes > 0)
				{
					u.levels.push_back(0);
					u.levelText.push_back("off (GPU memory shed)");
				}
			}
			return u;
		}

		// Exact total VRAM working-set estimate in bytes at w x h (persistent TD textures + arena scratch textures).
		static uint64_t EstimatedVramBytes(
			int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference,
			bool includeChroma = false, bool includeRealTimeInternalOutputs = false)
		{
			return EstimatedPersistentVramBytes(w, h, cellPx, preset, includeChroma, includeRealTimeInternalOutputs) +
			       EstimatedScratchVramBytes(w, h, cellPx, preset);
		}

		// VRAM governor user declaration under VpSkeleton::kUserTemporalDetail (sheddable Tier T1).
		// Budgets ONLY persistent TD-owned VRAM so scratch-arena textures are NOT double-counted against kUserScratchArenaExtra!
		// Callers using ApplyChromaDeltaGpu pass includeChroma = true (adds stepped levels [luma+chroma, luma-only, 0]
		// so the governor can shed GPU chroma before luma), and callers invoking ProcessFrameGpu without external
		// output textures pass includeRealTimeInternalOutputs = true so fallback engine-owned outputs are budgeted honestly.
		static VpSkeleton::VramUser MakeVramUser(
			int w, int h, int cellPx = kCanonicalBlockPx, Preset preset = Preset::Reference,
			bool includeChroma = false, bool includeRealTimeInternalOutputs = false)
		{
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserTemporalDetail;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserTemporalDetail);
			u.tier = VpSkeleton::Tier::T1;
			u.shedOrder = 0;
			u.lossless = false;
			const uint64_t lumaBytes = EstimatedPersistentVramBytes(
				w, h, cellPx, preset, false, includeRealTimeInternalOutputs);
			const uint64_t chromaBytes = includeChroma ? EstimatedChromaVramBytes(w, h) : 0;
			if (includeChroma && chromaBytes > 0)
			{
				u.levels.push_back(lumaBytes + chromaBytes);
				u.levelText.push_back(nullptr);
				if (lumaBytes > 0)
				{
					u.levels.push_back(lumaBytes);
					u.levelText.push_back("luma only (GPU chroma shed)");
				}
				u.levels.push_back(0);
				u.levelText.push_back("off (GPU memory shed)");
			}
			else
			{
				u.levels.push_back(lumaBytes);
				u.levelText.push_back(nullptr);
				if (lumaBytes > 0)
				{
					u.levels.push_back(0);
					u.levelText.push_back("off (GPU memory shed)");
				}
			}
			return u;
		}

		// Live allocated byte introspection over ALL 16 persistent luma pl_tex members (exact 1:1 match with Release(gpu)).
		uint64_t AllocatedPersistentLumaVramBytes() const
		{
			uint64_t total = 0;
			for (pl_tex t : {
				carryCleanTex_,
				inRecYTex_, inCleanYTex_, inRefCleanTex_, inCodecModelPbTex_,
				inMotionVecTex_, inMotionMetaTex_, inCellPriorTex_, inFaceMaskTex_,
				outYTex_, outCleanTex_, outDeltaTex_, outTdMapsTex_,
				outDiagFlagsTex_, outGuardCellTex_, outWarpDiagTex_ })
			{
				total += VpSkeleton::TexBytesOf(t);
			}
			return total;
		}

		// Live allocated byte introspection over ALL 6 persistent chroma pl_tex members (exact 1:1 match with Release(gpu)).
		uint64_t AllocatedPersistentChromaVramBytes() const
		{
			uint64_t total = 0;
			for (pl_tex t : {
				chromaCodedDeltaTex_, chromaDelivUvTex_, chromaDelivRgbTex_,
				chromaOutUvTex_, chromaOutRgbTex_, chromaOutDeltaTex_ })
			{
				total += VpSkeleton::TexBytesOf(t);
			}
			return total;
		}

		// Live total allocated persistent VRAM bytes held by this engine (luma + chroma).
		uint64_t AllocatedPersistentVramBytes() const
		{
			return AllocatedPersistentLumaVramBytes() + AllocatedPersistentChromaVramBytes();
		}

		// Zero-copy GPU-resident execution directly on caller-supplied pl_tex planes (ZERO CPU plane upload/download!).
		bool ProcessFrameGpu(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const GpuFrameInput& in,
			GpuFrameOutput& out,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			StandInRegistry* standIns = nullptr)
		{
			out.cellsW = 0;
			out.cellsH = 0;
			out.activeRefCount = 0;
			out.dispatchCount = 0;
			out.guardViolationCells = 0;
			out.focusVetoCells = 0;
			out.faceMaskedCells = 0;
			out.nonFiniteCells = 0;
			out.a2InvalidCells = 0;
			out.singularCells = 0;
			out.singleRefA2Cells = 0;
			out.motionStandInRefs = 0;
			out.borderBilinearPixels = 0;
			out.carryUsed = false;
			out.carryReset = false;
			out.scratchShort = false;
			out.usedExternalBandMemo = false;
			out.externalBandMemoRejected = false;
			out.succeeded = false;
			out.lastError.clear();
			out.tdMaps.clear();

			lastDispatchCount_ = 0;
			scratchShort_ = false;
			lastError_.clear();

			// Mode::Off -> reset any stale carry so Off->On never blends stale history, ZERO GPU dispatches.
			if (!cfg_.IsEnabled())
			{
				if (hasCarry_)
				{
					ResetCarry(gpu);
					out.carryReset = true;
				}
				if (in.w > 0 && in.h > 0)
				{
					const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
					out.cellsW = (in.w + cellPx - 1) / cellPx;
					out.cellsH = (in.h + cellPx - 1) / cellPx;
				}
				out.succeeded = true;
				return true;
			}

			auto failVisible = [&](const std::string& msg, StandInKind kind) -> bool
			{
				lastError_ = msg;
				out.lastError = msg;
				out.succeeded = false;
				++failures_;
				if (standIns != nullptr)
					standIns->Engage(kind, msg.c_str());
				return false;
			};

			if (in.w <= 0 || in.h <= 0 || in.receivedYTex == nullptr)
				return failVisible("TdV2GpuEngine::ProcessFrameGpu invalid dimensions or null receivedYTex", StandInKind::GpuDispatchFailed);

			const int w = in.w;
			const int h = in.h;
			const int cellPx = cfg_.cellPx > 0 ? cfg_.cellPx : kCanonicalBlockPx;
			out.cellsW = (w + cellPx - 1) / cellPx;
			out.cellsH = (h + cellPx - 1) / cellPx;
			const size_t numCells = static_cast<size_t>(out.cellsW) * static_cast<size_t>(out.cellsH);

			if (gpu == nullptr || dispatch == nullptr)
				return failVisible("TdV2GpuEngine::ProcessFrameGpu null pl_gpu or pl_dispatch", StandInKind::GpuDispatchFailed);

			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0) || !in.psdShape.IsValid())
			{
				ResetCarry(gpu);
				out.carryReset = true;
				out.nonFiniteCells = numCells;
				return failVisible(
					!(PeakCodeForBits(cfg_.sourceBits) > 0.0)
						? "TdV2GpuEngine::ProcessFrameGpu invalid sourceBits"
						: "TdV2GpuEngine::ProcessFrameGpu invalid/non-finite psdShape",
					StandInKind::NonFiniteClosure);
			}

			if (cfg_.enableLongAnchor && standIns != nullptr)
				standIns->Engage(StandInKind::LongPairDrift, "enableLongAnchor requested; 1-session long pair pending");

			const bool hasClean = (in.hasCleanSplit && in.cleanYTex != nullptr);
			pl_tex curCleanTex = hasClean ? in.cleanYTex : in.receivedYTex;

			// H6: Strict GPU input/output aliasing check across BOTH seed/cut AND active paths!
			// Never allow the same pl_tex to bind as both sampled input and storage output (prevents D3D11 SRV/UAV unbind
			// and Vulkan cross-workgroup halo races). Also reject distinct output targets aliasing each other.
			{
				const pl_tex inputs[] = {
					in.receivedYTex,
					in.cleanYTex,
					in.prevPeak.cleanTex,
					in.nextPeak.cleanTex,
					in.prevPeak2.cleanTex,
					in.codecModelPbTex,
					in.curLowBandTex,
					carryCleanTex_,
					in.motionVecTex,
					in.motionMetaTex,
					in.cellPriorTex,
					in.faceMaskTex
				};
				const pl_tex outputs[] = {
					out.outputYTex,
					out.outputCleanTex,
					out.tdDeltaTex,
					out.tdMapsTex,
					out.diagFlagsTex,
					out.guardCellTex,
					out.warpDiagTex
				};
				for (pl_tex inT : inputs)
				{
					if (inT == nullptr)
						continue;
					for (pl_tex outT : outputs)
					{
						if (outT != nullptr && inT == outT)
							return failVisible("TdV2GpuEngine::ProcessFrameGpu input/output texture aliasing forbidden", StandInKind::GpuInputOutputAlias);
					}
				}
				const size_t nOut = sizeof(outputs) / sizeof(outputs[0]);
				for (size_t i = 0; i < nOut; ++i)
				{
					if (outputs[i] == nullptr)
						continue;
					for (size_t j = i + 1; j < nOut; ++j)
					{
						if (outputs[j] != nullptr && outputs[i] == outputs[j])
							return failVisible("TdV2GpuEngine::ProcessFrameGpu output/output texture aliasing forbidden", StandInKind::GpuInputOutputAlias);
					}
				}
			}

			// H5 & GPU Hygiene: validate exact dimensions on EVERY caller-supplied GPU input & output texture!
			auto checkTexDim = [](pl_tex tex, int ew, int eh) -> bool
			{
				return tex == nullptr || (tex->params.w == ew && tex->params.h == eh);
			};
			if (!checkTexDim(in.receivedYTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu receivedYTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (hasClean && !checkTexDim(in.cleanYTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu cleanYTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.prevPeak.cleanTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu prevPeak.cleanTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.nextPeak.cleanTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu nextPeak.cleanTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.prevPeak2.cleanTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu prevPeak2.cleanTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.codecModelPbTex, w, h))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu codecModelPbTex dimension mismatch", StandInKind::GpuDispatchFailed);
			if (in.curLowBandTex != nullptr)
			{
				const pl_tex_params& mp = in.curLowBandTex->params;
				const bool fmtOk = (mp.sampleable && mp.format != nullptr &&
				                    mp.format->type == PL_FMT_FLOAT && mp.format->num_components >= 1);
				const int wHalfCheck = (w + 1) / 2;
				const int hHalfCheck = (h + 1) / 2;
				const bool dimFull = (mp.w == w && mp.h == h);
				const bool dimHalf = (cfg_.preset == Preset::RealTime && mp.w == wHalfCheck && mp.h == hHalfCheck);
				if (!fmtOk || !(dimFull || dimHalf))
				{
					out.externalBandMemoRejected = true;
					if (standIns != nullptr)
						standIns->Engage(StandInKind::BandMemoW2, "TdV2GpuEngine::ProcessFrameGpu curLowBandTex invalid dimensions or format");
					return failVisible("TdV2GpuEngine::ProcessFrameGpu curLowBandTex invalid dimensions or format (expected sampleable float w x h, or wHalf x hHalf in RealTime)", StandInKind::GpuDispatchFailed);
				}
			}
			if ((in.motionVecTex != nullptr) != (in.motionMetaTex != nullptr))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu partial prepacked motion (motionVecTex and motionMetaTex must both be supplied)", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.motionVecTex, 3 * out.cellsW, out.cellsH))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu motionVecTex dimension mismatch (expected 3*cellsW x cellsH)", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.motionMetaTex, 3 * out.cellsW, out.cellsH))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu motionMetaTex dimension mismatch (expected 3*cellsW x cellsH)", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.cellPriorTex, out.cellsW, out.cellsH))
				return failVisible("TdV2GpuEngine::ProcessFrameGpu cellPriorTex dimension mismatch (expected cellsW x cellsH)", StandInKind::GpuDispatchFailed);
			if (!checkTexDim(in.faceMaskTex, out.cellsW, out.cellsH))
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::FaceMaskSizeMismatch, "TdV2GpuEngine::ProcessFrameGpu faceMaskTex dimension mismatch (expected cellsW x cellsH)");
				return failVisible("TdV2GpuEngine::ProcessFrameGpu faceMaskTex dimension mismatch (expected cellsW x cellsH)", StandInKind::GpuDispatchFailed);
			}
			if (!checkTexDim(out.outputYTex, w, h) || !checkTexDim(out.outputCleanTex, w, h) || !checkTexDim(out.tdDeltaTex, w, h) ||
			    !checkTexDim(out.tdMapsTex, out.cellsW, 3 * out.cellsH) || !checkTexDim(out.diagFlagsTex, out.cellsW, out.cellsH) ||
			    !checkTexDim(out.guardCellTex, out.cellsW, out.cellsH) || !checkTexDim(out.warpDiagTex, out.cellsW, out.cellsH))
			{
				return failVisible("TdV2GpuEngine::ProcessFrameGpu output texture dimension mismatch", StandInKind::GpuDispatchFailed);
			}

			bool fadeNonFinite = false;
			const double effFadeKeep = ResolveFadeKeep(in.fadeKeep, fadeNonFinite);
			if (fadeNonFinite)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine::ProcessFrameGpu non-finite fadeKeep; failing closed to 0.0");
				out.nonFiniteCells = numCells;
			}
			const bool effectiveCut = in.isCutOrSpanInvalid || (effFadeKeep <= 0.0);

			const bool seqDiscontinuous = hasCarry_ && hasLastSeq_ &&
				!(in.seq == 0 && lastSeq_ == 0) &&
				(in.seq != lastSeq_ + 1);
			if (effectiveCut || seqDiscontinuous || (hasCarry_ && (carryW_ != w || carryH_ != h)))
			{
				ResetCarry(gpu);
				out.carryReset = true;
			}

			if (!hasClean && standIns != nullptr)
				standIns->Engage(StandInKind::P10gGrainSeparator, "no upstream P10g split supplied");

			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR32 || !fmtRG32 || !fmtRGBA32)
			{
				ResetCarry(gpu);
				out.carryReset = true;
				return failVisible("TdV2GpuEngine::ProcessFrameGpu FP32 formats unsupported", StandInKind::GpuDispatchFailed);
			}

			const bool hasPrepackedMotion = (in.motionVecTex != nullptr && in.motionMetaTex != nullptr);
			std::vector<ActiveRefSpec> refSpecs;
			refSpecs.reserve(kMaxGpuRefs);

			auto tryAddGpuRef = [&](pl_tex refTex, bool useCarry, int motionSlot, int refRole, const CellMotionField* mf, bool exactCopy,
			                        const Homography3x3& camH = Homography3x3{}) -> bool
			{
				if (!useCarry)
				{
					if (refTex == nullptr)
						return false;
				}
				else if (!hasCarry_ || carryW_ != w || carryH_ != h || carryCleanTex_ == nullptr)
				{
					return false;
				}
				if (static_cast<int>(refSpecs.size()) >= kMaxGpuRefs)
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::TierSteppedDown, "TdV2GpuEngine 3-ref slot cap: 4th ref omitted");
					return false;
				}

				ActiveRefSpec spec;
				spec.gpuCleanTex = useCarry ? carryCleanTex_ : refTex;
				spec.useGpuCarry = useCarry;
				spec.motionSlot = motionSlot;
				spec.refRole = refRole;
				spec.isExactCopy = exactCopy;
				spec.cameraHomography = camH;

				if (!hasPrepackedMotion)
				{
					if (mf == nullptr)
					{
						// H9 (plan s1.2 L108 L4): NEVER synthesize whole-frame Still blend when motion field is missing!
						++out.motionStandInRefs;
						if (standIns != nullptr)
						{
							standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "null motion field; ref skipped (no whole-frame Still blend)");
							standIns->Engage(StandInKind::MotionFieldStandIn, "null motion field; ref skipped");
						}
						return false;
					}
					else if (!mf->IsWellFormed(out.cellsW, out.cellsH))
					{
						++out.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "dimension-mismatched motion field; ref rejected");
						return false;
					}
					else
					{
						spec.motion = *mf;
					}
				}
				refSpecs.push_back(std::move(spec));
				return true;
			};

			auto pickUnusedSlot = [&](int preferred0, int preferred1, int preferred2) -> int
			{
				const int prefs[3] = { preferred0, preferred1, preferred2 };
				for (int s : prefs)
				{
					bool used = false;
					for (const auto& rs : refSpecs)
						if (rs.motionSlot == s) { used = true; break; }
					if (!used) return s;
				}
				return preferred0;
			};

			if (!effectiveCut)
			{
				if (in.prevPeak.cleanTex != nullptr)
					tryAddGpuRef(in.prevPeak.cleanTex, false, 0, 0, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent, in.prevPeak.cameraHomography);
				if (in.nextPeak.cleanTex != nullptr)
					tryAddGpuRef(in.nextPeak.cleanTex, false, 1, 1, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent, in.nextPeak.cameraHomography);
				if (in.prevPeak2.cleanTex != nullptr)
				{
					if (hasPrepackedMotion)
					{
						// H5 fixed-slot contract: 3-slot prepacked motionVecTex (0=prev, 1=next, 2=carry) has no slot for prevPeak2.
						++out.motionStandInRefs;
						if (standIns != nullptr)
							standIns->Engage(StandInKind::MotionFieldStandIn, "ProcessFrameGpu 3-slot prepacked motionVecTex has no fixed slot for prevPeak2; prevPeak2 skipped");
					}
					else
					{
						const int p2Slot = pickUnusedSlot(1, 2, 0);
						tryAddGpuRef(in.prevPeak2.cleanTex, false, p2Slot, 3, in.prevPeak2.motion, in.prevPeak2.isExactCopyOfCurrent, in.prevPeak2.cameraHomography);
					}
				}
				if (cfg_.enableCarryForward && hasCarry_ && carryW_ == w && carryH_ == h)
				{
					const int cSlot = hasPrepackedMotion ? 2 : pickUnusedSlot(2, 1, 0);
					if (tryAddGpuRef(nullptr, true, cSlot, 2, in.hasCarryMotion ? &in.carryMotion : nullptr, false))
						out.carryUsed = true;
				}
			}

			out.activeRefCount = refSpecs.size();

			// Shared zero-copy passthrough helper for seed/cut (no refs) and invalid A2 step-prior early closure:
			// copies curClean into carryCleanTex_ + caller output targets and zeroes cell targets cleanly (ZERO ScratchArena).
			// LOW-1: binds ONLY active target descriptors via compile-time guards so Mode::Shadow / partial-output RealTime
			// callers NEVER allocate wasted full-res outYTex_ / outCleanTex_ / outDeltaTex_ dummy textures!
			auto runZeroCopyPassthrough = [&](float mapsLightGain) -> bool
			{
				if (!Detail::EnsureTex(gpu, carryCleanTex_, w, h, fmtR32, true, true, true, false))
				{
					ResetCarry(gpu);
					out.carryReset = true;
					return failVisible("TdV2GpuEngine::ProcessFrameGpu carry seed alloc failed", StandInKind::GpuDispatchFailed);
				}
				const bool copyClean = (out.outputCleanTex != nullptr);
				const bool copyOutY = cfg_.ModifiesPixels() && (out.outputYTex != nullptr);
				const bool zeroDelta = (out.tdDeltaTex != nullptr);
				const bool distinctRec = copyOutY && (curCleanTex != in.receivedYTex);

				std::string hdr = Detail::StaticGeomDefines(w, h, out.cellsW, out.cellsH, cellPx);
				hdr += VpvGpu::DefineInt("TD_SEED_HAS_CLEAN", copyClean ? 1 : 0);
				hdr += VpvGpu::DefineInt("TD_SEED_HAS_OUT_Y", copyOutY ? 1 : 0);
				hdr += VpvGpu::DefineInt("TD_SEED_HAS_DELTA", zeroDelta ? 1 : 0);
				hdr += VpvGpu::DefineInt("TD_SEED_DISTINCT_REC", distinctRec ? 1 : 0);
				hdr += Shaders::kCoreHelpers;

				pl_shader_desc d[6]{};
				int nDescs = 0;
				d[nDescs++] = VpvGpu::Sampled("src_clean", curCleanTex);
				if (distinctRec)
					d[nDescs++] = VpvGpu::Sampled("src_rec", in.receivedYTex);
				d[nDescs++] = VpvGpu::Storage("dst_carry", carryCleanTex_);
				if (copyClean)
					d[nDescs++] = VpvGpu::Storage("dst_clean", out.outputCleanTex);
				if (copyOutY)
					d[nDescs++] = VpvGpu::Storage("dst_out_y", out.outputYTex);
				if (zeroDelta)
					d[nDescs++] = VpvGpu::Storage("dst_delta", out.tdDeltaTex);

				if (!VpvGpu::Pass(dispatch, "td_v2_p0_seed_copy", hdr, Shaders::kPassSeedCopy,
				                  d, nDescs, nullptr, 0, Detail::GroupsFor(w, 16), Detail::GroupsFor(h, 16), 0, 16, 16))
				{
					ResetCarry(gpu);
					out.carryReset = true;
					return failVisible("td_v2_p0_seed_copy dispatch failed", StandInKind::GpuDispatchFailed);
				}
				int seedDispatches = 1;

				// Explicitly zero any caller-supplied cell-grid output targets so stale GPU memory never persists.
				const bool zeroMaps = (out.tdMapsTex != nullptr);
				const bool zeroDiag = (out.diagFlagsTex != nullptr);
				const bool zeroGuard = (out.guardCellTex != nullptr);
				const bool zeroWarpDiag = (out.warpDiagTex != nullptr);
				if (zeroMaps || zeroDiag || zeroGuard || zeroWarpDiag)
				{
					pl_tex dstMaps = zeroMaps ? out.tdMapsTex : outTdMapsTex_;
					pl_tex dstDiag = zeroDiag ? out.diagFlagsTex : outDiagFlagsTex_;
					pl_tex dstGuard = zeroGuard ? out.guardCellTex : outGuardCellTex_;
					pl_tex dstWarp = zeroWarpDiag ? out.warpDiagTex : outWarpDiagTex_;
					if (!dstMaps && !Detail::EnsureTex(gpu, outTdMapsTex_, out.cellsW, 3 * out.cellsH, fmtRGBA32, true, true, false, true))
						return failVisible("TdV2GpuEngine::ProcessFrameGpu seed tdMaps target alloc failed", StandInKind::GpuDispatchFailed);
					if (!dstMaps) dstMaps = outTdMapsTex_;
					if (!dstDiag && !Detail::EnsureTex(gpu, outDiagFlagsTex_, out.cellsW, out.cellsH, fmtRGBA32, true, true, false, true))
						return failVisible("TdV2GpuEngine::ProcessFrameGpu seed diag target alloc failed", StandInKind::GpuDispatchFailed);
					if (!dstDiag) dstDiag = outDiagFlagsTex_;
					if (!dstGuard && !Detail::EnsureTex(gpu, outGuardCellTex_, out.cellsW, out.cellsH, fmtRG32, true, true, false, true))
						return failVisible("TdV2GpuEngine::ProcessFrameGpu seed guard target alloc failed", StandInKind::GpuDispatchFailed);
					if (!dstGuard) dstGuard = outGuardCellTex_;
					if (!dstWarp && !Detail::EnsureTex(gpu, outWarpDiagTex_, out.cellsW, out.cellsH, fmtRG32, true, true, false, true))
						return failVisible("TdV2GpuEngine::ProcessFrameGpu seed warpDiag target alloc failed", StandInKind::GpuDispatchFailed);
					if (!dstWarp) dstWarp = outWarpDiagTex_;

					const float uZeroMaps = zeroMaps ? 1.0f : 0.0f;
					const float uZeroDiag = zeroDiag ? 1.0f : 0.0f;
					const float uZeroGuard = zeroGuard ? 1.0f : 0.0f;
					const float uZeroWarp = zeroWarpDiag ? 1.0f : 0.0f;
					const float uMapsLightGain = mapsLightGain;
					pl_shader_desc dz[4] = {
						VpvGpu::Storage("dst_td_maps", dstMaps),
						VpvGpu::Storage("dst_diag", dstDiag),
						VpvGpu::Storage("dst_guard", dstGuard),
						VpvGpu::Storage("dst_warp_diag", dstWarp)
					};
					pl_shader_var vz[5] = {
						VpvGpu::Var("u_zero_maps", &uZeroMaps),
						VpvGpu::Var("u_zero_diag", &uZeroDiag),
						VpvGpu::Var("u_zero_guard", &uZeroGuard),
						VpvGpu::Var("u_zero_warp_diag", &uZeroWarp),
						VpvGpu::Var("u_maps_light_gain", &uMapsLightGain)
					};
					if (!VpvGpu::Pass(dispatch, "td_v2_p0_seed_zero_cells",
					                  Detail::StaticGeomDefines(w, h, out.cellsW, out.cellsH, cellPx),
					                  Shaders::kPassSeedZeroCells, dz, 4, vz, 5,
					                  Detail::GroupsFor(out.cellsW, 8), Detail::GroupsFor(out.cellsH, 8), 0, 8, 8))
					{
						ResetCarry(gpu);
						out.carryReset = true;
						return failVisible("td_v2_p0_seed_zero_cells dispatch failed", StandInKind::GpuDispatchFailed);
					}
					++seedDispatches;
				}

				TdMapsView::Block zeroBlock{};
				zeroBlock.f[TdMapsView::kLightGain] = mapsLightGain;
				out.tdMaps.assign(numCells, zeroBlock);
				out.dispatchCount = seedDispatches;
				lastDispatchCount_ = seedDispatches;
				hasCarry_ = true;
				carryW_ = w;
				carryH_ = h;
				hasLastSeq_ = true;
				lastSeq_ = in.seq;
				out.succeeded = true;
				return true;
			};

			// Seed / cut frame with no references: GPU-only copy into carryCleanTex_ + zero targets.
			if (refSpecs.empty())
			{
				if (in.faceMask != nullptr && !in.faceMask->empty())
				{
					if (in.faceMask->size() != numCells && standIns != nullptr)
					{
						standIns->Engage(
							StandInKind::FaceMaskSizeMismatch,
							in.faceMask->size() < numCells
								? "TdV2GpuEngine::ProcessFrameGpu faceMask undersized vs cell grid; masking trailing cells fail-closed"
								: "TdV2GpuEngine::ProcessFrameGpu faceMask oversized vs cell grid");
					}
					for (size_t i = 0; i < numCells; ++i)
						if (i >= in.faceMask->size() || (*in.faceMask)[i] != 0u) ++out.faceMaskedCells;
				}
				if ((in.faceMaskTex != nullptr || out.faceMaskedCells > 0) && standIns != nullptr)
					standIns->Engage(StandInKind::FaceNonRigidMask);
				return runZeroCopyPassthrough(0.0f);
			}

			// Validate coding step prior upfront (mirrors TdV2ReferenceEngine.h !a2Stats.valid early closure 1:1).
			const Detail::ResolvedA2StepPrior a2 = Detail::ResolveA2StepPrior(
				in.hasStepPrior, in.estimatedStepDelta, in.deadZonePhi, in.zeroCoeffFraction, standIns);
			if (!a2.stats.valid)
			{
				if (standIns != nullptr)
				{
					if (a2.inputNonFinite)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine non-finite A2 step/phi stats");
					standIns->Engage(StandInKind::A2StatsInvalid, "invalid stepDelta/deadZonePhi/zeroFrac; delta closed to 0");
				}
				out.a2InvalidCells = numCells;
				if (a2.inputNonFinite)
					out.nonFiniteCells = numCells;
				return runZeroCopyPassthrough(1.0f);
			}

			CoreDispatchParams p{};
			p.seq = in.seq;
			p.w = w;
			p.h = h;
			p.cellsW = out.cellsW;
			p.cellsH = out.cellsH;
			p.cellPx = cellPx;
			p.grainCellPx = cfg_.grainCellPx > 0 ? cfg_.grainCellPx : cellPx;
			p.active = in.activeRect.Empty() ? ActiveRect::Full(w, h) : in.activeRect.ClampedTo(w, h);
			p.recYTex = in.receivedYTex;
			p.curCleanTex = curCleanTex;
			p.hasCleanSplit = hasClean;
			p.motionVecTex = hasPrepackedMotion ? in.motionVecTex : nullptr;
			p.motionMetaTex = hasPrepackedMotion ? in.motionMetaTex : nullptr;
			p.cellPriorTex = in.cellPriorTex;
			p.faceMaskTex = in.faceMaskTex;
			p.codecModelPbTex = in.codecModelPbTex;
			p.curLowBandTex = in.curLowBandTex;
			p.retainedGrainVar = &in.retainedGrainVar;
			p.cellStepScale = &in.cellStepScale;
			p.explicitFocusChangeCells = in.explicitFocusChangeCells;
			p.explicitDefocusedCells = in.explicitDefocusedCells;
			p.faceMask = in.faceMask;
			p.a2 = a2;
			p.psdShape = in.psdShape;
			p.fadeKeep = effFadeKeep;
			p.transformIdentified8x8 = in.transformIdentified8x8;
			p.outYTex = out.outputYTex;
			p.outCleanTex = out.outputCleanTex;
			p.outDeltaTex = out.tdDeltaTex;
			p.outTdMapsTex = out.tdMapsTex;
			p.outDiagFlagsTex = out.diagFlagsTex;
			p.outGuardCellTex = out.guardCellTex;
			p.outWarpDiagTex = out.warpDiagTex;
			p.readbackCellDiag = out.readbackCellDiagnostics;

			CoreDispatchOutcome oc{};
			if (!ExecuteGpuCore(gpu, dispatch, p, refSpecs, arena, oc, standIns))
			{
				out.carryReset = out.carryReset || oc.carryReset;
				out.scratchShort = oc.scratchShort;
				out.lastError = lastError_;
				out.succeeded = false;
				return false;
			}

			out.dispatchCount = oc.dispatches;
			out.guardViolationCells = oc.guardViolationCells;
			out.focusVetoCells = oc.focusVetoCells;
			out.faceMaskedCells = oc.faceMaskedCells;
			out.nonFiniteCells += oc.nonFiniteCells;
			out.singularCells = oc.singularCells;
			out.singleRefA2Cells = oc.singleRefA2Cells;
			out.borderBilinearPixels = oc.borderBilinearPixels;
			out.usedExternalBandMemo = oc.usedExternalBandMemo;
			if (out.readbackCellDiagnostics)
				out.tdMaps = std::move(oc.tdMaps);
			out.succeeded = true;
			return true;
		}

		// Execute full TD v2 4-pass compute pipeline on CPU TdV2Plane inputs (routes through shared ExecuteGpuCore!).
		GpuProcessResult ProcessFrame(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const FrameProcessInput& in,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			StandInRegistry* standIns = nullptr,
			const std::vector<uint8_t>* faceMaskOverride = nullptr)
		{
			GpuProcessResult res;
			lastDispatchCount_ = 0;
			scratchShort_ = false;
			lastError_.clear();

			const int w = in.receivedY.w;
			const int h = in.receivedY.h;
			res.outputY = in.receivedY;

			// Mode::Off -> reset any stale carry so Off->On never blends stale history, ZERO GPU dispatches, byte-identical passthrough.
			if (!cfg_.IsEnabled())
			{
				if (hasCarry_)
				{
					ResetCarry(gpu);
					res.carryReset = true;
				}
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

			if (!(PeakCodeForBits(cfg_.sourceBits) > 0.0) || !in.psdShape.IsValid())
			{
				ResetCarry(gpu);
				res.carryReset = true;
				res.nonFiniteCells = numCells;
				return failVisible(
					!(PeakCodeForBits(cfg_.sourceBits) > 0.0)
						? "TdV2GpuEngine invalid sourceBits"
						: "TdV2GpuEngine invalid/non-finite psdShape",
					StandInKind::NonFiniteClosure);
			}

			if (cfg_.enableLongAnchor && standIns != nullptr)
				standIns->Engage(StandInKind::LongPairDrift, "enableLongAnchor requested; 1-session long pair pending");

			bool fadeNonFinite = false;
			const double effFadeKeep = ResolveFadeKeep(in.fadeKeep, fadeNonFinite);
			if (fadeNonFinite)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine non-finite fadeKeep; failing closed to 0.0");
				res.nonFiniteCells = numCells;
			}
			const bool effectiveCut = in.isCutOrSpanInvalid || (effFadeKeep <= 0.0);

			// Cut / invalid span, fadeKeep <= 0, resolution change, seek, or non-consecutive frame sequence resets recurrent carry cleanly.
			const bool seqDiscontinuous = hasCarry_ && hasLastSeq_ &&
				!(in.seq == 0 && lastSeq_ == 0) &&
				(in.seq != lastSeq_ + 1);
			if (effectiveCut || seqDiscontinuous || (hasCarry_ && (carryW_ != w || carryH_ != h)))
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

			// Layer 1: Grain-free clean validation (ownGrain = recY - curClean is computed on GPU in fused Pass 4!).
			const bool hasCleanSplit = (in.hasCleanSplit && !in.cleanY.Empty() && in.cleanY.w == w && in.cleanY.h == h);
			if (hasCleanSplit)
			{
				for (double cv : in.cleanY.v)
				{
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
				}
				res.outputClean = in.cleanY;
			}
			else
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::P10gGrainSeparator, "no upstream P10g split supplied");
				res.outputClean = in.receivedY;
			}

			const TdV2Plane& curCleanPlane = hasCleanSplit ? in.cleanY : in.receivedY;

			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR32 || !fmtRG32 || !fmtRGBA32)
			{
				const bool hadCarry = hasCarry_;
				ResetCarry(gpu);
				res.carryReset = res.carryReset || hadCarry;
				return failVisible("TdV2GpuEngine required FP32 texture formats unsupported on pl_gpu", StandInKind::GpuDispatchFailed);
			}

			// Assemble active references BEFORE acquiring ScratchArena or full IO textures so seed/cut frames
			// (refSpecs.empty(), 0 dispatches) never needlessly allocate scratch or trip GpuScratchShortage (M3).
			std::vector<ActiveRefSpec> refSpecs;
			refSpecs.reserve(kMaxGpuRefs);

			auto tryAddRefSpec = [&](const TdV2Plane* refClean, bool useCarry, int motionSlot, int refRole, const CellMotionField* mf, bool exactCopy,
			                         const Homography3x3& camH = Homography3x3{}) -> bool
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
				if (static_cast<int>(refSpecs.size()) >= kMaxGpuRefs)
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::TierSteppedDown, "TdV2GpuEngine 3-ref slot cap: 4th ref omitted");
					return false;
				}

				ActiveRefSpec spec;
				spec.cpuClean = refClean;
				spec.gpuCleanTex = useCarry ? carryCleanTex_ : nullptr;
				spec.useGpuCarry = useCarry;
				spec.motionSlot = motionSlot;
				spec.refRole = refRole;
				spec.isExactCopy = exactCopy;
				spec.cameraHomography = camH;

				if (mf == nullptr)
				{
					// H9 (plan s1.2 L108 L4): NEVER synthesize whole-frame Still blend when motion field is missing!
					++res.motionStandInRefs;
					if (standIns != nullptr)
					{
						standIns->Engage(StandInKind::MissingMotionNoWholeBlend, "null motion field; ref skipped (no whole-frame Still blend)");
						standIns->Engage(StandInKind::MotionFieldStandIn, "null motion field; ref skipped");
					}
					return false;
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

			auto pickUnusedSlot = [&](int preferred0, int preferred1, int preferred2) -> int
			{
				const int prefs[3] = { preferred0, preferred1, preferred2 };
				for (int s : prefs)
				{
					bool used = false;
					for (const auto& rs : refSpecs)
						if (rs.motionSlot == s) { used = true; break; }
					if (!used) return s;
				}
				return preferred0;
			};

			if (!effectiveCut)
			{
				if (in.prevPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.prevPeak.cleanPlane, false, 0, 0, in.prevPeak.motion, in.prevPeak.isExactCopyOfCurrent, in.prevPeak.cameraHomography);
				if (in.nextPeak.cleanPlane != nullptr)
					tryAddRefSpec(in.nextPeak.cleanPlane, false, 1, 1, in.nextPeak.motion, in.nextPeak.isExactCopyOfCurrent, in.nextPeak.cameraHomography);
				if (in.prevPeak2.cleanPlane != nullptr)
				{
					const int p2Slot = pickUnusedSlot(1, 2, 0);
					tryAddRefSpec(in.prevPeak2.cleanPlane, false, p2Slot, 3, in.prevPeak2.motion, in.prevPeak2.isExactCopyOfCurrent, in.prevPeak2.cameraHomography);
				}
				if (cfg_.enableCarryForward && hasCarry_ && carryW_ == w && carryH_ == h)
				{
					const int cSlot = pickUnusedSlot(2, 1, 0);
					if (tryAddRefSpec(nullptr, true, cSlot, 2, in.hasCarryMotion ? &in.carryMotion : nullptr, false))
						res.carryUsed = true;
				}
			}

			const std::vector<uint8_t>* effFaceMask = (faceMaskOverride != nullptr) ? faceMaskOverride : Detail::TryGetFaceMask(in);

			res.activeRefCount = refSpecs.size();

			// First frame / cut frame with no references: seed ONLY carryCleanTex_ directly (0 dispatches, 0 scratch).
			if (refSpecs.empty())
			{
				if (!Detail::EnsureTex(gpu, carryCleanTex_, w, h, fmtR32, true, true, true, false) ||
				    !Detail::UploadPlaneR32F(gpu, carryCleanTex_, curCleanPlane))
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine carry seed upload failed", StandInKind::GpuDispatchFailed);
				}
				if (effFaceMask != nullptr && !effFaceMask->empty())
				{
					if (effFaceMask->size() != numCells && standIns != nullptr)
					{
						standIns->Engage(
							StandInKind::FaceMaskSizeMismatch,
							effFaceMask->size() < numCells
								? "TdV2GpuEngine faceMask undersized vs cell grid; masking trailing cells fail-closed"
								: "TdV2GpuEngine faceMask oversized vs cell grid");
					}
					for (size_t i = 0; i < numCells; ++i)
						if (i >= effFaceMask->size() || (*effFaceMask)[i] != 0u) ++res.faceMaskedCells;
					if (res.faceMaskedCells > 0 && standIns != nullptr)
						standIns->Engage(StandInKind::FaceNonRigidMask);
				}
				hasCarry_ = true;
				carryW_ = w;
				carryH_ = h;
				hasLastSeq_ = true;
				lastSeq_ = in.seq;
				res.succeeded = true;
				return res;
			}

			// Validate coding step prior upfront (mirrors TdV2ReferenceEngine.h !a2Stats.valid early closure 1:1).
			const Detail::ResolvedA2StepPrior a2 = Detail::ResolveA2StepPrior(
				in.hasStepPrior, in.estimatedStepDelta, in.deadZonePhi, in.zeroCoeffFraction, standIns);
			if (!a2.stats.valid)
			{
				if (standIns != nullptr)
				{
					if (a2.inputNonFinite)
						standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine non-finite A2 step/phi stats");
					standIns->Engage(StandInKind::A2StatsInvalid, "invalid stepDelta/deadZonePhi/zeroFrac; delta closed to 0");
				}
				res.a2InvalidCells = numCells;
				if (a2.inputNonFinite)
					res.nonFiniteCells = numCells;
				for (size_t ci = 0; ci < numCells; ++ci)
				{
					res.tdMaps[ci] = TdMapsView::Block{};
					res.tdMaps[ci].f[TdMapsView::kLightGain] = 1.0f;
				}
				if (!Detail::EnsureTex(gpu, carryCleanTex_, w, h, fmtR32, true, true, true, false) ||
				    !Detail::UploadPlaneR32F(gpu, carryCleanTex_, curCleanPlane))
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine carry seed upload failed on invalid A2 stats", StandInKind::GpuDispatchFailed);
				}
				hasCarry_ = true;
				carryW_ = w;
				carryH_ = h;
				hasLastSeq_ = true;
				lastSeq_ = in.seq;
				res.succeeded = true;
				return res;
			}

			// Upload current frame inputs (inOwnGrainTex_ eliminated: computed on GPU as recY - curClean!).
			if (!Detail::EnsureTex(gpu, inRecYTex_, w, h, fmtR32, true, false, true, false) ||
			    !Detail::UploadPlaneR32F(gpu, inRecYTex_, in.receivedY))
			{
				const bool hadCarry = hasCarry_;
				ResetCarry(gpu);
				res.carryReset = res.carryReset || hadCarry;
				return failVisible("TdV2GpuEngine receivedY upload failed", StandInKind::GpuDispatchFailed);
			}
			pl_tex curCleanTex = inRecYTex_;
			if (hasCleanSplit)
			{
				if (!Detail::EnsureTex(gpu, inCleanYTex_, w, h, fmtR32, true, false, true, false) ||
				    !Detail::UploadPlaneR32F(gpu, inCleanYTex_, curCleanPlane))
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine cleanY upload failed", StandInKind::GpuDispatchFailed);
				}
				curCleanTex = inCleanYTex_;
			}

			// Optional explicit Field B CodecModel prediction P_B (M1: validate dimensions & upload to GPU).
			pl_tex codecModelPbTex = nullptr;
			if (in.codecModelPb != nullptr)
			{
				if (in.codecModelPb->Empty() || in.codecModelPb->w != w || in.codecModelPb->h != h)
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine codecModelPb dimension mismatch", StandInKind::GpuDispatchFailed);
				}
				if (!Detail::EnsureTex(gpu, inCodecModelPbTex_, w, h, fmtR32, true, false, true, false) ||
				    !Detail::UploadPlaneR32F(gpu, inCodecModelPbTex_, *in.codecModelPb))
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine codecModelPb upload failed", StandInKind::GpuDispatchFailed);
				}
				codecModelPbTex = inCodecModelPbTex_;
			}

			// Ensure host-readable output staging textures for ProcessFrame readback.
			if (!Detail::EnsureTex(gpu, outYTex_, w, h, fmtR32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outCleanTex_, w, h, fmtR32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outDeltaTex_, w, h, fmtRG32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outTdMapsTex_, res.cellsW, 3 * res.cellsH, fmtRGBA32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outDiagFlagsTex_, res.cellsW, res.cellsH, fmtRGBA32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outGuardCellTex_, res.cellsW, res.cellsH, fmtRG32, true, true, false, true) ||
			    !Detail::EnsureTex(gpu, outWarpDiagTex_, res.cellsW, res.cellsH, fmtRG32, true, true, false, true))
			{
				const bool hadCarry = hasCarry_;
				ResetCarry(gpu);
				res.carryReset = res.carryReset || hadCarry;
				return failVisible("TdV2GpuEngine output texture allocation failed", StandInKind::GpuDispatchFailed);
			}

			CoreDispatchParams p{};
			p.seq = in.seq;
			p.w = w;
			p.h = h;
			p.cellsW = res.cellsW;
			p.cellsH = res.cellsH;
			p.cellPx = cellPx;
			p.grainCellPx = grainCellPx;
			p.active = active;
			p.recYTex = inRecYTex_;
			p.curCleanTex = curCleanTex;
			p.hasCleanSplit = hasCleanSplit;
			p.retainedGrainVar = &in.retainedGrainVar;
			p.cellStepScale = &in.cellStepScale;
			p.explicitFocusChangeCells = in.explicitFocusChangeCells;
			p.explicitDefocusedCells = Detail::TryGetExplicitDefocusedCells(in);
			p.faceMask = effFaceMask;
			p.codecModelPbTex = codecModelPbTex;
			p.a2 = a2;
			p.psdShape = in.psdShape;
			p.fadeKeep = effFadeKeep;
			p.transformIdentified8x8 = in.transformIdentified8x8;
			p.outYTex = outYTex_;
			p.outCleanTex = outCleanTex_;
			p.outDeltaTex = outDeltaTex_;
			p.outTdMapsTex = outTdMapsTex_;
			p.outDiagFlagsTex = outDiagFlagsTex_;
			p.outGuardCellTex = outGuardCellTex_;
			p.outWarpDiagTex = outWarpDiagTex_;
			p.readbackCellDiag = true;

			CoreDispatchOutcome oc{};
			if (!ExecuteGpuCore(gpu, dispatch, p, refSpecs, arena, oc, standIns))
			{
				res.carryReset = res.carryReset || oc.carryReset;
				res.scratchShort = oc.scratchShort;
				res.lastError = lastError_;
				res.succeeded = false;
				return res;
			}

			res.dispatchCount = oc.dispatches;
			res.guardViolationCells = oc.guardViolationCells;
			res.focusVetoCells = oc.focusVetoCells;
			res.faceMaskedCells = oc.faceMaskedCells;
			res.nonFiniteCells = oc.nonFiniteCells;
			res.singularCells = oc.singularCells;
			res.singleRefA2Cells = oc.singleRefA2Cells;
			res.borderBilinearPixels = oc.borderBilinearPixels;
			res.tdMaps = std::move(oc.tdMaps);

			// Download full-res planes (Mode::Shadow skips outYTex_ download since outputY == receivedY bit-exact!).
			bool anyBoundEngaged = false;
			if (cfg_.ModifiesPixels())
			{
				TdV2Plane gpuOutY;
				if (!Detail::DownloadPlaneR32F(gpu, outYTex_, w, h, gpuOutY))
				{
					const bool hadCarry = hasCarry_;
					ResetCarry(gpu);
					res.carryReset = res.carryReset || hadCarry;
					return failVisible("TdV2GpuEngine outputY download failed", StandInKind::GpuDispatchFailed);
				}
				res.outputY = std::move(gpuOutY);
			}
			if (!Detail::DownloadPlaneR32F(gpu, outCleanTex_, w, h, res.outputClean) ||
			    !Detail::DownloadDeltaRG32F(gpu, outDeltaTex_, w, h, res.tdDelta, anyBoundEngaged))
			{
				const bool hadCarry = hasCarry_;
				ResetCarry(gpu);
				res.carryReset = res.carryReset || hadCarry;
				return failVisible("TdV2GpuEngine outputClean/tdDelta download failed", StandInKind::GpuDispatchFailed);
			}

			if (anyBoundEngaged && standIns != nullptr)
				standIns->Engage(StandInKind::TransformUnknownBound);

			res.succeeded = true;
			return res;
		}

		// Execute Chroma GPU Delta Apply Pass (cached member textures + 0-dispatch fast path for finite all-zero delta!).
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
			if (!std::isfinite(in.siting.shiftX) || !std::isfinite(in.siting.shiftY))
				return failChroma("ApplyChromaDeltaGpu non-finite siting shift", StandInKind::NonFiniteClosure);

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

			const ActiveRect lumaActive = in.activeRect.Empty()
				? ActiveRect::Full(in.lumaW, in.lumaH)
				: in.activeRect.ClampedTo(in.lumaW, in.lumaH);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, cSubX, cSubY, cW, cH);
			const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, in.lumaW, in.lumaH, dSubX, dSubY, dW, dH);
			const bool identityGrid = (in.codedSubsampling == delivSub && cW == dW && cH == dH);

			// Scan active coded delta; note !(NaN == 0.0) is true, so non-finite coded delta forces allZeroDelta = false.
			bool allZeroDelta = true;
			for (int cy = cActive.y0; cy < cActive.y1 && allZeroDelta; ++cy)
			{
				for (int cx = cActive.x0; cx < cActive.x1; ++cx)
				{
					const size_t i = static_cast<size_t>(cy) * static_cast<size_t>(cW) + static_cast<size_t>(cx);
					if (!(in.codedDeltaCb.v[i] == 0.0) || !(in.codedDeltaCr.v[i] == 0.0))
					{
						allZeroDelta = false;
						break;
					}
				}
			}

			// Speedup (7): when allZeroDelta is true, verify delivered pixels are all finite so M-NEW-1 NaN closure
			// is NEVER skipped; when finite too, return bit-exact passthrough with 0 GPU dispatches!
			if (allZeroDelta)
			{
				bool allDeliveredFinite = true;
				if (isRgb)
				{
					for (size_t i = 0; i < in.deliveredR.v.size(); ++i)
					{
						if (!std::isfinite(in.deliveredR.v[i]) ||
						    !std::isfinite(in.deliveredG.v[i]) ||
						    !std::isfinite(in.deliveredB.v[i]))
						{
							allDeliveredFinite = false;
							break;
						}
					}
				}
				else
				{
					for (size_t i = 0; i < in.deliveredCb.v.size(); ++i)
					{
						if (!std::isfinite(in.deliveredCb.v[i]) || !std::isfinite(in.deliveredCr.v[i]))
						{
							allDeliveredFinite = false;
							break;
						}
					}
				}

				if (allDeliveredFinite)
				{
					if (isRgb)
					{
						res.outR = in.deliveredR;
						res.outG = in.deliveredG;
						res.outB = in.deliveredB;
					}
					else
					{
						res.outCb = in.deliveredCb;
						res.outCr = in.deliveredCr;
					}
					res.deliveredDeltaCb = TdV2Plane(dW, dH, 0.0);
					res.deliveredDeltaCr = TdV2Plane(dW, dH, 0.0);
					res.dispatchCount = 0;
					res.succeeded = true;
					return res;
				}
			}

			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtRG32 || !fmtRGBA32)
				return failChroma("ApplyChromaDeltaGpu required FP32 formats unsupported", StandInKind::GpuDispatchFailed);

			// Reuse cached member textures (pl_tex_recreate is a no-op when size & format match; freed in Release(gpu)).
			if (!Detail::EnsureTex(gpu, chromaCodedDeltaTex_, cW, cH, fmtRG32, true, false, true, false))
				return failChroma("ApplyChromaDeltaGpu codedDelta texture alloc failed", StandInKind::GpuDispatchFailed);

			std::vector<float> codedBuf(static_cast<size_t>(cW) * static_cast<size_t>(cH) * 2u, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(cW) * static_cast<size_t>(cH); ++i)
			{
				codedBuf[i * 2u + 0u] = static_cast<float>(in.codedDeltaCb.v[i]);
				codedBuf[i * 2u + 1u] = static_cast<float>(in.codedDeltaCr.v[i]);
			}
			if (!Detail::UploadFloats(gpu, chromaCodedDeltaTex_, codedBuf.data()))
				return failChroma("ApplyChromaDeltaGpu codedDelta upload failed", StandInKind::GpuDispatchFailed);

			// Per-frame chroma rects, siting, identity flag, and matrix passed via VpvGpu::Var float UBOs (zero recompiles!).
			const float uDShiftX = dSubX > 1 ? in.siting.shiftX : 0.0f;
			const float uDShiftY = dSubY > 1 ? in.siting.shiftY : 0.0f;
			const float uCShiftX = cSubX > 1 ? in.siting.shiftX : 0.0f;
			const float uCShiftY = cSubY > 1 ? in.siting.shiftY : 0.0f;
			const float uAllZeroDelta = allZeroDelta ? 1.0f : 0.0f;
			const float uIdentityGrid = identityGrid ? 1.0f : 0.0f;
			const float uCAx0 = static_cast<float>(cActive.x0);
			const float uCAy0 = static_cast<float>(cActive.y0);
			const float uCAx1 = static_cast<float>(cActive.x1);
			const float uCAy1 = static_cast<float>(cActive.y1);
			const float uDAx0 = static_cast<float>(dActive.x0);
			const float uDAy0 = static_cast<float>(dActive.y0);
			const float uDAx1 = static_cast<float>(dActive.x1);
			const float uDAy1 = static_cast<float>(dActive.y1);
			const float uKr = static_cast<float>(in.matrix.kr);
			const float uKg = static_cast<float>(in.matrix.kg);
			const float uKb = static_cast<float>(in.matrix.kb);

			std::string hdr;
			hdr += VpvGpu::DefineInt("TD_DW", dW);
			hdr += VpvGpu::DefineInt("TD_DH", dH);
			hdr += VpvGpu::DefineInt("TD_DSUB_X", dSubX);
			hdr += VpvGpu::DefineInt("TD_DSUB_Y", dSubY);
			hdr += VpvGpu::DefineInt("TD_CSUB_X", cSubX);
			hdr += VpvGpu::DefineInt("TD_CSUB_Y", cSubY);
			hdr += VpvGpu::Define("TD_NUM_FLOOR", kEngineNumFloor);
			hdr += Shaders::kCoreHelpers;
			hdr += Shaders::kPassChromaDeltaHeader;

			const int gx = Detail::GroupsFor(dW, 16);
			const int gy = Detail::GroupsFor(dH, 16);

			auto recordChromaStandIns = [&]()
			{
				if (standIns == nullptr)
					return;
				if (res.nonFinitePixels > 0)
					standIns->Engage(StandInKind::NonFiniteClosure, "ApplyChromaDeltaGpu GPU non-finite closure");
				if (res.borderBilinearPixels > 0)
					standIns->Engage(StandInKind::BorderBilinearFallback, "ApplyChromaDeltaGpu active border");
			};

			if (!isRgb)
			{
				bool ok = Detail::EnsureTex(gpu, chromaDelivUvTex_, dW, dH, fmtRG32, true, false, true, false) &&
				          Detail::EnsureTex(gpu, chromaOutUvTex_, dW, dH, fmtRGBA32, true, true, false, true) &&
				          Detail::EnsureTex(gpu, chromaOutDeltaTex_, dW, dH, fmtRG32, true, true, false, true);
				if (ok)
				{
					std::vector<float> uvBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
					for (size_t i = 0; i < in.deliveredCb.v.size(); ++i)
					{
						uvBuf[i * 2u + 0u] = static_cast<float>(in.deliveredCb.v[i]);
						uvBuf[i * 2u + 1u] = static_cast<float>(in.deliveredCr.v[i]);
					}
					ok = Detail::UploadFloats(gpu, chromaDelivUvTex_, uvBuf.data());
				}
				if (ok)
				{
					pl_shader_desc d[4] = {
						VpvGpu::Sampled("deliv_uv", chromaDelivUvTex_),
						VpvGpu::Sampled("coded_delta", chromaCodedDeltaTex_),
						VpvGpu::Storage("out_uv", chromaOutUvTex_),
						VpvGpu::Storage("out_delta", chromaOutDeltaTex_)
					};
					pl_shader_var cv[14] = {
						VpvGpu::Var("u_dshift_x", &uDShiftX),
						VpvGpu::Var("u_dshift_y", &uDShiftY),
						VpvGpu::Var("u_cshift_x", &uCShiftX),
						VpvGpu::Var("u_cshift_y", &uCShiftY),
						VpvGpu::Var("u_all_zero_delta", &uAllZeroDelta),
						VpvGpu::Var("u_identity_grid", &uIdentityGrid),
						VpvGpu::Var("u_cax0", &uCAx0),
						VpvGpu::Var("u_cay0", &uCAy0),
						VpvGpu::Var("u_cax1", &uCAx1),
						VpvGpu::Var("u_cay1", &uCAy1),
						VpvGpu::Var("u_dax0", &uDAx0),
						VpvGpu::Var("u_day0", &uDAy0),
						VpvGpu::Var("u_dax1", &uDAx1),
						VpvGpu::Var("u_day1", &uDAy1)
					};
					ok = VpvGpu::Pass(dispatch, "td_v2_chroma_apply_yuv", hdr, Shaders::kPassChromaApplyYuv,
					                  d, 4, cv, 14, gx, gy, 0, 16, 16);
					if (ok)
						res.dispatchCount = 1;
				}
				if (ok)
				{
					std::vector<float> outBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
					std::vector<float> delOutBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
					ok = Detail::DownloadFloats(gpu, chromaOutUvTex_, outBuf.data()) &&
					     Detail::DownloadFloats(gpu, chromaOutDeltaTex_, delOutBuf.data());
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
							const unsigned fl = static_cast<unsigned>(Detail::NonNegRoundToSize(outBuf[i * 4u + 2u]));
							if ((fl & 1u) != 0u) ++res.nonFinitePixels;
							if ((fl & 2u) != 0u) ++res.borderBilinearPixels;
							res.deliveredDeltaCb.v[i] = static_cast<double>(delOutBuf[i * 2u + 0u]);
							res.deliveredDeltaCr.v[i] = static_cast<double>(delOutBuf[i * 2u + 1u]);
						}
					}
				}
				if (!ok)
					return failChroma("td_v2_chroma_apply_yuv dispatch or transfer failed", StandInKind::GpuDispatchFailed);
				recordChromaStandIns();
				res.succeeded = true;
				return res;
			}

			// Unclipped RGB delivery path (cached member textures).
			bool ok = Detail::EnsureTex(gpu, chromaDelivRgbTex_, dW, dH, fmtRGBA32, true, false, true, false) &&
			          Detail::EnsureTex(gpu, chromaOutRgbTex_, dW, dH, fmtRGBA32, true, true, false, true) &&
			          Detail::EnsureTex(gpu, chromaOutDeltaTex_, dW, dH, fmtRG32, true, true, false, true);
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
				ok = Detail::UploadFloats(gpu, chromaDelivRgbTex_, rgbBuf.data());
			}
			if (ok)
			{
				pl_shader_desc d[4] = {
					VpvGpu::Sampled("deliv_rgb", chromaDelivRgbTex_),
					VpvGpu::Sampled("coded_delta", chromaCodedDeltaTex_),
					VpvGpu::Storage("out_rgb", chromaOutRgbTex_),
					VpvGpu::Storage("out_delta", chromaOutDeltaTex_)
				};
				pl_shader_var rv[17] = {
					VpvGpu::Var("u_dshift_x", &uDShiftX),
					VpvGpu::Var("u_dshift_y", &uDShiftY),
					VpvGpu::Var("u_cshift_x", &uCShiftX),
					VpvGpu::Var("u_cshift_y", &uCShiftY),
					VpvGpu::Var("u_all_zero_delta", &uAllZeroDelta),
					VpvGpu::Var("u_identity_grid", &uIdentityGrid),
					VpvGpu::Var("u_cax0", &uCAx0),
					VpvGpu::Var("u_cay0", &uCAy0),
					VpvGpu::Var("u_cax1", &uCAx1),
					VpvGpu::Var("u_cay1", &uCAy1),
					VpvGpu::Var("u_dax0", &uDAx0),
					VpvGpu::Var("u_day0", &uDAy0),
					VpvGpu::Var("u_dax1", &uDAx1),
					VpvGpu::Var("u_day1", &uDAy1),
					VpvGpu::Var("u_kr", &uKr),
					VpvGpu::Var("u_kg", &uKg),
					VpvGpu::Var("u_kb", &uKb)
				};
				ok = VpvGpu::Pass(dispatch, "td_v2_chroma_apply_rgb", hdr, Shaders::kPassChromaApplyRgb,
				                  d, 4, rv, 17, gx, gy, 0, 16, 16);
				if (ok)
					res.dispatchCount = 1;
			}
			if (ok)
			{
				std::vector<float> rgbOutBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 4u, 0.0f);
				std::vector<float> delOutBuf(static_cast<size_t>(dW) * static_cast<size_t>(dH) * 2u, 0.0f);
				ok = Detail::DownloadFloats(gpu, chromaOutRgbTex_, rgbOutBuf.data()) &&
				     Detail::DownloadFloats(gpu, chromaOutDeltaTex_, delOutBuf.data());
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
						const unsigned fl = static_cast<unsigned>(Detail::NonNegRoundToSize(rgbOutBuf[i * 4u + 3u]));
						if ((fl & 1u) != 0u) ++res.nonFinitePixels;
						if ((fl & 2u) != 0u) ++res.borderBilinearPixels;
						res.deliveredDeltaCb.v[i] = static_cast<double>(delOutBuf[i * 2u + 0u]);
						res.deliveredDeltaCr.v[i] = static_cast<double>(delOutBuf[i * 2u + 1u]);
					}
				}
			}
			if (!ok)
				return failChroma("td_v2_chroma_apply_rgb dispatch or transfer failed", StandInKind::GpuDispatchFailed);
			recordChromaStandIns();
			res.succeeded = true;
			return res;
		}

	private:
		struct ActiveRefSpec
		{
			const TdV2Plane* cpuClean = nullptr; // Non-null only in ProcessFrame CPU upload path
			pl_tex gpuCleanTex = nullptr;        // Non-null in ProcessFrameGpu or carryCleanTex_
			bool useGpuCarry = false;
			int motionSlot = 0;                  // Motion texture slot in [0..2]
			int refRole = 0;                     // Independent A1 role: 0=PrevPeak, 1=NextPeak, 2=Carry, 3=PrevPeak2
			CellMotionField motion;
			bool isExactCopy = false;
			Homography3x3 cameraHomography;
		};

		struct CoreDispatchParams
		{
			uint64_t seq = 0;
			int w = 0;
			int h = 0;
			int cellsW = 0;
			int cellsH = 0;
			int cellPx = kCanonicalBlockPx;
			int grainCellPx = kCanonicalBlockPx;
			ActiveRect active;
			pl_tex recYTex = nullptr;
			pl_tex curCleanTex = nullptr;
			bool hasCleanSplit = false;
			pl_tex motionVecTex = nullptr;       // Optional pre-packed GPU motion (3*cw x ch, fixed slots 0/1/2)
			pl_tex motionMetaTex = nullptr;      // Optional pre-packed GPU motion meta (3*cw x ch, fixed slots 0/1/2)
			pl_tex cellPriorTex = nullptr;       // Optional pre-packed GPU cell prior (cw x ch)
			pl_tex faceMaskTex = nullptr;        // Optional pre-packed GPU Pass-1 face/non-rigid mask (cw x ch)
			pl_tex codecModelPbTex = nullptr;    // Optional explicit Field B CodecModel P_B (w x h R32F)
			pl_tex curLowBandTex = nullptr;      // Optional kBandMemo precomputed BandFromBase L = G(kSigmaBase) of curCleanY
			const std::vector<double>* retainedGrainVar = nullptr;
			const std::vector<double>* cellStepScale = nullptr;
			const std::vector<uint8_t>* explicitFocusChangeCells = nullptr;
			const std::vector<uint8_t>* explicitDefocusedCells = nullptr;
			const std::vector<uint8_t>* faceMask = nullptr;
			Detail::ResolvedA2StepPrior a2{};
			PsdShape psdShape{};
			double fadeKeep = 1.0;
			bool transformIdentified8x8 = false;
			pl_tex outYTex = nullptr;
			pl_tex outCleanTex = nullptr;
			pl_tex outDeltaTex = nullptr;
			pl_tex outTdMapsTex = nullptr;
			pl_tex outDiagFlagsTex = nullptr;
			pl_tex outGuardCellTex = nullptr;
			pl_tex outWarpDiagTex = nullptr;
			bool readbackCellDiag = false;
		};

		struct CoreDispatchOutcome
		{
			int dispatches = 0;
			size_t guardViolationCells = 0;
			size_t focusVetoCells = 0;
			size_t faceMaskedCells = 0;
			size_t nonFiniteCells = 0;
			size_t singularCells = 0;
			size_t singleRefA2Cells = 0;
			size_t borderBilinearPixels = 0;
			bool carryReset = false;
			bool scratchShort = false;
			bool usedExternalBandMemo = false;
			std::vector<TdMapsView::Block> tdMaps;
		};

		struct ScratchSet
		{
			// Preset::Reference full-res RGBA32F bands
			pl_tex curBands = nullptr;
			pl_tex refBands[kMaxGpuRefs] = {};
			pl_tex bandTmpH = nullptr;
			// Preset::RealTime compact bands (full-res FP32 RG32F (M,H) + half-res FP32 RG32F (L,D3))
			pl_tex curBandsMH = nullptr;
			pl_tex refBandsMH[kMaxGpuRefs] = {};
			pl_tex curBandsLD3 = nullptr;
			pl_tex refBandsLD3[kMaxGpuRefs] = {};
			pl_tex bandHalfTmp = nullptr;
			// Common scratch
			pl_tex warpedVal = nullptr;
			pl_tex refFlags[2] = {};
			pl_tex deltaScratch = nullptr;
			pl_tex motionGate = nullptr;
			pl_tex cellWeights = nullptr;
			pl_tex cellDiagC = nullptr;
			pl_tex cellStats = nullptr;
			pl_tex cellFocus = nullptr;

			bool AllNonNull(bool compactBands) const
			{
				if (!warpedVal || !refFlags[0] || !refFlags[1] || !deltaScratch ||
				    !motionGate || !cellWeights || !cellDiagC || !cellStats || !cellFocus)
				{
					return false;
				}
				if (compactBands)
				{
					if (!curBandsMH || !curBandsLD3 || !bandHalfTmp)
						return false;
					for (int i = 0; i < kMaxGpuRefs; ++i)
						if (!refBandsMH[i] || !refBandsLD3[i])
							return false;
					return true;
				}
				if (!curBands || !bandTmpH)
					return false;
				for (int i = 0; i < kMaxGpuRefs; ++i)
					if (!refBands[i])
						return false;
				return true;
			}
		};

		bool ExecuteGpuCore(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const CoreDispatchParams& p,
			const std::vector<ActiveRefSpec>& refSpecs,
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena,
			CoreDispatchOutcome& oc,
			StandInRegistry* standIns)
		{
			const int w = p.w;
			const int h = p.h;
			const int cw = p.cellsW;
			const int ch = p.cellsH;
			const int wHalf = (w + 1) / 2;
			const int hHalf = (h + 1) / 2;
			const size_t numCells = static_cast<size_t>(cw) * static_cast<size_t>(ch);
			const int K = static_cast<int>(refSpecs.size());
			const bool compactBands = (cfg_.preset == Preset::RealTime);

			auto resetCarryIfActive = [&]()
			{
				const bool hadCarry = hasCarry_;
				ResetCarry(gpu);
				oc.carryReset = oc.carryReset || hadCarry;
			};

			auto failCore = [&](const std::string& msg, StandInKind kind) -> bool
			{
				lastError_ = msg;
				++failures_;
				if (standIns != nullptr)
					standIns->Engage(kind, msg.c_str());
				return false;
			};

			const pl_fmt fmtR16 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R16);
			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRG32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RG32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR16 || !fmtR32 || !fmtRG32 || !fmtRGBA32)
			{
				resetCarryIfActive();
				return failCore("TdV2GpuEngine required FP32/R16 texture formats unsupported on pl_gpu", StandInKind::GpuDispatchFailed);
			}

			// Acquire per-frame scratch via RAII ArenaScopeGuard (or private fallback ONLY when arena == nullptr).
			Detail::ArenaScopeGuard scopeGuard(arena, kArenaScope);
			ScratchSet sc{};
			if (arena != nullptr)
			{
				DestroyPrivateScratch(gpu);

				const uint32_t uw = static_cast<uint32_t>(w);
				const uint32_t uh = static_cast<uint32_t>(h);
				const uint32_t uwH = static_cast<uint32_t>(wHalf);
				const uint32_t uhH = static_cast<uint32_t>(hHalf);
				const uint32_t ucw = static_cast<uint32_t>(cw);
				const uint32_t uch = static_cast<uint32_t>(ch);

				const VpSkeleton::TexShape shFullRGBA{ uw, uh, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shHalfRG32{ uwH, uhH, VpSkeleton::TexFormat::RG32F };
				const VpSkeleton::TexShape shFullRG{ uw, uh, VpSkeleton::TexFormat::RG32F };
				const VpSkeleton::TexShape shFullR32{ uw, uh, VpSkeleton::TexFormat::R32F };
				const VpSkeleton::TexShape shFullR16{ uw, uh, VpSkeleton::TexFormat::R16 };
				const VpSkeleton::TexShape shCell3RGBA{ 3u * ucw, uch, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shCellStats{ 3u * ucw, 3u * uch, VpSkeleton::TexFormat::RGBA32F };
				const VpSkeleton::TexShape shCellRGBA{ ucw, uch, VpSkeleton::TexFormat::RGBA32F };

				bool ok = scopeGuard.opened;
				if (ok)
				{
					if (compactBands)
					{
						// H7: ALL band storage and moment inputs strictly FP32 RG32F (plan L60 — never FP16!).
						sc.curBandsMH = arena->Acquire(kArenaOwner, shFullRG);
						for (int i = 0; i < kMaxGpuRefs; ++i)
							sc.refBandsMH[i] = arena->Acquire(kArenaOwner, shFullRG);
						sc.curBandsLD3 = arena->Acquire(kArenaOwner, shHalfRG32);
						for (int i = 0; i < kMaxGpuRefs; ++i)
							sc.refBandsLD3[i] = arena->Acquire(kArenaOwner, shHalfRG32);
						sc.bandHalfTmp = arena->Acquire(kArenaOwner, shHalfRG32);
					}
					else
					{
						sc.curBands = arena->Acquire(kArenaOwner, shFullRGBA);
						for (int i = 0; i < kMaxGpuRefs; ++i)
							sc.refBands[i] = arena->Acquire(kArenaOwner, shFullRGBA);
						sc.bandTmpH = arena->Acquire(kArenaOwner, shFullRGBA);
					}
					sc.warpedVal = arena->Acquire(kArenaOwner, shFullR32);
					sc.refFlags[0] = arena->Acquire(kArenaOwner, shFullR16);
					sc.refFlags[1] = arena->Acquire(kArenaOwner, shFullR16);
					sc.deltaScratch = arena->Acquire(kArenaOwner, shFullRG);
					sc.motionGate = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellWeights = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellDiagC = arena->Acquire(kArenaOwner, shCell3RGBA);
					sc.cellStats = arena->Acquire(kArenaOwner, shCellStats);
					sc.cellFocus = arena->Acquire(kArenaOwner, shCellRGBA);
					ok = sc.AllNonNull(compactBands);
				}
				if (!ok)
				{
					resetCarryIfActive();
					scratchShort_ = true;
					oc.scratchShort = true;
					return failCore("GPU scratch arena shortage (frame skipped visibly)", StandInKind::GpuScratchShortage);
				}
			}
			else
			{
				// LOW-2 / Rule 4: Private scratch fallback (arena == nullptr) MUST NEVER be silent.
				if (standIns != nullptr)
					standIns->Engage(StandInKind::GpuPrivateScratchFallback, "TdV2GpuEngine arena == nullptr; using owned priv* scratch textures");
				if (!EnsurePrivateScratch(gpu, w, h, wHalf, hHalf, cw, ch, compactBands,
				                          fmtR16, fmtR32, fmtRG32, fmtRGBA32, sc))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine private scratch texture allocation failed", StandInKind::GpuDispatchFailed);
				}
			}

			// Ensure persistent carryCleanTex_.
			if (!Detail::EnsureTex(gpu, carryCleanTex_, w, h, fmtR32, true, true, true, false))
			{
				resetCarryIfActive();
				return failCore("TdV2GpuEngine carryCleanTex_ allocation failed", StandInKind::GpuDispatchFailed);
			}

			// Resolve or lazily allocate fallback output targets (zero unnecessary allocation when caller supplies pl_tex!).
			pl_tex dstOutClean = p.outCleanTex;
			if (!dstOutClean)
			{
				if (!Detail::EnsureTex(gpu, outCleanTex_, w, h, fmtR32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outCleanTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstOutClean = outCleanTex_;
			}
			pl_tex dstOutDelta = p.outDeltaTex;
			if (!dstOutDelta)
			{
				if (!Detail::EnsureTex(gpu, outDeltaTex_, w, h, fmtRG32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outDeltaTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstOutDelta = outDeltaTex_;
			}
			pl_tex dstTdMaps = p.outTdMapsTex;
			if (!dstTdMaps)
			{
				if (!Detail::EnsureTex(gpu, outTdMapsTex_, cw, 3 * ch, fmtRGBA32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outTdMapsTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstTdMaps = outTdMapsTex_;
			}
			pl_tex dstDiagFlags = p.outDiagFlagsTex;
			if (!dstDiagFlags)
			{
				if (!Detail::EnsureTex(gpu, outDiagFlagsTex_, cw, ch, fmtRGBA32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outDiagFlagsTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstDiagFlags = outDiagFlagsTex_;
			}
			pl_tex dstGuardCell = p.outGuardCellTex;
			if (!dstGuardCell)
			{
				if (!Detail::EnsureTex(gpu, outGuardCellTex_, cw, ch, fmtRG32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outGuardCellTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstGuardCell = outGuardCellTex_;
			}
			pl_tex dstWarpDiag = p.outWarpDiagTex;
			if (!dstWarpDiag)
			{
				if (!Detail::EnsureTex(gpu, outWarpDiagTex_, cw, ch, fmtRG32, true, true, false, p.readbackCellDiag))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine outWarpDiagTex_ alloc failed", StandInKind::GpuDispatchFailed);
				}
				dstWarpDiag = outWarpDiagTex_;
			}
			// In Mode::Shadow, kPass4FusedApplyGuard skips out_y store completely; bind dead sc.warpedVal if null (zero UAV aliasing).
			pl_tex dstOutY = p.outYTex;
			if (!dstOutY)
			{
				if (!cfg_.ModifiesPixels())
				{
					dstOutY = sc.warpedVal;
				}
				else
				{
					if (!Detail::EnsureTex(gpu, outYTex_, w, h, fmtR32, true, true, false, true))
					{
						resetCarryIfActive();
						return failCore("TdV2GpuEngine outYTex_ alloc failed", StandInKind::GpuDispatchFailed);
					}
					dstOutY = outYTex_;
				}
			}

			// Upload motion & cellPrior ONLY when caller did not supply pre-packed GPU textures.
			pl_tex motionVecTex = p.motionVecTex;
			pl_tex motionMetaTex = p.motionMetaTex;
			if (!motionVecTex || !motionMetaTex)
			{
				if (!Detail::EnsureTex(gpu, inMotionVecTex_, 3 * cw, ch, fmtRGBA32, true, false, true, false) ||
				    !Detail::EnsureTex(gpu, inMotionMetaTex_, 3 * cw, ch, fmtRGBA32, true, false, true, false))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine motion staging alloc failed", StandInKind::GpuDispatchFailed);
				}
				std::vector<float> motVecBuf(static_cast<size_t>(3 * cw) * static_cast<size_t>(ch) * 4u, 0.0f);
				std::vector<float> motMetaBuf(static_cast<size_t>(3 * cw) * static_cast<size_t>(ch) * 4u, 0.0f);
				for (int k = 0; k < K; ++k)
				{
					const CellMotionField& mf = refSpecs[static_cast<size_t>(k)].motion;
					// H5: pack each reference into its FIXED motion slot (0=prevPeak, 1=nextPeak, 2=carry)
					// matching prepacked GPU motion layout identically!
					const int slot = (std::max)(0, (std::min)(2, refSpecs[static_cast<size_t>(k)].motionSlot));
					const bool hasConf = !mf.confidence.empty();
					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							const size_t srcIdx = static_cast<size_t>(cy) * cw + cx;
							const size_t dstTexel = static_cast<size_t>(cy) * (3 * cw) + static_cast<size_t>(slot * cw + cx);
							motVecBuf[dstTexel * 4u + 0u] = static_cast<float>(mf.vx[srcIdx]);
							motVecBuf[dstTexel * 4u + 1u] = static_cast<float>(mf.vy[srcIdx]);
							motVecBuf[dstTexel * 4u + 2u] = static_cast<float>(mf.camVx[srcIdx]);
							motVecBuf[dstTexel * 4u + 3u] = static_cast<float>(mf.camVy[srcIdx]);
							motMetaBuf[dstTexel * 4u + 0u] = static_cast<float>(static_cast<int>(mf.cls[srcIdx]));
							motMetaBuf[dstTexel * 4u + 1u] = mf.valid[srcIdx] ? 1.0f : 0.0f;
							motMetaBuf[dstTexel * 4u + 2u] = hasConf ? static_cast<float>(mf.confidence[srcIdx]) : 1.0f;
							motMetaBuf[dstTexel * 4u + 3u] = 0.0f;
						}
					}
				}
				if (!Detail::UploadFloats(gpu, inMotionVecTex_, motVecBuf.data()) ||
				    !Detail::UploadFloats(gpu, inMotionMetaTex_, motMetaBuf.data()))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine motion upload failed", StandInKind::GpuDispatchFailed);
				}
				motionVecTex = inMotionVecTex_;
				motionMetaTex = inMotionMetaTex_;
			}
			// Preserve full backwards compatibility when callers supply legacy 2-channel RG32F motionMetaTex (.z defaults to 1.0).
			const bool motionMetaHasConf = (motionMetaTex->params.format != nullptr && motionMetaTex->params.format->num_components >= 3);
			const float uMotionMetaHasConf = motionMetaHasConf ? 1.0f : 0.0f;

			pl_tex cellPriorTex = p.cellPriorTex;
			if (!cellPriorTex)
			{
				if (!Detail::EnsureTex(gpu, inCellPriorTex_, cw, ch, fmtRGBA32, true, false, true, false))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine cellPrior staging alloc failed", StandInKind::GpuDispatchFailed);
				}
				const int gW = (w + p.grainCellPx - 1) / p.grainCellPx;
				std::vector<float> cellPriorBuf(numCells * 4u, 0.0f);
				bool grainPriorOutOfRange = false;
				for (int cy = 0; cy < ch; ++cy)
				{
					for (int cx = 0; cx < cw; ++cx)
					{
						const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
						float gv = 0.0f;
						if (p.retainedGrainVar != nullptr && !p.retainedGrainVar->empty())
						{
							const int gcx = MapCellToGrainGrid(cx, p.cellPx, p.grainCellPx);
							const int gcy = MapCellToGrainGrid(cy, p.cellPx, p.grainCellPx);
							const size_t gIdx = static_cast<size_t>(gcy) * static_cast<size_t>(gW) + static_cast<size_t>(gcx);
							if (gIdx < p.retainedGrainVar->size())
								gv = static_cast<float>((*p.retainedGrainVar)[gIdx]);
							else
								grainPriorOutOfRange = true;
						}
						float expFc = 0.0f;
						if (p.explicitFocusChangeCells != nullptr && cIdx < p.explicitFocusChangeCells->size() &&
						    (*p.explicitFocusChangeCells)[cIdx] != 0u)
						{
							expFc += 1.0f;
						}
						if (p.explicitDefocusedCells != nullptr && cIdx < p.explicitDefocusedCells->size() &&
						    (*p.explicitDefocusedCells)[cIdx] != 0u)
						{
							expFc += 2.0f;
						}
						float stepScale = 1.0f;
						if (p.cellStepScale != nullptr && cIdx < p.cellStepScale->size())
							stepScale = static_cast<float>((*p.cellStepScale)[cIdx]);
						cellPriorBuf[cIdx * 4u + 0u] = gv;
						cellPriorBuf[cIdx * 4u + 1u] = expFc;
						cellPriorBuf[cIdx * 4u + 2u] = stepScale;
						cellPriorBuf[cIdx * 4u + 3u] = 0.0f;
					}
				}
				if (grainPriorOutOfRange && standIns != nullptr)
					standIns->Engage(StandInKind::GrainPriorOutOfRange, "TdV2GpuEngine retainedGrainVar undersized for cell grid");
				if (!Detail::UploadFloats(gpu, inCellPriorTex_, cellPriorBuf.data()))
				{
					resetCarryIfActive();
					return failCore("TdV2GpuEngine cell prior upload failed", StandInKind::GpuDispatchFailed);
				}
				cellPriorTex = inCellPriorTex_;
			}
			// Preserve full backwards compatibility when callers supply legacy 2-channel RG32F cellPriorTex (.z defaults to 1.0).
			const bool cellPriorHasStepScale = (cellPriorTex->params.format != nullptr && cellPriorTex->params.format->num_components >= 3);
			const float uCellPriorHasStepScale = cellPriorHasStepScale ? 1.0f : 0.0f;

			// H13: Optional Pass-1 face / non-rigid mask texture (zero upload when absent).
			// Reject dimension-mismatched explicit faceMaskTex visibly (never sample OOB);
			// engage FaceMaskSizeMismatch when CPU faceMask size mismatches numCells (masking trailing cells fail-closed when undersized).
			pl_tex faceMaskTex = p.faceMaskTex;
			if (faceMaskTex != nullptr && (faceMaskTex->params.w != cw || faceMaskTex->params.h != ch))
			{
				resetCarryIfActive();
				return failCore("TdV2GpuEngine faceMaskTex dimension mismatch (expected cellsW x cellsH)", StandInKind::FaceMaskSizeMismatch);
			}
			bool hasFaceMask = (faceMaskTex != nullptr);
			size_t cpuFaceMaskedCount = 0;
			if (!hasFaceMask && p.faceMask != nullptr && !p.faceMask->empty())
			{
				if (p.faceMask->size() != numCells && standIns != nullptr)
				{
					standIns->Engage(
						StandInKind::FaceMaskSizeMismatch,
						p.faceMask->size() < numCells
							? "TdV2GpuEngine faceMask undersized vs cell grid; masking trailing cells fail-closed"
							: "TdV2GpuEngine faceMask oversized vs cell grid");
				}
				std::vector<float> fmBuf(numCells, 0.0f);
				for (size_t i = 0; i < numCells; ++i)
				{
					if (i >= p.faceMask->size() || (*p.faceMask)[i] != 0u)
					{
						fmBuf[i] = 1.0f;
						++cpuFaceMaskedCount;
					}
				}
				if (cpuFaceMaskedCount > 0)
				{
					if (!Detail::EnsureTex(gpu, inFaceMaskTex_, cw, ch, fmtR32, true, false, true, false) ||
					    !Detail::UploadFloats(gpu, inFaceMaskTex_, fmBuf.data()))
					{
						resetCarryIfActive();
						return failCore("TdV2GpuEngine faceMask upload failed", StandInKind::GpuDispatchFailed);
					}
					faceMaskTex = inFaceMaskTex_;
					hasFaceMask = true;
				}
			}
			const float uHasFaceMask = hasFaceMask ? 1.0f : 0.0f;
			pl_tex faceMaskBind = hasFaceMask ? faceMaskTex : sc.cellDiagC; // Distinct dummy SRV when unused

			// Derived physics & pre-validated coding step prior (resolved upfront in ProcessFrame / ProcessFrameGpu).
			const double effStepDelta = p.a2.effStepDelta;
			const double effPhi = p.a2.effPhi;
			const bool validPhi = std::isfinite(effPhi) && effPhi > 0.0 && effPhi < 1.0;
			const RoundingCellStats& a2Stats = p.a2.stats;
			const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
			const double gainMD3 = gains.mid + gains.d3;
			const double baseQuantVar = (kDefaultStepPriorLsb * kDefaultStepPriorLsb) / 12.0;
			const double stepNoiseVar = a2Stats.valid ? (std::max)(0.0, a2Stats.totalVar) : 0.0;
			const double effNoiseVar = (std::max)(baseQuantVar, stepNoiseVar);
			const double structureFloor = 2.0 * gainMD3 * effNoiseVar;
			const double fSharp = AnalyticalEdgeFocusRatio(0.0);
			const double fDefocus = AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);
			const double fShiftDelta = fSharp - fDefocus;

			// Active rect & half-res active rect UBOs (zero recompiles on aspect-ratio/bar changes!).
			const ActiveRect& ar = p.active;
			const int hax0 = (std::max)(0, (std::min)(wHalf, ar.x0 / 2));
			const int hay0 = (std::max)(0, (std::min)(hHalf, ar.y0 / 2));
			const int hax1 = (std::max)(hax0, (std::min)(wHalf, (ar.x1 + 1) / 2));
			const int hay1 = (std::max)(hay0, (std::min)(hHalf, (ar.y1 + 1) / 2));

			const int bxStart = ((ar.x0 + 7) / 8) * 8;
			const int byStart = ((ar.y0 + 7) / 8) * 8;
			const int bxEnd = (ar.x1 / 8) * 8;
			const int byEnd = (ar.y1 / 8) * 8;
			const bool runDctPocs = p.transformIdentified8x8 && (effStepDelta > 0.0) && (bxStart < bxEnd) && (byStart < byEnd);

			const float uK = static_cast<float>(K);
			const float uAx0 = static_cast<float>(ar.x0);
			const float uAy0 = static_cast<float>(ar.y0);
			const float uAx1 = static_cast<float>(ar.x1);
			const float uAy1 = static_cast<float>(ar.y1);
			const float uHAx0 = static_cast<float>(hax0);
			const float uHAy0 = static_cast<float>(hay0);
			const float uHAx1 = static_cast<float>(hax1);
			const float uHAy1 = static_cast<float>(hay1);
			const float uBx0 = static_cast<float>(bxStart);
			const float uBy0 = static_cast<float>(byStart);
			const float uBx1 = static_cast<float>(bxEnd);
			const float uBy1 = static_cast<float>(byEnd);
			const float uXform8x8 = p.transformIdentified8x8 ? 1.0f : 0.0f;
			const float uPocsIters = static_cast<float>((std::max)(1, (std::min)(2, cfg_.cellProjIterations)));
			const float uResidualMode = (cfg_.residualMode == ResidualMode::Strict) ? 1.0f : 0.0f;
			const float uPoolR = static_cast<float>(cfg_.momentPoolRadiusCells > 0 ? cfg_.momentPoolRadiusCells : 0);
			const float uEstimator = static_cast<float>(static_cast<int>(cfg_.estimator));
			const float uRefMotionSlot0 = (K > 0) ? static_cast<float>(refSpecs[0].motionSlot) : 0.0f;
			const float uRefMotionSlot1 = (K > 1) ? static_cast<float>(refSpecs[1].motionSlot) : 0.0f;
			const float uRefMotionSlot2 = (K > 2) ? static_cast<float>(refSpecs[2].motionSlot) : 2.0f;
			const float uRefRole0 = (K > 0) ? static_cast<float>(refSpecs[0].refRole) : 0.0f;
			const float uRefRole1 = (K > 1) ? static_cast<float>(refSpecs[1].refRole) : 1.0f;
			const float uRefRole2 = (K > 2) ? static_cast<float>(refSpecs[2].refRole) : 2.0f;
			const float uExactCopy0 = (K > 0 && refSpecs[0].isExactCopy) ? 1.0f : 0.0f;
			const float uExactCopy1 = (K > 1 && refSpecs[1].isExactCopy) ? 1.0f : 0.0f;
			const float uExactCopy2 = (K > 2 && refSpecs[2].isExactCopy) ? 1.0f : 0.0f;
			const float uHasCleanSplit = p.hasCleanSplit ? 1.0f : 0.0f;
			const float uShadowMode = cfg_.ModifiesPixels() ? 0.0f : 1.0f;
			const float uBokehMode = (cfg_.bokehMode == BokehMode::LowBandCellOnly) ? 1.0f : 0.0f;
			const float uEnableHuber = cfg_.enablePerCornerHuber ? 1.0f : 0.0f;
			const float uEnableSubpixelHDebias = cfg_.enableSubpixelHAttenuationDebias ? 1.0f : 0.0f;
			const float uEnableEdgeAwareUpsample = cfg_.enableEdgeAwareWeightUpsample ? 1.0f : 0.0f;
			const float uEnableNbrSoftClamp = cfg_.enableNeighborhoodSoftClamp ? 1.0f : 0.0f;
			const float uPsdLow = static_cast<float>(p.psdShape.bandShareLow);
			const float uPsdMid = static_cast<float>(p.psdShape.bandShareMid);
			const float uPsdFine = static_cast<float>(p.psdShape.bandShareFine);
			const float uFadeKeep = static_cast<float>(p.fadeKeep);
			const float uStepNoiseVar = static_cast<float>(stepNoiseVar);
			const float uStructureFloor = static_cast<float>(structureFloor);
			const float uA2Valid = a2Stats.valid ? 1.0f : 0.0f;
			const float uA2SigmaSent = a2Stats.valid ? static_cast<float>(a2Stats.sigmaSentSq) : 0.0f;
			const float uA2SigmaZero = a2Stats.valid ? static_cast<float>(a2Stats.sigmaZeroSq) : 0.0f;
			const float uA2TotalVar = a2Stats.valid ? static_cast<float>(a2Stats.totalVar) : 0.0f;
			const float uStepDelta = static_cast<float>(effStepDelta);
			const float uDeadZonePhi = validPhi ? static_cast<float>(effPhi) : -1.0f;
			const float uPeakCode = static_cast<float>(PeakCodeForBits(cfg_.sourceBits));

			const std::string staticGeom = Detail::StaticGeomDefines(w, h, cw, ch, p.cellPx);
			const int rFine = VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaFine);
			const int rLow = VpUnit::BandFromBase::BandRadius(VpUnit::BandFromBase::kSigmaLow);
			const double hsLow = VpUnit::BandFromBase::HalfSigma(VpUnit::BandFromBase::kSigmaLow);
			const int rHLow = VpUnit::BandFromBase::Radius(hsLow, 3.0, true, 0.0);

			int dispatches = 0;
			auto runPass = [&](const char* desc, const std::string& hdr, const char* body,
			                   const pl_shader_desc* descs, int nDescs,
			                   const pl_shader_var* vars, int nVars,
			                   int gx, int gy, int gw = 16, int gh = 16, size_t shmem = 0) -> bool
			{
				if (!VpvGpu::Pass(dispatch, desc, hdr, body, descs, nDescs, vars, nVars, gx, gy, shmem, gw, gh))
					return false;
				++dispatches;
				return true;
			};

			const int fullGx = Detail::GroupsFor(w, 16);
			const int fullGy = Detail::GroupsFor(h, 16);
			const int halfGx = Detail::GroupsFor(wHalf, 16);
			const int halfGy = Detail::GroupsFor(hHalf, 16);
			const int cellGx8 = Detail::GroupsFor(cw, 8);
			const int cellGy8 = Detail::GroupsFor(ch, 8);

			// ---- PASS 1A: Camera-Relative Motion Gate + Karis 3x3 Silhouette Dilation ----
			{
				std::string hdr = staticGeom;
				hdr += VpvGpu::DefineInt("TD_CLS_OBJECT", static_cast<int>(RegionMotionClass::Object));
				hdr += VpvGpu::DefineInt("TD_CLS_OVERLAY", static_cast<int>(RegionMotionClass::Overlay));
				hdr += VpvGpu::DefineInt("TD_CLS_DYN_TEX", static_cast<int>(RegionMotionClass::DynamicTexture));
				hdr += VpvGpu::Define("TD_STILL_SPEED", kStillObjectSpeedPx);
				hdr += VpvGpu::Define("TD_FAST_CUTOFF", kFastObjectCutoffPx);
				hdr += Shaders::kCoreHelpers;
				hdr += Shaders::kPass1MotionGateHeader;
				pl_shader_desc d[3] = {
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("motion_meta", motionMetaTex),
					VpvGpu::Storage("motion_gate", sc.motionGate)
				};
				pl_shader_var v[5] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefMotionSlot0),
					VpvGpu::Var("u_ref_motion_slot_1", &uRefMotionSlot1),
					VpvGpu::Var("u_ref_motion_slot_2", &uRefMotionSlot2),
					VpvGpu::Var("u_motion_meta_has_conf", &uMotionMetaHasConf)
				};
				if (!runPass("td_v2_p1_motion_gate", hdr, Shaders::kPass1MotionGate, d, 3, v, 5,
				             Detail::GroupsFor(K * cw, 16), Detail::GroupsFor(ch, 16), 16, 16))
				{
					resetCarryIfActive();
					return failCore("td_v2_p1_motion_gate dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 1C Helper: Reference (LDS single-loop H+V) OR RealTime (half-res FP32 L/D3 + 1-pass LDS FP32 RG32F M/H) ----
			if (standIns != nullptr)
				standIns->Engage(StandInKind::BandMemoW2);

			const std::string refBandHdrH = compactBands ? std::string() : (staticGeom + Shaders::BuildBandTapsHeader() + Shaders::kCoreHelpers + Shaders::kPass1BandHHeader);
			const std::string refBandHdrV = compactBands ? std::string() : (staticGeom + Shaders::BuildBandTapsHeader() + Shaders::kPass1BandVHeader);
			const std::string rtHalfHdrH = compactBands ? (staticGeom + Shaders::BuildHalfBandTapsHeader() + Shaders::kCoreHelpers + Shaders::kPass1HalfHHeader) : std::string();
			const std::string rtHalfHdrV = compactBands ? (staticGeom + Shaders::BuildHalfBandTapsHeader() + Shaders::kPass1HalfVHeader) : std::string();
			const std::string rtFullMhHdr = compactBands ? (staticGeom + Shaders::BuildFineTapHeader() + Shaders::kCoreHelpers + Shaders::kHalfBandSamplerHelper + Shaders::kPass1FullMHHeader) : std::string();

			const size_t shmemBandH = static_cast<size_t>(16 * (16 + 2 * rLow)) * sizeof(float);
			const size_t shmemBandV = static_cast<size_t>((16 + 2 * rLow) * 16) * 4u * sizeof(float);
			const size_t shmemHalfH = static_cast<size_t>(16 * (16 + 2 * rHLow)) * sizeof(float);
			const size_t shmemHalfV = static_cast<size_t>((16 + 2 * rHLow) * 16) * 2u * sizeof(float);
			const size_t fineTileDim = static_cast<size_t>(16 + 2 * rFine);
			const size_t shmemFullMH = (fineTileDim * fineTileDim + fineTileDim * 16u) * sizeof(float);

			auto runBandDecomp = [&](pl_tex srcTex, pl_tex dstRefFull, pl_tex dstRtMH, pl_tex dstRtLD3) -> bool
			{
				if (!compactBands)
				{
					pl_shader_var rectV[4] = {
						VpvGpu::Var("u_ax0", &uAx0),
						VpvGpu::Var("u_ay0", &uAy0),
						VpvGpu::Var("u_ax1", &uAx1),
						VpvGpu::Var("u_ay1", &uAy1)
					};
					pl_shader_desc dh[2] = {
						VpvGpu::Sampled("src_tex", srcTex),
						VpvGpu::Storage("band_tmp_h", sc.bandTmpH)
					};
					if (!runPass("td_v2_p1_band_h", refBandHdrH, Shaders::kPass1BandH, dh, 2, rectV, 4, fullGx, fullGy, 16, 16, shmemBandH))
						return false;
					pl_shader_desc dv[2] = {
						VpvGpu::Sampled("band_tmp_h", sc.bandTmpH),
						VpvGpu::Storage("bands_out", dstRefFull)
					};
					return runPass("td_v2_p1_band_v", refBandHdrV, Shaders::kPass1BandV, dv, 2, rectV, 4, fullGx, fullGy, 16, 16, shmemBandV);
				}

				// Compact RealTime path (SC-05): half-res FP32 RG32F (L, D3) + 1-pass 2D LDS full-res FP32 RG32F (M, H).
				pl_shader_var rectAllV[8] = {
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_hax0", &uHAx0),
					VpvGpu::Var("u_hay0", &uHAy0),
					VpvGpu::Var("u_hax1", &uHAx1),
					VpvGpu::Var("u_hay1", &uHAy1)
				};
				pl_shader_desc dhh[2] = {
					VpvGpu::Sampled("src_tex", srcTex),
					VpvGpu::Storage("band_half_tmp", sc.bandHalfTmp)
				};
				if (!runPass("td_v2_p1_half_h", rtHalfHdrH, Shaders::kPass1HalfH, dhh, 2, rectAllV, 8, halfGx, halfGy, 16, 16, shmemHalfH))
					return false;

				pl_shader_var halfRectV[4] = {
					VpvGpu::Var("u_hax0", &uHAx0),
					VpvGpu::Var("u_hay0", &uHAy0),
					VpvGpu::Var("u_hax1", &uHAx1),
					VpvGpu::Var("u_hay1", &uHAy1)
				};
				pl_shader_desc dhv[2] = {
					VpvGpu::Sampled("band_half_tmp", sc.bandHalfTmp),
					VpvGpu::Storage("bands_ld3_out", dstRtLD3)
				};
				if (!runPass("td_v2_p1_half_v", rtHalfHdrV, Shaders::kPass1HalfV, dhv, 2, halfRectV, 4, halfGx, halfGy, 16, 16, shmemHalfV))
					return false;

				pl_shader_desc dmh[3] = {
					VpvGpu::Sampled("src_tex", srcTex),
					VpvGpu::Sampled("bands_ld3_in", dstRtLD3),
					VpvGpu::Storage("bands_mh_out", dstRtMH)
				};
				return runPass("td_v2_p1_full_mh", rtFullMhHdr, Shaders::kPass1FullMH, dmh, 3, rectAllV, 8, fullGx, fullGy, 16, 16, shmemFullMH);
			};

			// Decompose current frame grain-free clean into curBands.
			// SOTA #10 (kBandMemo): when caller supplies a valid precomputed low band L = G(kSigmaBase),
			// skip the current-frame coarse low-band H+V blur passes in ONE single fused shader pass!
			if (p.curLowBandTex != nullptr)
			{
				const bool memoIsHalf = (p.curLowBandTex->params.w == wHalf && p.curLowBandTex->params.h == hHalf &&
				                         (wHalf != w || hHalf != h));
				const int rHDiff = VpUnit::BandFromBase::BandRadius(
					0.5 * std::sqrt(VpUnit::BandFromBase::kSigmaLow * VpUnit::BandFromBase::kSigmaLow -
					                VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase));
				const size_t hDiffDim = static_cast<size_t>(10 + 2 * rHDiff);
				const size_t shmemMemo = (fineTileDim * fineTileDim + fineTileDim * 16u +
				                          hDiffDim * hDiffDim + hDiffDim * 10u + 100u) * sizeof(float);

				std::string memoHdr = staticGeom;
				memoHdr += VpvGpu::DefineInt("TD_MEMO_COMPACT", compactBands ? 1 : 0);
				memoHdr += VpvGpu::DefineInt("TD_MEMO_IS_HALF", memoIsHalf ? 1 : 0);
				memoHdr += Shaders::BuildFineTapHeader();
				memoHdr += Shaders::BuildHalfDiffTapHeader();
				memoHdr += Shaders::kCoreHelpers;
				memoHdr += Shaders::kHalfBandSamplerHelper;
				memoHdr += Shaders::kPass1BandFromMemoHeader;

				pl_shader_var rectAllV[8] = {
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_hax0", &uHAx0),
					VpvGpu::Var("u_hay0", &uHAy0),
					VpvGpu::Var("u_hax1", &uHAx1),
					VpvGpu::Var("u_hay1", &uHAy1)
				};
				bool memoOk = false;
				if (!compactBands)
				{
					pl_shader_desc dm[3] = {
						VpvGpu::Sampled("src_tex", p.curCleanTex),
						VpvGpu::Sampled("cur_low_band", p.curLowBandTex),
						VpvGpu::Storage("bands_out", sc.curBands)
					};
					memoOk = runPass("td_v2_p1_band_from_memo", memoHdr, Shaders::kPass1BandFromMemo,
					                 dm, 3, rectAllV, 8, fullGx, fullGy, 16, 16, shmemMemo);
				}
				else
				{
					pl_shader_desc dm[4] = {
						VpvGpu::Sampled("src_tex", p.curCleanTex),
						VpvGpu::Sampled("cur_low_band", p.curLowBandTex),
						VpvGpu::Storage("bands_mh_out", sc.curBandsMH),
						VpvGpu::Storage("bands_ld3_out", sc.curBandsLD3)
					};
					memoOk = runPass("td_v2_p1_band_from_memo", memoHdr, Shaders::kPass1BandFromMemo,
					                 dm, 4, rectAllV, 8, fullGx, fullGy, 16, 16, shmemMemo);
				}
				if (!memoOk)
				{
					resetCarryIfActive();
					return failCore("td_v2_p1_band_from_memo current clean dispatch failed", StandInKind::GpuDispatchFailed);
				}
				oc.usedExternalBandMemo = true;
			}
			else if (!runBandDecomp(p.curCleanTex, sc.curBands, sc.curBandsMH, sc.curBandsLD3))
			{
				resetCarryIfActive();
				return failCore("td_v2_p1_band current clean dispatch failed", StandInKind::GpuDispatchFailed);
			}

			// ---- PASS 1B + 1C: Reverse-Order Warp (k = K-1 .. 0) + Band Decomp ----
			// Looping k = K-1 down to 0 reuses ONE sc.warpedVal (R32F) and leaves k == 0 (refs[0].clean = P_B)
			// bit-exact in sc.warpedVal for Pass 3B (kPass3bDctPocs) with ZERO extra VRAM!
			const std::string warpHdr = staticGeom +
				VpvGpu::DefineInt("TD_CLS_CAMERA", static_cast<int>(RegionMotionClass::Camera)) +
				Shaders::kCoreHelpers + Shaders::kPass1WarpHeader;
			const size_t shmemWarp = (20u * 20u * 2u) * sizeof(float);
			int curFlagsSlot = 0;

			for (int k = K - 1; k >= 0; --k)
			{
				const ActiveRefSpec& rSpec = refSpecs[static_cast<size_t>(k)];
				pl_tex refSrcTex = rSpec.gpuCleanTex;
				if (refSrcTex == nullptr)
				{
					if (!Detail::EnsureTex(gpu, inRefCleanTex_, w, h, fmtR32, true, false, true, false) ||
					    !Detail::UploadPlaneR32F(gpu, inRefCleanTex_, *rSpec.cpuClean))
					{
						resetCarryIfActive();
						return failCore("TdV2GpuEngine ref clean upload failed", StandInKind::GpuDispatchFailed);
					}
					refSrcTex = inRefCleanTex_;
				}

				const bool isFirst = (k == K - 1);
				const int dstFlagsSlot = isFirst ? 0 : (1 - curFlagsSlot);
				const int srcFlagsSlot = isFirst ? 1 : curFlagsSlot; // Always distinct from dstFlagsSlot (zero SRV/UAV alias!)
				const bool sameRefCur = (refSrcTex == p.curCleanTex);
				const float uRefIdx = static_cast<float>(k);
				const float uMotionSlot = static_cast<float>(rSpec.motionSlot);
				const float uFlagsClear = isFirst ? 1.0f : 0.0f;
				const float uSameRefCur = sameRefCur ? 1.0f : 0.0f;
				const float uUseH = rSpec.cameraHomography.valid ? 1.0f : 0.0f;
				const float uH[9] = {
					static_cast<float>(rSpec.cameraHomography.h[0]),
					static_cast<float>(rSpec.cameraHomography.h[1]),
					static_cast<float>(rSpec.cameraHomography.h[2]),
					static_cast<float>(rSpec.cameraHomography.h[3]),
					static_cast<float>(rSpec.cameraHomography.h[4]),
					static_cast<float>(rSpec.cameraHomography.h[5]),
					static_cast<float>(rSpec.cameraHomography.h[6]),
					static_cast<float>(rSpec.cameraHomography.h[7]),
					static_cast<float>(rSpec.cameraHomography.h[8])
				};

				pl_shader_desc dw[7] = {
					VpvGpu::Sampled("ref_clean", refSrcTex),
					VpvGpu::Sampled("cur_clean", sameRefCur ? sc.cellFocus : p.curCleanTex),
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("motion_meta", motionMetaTex),
					VpvGpu::Sampled("ref_flags_in", sc.refFlags[srcFlagsSlot]),
					VpvGpu::Storage("warped_val", sc.warpedVal),
					VpvGpu::Storage("ref_flags_out", sc.refFlags[dstFlagsSlot])
				};
				pl_shader_var wv[18] = {
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_ref_idx", &uRefIdx),
					VpvGpu::Var("u_motion_slot", &uMotionSlot),
					VpvGpu::Var("u_flags_clear", &uFlagsClear),
					VpvGpu::Var("u_same_ref_cur", &uSameRefCur),
					VpvGpu::Var("u_use_homography", &uUseH),
					VpvGpu::Var("u_h0", &uH[0]), VpvGpu::Var("u_h1", &uH[1]), VpvGpu::Var("u_h2", &uH[2]),
					VpvGpu::Var("u_h3", &uH[3]), VpvGpu::Var("u_h4", &uH[4]), VpvGpu::Var("u_h5", &uH[5]),
					VpvGpu::Var("u_h6", &uH[6]), VpvGpu::Var("u_h7", &uH[7]), VpvGpu::Var("u_h8", &uH[8])
				};
				if (!runPass("td_v2_p1_warp", warpHdr, Shaders::kPass1WarpAndGate, dw, 7, wv, 18, fullGx, fullGy, 16, 16, shmemWarp))
				{
					resetCarryIfActive();
					return failCore("td_v2_p1_warp dispatch failed", StandInKind::GpuDispatchFailed);
				}
				curFlagsSlot = dstFlagsSlot;

				if (!runBandDecomp(sc.warpedVal, sc.refBands[k], sc.refBandsMH[k], sc.refBandsLD3[k]))
				{
					resetCarryIfActive();
					return failCore("td_v2_p1_band ref dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}
			pl_tex finalRefFlagsTex = sc.refFlags[curFlagsSlot];

			// ---- PASS 2A (Speedup 2): Coalesced 16x16 LDS Cell-Moment Reduction + Fused Focus Guard ----
			if (standIns != nullptr)
				standIns->Engage(StandInKind::FocusMapK32);
			const bool sameCleanRec = (p.curCleanTex == p.recYTex);
			const float uSameCleanRec = sameCleanRec ? 1.0f : 0.0f;
			{
				std::string p2aHdr = staticGeom;
				p2aHdr += VpvGpu::DefineInt("TD_COMPACT_BANDS", compactBands ? 1 : 0);
				p2aHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				p2aHdr += VpvGpu::Define("TD_GAIN_D3", gains.d3);
				p2aHdr += VpvGpu::Define("TD_F_SHARP", fSharp);
				p2aHdr += VpvGpu::Define("TD_F_DEFOCUS", fDefocus);
				p2aHdr += VpvGpu::Define("TD_F_SHIFT_DELTA", fShiftDelta);
				p2aHdr += Shaders::kCoreHelpers;
				if (compactBands)
					p2aHdr += Shaders::kHalfBandSamplerHelper;
				p2aHdr += Shaders::kPass2CellStatsHeader;

				pl_shader_var v2a[15] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_hax0", &uHAx0),
					VpvGpu::Var("u_hay0", &uHAy0),
					VpvGpu::Var("u_hax1", &uHAx1),
					VpvGpu::Var("u_hay1", &uHAy1),
					VpvGpu::Var("u_step_noise_var", &uStepNoiseVar),
					VpvGpu::Var("u_structure_floor", &uStructureFloor),
					VpvGpu::Var("u_has_face_mask", &uHasFaceMask),
					VpvGpu::Var("u_has_clean_split", &uHasCleanSplit),
					VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode)
				};
				const size_t shmemP2a = (256u * 4u + 256u * 4u + 256u + 16u) * sizeof(float);
				// Distinct dummy texture (sc.cellWeights is unused in Pass 2A) when curCleanTex aliases recYTex.
				pl_tex p2aCurCleanBind = sameCleanRec ? sc.cellWeights : p.curCleanTex;

				bool ok2a = false;
				const std::string p2aBody = Shaders::BuildPass2CellStatsBody();
				if (!compactBands)
				{
					pl_shader_desc d2a[13] = {
						VpvGpu::Sampled("cur_bands", sc.curBands),
						VpvGpu::Sampled("ref_bands_0", sc.refBands[0]),
						VpvGpu::Sampled("ref_bands_1", sc.refBands[1]),
						VpvGpu::Sampled("ref_bands_2", sc.refBands[2]),
						VpvGpu::Sampled("ref_flags", finalRefFlagsTex),
						VpvGpu::Sampled("motion_gate", sc.motionGate),
						VpvGpu::Sampled("cell_prior", cellPriorTex),
						VpvGpu::Sampled("face_mask", faceMaskBind),
						VpvGpu::Sampled("rec_y", p.recYTex),
						VpvGpu::Sampled("cur_clean", p2aCurCleanBind),
						VpvGpu::Storage("cell_focus", sc.cellFocus),
						VpvGpu::Storage("warp_diag", dstWarpDiag),
						VpvGpu::Storage("cell_stats", sc.cellStats)
					};
					ok2a = runPass("td_v2_p2a_cell_stats", p2aHdr, p2aBody.c_str(), d2a, 13, v2a, 15, cw, ch, 16, 16, shmemP2a);
				}
				else
				{
					pl_shader_desc d2a[17] = {
						VpvGpu::Sampled("cur_bands_mh", sc.curBandsMH),
						VpvGpu::Sampled("cur_bands_ld3", sc.curBandsLD3),
						VpvGpu::Sampled("ref_bands_mh_0", sc.refBandsMH[0]),
						VpvGpu::Sampled("ref_bands_mh_1", sc.refBandsMH[1]),
						VpvGpu::Sampled("ref_bands_mh_2", sc.refBandsMH[2]),
						VpvGpu::Sampled("ref_bands_ld3_0", sc.refBandsLD3[0]),
						VpvGpu::Sampled("ref_bands_ld3_1", sc.refBandsLD3[1]),
						VpvGpu::Sampled("ref_bands_ld3_2", sc.refBandsLD3[2]),
						VpvGpu::Sampled("ref_flags", finalRefFlagsTex),
						VpvGpu::Sampled("motion_gate", sc.motionGate),
						VpvGpu::Sampled("cell_prior", cellPriorTex),
						VpvGpu::Sampled("face_mask", faceMaskBind),
						VpvGpu::Sampled("rec_y", p.recYTex),
						VpvGpu::Sampled("cur_clean", p2aCurCleanBind),
						VpvGpu::Storage("cell_focus", sc.cellFocus),
						VpvGpu::Storage("warp_diag", dstWarpDiag),
						VpvGpu::Storage("cell_stats", sc.cellStats)
					};
					ok2a = runPass("td_v2_p2a_cell_stats", p2aHdr, p2aBody.c_str(), d2a, 17, v2a, 15, cw, ch, 16, 16, shmemP2a);
				}
				if (!ok2a)
				{
					resetCarryIfActive();
					return failCore("td_v2_p2a_cell_stats dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// Exact Parseval passband supremum ||H_M||_inf^2 for A2 rounding-cell upper bound & transform-unknown bound (mirrors TdV2ReferenceEngine.h 1:1).
			const double s1Sq = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
			const double s2Sq = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
			const double wStarSq = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2Sq - s1Sq);
			const double midPassbandPeak = std::exp(-0.5 * s1Sq * wStarSq) - std::exp(-0.5 * s2Sq * wStarSq);
			const double a2PeakMidSq = midPassbandPeak * midPassbandPeak;

			// ---- PASS 2B (Speedup 2): Exact Chan-Pebay Merge + Blind A1/A2/A3 + KxK Solve + TdMaps ----
			{
				std::string p2bHdr = staticGeom;
				p2bHdr += VpvGpu::Define("TD_GAIN_LOW", gains.low);
				p2bHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				p2bHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				p2bHdr += VpvGpu::Define("TD_ORIGIN_LOW", gains.originLow);
				p2bHdr += VpvGpu::Define("TD_ORIGIN_MID", gains.originMid);
				p2bHdr += VpvGpu::Define("TD_ORIGIN_FINE", gains.originFine);
				p2bHdr += VpvGpu::Define("TD_A2_PEAK_MID_SQ", a2PeakMidSq);
				p2bHdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				p2bHdr += VpvGpu::Define("TD_TRACE_REG", kDefaultTraceRegScale);
				p2bHdr += VpvGpu::Define("TD_SOLVE_FLOOR", kSolveNumericalFloor);
				p2bHdr += VpvGpu::Define("TD_PIVOT_FLOOR", kSolvePivotFloor);
				p2bHdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				p2bHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", kCellStepScaleMax);
				p2bHdr += Shaders::BuildFineTapHeader();
				p2bHdr += Shaders::kCoreHelpers;
				p2bHdr += Shaders::kPass2MergeSolveHeader;

				pl_shader_desc d2b[9] = {
					VpvGpu::Sampled("cell_stats", sc.cellStats),
					VpvGpu::Sampled("cell_focus", sc.cellFocus),
					VpvGpu::Sampled("motion_gate", sc.motionGate),
					VpvGpu::Sampled("motion_vec", motionVecTex),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Storage("cell_weights", sc.cellWeights),
					VpvGpu::Storage("cell_diag_c", sc.cellDiagC),
					VpvGpu::Storage("td_maps", dstTdMaps),
					VpvGpu::Storage("cell_diag_flags", dstDiagFlags)
				};
				pl_shader_var v2b[24] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_pool_r", &uPoolR),
					VpvGpu::Var("u_estimator", &uEstimator),
					VpvGpu::Var("u_ref_role_0", &uRefRole0),
					VpvGpu::Var("u_ref_role_1", &uRefRole1),
					VpvGpu::Var("u_ref_role_2", &uRefRole2),
					VpvGpu::Var("u_ref_motion_slot_0", &uRefMotionSlot0),
					VpvGpu::Var("u_ref_motion_slot_1", &uRefMotionSlot1),
					VpvGpu::Var("u_ref_motion_slot_2", &uRefMotionSlot2),
					VpvGpu::Var("u_exact_copy_0", &uExactCopy0),
					VpvGpu::Var("u_exact_copy_1", &uExactCopy1),
					VpvGpu::Var("u_exact_copy_2", &uExactCopy2),
					VpvGpu::Var("u_a2_valid", &uA2Valid),
					VpvGpu::Var("u_a2_sigma_sent", &uA2SigmaSent),
					VpvGpu::Var("u_a2_sigma_zero", &uA2SigmaZero),
					VpvGpu::Var("u_a2_total_var", &uA2TotalVar),
					VpvGpu::Var("u_has_clean_split", &uHasCleanSplit),
					VpvGpu::Var("u_bokeh_mode", &uBokehMode),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uCellPriorHasStepScale),
					VpvGpu::Var("u_enable_subpixel_h_debias", &uEnableSubpixelHDebias),
					VpvGpu::Var("u_psd_low", &uPsdLow),
					VpvGpu::Var("u_psd_mid", &uPsdMid),
					VpvGpu::Var("u_psd_fine", &uPsdFine),
					VpvGpu::Var("u_fade_keep", &uFadeKeep)
				};
				if (!runPass("td_v2_p2b_merge_solve", p2bHdr, Shaders::kPass2MergeAndSolve, d2b, 9, v2b, 24, cellGx8, cellGy8, 8, 8))
				{
					resetCarryIfActive();
					return failCore("td_v2_p2b_merge_solve dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 3A: Spatial-Only Weight Interpolation + 3-Sigma Huber Taper + Pixel Bound ----
			{
				std::string bHdr = staticGeom;
				bHdr += VpvGpu::DefineInt("TD_COMPACT_BANDS", compactBands ? 1 : 0);
				bHdr += VpvGpu::Define("TD_GAIN_LOW", gains.low);
				bHdr += VpvGpu::Define("TD_GAIN_MID", gains.mid);
				bHdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				bHdr += VpvGpu::Define("TD_A2_PEAK_MID_SQ", a2PeakMidSq);
				bHdr += VpvGpu::Define("TD_HUBER_KNEE_SQ", kHuberKneeSq);
				bHdr += VpvGpu::Define("TD_PIXEL_BOUND_SIGMA", kPixelBoundSigma);
				bHdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				bHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", kCellStepScaleMax);
				bHdr += Shaders::kCoreHelpers;
				if (compactBands)
					bHdr += Shaders::kHalfBandSamplerHelper;
				bHdr += Shaders::kPass3BlendHeader;

				pl_shader_var bv[20] = {
					VpvGpu::Var("u_k", &uK),
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_hax0", &uHAx0),
					VpvGpu::Var("u_hay0", &uHAy0),
					VpvGpu::Var("u_hax1", &uHAx1),
					VpvGpu::Var("u_hay1", &uHAy1),
					VpvGpu::Var("u_bx0", &uBx0),
					VpvGpu::Var("u_by0", &uBy0),
					VpvGpu::Var("u_bx1", &uBx1),
					VpvGpu::Var("u_by1", &uBy1),
					VpvGpu::Var("u_xform_8x8", &uXform8x8),
					VpvGpu::Var("u_a2_valid", &uA2Valid),
					VpvGpu::Var("u_a2_total_var", &uA2TotalVar),
					VpvGpu::Var("u_enable_per_corner_huber", &uEnableHuber),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uCellPriorHasStepScale),
					VpvGpu::Var("u_enable_edge_aware_upsample", &uEnableEdgeAwareUpsample),
					VpvGpu::Var("u_enable_nbr_soft_clamp", &uEnableNbrSoftClamp)
				};

				const size_t shmemP3a = (18u * 18u * 2u) * sizeof(float);
				pl_tex deltaTarget = runDctPocs ? sc.deltaScratch : dstOutDelta;
				bool ok3a = false;
				if (!compactBands)
				{
					pl_shader_desc db[9] = {
						VpvGpu::Sampled("cur_bands", sc.curBands),
						VpvGpu::Sampled("ref_bands_0", sc.refBands[0]),
						VpvGpu::Sampled("ref_bands_1", sc.refBands[1]),
						VpvGpu::Sampled("ref_bands_2", sc.refBands[2]),
						VpvGpu::Sampled("ref_flags", finalRefFlagsTex),
						VpvGpu::Sampled("cell_weights", sc.cellWeights),
						VpvGpu::Sampled("cell_diag_c", sc.cellDiagC),
						VpvGpu::Sampled("cell_prior", cellPriorTex),
						VpvGpu::Storage("delta_out", deltaTarget)
					};
					ok3a = runPass("td_v2_p3_blend_bound", bHdr, Shaders::kPass3BlendAndBound, db, 9, bv, 20, fullGx, fullGy, 16, 16, shmemP3a);
				}
				else
				{
					pl_shader_desc db[13] = {
						VpvGpu::Sampled("cur_bands_mh", sc.curBandsMH),
						VpvGpu::Sampled("cur_bands_ld3", sc.curBandsLD3),
						VpvGpu::Sampled("ref_bands_mh_0", sc.refBandsMH[0]),
						VpvGpu::Sampled("ref_bands_mh_1", sc.refBandsMH[1]),
						VpvGpu::Sampled("ref_bands_mh_2", sc.refBandsMH[2]),
						VpvGpu::Sampled("ref_bands_ld3_0", sc.refBandsLD3[0]),
						VpvGpu::Sampled("ref_bands_ld3_1", sc.refBandsLD3[1]),
						VpvGpu::Sampled("ref_bands_ld3_2", sc.refBandsLD3[2]),
						VpvGpu::Sampled("ref_flags", finalRefFlagsTex),
						VpvGpu::Sampled("cell_weights", sc.cellWeights),
						VpvGpu::Sampled("cell_diag_c", sc.cellDiagC),
						VpvGpu::Sampled("cell_prior", cellPriorTex),
						VpvGpu::Storage("delta_out", deltaTarget)
					};
					ok3a = runPass("td_v2_p3_blend_bound", bHdr, Shaders::kPass3BlendAndBound, db, 13, bv, 20, fullGx, fullGy, 16, 16, shmemP3a);
				}
				if (!ok3a)
				{
					resetCarryIfActive();
					return failCore("td_v2_p3_blend_bound dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 3B: Optional Codec-Grid-Aligned 8x8 DCT-II STRICT/CELL POCS ----
			// Uses explicit Field B CodecModel P_B when supplied; otherwise falls back visibly to sc.warpedVal (refs[0].clean)!
			if (runDctPocs)
			{
				const bool hasExplicitPb = (p.codecModelPbTex != nullptr);
				const float uHasExplicitPb = hasExplicitPb ? 1.0f : 0.0f;
				// H14: visibly record CodecModelPbStandIn ONLY when STRICT/CELL runs without explicit Field B P_B!
				if (!hasExplicitPb && standIns != nullptr)
					standIns->Engage(StandInKind::CodecModelPbStandIn, "TdV2GpuEngine STRICT/CELL using refs[0].clean stand-in for Field B P_B");

				std::string pHdr = staticGeom;
				pHdr += VpvGpu::Define("TD_STEP_SCALE_MIN", kCellStepScaleMin);
				pHdr += VpvGpu::Define("TD_STEP_SCALE_MAX", kCellStepScaleMax);
				pHdr += Shaders::BuildDctBasisHeader();
				pHdr += Shaders::kCoreHelpers;
				pHdr += Shaders::kPass3bDctHeader;
				pl_shader_desc dp[7] = {
					VpvGpu::Sampled("delta_in", sc.deltaScratch),
					VpvGpu::Sampled("cur_clean", p.curCleanTex),
					VpvGpu::Sampled("pred_clean", hasExplicitPb ? p.codecModelPbTex : sc.warpedVal),
					VpvGpu::Sampled("cell_focus", sc.cellFocus),
					VpvGpu::Sampled("ref_flags", finalRefFlagsTex),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Storage("delta_out", dstOutDelta)
				};
				pl_shader_var pv[11] = {
					VpvGpu::Var("u_bx0", &uBx0),
					VpvGpu::Var("u_by0", &uBy0),
					VpvGpu::Var("u_bx1", &uBx1),
					VpvGpu::Var("u_by1", &uBy1),
					VpvGpu::Var("u_pocs_iters", &uPocsIters),
					VpvGpu::Var("u_residual_mode", &uResidualMode),
					VpvGpu::Var("u_step_delta", &uStepDelta),
					VpvGpu::Var("u_deadzone_phi", &uDeadZonePhi),
					VpvGpu::Var("u_peak_code", &uPeakCode),
					VpvGpu::Var("u_has_explicit_pb", &uHasExplicitPb),
					VpvGpu::Var("u_cell_prior_has_step_scale", &uCellPriorHasStepScale)
				};
				const size_t shmemBytes = (5u * 64u) * sizeof(float);
				if (!runPass("td_v2_p3_dct_pocs", pHdr, Shaders::kPass3bDctPocs, dp, 7, pv, 11,
				             Detail::GroupsFor(w, 8), Detail::GroupsFor(h, 8), 8, 8, shmemBytes))
				{
					resetCarryIfActive();
					return failCore("td_v2_p3_dct_pocs dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// ---- PASS 4 (Speedup 4): Fused Recompose + Layer-4 TdGuardPort + Apply in ONE Dispatch! ----
			{
				std::string p4Hdr = staticGeom;
				p4Hdr += Shaders::BuildFineTapHeader();
				p4Hdr += VpvGpu::Define("TD_GAIN_FINE", gains.fine);
				p4Hdr += VpvGpu::Define("TD_MIN_VAR_RETENTION", TdGuardPort::kMinVarianceRetention);
				p4Hdr += Shaders::kCoreHelpers;
				p4Hdr += Shaders::kPass4FusedApplyGuardHeader;

				pl_shader_desc d4[9] = {
					VpvGpu::Sampled("rec_y", p.recYTex),
					VpvGpu::Sampled("cur_clean", sameCleanRec ? sc.motionGate : p.curCleanTex),
					VpvGpu::Sampled("delta_tex", dstOutDelta),
					VpvGpu::Sampled("cell_prior", cellPriorTex),
					VpvGpu::Sampled("cell_focus", sc.cellFocus),
					VpvGpu::Storage("out_clean", dstOutClean),
					VpvGpu::Storage("carry_clean", carryCleanTex_),
					VpvGpu::Storage("guard_cell", dstGuardCell),
					VpvGpu::Storage("out_y", dstOutY)
				};
				pl_shader_var v4[8] = {
					VpvGpu::Var("u_ax0", &uAx0),
					VpvGpu::Var("u_ay0", &uAy0),
					VpvGpu::Var("u_ax1", &uAx1),
					VpvGpu::Var("u_ay1", &uAy1),
					VpvGpu::Var("u_has_clean_split", &uHasCleanSplit),
					VpvGpu::Var("u_same_clean_rec", &uSameCleanRec),
					VpvGpu::Var("u_shadow_mode", &uShadowMode),
					VpvGpu::Var("u_fade_keep", &uFadeKeep)
				};
				const size_t shmemP4 = (3u * fineTileDim * fineTileDim + 3u * fineTileDim * 16u + 512u + 2u) * sizeof(float);
				const std::string p4Body = Shaders::BuildPass4FusedApplyGuardBody();
				if (!runPass("td_v2_p4_fused_apply_guard", p4Hdr, p4Body.c_str(),
				             d4, 9, v4, 8, cw, ch, 16, 16, shmemP4))
				{
					resetCarryIfActive();
					return failCore("td_v2_p4_fused_apply_guard dispatch failed", StandInKind::GpuDispatchFailed);
				}
			}

			// Update persistent carry tracking (strictly grain-free carryCleanTex_).
			hasCarry_ = true;
			carryW_ = w;
			carryH_ = h;
			hasLastSeq_ = true;
			lastSeq_ = p.seq;
			oc.dispatches = dispatches;
			lastDispatchCount_ = dispatches;

			if (!p.readbackCellDiag)
			{
				// Zero-copy realtime path (no CPU readback): engage ALL host-deterministic StandInRegistry tokens cleanly (Rule 4)!
				if (standIns != nullptr)
				{
					const bool hasActiveArea = !ar.Empty();
					// 1. Pass 4 always evaluates Layer-4 TdGuardPort on active cells when K > 0.
					if (K > 0 && hasActiveArea)
						standIns->Engage(StandInKind::GrainGuardW4c);

					// 2. Single-ref / role-unpartnered A1->A2 fallback (mirrors td_find_independent_a1_partner in Pass 2B 1:1).
					if (cfg_.estimator != VarianceEstimator::A2_RoundingCells && K > 0 && hasActiveArea)
					{
						bool anyRefLacksA1Partner = (K == 1);
						if (!anyRefLacksA1Partner)
						{
							for (int k = 0; k < K; ++k)
							{
								const int myRole = refSpecs[static_cast<size_t>(k)].refRole;
								bool foundPartner = false;
								for (int j = 0; j < K; ++j)
								{
									if (j == k)
										continue;
									const int other = refSpecs[static_cast<size_t>(j)].refRole;
									if (other == 2) // Carry never instruments another ref in A1
										continue;
									if ((myRole == 0 && (other == 1 || other == 3)) ||
									    (myRole == 1 && (other == 0 || other == 3)) ||
									    (myRole == 3 && (other == 1 || other == 0)) ||
									    (myRole == 2 && other == 1))
									{
										foundPartner = true;
										break;
									}
								}
								if (!foundPartner)
								{
									anyRefLacksA1Partner = true;
									break;
								}
							}
						}
						if (anyRefLacksA1Partner)
							standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
					}

					// 3. TransformUnknownBound: engaged when Pass 3B did not run OR when non-8-aligned remainder strips exist.
					const bool hasRemainderStrips =
						(bxStart > ar.x0) || (bxEnd < ar.x1) || (byStart > ar.y0) || (byEnd < ar.y1);
					if (hasActiveArea && (!runDctPocs || hasRemainderStrips))
						standIns->Engage(StandInKind::TransformUnknownBound);

					// 4. Pass-1 face / non-rigid mask visibility.
					if (p.faceMaskTex != nullptr || cpuFaceMaskedCount > 0)
						standIns->Engage(StandInKind::FaceNonRigidMask);
				}
				oc.faceMaskedCells = cpuFaceMaskedCount;
				return true;
			}

			// Read back tiny cell-grid diagnostics & U19 TdMaps.
			oc.tdMaps.resize(numCells);
			std::vector<float> mapsBuf(numCells * 3u * 4u, 0.0f);
			std::vector<float> diagBuf(numCells * 4u, 0.0f);
			std::vector<float> guardBuf(numCells * 2u, 0.0f);
			std::vector<float> warpDiagBuf(numCells * 2u, 0.0f);
			if (!Detail::DownloadFloats(gpu, dstTdMaps, mapsBuf.data()) ||
			    !Detail::DownloadFloats(gpu, dstDiagFlags, diagBuf.data()) ||
			    !Detail::DownloadFloats(gpu, dstGuardCell, guardBuf.data()) ||
			    !Detail::DownloadFloats(gpu, dstWarpDiag, warpDiagBuf.data()))
			{
				resetCarryIfActive();
				return failCore("TdV2GpuEngine diagnostic download failed", StandInKind::GpuDispatchFailed);
			}

			bool anyGuardEvaluated = false;
			bool anyBoundEngaged = false;
			for (int cy = 0; cy < ch; ++cy)
			{
				for (int cx = 0; cx < cw; ++cx)
				{
					const size_t cIdx = static_cast<size_t>(cy) * cw + cx;
					float packed[TdMapsView::kPlanes][4] = {};
					for (int pl = 0; pl < TdMapsView::kPlanes; ++pl)
					{
						const size_t row = static_cast<size_t>(pl * ch + cy);
						const size_t texel = row * static_cast<size_t>(cw) + static_cast<size_t>(cx);
						for (int c = 0; c < 4; ++c)
							packed[pl][c] = mapsBuf[texel * 4u + static_cast<size_t>(c)];
					}
					oc.tdMaps[cIdx] = TdMapsView::Unpack(packed);

					if (diagBuf[cIdx * 4u + 0u] > 0.5f)
						++oc.focusVetoCells;
					const bool cellDiagNonFinite = (diagBuf[cIdx * 4u + 1u] > 0.5f);
					if (diagBuf[cIdx * 4u + 2u] > 0.5f)
						++oc.singularCells;
					if (diagBuf[cIdx * 4u + 3u] > 0.5f)
						++oc.singleRefA2Cells;

					const unsigned gFlags = static_cast<unsigned>(Detail::NonNegRoundToSize(guardBuf[cIdx * 2u + 1u]));
					if ((gFlags & 2u) != 0u)
						anyGuardEvaluated = true;
					if ((gFlags & 1u) != 0u && guardBuf[cIdx * 2u + 0u] > 0.0f)
						++oc.guardViolationCells;
					const bool cellGuardNonFinite = ((gFlags & 4u) != 0u);
					if ((gFlags & 8u) != 0u)
						anyBoundEngaged = true;

					oc.borderBilinearPixels += Detail::NonNegRoundToSize(warpDiagBuf[cIdx * 2u + 0u]);
					const unsigned wMask = static_cast<unsigned>(Detail::NonNegRoundToSize(warpDiagBuf[cIdx * 2u + 1u]));
					const bool cellRefNonFinite = ((wMask & 16u) != 0u);
					if ((wMask & 8u) != 0u)
						++oc.faceMaskedCells;

					// Unique-cell nonFiniteCells counting matching CPU TdV2ReferenceEngine::nonFiniteCellFlag 1:1.
					if (cellDiagNonFinite || cellGuardNonFinite || cellRefNonFinite)
						++oc.nonFiniteCells;
				}
			}

			// Mirror all visible StandInRegistry engagements identically to TdV2ReferenceEngine.
			if (standIns != nullptr)
			{
				if (oc.borderBilinearPixels > 0)
					standIns->Engage(StandInKind::BorderBilinearFallback);
				if (oc.nonFiniteCells > 0)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuEngine GPU non-finite closure");
				if (oc.singularCells > 0)
					standIns->Engage(StandInKind::SingularSolveClosure, "TdV2GpuEngine singular covariance solve");
				if (oc.singleRefA2Cells > 0)
					standIns->Engage(StandInKind::SingleRefA1ToA2Fallback);
				if (anyGuardEvaluated)
					standIns->Engage(StandInKind::GrainGuardW4c);
				if (anyBoundEngaged)
					standIns->Engage(StandInKind::TransformUnknownBound);
				if (oc.faceMaskedCells > 0)
					standIns->Engage(StandInKind::FaceNonRigidMask);
			}

			return true;
		}

		bool EnsurePrivateScratch(pl_gpu gpu, int w, int h, int wHalf, int hHalf, int cw, int ch, bool compactBands,
		                          pl_fmt fmtR16, pl_fmt fmtR32, pl_fmt fmtRG32, pl_fmt fmtRGBA32, ScratchSet& out)
		{
			bool ok = Detail::EnsureTex(gpu, privWarpedVal_, w, h, fmtR32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privRefFlags_[0], w, h, fmtR16, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privRefFlags_[1], w, h, fmtR16, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privDeltaScratch_, w, h, fmtRG32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privMotionGate_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellWeights_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellDiagC_, 3 * cw, ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellStats_, 3 * cw, 3 * ch, fmtRGBA32, true, true, false, false) &&
			          Detail::EnsureTex(gpu, privCellFocus_, cw, ch, fmtRGBA32, true, true, false, false);
			if (ok && compactBands)
			{
				// H7: ALL band storage strictly FP32 RG32F per plan L60 (never FP16).
				ok = Detail::EnsureTex(gpu, privCurBandsMH_, w, h, fmtRG32, true, true, false, false) &&
				     Detail::EnsureTex(gpu, privCurBandsLD3_, wHalf, hHalf, fmtRG32, true, true, false, false) &&
				     Detail::EnsureTex(gpu, privBandHalfTmp_, wHalf, hHalf, fmtRG32, true, true, false, false);
				for (int i = 0; i < kMaxGpuRefs && ok; ++i)
				{
					ok = Detail::EnsureTex(gpu, privRefBandsMH_[i], w, h, fmtRG32, true, true, false, false) &&
					     Detail::EnsureTex(gpu, privRefBandsLD3_[i], wHalf, hHalf, fmtRG32, true, true, false, false);
				}
			}
			else if (ok)
			{
				ok = Detail::EnsureTex(gpu, privCurBands_, w, h, fmtRGBA32, true, true, false, false) &&
				     Detail::EnsureTex(gpu, privBandTmpH_, w, h, fmtRGBA32, true, true, false, false);
				for (int i = 0; i < kMaxGpuRefs && ok; ++i)
					ok = Detail::EnsureTex(gpu, privRefBands_[i], w, h, fmtRGBA32, true, true, false, false);
			}
			if (!ok)
				return false;

			out.warpedVal = privWarpedVal_;
			out.refFlags[0] = privRefFlags_[0];
			out.refFlags[1] = privRefFlags_[1];
			out.deltaScratch = privDeltaScratch_;
			out.motionGate = privMotionGate_;
			out.cellWeights = privCellWeights_;
			out.cellDiagC = privCellDiagC_;
			out.cellStats = privCellStats_;
			out.cellFocus = privCellFocus_;
			if (compactBands)
			{
				out.curBandsMH = privCurBandsMH_;
				out.curBandsLD3 = privCurBandsLD3_;
				out.bandHalfTmp = privBandHalfTmp_;
				for (int i = 0; i < kMaxGpuRefs; ++i)
				{
					out.refBandsMH[i] = privRefBandsMH_[i];
					out.refBandsLD3[i] = privRefBandsLD3_[i];
				}
			}
			else
			{
				out.curBands = privCurBands_;
				out.bandTmpH = privBandTmpH_;
				for (int i = 0; i < kMaxGpuRefs; ++i)
					out.refBands[i] = privRefBands_[i];
			}
			return true;
		}

		void DestroyPrivateScratch(pl_gpu gpu)
		{
			if (!gpu)
				return;
			for (pl_tex* t : {
				&privCurBands_, &privRefBands_[0], &privRefBands_[1], &privRefBands_[2], &privBandTmpH_,
				&privCurBandsMH_, &privRefBandsMH_[0], &privRefBandsMH_[1], &privRefBandsMH_[2],
				&privCurBandsLD3_, &privRefBandsLD3_[0], &privRefBandsLD3_[1], &privRefBandsLD3_[2], &privBandHalfTmp_,
				&privWarpedVal_, &privRefFlags_[0], &privRefFlags_[1],
				&privDeltaScratch_,
				&privMotionGate_, &privCellWeights_, &privCellDiagC_, &privCellStats_, &privCellFocus_ })
			{
				if (*t != nullptr)
					pl_tex_destroy(gpu, t);
			}
		}

		Config cfg_;
		bool hasCarry_ = false;
		int carryW_ = 0;
		int carryH_ = 0;
		bool hasLastSeq_ = false;
		uint64_t lastSeq_ = 0;
		pl_tex carryCleanTex_ = nullptr;

		// Owned private scratch textures (used ONLY when arena == nullptr).
		pl_tex privCurBands_ = nullptr;
		pl_tex privRefBands_[kMaxGpuRefs] = {};
		pl_tex privBandTmpH_ = nullptr;
		pl_tex privCurBandsMH_ = nullptr;
		pl_tex privRefBandsMH_[kMaxGpuRefs] = {};
		pl_tex privCurBandsLD3_ = nullptr;
		pl_tex privRefBandsLD3_[kMaxGpuRefs] = {};
		pl_tex privBandHalfTmp_ = nullptr;
		pl_tex privWarpedVal_ = nullptr;
		pl_tex privRefFlags_[2] = {};
		pl_tex privDeltaScratch_ = nullptr;
		pl_tex privMotionGate_ = nullptr;
		pl_tex privCellWeights_ = nullptr;
		pl_tex privCellDiagC_ = nullptr;
		pl_tex privCellStats_ = nullptr;
		pl_tex privCellFocus_ = nullptr;

		// Owned IO staging and readback textures (inOwnGrainTex_ eliminated by fused Pass 4!).
		pl_tex inRecYTex_ = nullptr;
		pl_tex inCleanYTex_ = nullptr;
		pl_tex inRefCleanTex_ = nullptr;
		pl_tex inCodecModelPbTex_ = nullptr;
		pl_tex inMotionVecTex_ = nullptr;
		pl_tex inMotionMetaTex_ = nullptr;
		pl_tex inCellPriorTex_ = nullptr;
		pl_tex inFaceMaskTex_ = nullptr;
		pl_tex outYTex_ = nullptr;
		pl_tex outCleanTex_ = nullptr;
		pl_tex outDeltaTex_ = nullptr;
		pl_tex outTdMapsTex_ = nullptr;
		pl_tex outDiagFlagsTex_ = nullptr;
		pl_tex outGuardCellTex_ = nullptr;
		pl_tex outWarpDiagTex_ = nullptr;

		// Cached Chroma GPU Apply textures (Speedup 7: reused across frames, freed in Release(gpu)).
		pl_tex chromaCodedDeltaTex_ = nullptr;
		pl_tex chromaDelivUvTex_ = nullptr;
		pl_tex chromaDelivRgbTex_ = nullptr;
		pl_tex chromaOutUvTex_ = nullptr;
		pl_tex chromaOutRgbTex_ = nullptr;
		pl_tex chromaOutDeltaTex_ = nullptr;

		std::string lastError_;
		uint64_t failures_ = 0;
		int lastDispatchCount_ = 0;
		bool scratchShort_ = false;
	};

} // namespace Gpu
} // namespace TdV2
