// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Milestone 2: Native Direct3D 11 CS 5.0 GPU Stage A (3-frame same-polarity symmetric residual,
// Sobel gradient + bilinear 2nd-order curvature Taylor truncation variance, optional half-res
// Cb/Cr 3-DOF chi^2 residual, 4x4 cell groupshared Neyman-Pearson matched-filter reduction,
// bounded InterlockedAdd active-cell compaction <= 4 KB, and 3-slot async staging readback ring).
//
// Zero Windows headers in this header (forward-declared D3D11 types + PIMPL).
// Zero static / thread_local mutable state. Strictly default-OFF (0 VRAM, 0 dispatches when OFF).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "FastObjectResidualRef.h"
#include "FastObjectTypes.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace FastObjects
{
	// GPU-side failure classification (Rule 4: zero silent fallbacks; always counted & surfaced).
	enum class GpuFailure : uint8_t
	{
		None = 0,
		Unavailable,
		ShaderCompile,
		ResourceCreate,
		InvalidInput,
		Readback,
		Count
	};

	inline const char* GpuFailureText(GpuFailure f)
	{
		switch (f)
		{
		case GpuFailure::Unavailable: return "gpu unavailable";
		case GpuFailure::ShaderCompile: return "cs_5_0 shader compile failed";
		case GpuFailure::ResourceCreate: return "gpu resource creation failed";
		case GpuFailure::InvalidInput: return "invalid gpu residual input";
		case GpuFailure::Readback: return "gpu staging readback failed";
		default: return "none";
		}
	}

	// Exact 16-byte active cell record emitted by Pass 2 (CSCompactActiveCells) into RWStructuredBuffer.
	// Slot 0 of the buffer is the header: cellXY = exact uncapped totalActiveCells, flagsGrow = (cellW & 0xFFFF) | (cellH << 16).
	// Slots [1..kMaxActiveCells] hold the compacted active cells (total buffer <= 257 * 16 B = 4,112 B).
	struct GpuActiveCell
	{
		uint32_t cellXY = 0;     // Low 16 bits: cellX; High 16 bits: cellY
		uint32_t flagsGrow = 0;  // Low 16 bits: growCount; High 16 bits: cellFlags (bit 0 active, bit 1 streak, bit 2 guardOk)
		float maxAbsZ = 0.0f;    // Peak |z| (or sqrt(maxChi2) when chroma is larger)
		float zCell = 0.0f;      // Signed Neyman-Pearson matched-filter cell z-score

		uint32_t CellX() const { return cellXY & 0xFFFFu; }
		uint32_t CellY() const { return (cellXY >> 16) & 0xFFFFu; }
		uint32_t GrowCount() const { return flagsGrow & 0xFFFFu; }
		uint32_t Flags() const { return (flagsGrow >> 16) & 0xFFFFu; }
		bool IsStreakTrigger() const { return (Flags() & 2u) != 0u; }
	};
	static_assert(sizeof(GpuActiveCell) == 16, "GpuActiveCell must be 16-byte aligned for CS 5.0 StructuredBuffer");

	// Guard bitmask flags packed into optional guardMap SRV (DXGI_FORMAT_R32_UINT, guardGridW x guardGridH).
	namespace GpuGuardBits
	{
		constexpr uint32_t kExcluded = 1u << 0;       // Bar / text-guard / P8 Overlay / CE-WATER >= 0.5 / large P8 Object
		constexpr uint32_t kNearLargeObj = 1u << 1;   // Within 1 block of a large P8 Label::Object
		constexpr uint32_t kOverlay = 1u << 2;        // P8 Label::Overlay
		constexpr uint32_t kWater = 1u << 3;          // CE-WATER waterProb >= 0.5
		constexpr uint32_t kNonCamera = 1u << 4;      // P8 blockLabels != Label::Camera (excluded from MAD noise floor)
	}

	// GPU Stage A inputs (D3D11 SRVs on the caller's device; zero private copies required).
	struct GpuResidualInput
	{
		// Half-res luma triplet SRVs (R16_UNORM or R32_FLOAT, width x height).
		// Bidirectional: prev = Y_{t-1}, cur = Y_t, next = Y_{t+1}.
		// Causal fallback (causalTwoBack = true): prev = Y_{t-1}, cur = Y_t, next = Y_{t-2}.
		ID3D11ShaderResourceView* lumaPrevSrv = nullptr;
		ID3D11ShaderResourceView* lumaCurSrv = nullptr;
		ID3D11ShaderResourceView* lumaNextSrv = nullptr;
		int width = 0;
		int height = 0;
		float sourceScale = 2.0f;

		// Optional native half-res chroma triplet SRVs (R32_FLOAT / R16_UNORM, same width x height).
		ID3D11ShaderResourceView* cbPrevSrv = nullptr;
		ID3D11ShaderResourceView* cbCurSrv = nullptr;
		ID3D11ShaderResourceView* cbNextSrv = nullptr;
		ID3D11ShaderResourceView* crPrevSrv = nullptr;
		ID3D11ShaderResourceView* crCurSrv = nullptr;
		ID3D11ShaderResourceView* crNextSrv = nullptr;

		// Camera homographies in picture-centred source px (same convention as ResidualInput).
		CameraModel::Params camPrevToCur;
		CameraModel::Params camCurToNext;
		bool camPrevUsable = false;
		bool camNextUsable = false;
		double camSigmaPx = 0.0;

		CameraModel::Rect picture;
		bool sceneDiscontinuity = false;
		bool causalTwoBack = false;
		bool dormantCoarseMode = false; // True -> 2x2 decimated coarse wake-up pass (~1/4 threads, <= 0.06 ms @ 4K)
		bool writeZMap = false;         // Optional R32_FLOAT half-res z-map UAV (default false: saves 8.3 MB VRAM & writes @ 4K)
		bool writeCellGrid = false;     // Optional R32G32B32A32_FLOAT cell-grid UAV (default false: saves 2.07 MB VRAM & writes @ 4K)

		// Optional P4 fade normalization.
		bool fadePrev = false;
		double fadeGainPrev = 1.0;
		double fadeOffsetPrev = 0.0;
		bool fadeNext = false;
		double fadeGainNext = 1.0;
		double fadeOffsetNext = 0.0;

		// Optional P10g per-block grain variance SRV (R32_FLOAT, grainGridW x grainGridH).
		ID3D11ShaderResourceView* grainVarSrv = nullptr;
		int grainGridW = 0;
		int grainGridH = 0;
		double grainBlockSrcPx = 16.0;
		// Optional scalar grain std (in input luma units); < 0 triggers GPU Camera-block MAD fallback when grainVarSrv == nullptr.
		double uniformGrainSigma = -1.0;
		double uniformChromaGrainSigma = -1.0; // < 0 computes GPU chroma MAD when uniformGrainSigma < 0, else reuses uniformGrainSigma

		// Optional packed guard grid SRV (DXGI_FORMAT_R32_UINT, GpuGuardBits, guardGridW x guardGridH).
		ID3D11ShaderResourceView* guardMapSrv = nullptr;
		int guardGridW = 0;
		int guardGridH = 0;
		double guardBlockSrcPx = 16.0;
	};

	struct GpuTiming
	{
		bool valid = false;
		double gpuMs = 0.0; // Total Pass 0 + Pass 1 + Pass 2 GPU dispatch time (ms) via D3D11 timestamp queries
	};

	// Compact GPU->CPU readback result (<= 4,112 B total transfer).
	struct GpuReadbackResult
	{
		uint64_t frameIndex = 0;
		uint32_t totalActiveCells = 0; // Exact uncapped count from Pass 2 InterlockedAdd header
		uint32_t cellCount = 0;        // min(totalActiveCells, kMaxActiveCells)
		uint32_t cellW = 0;
		uint32_t cellH = 0;
		uint32_t abstainReason = AbstainReason::None;
		bool clutterOverflow = false;  // True when totalActiveCells > cfg.maxActiveCells (Rule 4)
		bool chromaUsed = false;
		bool causalMode = false;
		bool dormantCoarse = false;
		float noiseSigmaEst = 0.0f;    // Estimated baseline diff noise floor sqrt(baseDiffVarLuma) (incl. GPU MAD)
		float noiseSigmaCbEst = 0.0f;  // Estimated baseline Cb diff noise floor sqrt(baseDiffVarCb)
		float noiseSigmaCrEst = 0.0f;  // Estimated baseline Cr diff noise floor sqrt(baseDiffVarCr)
		GpuTiming timing;
		GpuActiveCell cells[kMaxActiveCells];

		void Clear()
		{
			frameIndex = 0;
			totalActiveCells = 0;
			cellCount = 0;
			cellW = 0;
			cellH = 0;
			abstainReason = AbstainReason::None;
			clutterOverflow = false;
			chromaUsed = false;
			causalMode = false;
			dormantCoarse = false;
			noiseSigmaEst = 0.0f;
			noiseSigmaCbEst = 0.0f;
			noiseSigmaCrEst = 0.0f;
			timing = GpuTiming{};
		}
	};

	// Returns the exact HLSL CS 5.0 source compiled by FastObjectGpu (for inspection / standalone compile tests).
	std::string FastObjectGpuHlslSource();

	// Builds the packed uint32 guard grid (GpuGuardBits) from P8 labels/excluded/CE-WATER maps,
	// matching FastObjectResidualRef's large-object connected-component exclusion exactly.
	void BuildGpuGuardGrid(
		const ResidualInput& in,
		double maxMinorAxisFrac,
		std::vector<uint32_t>& outGuardGrid);

	// Sparse CPU blob moment extractor operating strictly over the <= 256 active 4x4 cells returned by GPU Pass 2
	// (+ 1-cell border halo for hysteresis connectivity). Produces exact BlobCandidates matching DetectFastObjectBlobs
	// while touching 0 pixels on quiet/overflow frames and < 0.02% of a 4K frame on active sports frames.
	bool ExtractBlobsFromGpuCells(
		const Config& cfg,
		const ResidualInput& cpuIn,
		const GpuReadbackResult& gpuRes,
		ResidualWorkspace& ws,
		ResidualOutput& out);

	// Native Direct3D 11 CS 5.0 Stage A residual & 4x4 cell compaction engine.
	// Owns its compiled shaders, transient UAVs, and 3-slot async staging readback ring on one ID3D11Device.
	// Not thread-safe; call from the device context's owning thread.
	class FastObjectGpu
	{
	public:
		static constexpr int kStagingRingSlots = 3;

		FastObjectGpu();
		~FastObjectGpu();

		// Compiles all 3 CS 5.0 kernels on `device`. Returns false with diagnostic reason in `why` on failure.
		// Does NOT allocate frame-sized UAV textures until the first enabled Submit (0 VRAM when OFF).
		bool Init(ID3D11Device* device, std::string* why = nullptr);
		bool Available() const { return ready_; }

		// Explicitly releases all COM resources (textures, UAVs, staging buffers, queries, shaders).
		// Guaranteed 0 VRAM leaks across create / resize / destroy cycles.
		void Release();

		// Explicit VRAM accounting helper (bytes) for VpSkeleton::VramUsers.
		// Returns 0 when enabled == false or dimensions <= 0.
		// With default writeZMap=false & writeCellGrid=false, allocates only ~16.8 KB total even at 4K!
		static size_t EstimatedVramBytes(
			int width,
			int height,
			bool writeZMap = false,
			bool enabled = true,
			bool writeCellGrid = false);

		void EnableTiming(bool on) { timingEnabled_ = on; }
		bool TimingEnabled() const { return timingEnabled_; }

		// Queues Pass 0 (clear header) + Pass 1 (3-frame residual & 4x4 groupshared reduce) +
		// Pass 2 (InterlockedAdd active-cell compaction) + async CopyResource into the next staging ring slot.
		// When cfg.enabled == false or cfg.originalPathway == true: returns false immediately (0 VRAM, 0 dispatches).
		bool Submit(
			ID3D11DeviceContext* context,
			const Config& cfg,
			const GpuResidualInput& in,
			uint64_t frameIndex = 0);

		// Collects the oldest in-flight staging ring slot.
		// If `wait == false` and the GPU has not completed that slot yet, returns false without blocking.
		// If `wait == true`, blocks until the GPU completes that slot and populates `out`.
		bool Collect(ID3D11DeviceContext* context, bool wait, GpuReadbackResult& out);

		// Convenience synchronous path for unit/parity tests and GPU timing harnesses (Submit + Collect(wait=true)).
		bool RunSync(
			ID3D11DeviceContext* context,
			const Config& cfg,
			const GpuResidualInput& in,
			GpuReadbackResult& out,
			uint64_t frameIndex = 0);

		int PendingCount() const;
		size_t AllocatedVramBytes() const { return allocatedVramBytes_; }
		GpuFailure LastFailure() const { return lastFailure_; }
		const std::string& LastError() const { return lastError_; }

		// Read-only SRV accessors for transient UAV outputs (for downstream GPU consumers / parity tests).
		ID3D11ShaderResourceView* ZMapSrv() const;
		ID3D11ShaderResourceView* CellGridSrv() const;

	private:
		FastObjectGpu(const FastObjectGpu&) = delete;
		FastObjectGpu& operator=(const FastObjectGpu&) = delete;

		bool EnsureFrameResources(int width, int height, bool writeZMap, bool writeCellGrid);
		void SetFailure(GpuFailure f, const std::string& msg);

		struct Impl;
		Impl* impl_ = nullptr;
		bool ready_ = false;
		bool timingEnabled_ = false;
		size_t allocatedVramBytes_ = 0;
		GpuFailure lastFailure_ = GpuFailure::None;
		std::string lastError_;
	};

	// Convenience D3D11 upload helper for CPU float planes (used by parity tests & real-clip blind eval).
	// Uploads ResidualInput planes to R32_FLOAT / R32_UINT textures and produces a ready GpuResidualInput.
	class FastObjectGpuUploader
	{
	public:
		FastObjectGpuUploader();
		~FastObjectGpuUploader();

		bool Init(ID3D11Device* device, std::string* why = nullptr);
		void Release();

		bool Upload(
			ID3D11DeviceContext* context,
			const Config& cfg,
			const ResidualInput& cpuIn,
			GpuResidualInput& outGpuIn,
			std::string* why = nullptr);

		size_t AllocatedVramBytes() const { return allocatedVramBytes_; }

	private:
		FastObjectGpuUploader(const FastObjectGpuUploader&) = delete;
		FastObjectGpuUploader& operator=(const FastObjectGpuUploader&) = delete;

		struct Impl;
		Impl* impl_ = nullptr;
		size_t allocatedVramBytes_ = 0;
	};
}
