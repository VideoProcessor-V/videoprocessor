// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// GPU execution helpers for kP7CameraFit pair-global (ClearPairGlobalGpu / ExecutePairGlobalGpu / AcquireOrFillPairGlobalGpu).
// Split out of PairGlobalFill.h so the SkeletonMemo owner-fill header never names pl_gpu (R2).
#include <VP-V/modules/motion/estimation/global_model/PairGlobalFill.h>
#include <VP-V/modules/motion/estimation/global_model/MatcherGlobalD1.h>

#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>

namespace VpMotion
{
	// Cut / flash clear (U10 C3: moved verbatim from DejudderWarp::CameraSkipSearches): zeroes every non-null
	// globalVector[0..1] and globalSecond[0..1] texture with the exact pass label.
	inline bool ClearPairGlobalGpu(pl_tex globalVector[2], pl_tex globalSecond[2],
		const MatcherGlobalD1::DispatchFn& dispatch, PairGlobal& outRecord)
	{
		outRecord = PairGlobal();
		outRecord.src = PairGlobalSource::Cleared;
		outRecord.valid = false;
		if (!dispatch)
			return false;
		static const char* const kClearBody =
			"ivec2 b = ivec2(gl_GlobalInvocationID.xy);\n"
			"if (b.x < imageSize(dst).x && b.y < imageSize(dst).y) imageStore(dst, b, fillv);\n";
		const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const pl_tex targets[4] = {
			globalVector ? globalVector[0] : nullptr,
			globalVector ? globalVector[1] : nullptr,
			globalSecond ? globalSecond[0] : nullptr,
			globalSecond ? globalSecond[1] : nullptr
		};
		for (pl_tex t : targets)
		{
			if (!t)
				continue;
			const pl_shader_desc out[1] = { MatcherGlobalD1::Storage("dst", t) };
			const pl_shader_var var[1] = { { pl_var_vec4("fillv"), none, true } };
			if (!dispatch("VideoProcessor de-judder: global motion cleared (camera model: cut / flash)",
					nullptr, kClearBody, out, 1, var, 1, 1, 1, 1))
				return false;
		}
		return true;
	}

	// GPU execution context for ExecutePairGlobalGpu / AcquireOrFillPairGlobalGpu (caller owns all pl_tex handles).
	struct PairGlobalGpuContext
	{
		pl_gpu gpu = nullptr;
		bool serialReference = false;
		bool twoCandidates = false;
		pl_tex l1A = nullptr;
		pl_tex l1B = nullptr;
		pl_tex l2A = nullptr;
		pl_tex l2B = nullptr;
		pl_tex* globalVector = nullptr; // array of 2: [0] fwd, [1] bwd
		pl_tex* globalSecond = nullptr; // optional array of 2: [0] fwd, [1] bwd
		pl_tex* singleSad2 = nullptr;   // caller-owned D1 scratch (serial / single)
		pl_tex* singleSad1 = nullptr;
		pl_tex* singlePick = nullptr;
		pl_tex* bothSad2 = nullptr;     // caller-owned D1 scratch (parallel bidirectional)
		pl_tex* bothSad1 = nullptr;
		pl_tex* bothPick = nullptr;
		pl_tex* gapSeedCheckTex = nullptr; // optional GPU-only 1x1 seed-vs-zero verdict texture (.w > 0.5 => seed holds; zero pl_tex_download)
		MatcherGlobalD1::DispatchFn dispatch;
		MatcherGlobalD1::DispatchPerBlockFn dispatchPerBlock;
	};

	// Runs SelectPairGlobal and materializes the selected pair global onto globalVector[0..1] (and globalSecond[0..1]
	// where clearSecond applies), or runs MatcherGlobalD1::RunSingle / RunBoth when needsD1Search is true.
	// Ruling 2 (coreplan-C8-10-U10.md): on ANY failure (D1 format/texture create/dispatch), preserves exact outError,
	// engages outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError), and returns false (ABORT, no zero-global
	// continuation). On success, releases outFallback = FallbackCatalog::PairGlobalUnavailable(false) and returns true.
	inline bool ExecutePairGlobalGpu(
		const PairGlobalSelectionInput& in,
		const PairGlobalGpuContext& ctx,
		PairGlobal& outRecord,
		PairGlobalSelectionOutcome& outOutcome,
		FallbackCatalog::Decision& outFallback,
		std::string& outError)
	{
		outError.clear();
		SelectPairGlobal(in, outRecord, outOutcome);

		if (in.cutOrFlash)
		{
			if (!ClearPairGlobalGpu(ctx.globalVector, ctx.globalSecond, ctx.dispatch, outRecord))
			{
				if (outError.empty())
					outError = "global motion: clear dispatch failed";
				outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
				return false;
			}
			outFallback = FallbackCatalog::PairGlobalUnavailable(false);
			return true;
		}

		auto runD1Search = [&]() -> bool {
			if (!ctx.globalVector || !ctx.globalVector[0] || !ctx.globalVector[1])
			{
				outError = "global motion: missing output textures";
				outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
				return false;
			}
			bool ok = false;
			if (ctx.serialReference)
			{
				pl_tex dummySad2 = nullptr, dummySad1 = nullptr, dummyPick = nullptr;
				pl_tex& s2 = ctx.singleSad2 ? *ctx.singleSad2 : dummySad2;
				pl_tex& s1 = ctx.singleSad1 ? *ctx.singleSad1 : dummySad1;
				pl_tex& sp = ctx.singlePick ? *ctx.singlePick : dummyPick;
				pl_tex sec0 = (ctx.twoCandidates && ctx.globalSecond) ? ctx.globalSecond[0] : nullptr;
				pl_tex sec1 = (ctx.twoCandidates && ctx.globalSecond) ? ctx.globalSecond[1] : nullptr;
				ok = MatcherGlobalD1::RunSingle(ctx.gpu, true, ctx.l1A, ctx.l1B, ctx.l2A, ctx.l2B,
						ctx.globalVector[0], sec0, s2, s1, sp, ctx.dispatch, ctx.dispatchPerBlock, outError)
					&& MatcherGlobalD1::RunSingle(ctx.gpu, true, ctx.l1B, ctx.l1A, ctx.l2B, ctx.l2A,
						ctx.globalVector[1], sec1, s2, s1, sp, ctx.dispatch, ctx.dispatchPerBlock, outError);
			}
			else
			{
				if (!ctx.bothSad2 || !ctx.bothSad1 || !ctx.bothPick)
				{
					outError = "global motion: missing scratch texture handles";
					outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
					return false;
				}
				pl_tex secondPair[2] = {
					(ctx.twoCandidates && ctx.globalSecond) ? ctx.globalSecond[0] : nullptr,
					(ctx.twoCandidates && ctx.globalSecond) ? ctx.globalSecond[1] : nullptr
				};
				ok = MatcherGlobalD1::RunBoth(ctx.gpu, ctx.l1A, ctx.l1B, ctx.l2A, ctx.l2B,
					ctx.globalVector, (ctx.twoCandidates && ctx.globalSecond) ? secondPair : nullptr,
					*ctx.bothSad2, *ctx.bothSad1, *ctx.bothPick, ctx.dispatchPerBlock, outError);
			}
			if (!ok)
			{
				// Ruling 2: preserve exact error text ("no 32-bit float storage format" / "cannot create the candidate textures"),
				// engage visible FallbackCatalog::PairGlobalUnavailable, and ABORT (no zero-global continuation).
				outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
				return false;
			}
			return true;
		};

		if (outOutcome.source == PairGlobalSource::CameraModel
			|| outOutcome.source == PairGlobalSource::Engine
			|| outOutcome.source == PairGlobalSource::GapSeed)
		{
			if (!ctx.dispatch || !ctx.globalVector || !ctx.globalVector[0] || !ctx.globalVector[1])
			{
				outError = !ctx.dispatch ? "global motion: missing dispatch callback"
					: "global motion: missing output textures";
				outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
				return false;
			}
			const pl_tex gpuSeedCheck = (outOutcome.source == PairGlobalSource::GapSeed && ctx.gapSeedCheckTex != nullptr)
				? *ctx.gapSeedCheckTex : nullptr;
			if (gpuSeedCheck != nullptr)
			{
				// HIGH-1 / G6: GPU-only gap-seed mux (zero blocking pl_tex_download on the render thread).
				// Run D1 first, then conditionally overwrite with the gap seed on GPU iff seedCheck.w > 0.5.
				if (!runD1Search())
					return false;
			}
			static const char* const kStoreOneBody =
				"if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) imageStore(dst, ivec2(0), gvec);\n";
			static const char* const kStoreIfSeedHoldsBody =
				"if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u && texelFetch(gs_check, ivec2(0), 0).w > 0.5) imageStore(dst, ivec2(0), gvec);\n";
			const char* vecLabel = nullptr;
			const char* secondLabel = nullptr;
			if (outOutcome.source == PairGlobalSource::CameraModel)
			{
				vecLabel = "VideoProcessor de-judder: global motion (camera model)";
				secondLabel = "VideoProcessor de-judder: global motion second (camera model)";
			}
			else if (outOutcome.source == PairGlobalSource::Engine)
			{
				vecLabel = "VideoProcessor de-judder: global motion (engine)";
			}
			else
			{
				vecLabel = "VideoProcessor de-judder: global motion (gap seed)";
				secondLabel = "VideoProcessor de-judder: global motion second (gap seed)";
			}
			for (int d = 0; d < 2; ++d)
			{
				const float value[4] = { outRecord.vec[d][0], outRecord.vec[d][1], outRecord.vec[d][2], outRecord.vec[d][3] };
				const pl_shader_var var[1] = { { pl_var_vec4("gvec"), value, true } };
				bool dispatched = false;
				if (gpuSeedCheck != nullptr)
				{
					const pl_shader_desc out[2] = {
						MatcherGlobalD1::Sampled("gs_check", gpuSeedCheck, false),
						MatcherGlobalD1::Storage("dst", ctx.globalVector[d])
					};
					dispatched = ctx.dispatch(vecLabel, nullptr, kStoreIfSeedHoldsBody, out, 2, var, 1, 1, 1, 1);
				}
				else
				{
					const pl_shader_desc out[1] = { MatcherGlobalD1::Storage("dst", ctx.globalVector[d]) };
					dispatched = ctx.dispatch(vecLabel, nullptr, kStoreOneBody, out, 1, var, 1, 1, 1, 1);
				}
				if (!dispatched)
				{
					if (outError.empty())
						outError = std::string("dispatch failed: ") + vecLabel;
					outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
					return false;
				}
				if (ctx.twoCandidates && outOutcome.clearSecond && secondLabel != nullptr
					&& ctx.globalSecond != nullptr && ctx.globalSecond[d] != nullptr)
				{
					const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					const pl_shader_var var2[1] = { { pl_var_vec4("gvec"), none, true } };
					bool dispatched2 = false;
					if (gpuSeedCheck != nullptr)
					{
						const pl_shader_desc out2[2] = {
							MatcherGlobalD1::Sampled("gs_check", gpuSeedCheck, false),
							MatcherGlobalD1::Storage("dst", ctx.globalSecond[d])
						};
						dispatched2 = ctx.dispatch(secondLabel, nullptr, kStoreIfSeedHoldsBody, out2, 2, var2, 1, 1, 1, 1);
					}
					else
					{
						const pl_shader_desc out2[1] = { MatcherGlobalD1::Storage("dst", ctx.globalSecond[d]) };
						dispatched2 = ctx.dispatch(secondLabel, nullptr, kStoreOneBody, out2, 1, var2, 1, 1, 1, 1);
					}
					if (!dispatched2)
					{
						if (outError.empty())
							outError = std::string("dispatch failed: ") + secondLabel;
						outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
						return false;
					}
				}
			}
			outFallback = FallbackCatalog::PairGlobalUnavailable(false);
			return true;
		}

		if (outOutcome.needsD1Search)
		{
			if (!runD1Search())
				return false;
			outRecord.valid = true;
			outFallback = FallbackCatalog::PairGlobalUnavailable(false);
			return true;
		}

		// !globalPrior && !cutOrFlash: no global motion prior requested.
		outFallback = FallbackCatalog::PairGlobalUnavailable(false);
		return true;
	}

	namespace pair_global_detail
	{
		inline bool AcquireOrFillPairGlobalGpuImpl(
			VpSkeleton::SkeletonMemo* memo,
			uint64_t seq,
			bool loadP2IfDirect,
			const P2PairRecord* directP2Fallback,
			const PairGlobalSelectionInput& in,
			const PairGlobalGpuContext& ctx,
			PairGlobal& outRecord,
			PairGlobalSelectionOutcome& outOutcome,
			FallbackCatalog::Decision& outFallback,
			std::string& outError,
			bool* outUsedMemoGpu,
			VpSkeleton::AcquireResult* outMemoPin)
		{
			outError.clear();
			if (outUsedMemoGpu != nullptr)
				*outUsedMemoGpu = false;
			if (outMemoPin != nullptr)
				*outMemoPin = VpSkeleton::AcquireResult();

			if (!in.cutOrFlash && in.globalPrior && memo != nullptr
				&& memo->HasOwnerFill(VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Gpu))
			{
				PairGlobalGpuView view;
				VpSkeleton::AcquireResult res;
				FallbackCatalog::Decision acqFallback;
				if (AcquirePairGlobalGpu(*memo, seq, view, &res, &acqFallback))
				{
					const pl_tex srcVec[2] = { view.globalVector, view.globalVectorBwd };
					const pl_tex srcSec[2] = { view.globalSecond, view.globalSecondBwd };
					const bool foreignVec = ctx.globalVector != nullptr && ctx.globalVector[0] != nullptr
						&& srcVec[0] != ctx.globalVector[0];
					auto copyOne = [&](pl_tex src, pl_tex dst, const char* label) -> bool {
						if (!src || !dst || src == dst)
							return true;
						if (!ctx.dispatch)
						{
							outError = "pair global: missing dispatch callback for memo gpu copy";
							return false;
						}
						static const char* const kCopyOneBody =
							"if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) imageStore(dst, ivec2(0), texelFetch(src, ivec2(0), 0));\n";
						const pl_shader_desc copyDesc[2] = {
							MatcherGlobalD1::Sampled("src", src, false),
							MatcherGlobalD1::Storage("dst", dst)
						};
						if (!ctx.dispatch(label, nullptr, kCopyOneBody, copyDesc, 2, nullptr, 0, 1, 1, 1))
						{
							if (outError.empty())
								outError = std::string("dispatch failed: ") + label;
							return false;
						}
						return true;
					};
					auto clearOne = [&](pl_tex dst, const char* label) -> bool {
						if (!dst)
							return true;
						if (!ctx.dispatch)
						{
							outError = "pair global: missing dispatch callback for memo second clear";
							return false;
						}
						static const char* const kClearOneBody =
							"if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) imageStore(dst, ivec2(0), gvec);\n";
						const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						const pl_shader_desc clearDesc[1] = { MatcherGlobalD1::Storage("dst", dst) };
						const pl_shader_var clearVar[1] = { { pl_var_vec4("gvec"), none, true } };
						if (!ctx.dispatch(label, nullptr, kClearOneBody, clearDesc, 1, clearVar, 1, 1, 1, 1))
						{
							if (outError.empty())
								outError = std::string("dispatch failed: ") + label;
							return false;
						}
						return true;
					};
					if (ctx.globalVector != nullptr && foreignVec && ctx.globalVector[1] != nullptr && srcVec[1] == nullptr)
					{
						outRecord = PairGlobal();
						outOutcome = PairGlobalSelectionOutcome();
						outError = "pair global: memo gpu view missing backward texture";
						outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
						return false;
					}
					if (ctx.twoCandidates && ctx.globalSecond != nullptr && foreignVec
						&& srcSec[0] != nullptr && srcSec[0] != ctx.globalSecond[0]
						&& ctx.globalSecond[1] != nullptr && srcSec[1] == nullptr)
					{
						outRecord = PairGlobal();
						outOutcome = PairGlobalSelectionOutcome();
						outError = "pair global: memo gpu view missing backward second texture";
						outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
						return false;
					}
					if (ctx.globalVector != nullptr)
					{
						for (int d = 0; d < 2; ++d)
						{
							if (!copyOne(srcVec[d], ctx.globalVector[d], "VideoProcessor de-judder: global motion (memo gpu view)"))
							{
								outRecord = PairGlobal();
								outOutcome = PairGlobalSelectionOutcome();
								outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
								return false;
							}
						}
					}
					if (ctx.twoCandidates && ctx.globalSecond != nullptr)
					{
						for (int d = 0; d < 2; ++d)
						{
							if (ctx.globalSecond[d] == nullptr)
								continue;
							const bool secOk = srcSec[d] != nullptr
								? copyOne(srcSec[d], ctx.globalSecond[d], "VideoProcessor de-judder: global motion second (memo gpu view)")
								: (!foreignVec || clearOne(ctx.globalSecond[d], "VideoProcessor de-judder: global motion second (memo clear)"));
							if (!secOk)
							{
								outRecord = PairGlobal();
								outOutcome = PairGlobalSelectionOutcome();
								outFallback = FallbackCatalog::PairGlobalUnavailable(true, outError);
								return false;
							}
						}
					}
					outRecord = view.record;
					if (view.hasOutcome)
					{
						outOutcome = view.outcome;
					}
					else
					{
						outOutcome = PairGlobalSelectionOutcome();
						outOutcome.source = outRecord.src;
						outOutcome.modelGlobalUsed = (outRecord.src == PairGlobalSource::CameraModel);
						outOutcome.globalFromEngine = (outRecord.src == PairGlobalSource::Engine);
						outOutcome.searchSeedUsed = (outRecord.src == PairGlobalSource::GapSeed);
						outOutcome.clearSecond = (outRecord.src == PairGlobalSource::Cleared
							|| outRecord.src == PairGlobalSource::CameraModel
							|| outRecord.src == PairGlobalSource::GapSeed);
					}
					outFallback = acqFallback;
					outError.clear();
					if (outMemoPin != nullptr)
						*outMemoPin = std::move(res);
					if (outUsedMemoGpu != nullptr)
						*outUsedMemoGpu = true;
					return true;
				}
				const std::string cause = memo->LastCause(VpSkeleton::kP7CameraFit);
				const bool providerAbsent = (res.cause == VpSkeleton::AcquireCause::NoGpuDispatcher)
					|| (res.cause == VpSkeleton::AcquireCause::OwnerDeclined
						&& cause.find("pair global: gpu provider absent") != std::string::npos);
				if (!providerAbsent)
				{
					outRecord = PairGlobal();
					outOutcome = PairGlobalSelectionOutcome();
					outError = !cause.empty() ? cause : VpSkeleton::Token(res.cause);
					outFallback = acqFallback.engaged ? acqFallback : FallbackCatalog::PairGlobalUnavailable(true, outError);
					return false;
				}
			}

			if (loadP2IfDirect)
			{
				PairGlobalSelectionInput directIn = in;
				if (!directIn.cutOrFlash && directIn.globalPrior && !directIn.useModelGlobal
					&& directIn.opticalFlowGlobal && directIn.seeded)
					LoadEngineGlobalFromP2(memo, seq, directP2Fallback, directIn);
				else
					directIn.engineValid = false;
				return ExecutePairGlobalGpu(directIn, ctx, outRecord, outOutcome, outFallback, outError);
			}

			return ExecutePairGlobalGpu(in, ctx, outRecord, outOutcome, outFallback, outError);
		}
	}

	// Unified GPU entry point for BOTH skeleton-wired and standalone callers:
	//   - When `!in.cutOrFlash && in.globalPrior && memo != nullptr && memo->HasOwnerFill(kP7CameraFit, Gpu)`:
	//     invokes memo->Acquire(kP7CameraFit, seq, Gpu, PairGlobalKey()). If served, returns the published view; if the
	//     registered fill failed for a real reason (not merely an unbound default GPU provider / missing dispatcher),
	//     engages FallbackCatalog::PairGlobalUnavailable and aborts (Ruling 2).
	//   - Otherwise (standalone MotionWarp with memo == nullptr, unregistered GPU fill, or default unbound GPU provider):
	//     executes ExecutePairGlobalGpu directly through the ONE PairGlobalFill owner code.
	inline bool AcquireOrFillPairGlobalGpu(
		VpSkeleton::SkeletonMemo* memo,
		uint64_t seq,
		const PairGlobalSelectionInput& in,
		const PairGlobalGpuContext& ctx,
		PairGlobal& outRecord,
		PairGlobalSelectionOutcome& outOutcome,
		FallbackCatalog::Decision& outFallback,
		std::string& outError,
		bool* outUsedMemoGpu = nullptr,
		VpSkeleton::AcquireResult* outMemoPin = nullptr)
	{
		return pair_global_detail::AcquireOrFillPairGlobalGpuImpl(
			memo, seq, false, nullptr, in, ctx, outRecord, outOutcome, outFallback, outError, outUsedMemoGpu, outMemoPin);
	}

	// Convenience overload: defers LoadEngineGlobalFromP2 (kP2VectorPair when memo != nullptr, else directP2Fallback)
	// until after the P7 GPU memo-hit check, running it only right before direct ExecutePairGlobalGpu when engine global
	// is eligible (and clearing engineValid otherwise).
	inline bool AcquireOrFillPairGlobalGpu(
		VpSkeleton::SkeletonMemo* memo,
		uint64_t seq,
		const P2PairRecord* directP2Fallback,
		PairGlobalSelectionInput in,
		const PairGlobalGpuContext& ctx,
		PairGlobal& outRecord,
		PairGlobalSelectionOutcome& outOutcome,
		FallbackCatalog::Decision& outFallback,
		std::string& outError,
		bool* outUsedMemoGpu = nullptr,
		VpSkeleton::AcquireResult* outMemoPin = nullptr)
	{
		return pair_global_detail::AcquireOrFillPairGlobalGpuImpl(
			memo, seq, true, directP2Fallback, in, ctx, outRecord, outOutcome, outFallback, outError, outUsedMemoGpu, outMemoPin);
	}
}
