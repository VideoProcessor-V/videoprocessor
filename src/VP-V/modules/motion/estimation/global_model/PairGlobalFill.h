// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// motion/estimation/global_model: the OWNER FILL of kP7CameraFit's pair-global sub-record (U10 C1; VariantKey{ param0 = 1 },
// kAxisParam0) for both AcquireForm::Cpu (80-byte deterministic little-endian PairGlobal POD) and AcquireForm::Gpu
// (zero-copy 1x1 RGBA16F globalVector / globalSecond view).
// Contract (coreplan-C8-10-U10.md):
//   - Owner unit: "motion/estimation/global_model" (ProductCatalog.h kP7CameraFit row).
//   - Variant axis: kAxisParam0 only; param0 == 1 is the pair-global sub-record. Any other param0 value declines visibly;
//     any other variant axis is refused by SkeletonMemo as VariantUndeclared.
//   - CPU form: any thread (renderThreadOnly = false), keepFrames = 1. When no custom provider seam is injected, reads
//     kP2VectorPair (AcquireForm::Cpu) via nested Acquire and decodes P2PairRecord for the engine-global fallback; if absent
//     or GlobalSet() is false, declines visibly.
//   - GPU form: zero-copy frame-scoped view (view = true, frameScoped = true, renderThreadOnly = true, gpu.bytes = 0,
//     Scope::Frame) so matcher-owned pl_tex handles never outlive the render frame.
//   - Failures are NEVER silent: declines carry explicit declineReason and consumer helpers populate
//     FallbackCatalog::PairGlobalUnavailable ("motion.pair_global_unavailable").
// Header-only, standalone C++14 (registered by VectorEngineExtension::OnFrame on the first VP-V Entrance).
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/VariantKey.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/motion/estimation/OpticalFlowTrust.h>
#include <VP-V/modules/motion/estimation/P2PairRecord.h>
#include <VP-V/modules/motion/estimation/global_model/MatcherGlobalD1.h>

#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace VpMotion
{
	static constexpr const char* kPairGlobalUnit = "motion/estimation/global_model";   // ProductCatalog.h kP7CameraFit owner
	static constexpr uint16_t kPairGlobalParam0 = 1u;                                  // VariantKey::param0 for pair-global sub-record
	static constexpr uint32_t kPairGlobalRecordMagic = 0x31475050u;                    // "PPG1" little-endian
	static constexpr uint8_t kPairGlobalFlagValid = 1u;
	// Deterministic wire size: magic(4) + src(1) + flags(1) + reserved(2) + p2Epoch(8) + vec[2][4](32) + second[2][4](32) = 80 bytes.
	static constexpr size_t kPairGlobalRecordBytes = 80u;
	static_assert(kPairGlobalRecordBytes == 4u + 1u + 1u + 2u + 8u + 2u * 4u * sizeof(float) * 2u,
		"PairGlobal wire format must be exactly 80 bytes");

	inline VpSkeleton::VariantKey PairGlobalKey()
	{
		VpSkeleton::VariantKey k;
		k.param0 = kPairGlobalParam0;
		return k;
	}

	enum class PairGlobalSource : uint8_t
	{
		Cleared = 0,
		CameraModel = 1,
		Engine = 2,
		GapSeed = 3,
		MatcherD1 = 4
	};

	inline const char* PairGlobalSourceName(PairGlobalSource s)
	{
		switch (s)
		{
		case PairGlobalSource::Cleared: return "cleared";
		case PairGlobalSource::CameraModel: return "camera_model";
		case PairGlobalSource::Engine: return "engine";
		case PairGlobalSource::GapSeed: return "gap_seed";
		case PairGlobalSource::MatcherD1: return "matcher_d1";
		}
		return "?";
	}

	// D1 format pair-global record: fwd [0], bwd [1]; each (x, y, ambiguity, w) in 1/8-size texels.
	struct PairGlobal
	{
		float vec[2][4]{};
		float second[2][4]{};
		PairGlobalSource src = PairGlobalSource::Cleared;
		uint64_t p2Epoch = 0;
		bool valid = false;
	};

	// D1 pyramid scale: L1 is 1/8 of the matcher's input resolution (8 input px per D1 texel).
	static constexpr float kPairGlobalD1TexelScale = 8.0f;
	// D1 confidence z for camera-model prior: confident midpoint below OpticalFlowTrust::GlobalUsable's 0.9 ambiguity ceiling.
	static constexpr float kPairGlobalConfidentZ = 0.5f;
	// D1 validity w channel for a populated vector (OpticalFlowTrust::GlobalUsable requires w > 0.5).
	static constexpr float kPairGlobalValidW = 1.0f;

	// Pure CPU selection law inputs (U10 C3: moved verbatim from matcher/DejudderWarp.cpp).
	struct PairGlobalSelectionInput
	{
		bool cutOrFlash = false;
		bool globalPrior = false;
		bool useModelGlobal = false;
		float modelFwd[2] = {};
		float modelBwd[2] = {};
		float modelInputScale = 1.0f;
		bool opticalFlowGlobal = false;
		bool opticalFlowGlobalFallback = true;
		bool seeded = false;
		bool engineValid = false;
		float engineVec[2][4] = {};
		uint64_t engineEpoch = 0;
		bool gapSeedCandidate = false;
		float gapSeedVec[2][4] = {};
		std::function<bool()> gapSeedHolds; // invoked ONLY when gapSeed is reached (lazy, exact parity with DW.cpp)
	};

	struct PairGlobalSelectionOutcome
	{
		PairGlobalSource source = PairGlobalSource::Cleared;
		bool modelGlobalUsed = false;
		bool globalFromEngine = false;
		bool engineGlobalFallback = false; // increment globalFallbacks when true
		bool searchSeedUsed = false;
		bool searchSeedRejected = false;
		bool needsD1Search = false;        // true when MatcherD1 GPU search must run
		bool clearSecond = false;          // true for Cut/Flash, CameraModel, GapSeed
	};

	// Pure CPU pair-global selection law (exact branch precedence moved verbatim from DejudderWarp.cpp):
	//   1. cutOrFlash (highest precedence: clears vec + second)
	//   2. CameraModel (useModelGlobal && globalPrior)
	//   3. Engine (globalPrior && opticalFlowGlobal && seeded && engineValid && engineGlobalClear)
	//   4. GapSeed (gapSeedCandidate && globalPrior && gapSeedHolds()) â€” rejected falls through to MatcherD1
	//   5. MatcherD1 (globalPrior)
	//   6. Cleared (!globalPrior)
	inline void SelectPairGlobal(const PairGlobalSelectionInput& in, PairGlobal& outRecord, PairGlobalSelectionOutcome& out)
	{
		outRecord = PairGlobal();
		out = PairGlobalSelectionOutcome();

		if (in.cutOrFlash)
		{
			outRecord.src = PairGlobalSource::Cleared;
			outRecord.valid = false;
			out.source = PairGlobalSource::Cleared;
			out.clearSecond = true;
			return;
		}

		const bool engineGlobalClear = !in.opticalFlowGlobalFallback
			|| (OpticalFlowTrust::GlobalUsable(in.engineVec[0]) && OpticalFlowTrust::GlobalUsable(in.engineVec[1]));
		const bool modelGlobal = in.useModelGlobal && in.globalPrior;
		const bool engineEligible = in.globalPrior && in.opticalFlowGlobal && in.seeded && in.engineValid;

		if (!modelGlobal && engineEligible && !engineGlobalClear)
			out.engineGlobalFallback = true;

		if (modelGlobal)
		{
			// Plan 19 phase 2: camera model's centre translation in D1 1/8 texels; z = 0.5 confident; M2 second cleared.
			outRecord.vec[0][0] = in.modelFwd[0] * in.modelInputScale / kPairGlobalD1TexelScale;
			outRecord.vec[0][1] = in.modelFwd[1] * in.modelInputScale / kPairGlobalD1TexelScale;
			outRecord.vec[0][2] = kPairGlobalConfidentZ;
			outRecord.vec[0][3] = kPairGlobalValidW;
			outRecord.vec[1][0] = in.modelBwd[0] * in.modelInputScale / kPairGlobalD1TexelScale;
			outRecord.vec[1][1] = in.modelBwd[1] * in.modelInputScale / kPairGlobalD1TexelScale;
			outRecord.vec[1][2] = kPairGlobalConfidentZ;
			outRecord.vec[1][3] = kPairGlobalValidW;
			outRecord.src = PairGlobalSource::CameraModel;
			outRecord.valid = true;
			out.source = PairGlobalSource::CameraModel;
			out.modelGlobalUsed = true;
			out.clearSecond = true;
		}
		else if (engineEligible && engineGlobalClear)
		{
			for (int d = 0; d < 2; ++d)
				for (int k = 0; k < 4; ++k)
					outRecord.vec[d][k] = in.engineVec[d][k];
			outRecord.src = PairGlobalSource::Engine;
			outRecord.p2Epoch = in.engineEpoch;
			outRecord.valid = true;
			out.source = PairGlobalSource::Engine;
			out.globalFromEngine = true;
			out.clearSecond = false;
		}
		else if (in.gapSeedCandidate && in.globalPrior)
		{
			const bool holds = !in.gapSeedHolds || in.gapSeedHolds();
			if (holds)
			{
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
						outRecord.vec[d][k] = in.gapSeedVec[d][k];
				outRecord.src = PairGlobalSource::GapSeed;
				outRecord.valid = true;
				out.source = PairGlobalSource::GapSeed;
				out.searchSeedUsed = true;
				out.clearSecond = true;
			}
			else
			{
				outRecord.src = PairGlobalSource::MatcherD1;
				outRecord.valid = false;
				out.source = PairGlobalSource::MatcherD1;
				out.searchSeedRejected = true;
				out.needsD1Search = true;
			}
		}
		else if (in.globalPrior)
		{
			outRecord.src = PairGlobalSource::MatcherD1;
			outRecord.valid = false;
			out.source = PairGlobalSource::MatcherD1;
			out.needsD1Search = true;
		}
		else
		{
			outRecord.src = PairGlobalSource::Cleared;
			outRecord.valid = false;
			out.source = PairGlobalSource::Cleared;
		}
	}

	// Loads the engine whole-frame motion into PairGlobalSelectionInput:
	//   - When `memo != nullptr`, acquires kP2VectorPair (AcquireForm::Cpu) through SkeletonMemo and decodes P2PairRecord
	//     (does NOT silently fall back to `directFallback` on a memo miss/decline).
	//   - When `memo == nullptr` (standalone MotionWarp in unit tests), reads `directFallback` when GlobalSet() is true.
	inline bool LoadEngineGlobalFromP2(VpSkeleton::SkeletonMemo* memo, uint64_t seq,
		const P2PairRecord* directFallback, PairGlobalSelectionInput& in)
	{
		in.engineValid = false;
		in.engineEpoch = 0;
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				in.engineVec[d][k] = 0.0f;

		if (memo != nullptr)
		{
			const VpSkeleton::AcquireResult p2 = memo->Acquire(VpSkeleton::kP2VectorPair, seq, VpSkeleton::AcquireForm::Cpu);
			P2PairRecord rec;
			if (p2.Ok() && DecodeP2PairRecord(p2.cpu, p2.cpuBytes, rec) && rec.GlobalSet())
			{
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
						in.engineVec[d][k] = rec.global[d][k];
				in.engineEpoch = rec.generation;
				in.engineValid = true;
				return true;
			}
			return false;
		}
		if (directFallback != nullptr && directFallback->GlobalSet())
		{
			for (int d = 0; d < 2; ++d)
				for (int k = 0; k < 4; ++k)
					in.engineVec[d][k] = directFallback->global[d][k];
			in.engineEpoch = directFallback->generation;
			in.engineValid = true;
			return true;
		}
		return false;
	}

	namespace pair_global_detail
	{
		inline void PutLe16(uint8_t* out, size_t& n, uint16_t v)
		{
			out[n++] = static_cast<uint8_t>(v & 0xFFu);
			out[n++] = static_cast<uint8_t>((v >> 8) & 0xFFu);
		}
		inline void PutLe32(uint8_t* out, size_t& n, uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
				out[n++] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
		}
		inline void PutLe64(uint8_t* out, size_t& n, uint64_t v)
		{
			for (int i = 0; i < 8; ++i)
				out[n++] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
		}
		inline void PutLeFloat(uint8_t* out, size_t& n, float v)
		{
			uint32_t bits = 0;
			std::memcpy(&bits, &v, sizeof(bits));
			PutLe32(out, n, bits);
		}
		inline uint16_t GetLe16(const uint8_t* in, size_t& n)
		{
			const uint16_t v = static_cast<uint16_t>(in[n]) | (static_cast<uint16_t>(in[n + 1]) << 8);
			n += 2;
			return v;
		}
		inline uint32_t GetLe32(const uint8_t* in, size_t& n)
		{
			uint32_t v = 0;
			for (int i = 0; i < 4; ++i)
				v |= static_cast<uint32_t>(in[n++]) << (8 * i);
			return v;
		}
		inline uint64_t GetLe64(const uint8_t* in, size_t& n)
		{
			uint64_t v = 0;
			for (int i = 0; i < 8; ++i)
				v |= static_cast<uint64_t>(in[n++]) << (8 * i);
			return v;
		}
		inline float GetLeFloat(const uint8_t* in, size_t& n)
		{
			const uint32_t bits = GetLe32(in, n);
			float v = 0.0f;
			std::memcpy(&v, &bits, sizeof(v));
			return v;
		}
	}

	// Deterministic little-endian POD codec for AcquireForm::Cpu. Returns kPairGlobalRecordBytes on success, 0 on error.
	inline size_t EncodePairGlobal(const PairGlobal& r, uint8_t* out, size_t cap)
	{
		if (out == nullptr || cap < kPairGlobalRecordBytes)
			return 0;
		const uint8_t rawSrc = static_cast<uint8_t>(r.src);
		if (rawSrc > static_cast<uint8_t>(PairGlobalSource::MatcherD1))
			return 0;
		size_t n = 0;
		pair_global_detail::PutLe32(out, n, kPairGlobalRecordMagic);
		out[n++] = rawSrc;
		out[n++] = r.valid ? kPairGlobalFlagValid : 0u;
		pair_global_detail::PutLe16(out, n, 0u);
		pair_global_detail::PutLe64(out, n, r.p2Epoch);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				pair_global_detail::PutLeFloat(out, n, r.vec[d][k]);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				pair_global_detail::PutLeFloat(out, n, r.second[d][k]);
		return n;
	}

	// False (and `out` untouched) when size != kPairGlobalRecordBytes or header fields are invalid.
	inline bool DecodePairGlobal(const uint8_t* in, size_t bytes, PairGlobal& out)
	{
		if (in == nullptr || bytes != kPairGlobalRecordBytes)
			return false;
		size_t n = 0;
		if (pair_global_detail::GetLe32(in, n) != kPairGlobalRecordMagic)
			return false;
		const uint8_t rawSrc = in[n++];
		const uint8_t flags = in[n++];
		const uint16_t reserved = pair_global_detail::GetLe16(in, n);
		if (rawSrc > static_cast<uint8_t>(PairGlobalSource::MatcherD1)
			|| (flags & ~kPairGlobalFlagValid) != 0u
			|| reserved != 0u)
			return false;
		PairGlobal r;
		r.src = static_cast<PairGlobalSource>(rawSrc);
		r.valid = (flags & kPairGlobalFlagValid) != 0u;
		r.p2Epoch = pair_global_detail::GetLe64(in, n);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				r.vec[d][k] = pair_global_detail::GetLeFloat(in, n);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				r.second[d][k] = pair_global_detail::GetLeFloat(in, n);
		out = r;
		return true;
	}

	// Zero-copy GPU view of the 1x1 RGBA16F globalVector / globalSecond textures plus the decoded PairGlobal record.
	struct PairGlobalGpuView
	{
		pl_tex globalVector = nullptr;      // forward [0]
		pl_tex globalSecond = nullptr;      // forward second [0]
		pl_tex globalVectorBwd = nullptr;   // backward [1]
		pl_tex globalSecondBwd = nullptr;   // backward second [1]
		PairGlobal record;
		PairGlobalSelectionOutcome outcome;
		bool hasOutcome = false;
	};

	// Tag stored in GpuMemoHandle::textureSet to identify a PairGlobalFill GPU view carrying a const PairGlobalGpuView.
	inline const void* PairGlobalGpuTag()
	{
		static const char tag = 0;
		return &tag;
	}

	// Extracts the PairGlobalGpuView from a kP7CameraFit GPU AcquireResult produced by MakeP7PairGlobalGpuOwnerFill (null otherwise).
	inline const PairGlobalGpuView* PairGlobalGpuOf(const VpSkeleton::AcquireResult& r)
	{
		return r.Ok() && r.gpu.textureSet == PairGlobalGpuTag()
			? static_cast<const PairGlobalGpuView*>(r.gpu.keepAlive.get())
			: nullptr;
	}

	using PairGlobalCpuFn = std::function<bool(const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&,
		PairGlobal& outRecord, std::string& outWhy)>;
	using PairGlobalGpuFn = std::function<bool(const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&,
		PairGlobalGpuView& outView, std::string& outWhy)>;

	inline VpSkeleton::OwnerFill MakeP7PairGlobalCpuOwnerFill(PairGlobalCpuFn fn = PairGlobalCpuFn())
	{
		VpSkeleton::OwnerFill f;
		f.variantAxes = VpSkeleton::VariantKey::kAxisParam0;
		f.renderThreadOnly = false;
		f.keepFrames = 1;
		f.fill = [fn](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
			if (req.variant.param0 != kPairGlobalParam0)
			{
				out.declineReason = "pair global: param0 must be 1";
				return false;
			}
			PairGlobal rec;
			std::string why;
			if (fn)
			{
				if (!fn(req, memo, rec, why))
				{
					out.declineReason = why.empty() ? "pair global: unavailable" : why;
					return false;
				}
			}
			else
			{
				const VpSkeleton::AcquireResult p2 = memo.Acquire(VpSkeleton::kP2VectorPair, req.seq, VpSkeleton::AcquireForm::Cpu);
				P2PairRecord p2Rec;
				if (!p2.Ok() || !DecodeP2PairRecord(p2.cpu, p2.cpuBytes, p2Rec) || !p2Rec.GlobalSet())
				{
					out.declineReason = "pair global: unavailable";
					return false;
				}
				for (int d = 0; d < 2; ++d)
					for (int k = 0; k < 4; ++k)
					{
						rec.vec[d][k] = p2Rec.global[d][k];
						rec.second[d][k] = p2Rec.second[d][k];
					}
				rec.src = PairGlobalSource::Engine;
				rec.p2Epoch = p2Rec.generation;
				rec.valid = true;
			}
			out.bytes.resize(kPairGlobalRecordBytes);
			if (EncodePairGlobal(rec, out.bytes.data(), out.bytes.size()) != kPairGlobalRecordBytes)
			{
				out.bytes.clear();
				out.declineReason = "pair global: record not encoded";
				return false;
			}
			return true;
		};
		return f;
	}

	inline VpSkeleton::OwnerFill MakeP7PairGlobalGpuOwnerFill(PairGlobalGpuFn fn = PairGlobalGpuFn())
	{
		VpSkeleton::OwnerFill f;
		f.variantAxes = VpSkeleton::VariantKey::kAxisParam0;
		f.view = true;               // zero-copy view of 1x1 RGBA16F globalVector / globalSecond (0 extra VRAM bytes)
		f.frameScoped = true;        // valid only for the current render frame (released at the next OnSeal)
		f.renderThreadOnly = true;   // GPU handles are render-thread only
		f.fill = [fn](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
			if (req.variant.param0 != kPairGlobalParam0)
			{
				out.declineReason = "pair global: param0 must be 1";
				return false;
			}
			if (!fn)
			{
				out.declineReason = "pair global: gpu provider absent";
				return false;
			}
			PairGlobalGpuView view;
			std::string why;
			if (!fn(req, memo, view, why) || view.globalVector == nullptr)
			{
				out.declineReason = why.empty() ? "pair global: unavailable" : why;
				return false;
			}
			const std::shared_ptr<const PairGlobalGpuView> held = std::make_shared<const PairGlobalGpuView>(view);
			out.gpu.resource = const_cast<void*>(static_cast<const void*>(view.globalVector));
			out.gpu.textureSet = PairGlobalGpuTag();
			out.gpu.index = 0;
			out.gpu.width = 1u;
			out.gpu.height = 1u;
			out.gpu.bytes = 0u;   // zero-copy view: nothing allocated
			out.gpu.keepAlive = std::const_pointer_cast<void>(std::static_pointer_cast<const void>(held));
			out.scope = VpSkeleton::OwnerFillOutput::Scope::Frame;
			return true;
		};
		return f;
	}

	// Consumer helper: acquires the kP7CameraFit pair-global CPU form through SkeletonMemo::Acquire, decodes PairGlobal,
	// and reports visible fallback status via FallbackCatalog::PairGlobalUnavailable.
	inline bool AcquirePairGlobalCpu(VpSkeleton::SkeletonMemo& memo, uint64_t seq, PairGlobal& outRecord,
		VpSkeleton::AcquireResult* outResult = nullptr, FallbackCatalog::Decision* outFallback = nullptr)
	{
		outRecord = PairGlobal();
		VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP7CameraFit, seq, VpSkeleton::AcquireForm::Cpu, PairGlobalKey());
		std::string why;
		bool ok = false;
		if (!r.Ok())
		{
			why = memo.LastCause(VpSkeleton::kP7CameraFit);
			if (why.empty())
				why = VpSkeleton::Token(r.cause);
		}
		else if (!DecodePairGlobal(r.cpu, r.cpuBytes, outRecord))
		{
			outRecord = PairGlobal();
			why = "pair global: invalid cpu record";
		}
		else
		{
			ok = true;
		}
		if (outFallback != nullptr)
			*outFallback = FallbackCatalog::PairGlobalUnavailable(!ok, ok ? std::string() : why);
		if (outResult != nullptr)
			*outResult = std::move(r);
		return ok;
	}

	// Consumer helper: acquires the kP7CameraFit pair-global GPU view through SkeletonMemo::Acquire, extracts PairGlobalGpuView,
	// and reports visible fallback status via FallbackCatalog::PairGlobalUnavailable.
	inline bool AcquirePairGlobalGpu(VpSkeleton::SkeletonMemo& memo, uint64_t seq, PairGlobalGpuView& outView,
		VpSkeleton::AcquireResult* outResult = nullptr, FallbackCatalog::Decision* outFallback = nullptr)
	{
		outView = PairGlobalGpuView();
		VpSkeleton::AcquireResult r = memo.Acquire(VpSkeleton::kP7CameraFit, seq, VpSkeleton::AcquireForm::Gpu, PairGlobalKey());
		std::string why;
		bool ok = false;
		if (!r.Ok())
		{
			why = memo.LastCause(VpSkeleton::kP7CameraFit);
			if (why.empty())
				why = VpSkeleton::Token(r.cause);
		}
		else if (!r.gpu.Valid() || r.gpu.resource == nullptr)
		{
			why = "pair global: invalid gpu handle";
		}
		else if (const PairGlobalGpuView* v = PairGlobalGpuOf(r))
		{
			outView = *v;
			ok = outView.globalVector != nullptr;
			if (!ok)
				why = "pair global: null globalVector";
		}
		else
		{
			outView.globalVector = static_cast<pl_tex>(r.gpu.resource);
			ok = outView.globalVector != nullptr;
			if (!ok)
				why = "pair global: null globalVector";
		}
		if (outFallback != nullptr)
			*outFallback = FallbackCatalog::PairGlobalUnavailable(!ok, ok ? std::string() : why);
		if (outResult != nullptr)
			*outResult = std::move(r);
		return ok;
	}

	// Registers ONLY the CPU kP7CameraFit pair-global owner fill under kPairGlobalUnit with the skeleton found via the
	// hub (or an injected demand in tests). Used when no GPU pair-global provider is bound so
	// memo->HasOwnerFill(kP7CameraFit, Gpu) stays cleanly false (no stub decline pollution in memo->Decisions()).
	inline bool TryRegisterPairGlobalCpuFill(Vpv::Skeleton::IExternalDemand* demand = nullptr,
		PairGlobalCpuFn cpuFn = PairGlobalCpuFn())
	{
		if (demand == nullptr)
			demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
				Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
		if (demand == nullptr)
			return false;
		const VpSkeleton::SkeletonMemo::Registration cpuReg = demand->RegisterOwnerFill(
			VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Cpu, kPairGlobalUnit, MakeP7PairGlobalCpuOwnerFill(std::move(cpuFn)));
		return cpuReg == VpSkeleton::SkeletonMemo::Registration::Registered
			|| cpuReg == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
	}

	// Registers BOTH the CPU and GPU kP7CameraFit pair-global owner fills under kPairGlobalUnit with the skeleton found via the
	// hub (or an injected demand in tests). True = both registered (or already registered).
	inline bool TryRegisterPairGlobalFills(Vpv::Skeleton::IExternalDemand* demand = nullptr,
		PairGlobalCpuFn cpuFn = PairGlobalCpuFn(), PairGlobalGpuFn gpuFn = PairGlobalGpuFn())
	{
		if (demand == nullptr)
			demand = static_cast<Vpv::Skeleton::IExternalDemand*>(
				Vpv::Hub::ExtensionHub::Instance().Query(Vpv::Skeleton::kExternalDemandQueryId));
		if (demand == nullptr)
			return false;
		const VpSkeleton::SkeletonMemo::Registration cpuReg = demand->RegisterOwnerFill(
			VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Cpu, kPairGlobalUnit, MakeP7PairGlobalCpuOwnerFill(std::move(cpuFn)));
		const VpSkeleton::SkeletonMemo::Registration gpuReg = demand->RegisterOwnerFill(
			VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Gpu, kPairGlobalUnit, MakeP7PairGlobalGpuOwnerFill(std::move(gpuFn)));
		const bool cpuOk = cpuReg == VpSkeleton::SkeletonMemo::Registration::Registered
			|| cpuReg == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
		const bool gpuOk = gpuReg == VpSkeleton::SkeletonMemo::Registration::Registered
			|| gpuReg == VpSkeleton::SkeletonMemo::Registration::AlreadyRegistered;
		return cpuOk && gpuOk;
	}
}
