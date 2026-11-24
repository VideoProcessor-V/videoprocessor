/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for SID-SOTA-4:
// Multi-frame static-patch cross-covariance SVD rank-cliff probe (StaticPatchRankProbe & ChainEstimator kReasonStaticRankCliff)
// (Rule 3 mandatory single-frame in-test negative controls, native/motion/cut negative controls, zero pixel mutation, C++14 /W4).
//
// Covers:
//   1. Sota4Grainy8Bit1080pTo2160pStaticSequenceExposesRankCliffWhereSingleFrameFails
//   2. Sota4AdversarialNegativeControlsNative4KMovingCameraAndCutReset
//   3. Sota4JacobiEigenAccuracyNonFiniteGuardsPixelImmutabilityAndOriginalPathway
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"
#include "W1DescaleRef.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/StaticPatchRankProbe.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota4StaticRankFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kLanczos3 = 5;

		struct SplitMix64
		{
			uint64_t state;
			explicit SplitMix64(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double Uniform01()
			{
				return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
			}

			// Exact zero-mean unit-variance 4-sample Irwin-Hall random variable (fast for full 4K planes,
			// exact variance = 1.0, i.i.d. across pixels and frames).
			double FastUnitZeroMean()
			{
				const uint64_t u = NextU64();
				const double a = static_cast<double>(u & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double b = static_cast<double>((u >> 16) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double c = static_cast<double>((u >> 32) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double d = static_cast<double>((u >> 48) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				// Var(a + b + c + d) = 4 / 12 = 1 / 3 => scale by sqrt(3) for exact unit variance.
				return (a + b + c + d) * 1.7320508075688772;
			}
		};

		// Quantizes to true 8-bit-in-10-bit code ladder (step = 4, [64, 940]).
		inline uint16_t Quantize8BitIn10(double v)
		{
			double q = std::floor(v / 4.0 + 0.5) * 4.0;
			if (q < 64.0)
				q = 64.0;
			if (q > 940.0)
				q = 940.0;
			return static_cast<uint16_t>(q);
		}

		// Builds a clean floating-point 2-D base scene (either 2x separable Lanczos3 upscaled from 1920x1080 -> 3840x2160
		// or native broadband 3840x2160 when srcW == outW && srcH == outH).
		inline std::vector<double> BuildClean2DBase(int srcW, int srcH, int outW, int outH, int kernel, uint64_t seed)
		{
			SplitMix64 rng(seed);
			std::vector<double> latent(static_cast<size_t>(srcW) * static_cast<size_t>(srcH), 512.0);
			for (size_t i = 0; i < latent.size(); ++i)
				latent[i] = 512.0 + 56.0 * rng.FastUnitZeroMean();

			if (srcW == outW && srcH == outH)
				return latent;

			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> tmp(static_cast<size_t>(outW) * static_cast<size_t>(srcH), 0.0);

			// Horizontal 2x pass (exact 2-phase stencil for integer 2x, general TapWeights otherwise):
			{
				const double rx = static_cast<double>(srcW) / static_cast<double>(outW);
				std::vector<double> wxTab(static_cast<size_t>(outW) * static_cast<size_t>(taps));
				std::vector<int> cxTab(static_cast<size_t>(outW));
				for (int x = 0; x < outW; ++x)
				{
					const double p = (static_cast<double>(x) + 0.5) * rx - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, &wxTab[static_cast<size_t>(x) * static_cast<size_t>(taps)]);
					cxTab[static_cast<size_t>(x)] = static_cast<int>(fl) - r + 1;
				}
				for (int y = 0; y < srcH; ++y)
				{
					const double* sRow = &latent[static_cast<size_t>(y) * static_cast<size_t>(srcW)];
					double* dRow = &tmp[static_cast<size_t>(y) * static_cast<size_t>(outW)];
					for (int x = 0; x < outW; ++x)
					{
						const double* w = &wxTab[static_cast<size_t>(x) * static_cast<size_t>(taps)];
						const int c0 = cxTab[static_cast<size_t>(x)];
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
						{
							int cx = c0 + s;
							if (cx < 0) cx = 0;
							if (cx > srcW - 1) cx = srcW - 1;
							acc += w[s] * sRow[cx];
						}
						dRow[x] = acc;
					}
				}
			}

			// Vertical pass:
			std::vector<double> out(static_cast<size_t>(outW) * static_cast<size_t>(outH), 0.0);
			{
				const double ry = static_cast<double>(srcH) / static_cast<double>(outH);
				std::vector<double> wyTab(static_cast<size_t>(outH) * static_cast<size_t>(taps));
				std::vector<int> cyTab(static_cast<size_t>(outH));
				for (int y = 0; y < outH; ++y)
				{
					const double p = (static_cast<double>(y) + 0.5) * ry - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, &wyTab[static_cast<size_t>(y) * static_cast<size_t>(taps)]);
					cyTab[static_cast<size_t>(y)] = static_cast<int>(fl) - r + 1;
				}
				for (int y = 0; y < outH; ++y)
				{
					const double* w = &wyTab[static_cast<size_t>(y) * static_cast<size_t>(taps)];
					const int c0 = cyTab[static_cast<size_t>(y)];
					double* dRow = &out[static_cast<size_t>(y) * static_cast<size_t>(outW)];
					for (int x = 0; x < outW; ++x)
					{
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
						{
							int cy = c0 + s;
							if (cy < 0) cy = 0;
							if (cy > srcH - 1) cy = srcH - 1;
							acc += w[s] * tmp[static_cast<size_t>(cy) * static_cast<size_t>(outW) + static_cast<size_t>(x)];
						}
						dRow[x] = acc;
					}
				}
			}
			return out;
		}

		// Renders frame `t` from a clean 2-D base scene with optional sub-pixel/integer camera pan `(panX, panY)`
		// and heavy independent temporal grain `grainSigmaCodes` (in 10-bit code units), quantized to 8-bit (step = 4).
		inline ChainFx::Plane RenderGrainy8BitFrame(
			const std::vector<double>& cleanBase,
			int w,
			int h,
			double panX,
			double panY,
			double grainSigmaCodes,
			uint64_t grainSeed)
		{
			ChainFx::Plane p;
			p.w = w;
			p.h = h;
			p.luma.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
			SplitMix64 rng(grainSeed);

			const bool noPan = (panX == 0.0 && panY == 0.0);
			for (int y = 0; y < h; ++y)
			{
				uint16_t* dst = &p.luma[static_cast<size_t>(y) * static_cast<size_t>(w)];
				if (noPan)
				{
					const double* src = &cleanBase[static_cast<size_t>(y) * static_cast<size_t>(w)];
					for (int x = 0; x < w; ++x)
						dst[x] = Quantize8BitIn10(src[x] + grainSigmaCodes * rng.FastUnitZeroMean());
				}
				else
				{
					// Bilinear sub-pixel camera pan over wrapping scene coordinates:
					double sy = std::fmod(static_cast<double>(y) + panY, static_cast<double>(h));
					if (sy < 0.0) sy += static_cast<double>(h);
					const int y0 = static_cast<int>(std::floor(sy)) % h;
					const int y1 = (y0 + 1) % h;
					const double fy = sy - std::floor(sy);
					const double* r0 = &cleanBase[static_cast<size_t>(y0) * static_cast<size_t>(w)];
					const double* r1 = &cleanBase[static_cast<size_t>(y1) * static_cast<size_t>(w)];
					for (int x = 0; x < w; ++x)
					{
						double sx = std::fmod(static_cast<double>(x) + panX, static_cast<double>(w));
						if (sx < 0.0) sx += static_cast<double>(w);
						const int x0 = static_cast<int>(std::floor(sx)) % w;
						const int x1 = (x0 + 1) % w;
						const double fx = sx - std::floor(sx);
						const double val = (1.0 - fy) * ((1.0 - fx) * r0[x0] + fx * r0[x1]) +
							fy * ((1.0 - fx) * r1[x0] + fx * r1[x1]);
						dst[x] = Quantize8BitIn10(val + grainSigmaCodes * rng.FastUnitZeroMean());
					}
				}
			}
			return p;
		}
	}

	TEST_CLASS(SourceIdSota4StaticRankTests)
	{
	public:
		// 1. Heavily grainy 8-bit 1080p -> 2160p static 2-D sequence (grainSigma = 36 codes = 9 8-bit LSBs!):
		//      - Mandatory In-Test Single-Frame Negative Control: SingleFrameRankProbe AND single-frame ChainEstimator
		//        BOTH fail/abstain on frame 1 (temporal grain buries the rank-4 cliff on a single frame);
		//      - Feeding K = 4..8 static frames cancels uncorrelated lag-1 grain, exposes effectiveRank == 4
		//        (estimatedRatio ~= 0.5), sets kReasonStaticRankCliff ("static rank cliff"), increments
		//        staticRankCliffsDetected with staticRankMotionAbstains == 0, and leaves input pixels untouched!
		TEST_METHOD(Sota4Grainy8Bit1080pTo2160pStaticSequenceExposesRankCliffWhereSingleFrameFails)
		{
			using namespace Sota4StaticRankFx;

			const C::StaticPatchRankSettings defCfg;
			Assert::IsFalse(defCfg.enabled, L"StaticPatchRankSettings must default to OFF");
			Assert::AreEqual(std::string("static rank cliff"),
				SourceGeometry::ReasonWords(C::kReasonStaticRankCliff),
				L"ReasonWords(kReasonStaticRankCliff) must render 'static rank cliff'");

			const std::vector<double> cleanUp2x = BuildClean2DBase(1920, 1080, kUhdW, kUhdH, kLanczos3, 0x4101ull);
			constexpr int kFrames = 6;
			constexpr double kHeavyGrainCodes = 36.0; // 9 8-bit LSBs of independent temporal grain
			const double floorCodes8 = 4.0 / std::sqrt(12.0);

			std::vector<ChainFx::Plane> frames;
			frames.reserve(kFrames);
			for (int k = 0; k < kFrames; ++k)
				frames.push_back(RenderGrainy8BitFrame(cleanUp2x, kUhdW, kUhdH, 0.0, 0.0, kHeavyGrainCodes, 0x4200ull + static_cast<uint64_t>(k * 101)));

			// Extract co-located LineSets across the K frames for direct unit verification:
			const C::LineRect active{ 0, 0, kUhdW, kUhdH };
			const C::LinePlan fixedPlan = C::LineSampler::Plan(active, 1);
			Assert::IsTrue(fixedPlan.valid);

			C::StaticPatchRankSettings onCfg;
			onCfg.enabled = true;
			C::StaticPatchWorkspace wsH, wsV;

			for (int k = 0; k < kFrames; ++k)
			{
				C::LineSet lsH, lsV;
				Assert::IsTrue(C::LineSampler::Fill(fixedPlan, 0, frames[static_cast<size_t>(k)].luma.data(), kUhdW, 1.0, lsH));
				Assert::IsTrue(C::LineSampler::Fill(fixedPlan, 1, frames[static_cast<size_t>(k)].luma.data(), kUhdW, 1.0, lsV));

				// Mandatory In-Test Single-Frame Negative Control on EVERY frame k:
				// Single-frame covariance E[dx_k dx_k^T] is swamped by 36-code grain and FAILS to detect the cliff!
				const C::StaticRankResult singleH = C::SingleFrameRankProbe(lsH, floorCodes8, onCfg);
				Assert::IsFalse(singleH.rankCliffDetected,
					L"in-test negative control: single-frame covariance must FAIL to detect rank cliff under heavy 8-bit grain");

				// Verify SubmitFrame leaves input LineSet pixels 100% bit-identical:
				const std::vector<double> beforeBytes = lsH.data;
				const C::StaticRankResult multiH = C::SubmitFrame(lsH, floorCodes8, onCfg, wsH);
				const C::StaticRankResult multiV = C::SubmitFrame(lsV, floorCodes8, onCfg, wsV);
				Assert::AreEqual(beforeBytes.size(), lsH.data.size());
				Assert::AreEqual(0, std::memcmp(beforeBytes.data(), lsH.data.data(), beforeBytes.size() * sizeof(double)),
					L"SubmitFrame must NEVER modify input LineSet pixels");

				if (k + 1 < onCfg.minFrames)
				{
					Assert::IsFalse(multiH.rankCliffDetected);
					Assert::IsTrue(multiH.reason == C::StaticRankReason::WarmingUp);
				}
				else
				{
					Assert::IsTrue(multiH.valid && multiH.rankCliffDetected,
						L"K >= 4 static grainy frames MUST expose 1080p->2160p rank cliff on H");
					Assert::IsTrue(multiH.reason == C::StaticRankReason::RankCliffDetected);
					Assert::AreEqual(4, multiH.effectiveRank, L"2x upscale over 8-tap patch must expose rank r == 4");
					Assert::AreEqual(0.5, multiH.estimatedRatio, 1e-9, L"estimatedRatio == 4 / 8 == 0.5");
					Assert::IsTrue(multiH.cliffRatio >= onCfg.minCliffEigenRatio);
					Assert::IsTrue(multiH.trailingEnergyShare <= onCfg.maxTrailingEnergyShare);
					Assert::AreEqual(0, multiH.motionRejectedFrames);

					Assert::IsTrue(multiV.valid && multiV.rankCliffDetected,
						L"K >= 4 static grainy frames MUST expose 1080p->2160p rank cliff on V");
					Assert::AreEqual(0.5, multiV.estimatedRatio, 0.125);
				}
			}

			// End-to-end ChainEstimator::Deliver (ChainFx::Feed) across the 2-D grainy static 8-bit sequence:
			//   - Frame 0 (in-test single-frame negative control): neither single-frame chain nor rank probe detects upscale;
			//   - Frames 3..5 (K = 4..6): staticRankCliffsDetected increments, motion abstains == 0,
			//     EvaluationDetail reports ~0.5 estimatedRatio, kReasonStaticRankCliff is set, and input Plane bytes are untouched!
			{
				const std::vector<uint16_t> frame0Orig = frames[0].luma;
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.staticPatchRank.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				C::EvaluationDetail det;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
				est.SetMode(C::EstimatorMode::On);

				for (int k = 0; k < kFrames; ++k)
				{
					clk.Set(static_cast<double>(k) * 1500.0);
					ChainFx::Feed(est, frames[static_cast<size_t>(k)], static_cast<uint64_t>(k + 1), static_cast<double>(k) * 1.5);

					G::Record rec;
					Assert::IsTrue(est.Held().ReadLatest(0, rec));
					if (k == 0)
					{
						// Single-frame negative control inside live ChainEstimator:
						const SourceGeometry::Delivered deliv{ kUhdW, kUhdH };
						Assert::IsFalse(SourceGeometry::ConfidentOf(rec, deliv).AnyConfident(),
							L"single-frame negative control: frame 1 fails/abstains under heavy 8-bit grain");
						Assert::AreEqual(uint64_t(0), est.GetCounters().staticRankCliffsDetected);
						Assert::AreEqual(uint32_t(0), rec.measured[0].reasonBits & C::kReasonStaticRankCliff);
					}
				}

				Assert::AreEqual(0, std::memcmp(frame0Orig.data(), frames[0].luma.data(), frame0Orig.size() * sizeof(uint16_t)),
					L"ChainEstimator::Deliver must leave caller frame pixels 100% untouched");

				G::Record recFinal;
				Assert::IsTrue(est.Held().ReadLatest(0, recFinal));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual(uint64_t(kFrames * 2), cnt.staticRankEvals);
				Assert::AreEqual(uint64_t(0), cnt.staticRankMotionAbstains,
					L"static 2-D video sequence fed through ChainEstimator::Deliver MUST NOT suffer false motion rejections from line-sampler jitter");
				Assert::IsTrue(cnt.staticRankCliffsDetected >= 2u,
					L"ChainEstimator::Deliver must detect static rank cliff over K=4..6 static grainy 2-D frames");
				Assert::IsTrue(det.staticRankCliffDetected[0] && det.staticRankCliffDetected[1]);
				Assert::AreEqual(0.5, det.staticRankEstimatedRatio[0], 0.125);
				Assert::AreEqual(0.5, det.staticRankEstimatedRatio[1], 0.125);
				Assert::IsTrue((recFinal.measured[0].reasonBits & C::kReasonStaticRankCliff) != 0u);
				Assert::IsTrue((recFinal.measured[1].reasonBits & C::kReasonStaticRankCliff) != 0u);
				est.Stop();
			}

			// Default-OFF negative control on identical frames:
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings defEstCfg;
				Assert::IsFalse(defEstCfg.staticPatchRank.enabled);
				C::ChainEstimator estDef(defEstCfg, clk.Fn());
				estDef.SetSynchronous(true);
				estDef.SetMode(C::EstimatorMode::On);
				for (int k = 0; k < kFrames; ++k)
				{
					clk.Set(static_cast<double>(k) * 1500.0);
					ChainFx::Feed(estDef, frames[static_cast<size_t>(k)], static_cast<uint64_t>(k + 1), static_cast<double>(k) * 1.5);
				}
				G::Record recDef;
				Assert::IsTrue(estDef.Held().ReadLatest(0, recDef));
				Assert::AreEqual(uint64_t(0), estDef.GetCounters().staticRankEvals);
				Assert::AreEqual(uint64_t(0), estDef.GetCounters().staticRankCliffsDetected);
				Assert::AreEqual(uint32_t(0), recDef.measured[0].reasonBits & C::kReasonStaticRankCliff);
				estDef.Stop();
			}
		}

		// 2. Mandatory adversarial negative controls:
		//      (a) Native grainy 8-bit static 4K sequence -> NativeFullRank (effectiveRank == 8, zero cliff),
		//      (b) Moving-camera grainy 1080p->2160p sequence (both fast pan & slow 1.5-px pan) -> MotionRejected,
		//          increments staticRankMotionAbstains, zero false cliff,
		//      (c) Cut (NoteEvent::Cut) and discontinuity (NoteDiscontinuity) clear accumulator cleanly back to WarmingUp.
		TEST_METHOD(Sota4AdversarialNegativeControlsNative4KMovingCameraAndCutReset)
		{
			using namespace Sota4StaticRankFx;

			const double floorCodes8 = 4.0 / std::sqrt(12.0);
			C::StaticPatchRankSettings onCfg;
			onCfg.enabled = true;

			// (a) Native grainy 8-bit static 4K sequence (K = 6 frames):
			{
				const std::vector<double> cleanNat4K = BuildClean2DBase(kUhdW, kUhdH, kUhdW, kUhdH, kLanczos3, 0x5101ull);
				const C::LineRect active{ 0, 0, kUhdW, kUhdH };
				const C::LinePlan fixedPlan = C::LineSampler::Plan(active, 1);
				C::StaticPatchWorkspace wsNat;

				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.staticPatchRank.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				for (int k = 0; k < 6; ++k)
				{
					const ChainFx::Plane natFrame = RenderGrainy8BitFrame(
						cleanNat4K, kUhdW, kUhdH, 0.0, 0.0, 32.0, 0x5200ull + static_cast<uint64_t>(k * 73));
					C::LineSet lsH;
					Assert::IsTrue(C::LineSampler::Fill(fixedPlan, 0, natFrame.luma.data(), kUhdW, 1.0, lsH));
					const C::StaticRankResult rNat = C::SubmitFrame(lsH, floorCodes8, onCfg, wsNat);
					if (k + 1 >= onCfg.minFrames)
					{
						Assert::IsTrue(rNat.valid);
						Assert::IsFalse(rNat.rankCliffDetected, L"Control (a): native grainy 4K static stream MUST NOT report a rank cliff");
						Assert::IsTrue(rNat.reason == C::StaticRankReason::NativeFullRank);
						Assert::AreEqual(8, rNat.effectiveRank);
						Assert::AreEqual(1.0, rNat.estimatedRatio, 0.0);
					}
					clk.Set(static_cast<double>(k) * 1500.0);
					ChainFx::Feed(est, natFrame, static_cast<uint64_t>(k + 1), static_cast<double>(k) * 1.5);
				}

				G::Record recNat;
				Assert::IsTrue(est.Held().ReadLatest(0, recNat));
				Assert::AreEqual(uint64_t(0), est.GetCounters().staticRankCliffsDetected,
					L"Control (a): ChainEstimator must report 0 staticRankCliffsDetected on native grainy 4K");
				Assert::AreEqual(uint32_t(0), recNat.measured[0].reasonBits & C::kReasonStaticRankCliff);
				est.Stop();
			}

			// (b) Moving-camera grainy 1080p->2160p sequence:
			//     Test BOTH fast pan (9.0 px/frame -> Stage 1 chi-square rejection) AND slow pan (1.5 px/frame -> Stage 2 directional shift rejection)!
			const std::vector<double> cleanUp2x = BuildClean2DBase(1920, 1080, kUhdW, kUhdH, kLanczos3, 0x6101ull);
			const double panSpeeds[2] = { 9.0, 1.5 };
			for (int ps = 0; ps < 2; ++ps)
			{
				const double speed = panSpeeds[ps];
				const C::LineRect active{ 0, 0, kUhdW, kUhdH };
				const C::LinePlan fixedPlan = C::LineSampler::Plan(active, 1);
				C::StaticPatchWorkspace wsPan;

				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.staticPatchRank.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				for (int k = 0; k < 6; ++k)
				{
					const ChainFx::Plane panFrame = RenderGrainy8BitFrame(
						cleanUp2x, kUhdW, kUhdH, speed * static_cast<double>(k), speed * static_cast<double>(k),
						20.0, 0x6200ull + static_cast<uint64_t>(ps * 500 + k * 67));
					C::LineSet lsH;
					Assert::IsTrue(C::LineSampler::Fill(fixedPlan, 0, panFrame.luma.data(), kUhdW, 1.0, lsH));
					const C::StaticRankResult rPan = C::SubmitFrame(lsH, floorCodes8, onCfg, wsPan);
					Assert::IsFalse(rPan.rankCliffDetected, L"Control (b): moving camera MUST NEVER report a false rank cliff");
					if (k >= 1)
						Assert::IsTrue(rPan.reason == C::StaticRankReason::MotionRejected,
							L"Control (b): moving camera transition must be rejected as MotionRejected");

					clk.Set(static_cast<double>(k) * 1500.0);
					ChainFx::Feed(est, panFrame, static_cast<uint64_t>(k + 1), static_cast<double>(k) * 1.5);
				}

				G::Record recPan;
				Assert::IsTrue(est.Held().ReadLatest(0, recPan));
				const C::EstimatorCounters cntPan = est.GetCounters();
				Assert::AreEqual(uint64_t(0), cntPan.staticRankCliffsDetected,
					L"Control (b): ChainEstimator must report 0 staticRankCliffsDetected on moving camera");
				Assert::IsTrue(cntPan.staticRankMotionAbstains >= 5u,
					L"Control (b): ChainEstimator must increment staticRankMotionAbstains on moving camera");
				Assert::AreEqual(uint32_t(0), recPan.measured[0].reasonBits & C::kReasonStaticRankCliff);
				est.Stop();
			}

			// (c) Cut & Discontinuity reset accumulator cleanly:
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.staticPatchRank.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				C::EvaluationDetail det;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
				est.SetMode(C::EstimatorMode::On);

				// Warm up 5 static frames -> rank cliff active:
				for (int k = 0; k < 5; ++k)
				{
					const ChainFx::Plane f = RenderGrainy8BitFrame(cleanUp2x, kUhdW, kUhdH, 0.0, 0.0, 32.0, 0x7100ull + static_cast<uint64_t>(k * 31));
					clk.Set(static_cast<double>(k) * 1500.0);
					ChainFx::Feed(est, f, static_cast<uint64_t>(k + 1), static_cast<double>(k) * 1.5);
				}
				Assert::IsTrue(det.staticRankCliffDetected[0], L"fixture: cliff active before scene cut");

				// Signal a scene cut at seq = 6 -> accumulator must reset to WarmingUp (cliff cleared on frame 6!):
				const std::vector<double> cleanNatAfterCut = BuildClean2DBase(kUhdW, kUhdH, kUhdW, kUhdH, kLanczos3, 0x7901ull);
				const ChainFx::Plane afterCut = RenderGrainy8BitFrame(cleanNatAfterCut, kUhdW, kUhdH, 0.0, 0.0, 32.0, 0x7999ull);
				est.NoteEvent(C::EstimatorEvent::Cut, 6);
				clk.Set(7500.0);
				ChainFx::Feed(est, afterCut, 6, 7.5);

				G::Record recCut;
				Assert::IsTrue(est.Held().ReadLatest(0, recCut));
				Assert::IsFalse(det.staticRankCliffDetected[0], L"Control (c1): scene cut must clear static-patch accumulator immediately");
				Assert::AreEqual(uint32_t(0), recCut.measured[0].reasonBits & C::kReasonStaticRankCliff);

				// Build up again across 4 static frames (seq 7..10), then fire NoteDiscontinuity("seek"):
				for (int k = 0; k < 4; ++k)
				{
					const ChainFx::Plane f = RenderGrainy8BitFrame(cleanUp2x, kUhdW, kUhdH, 0.0, 0.0, 32.0, 0x8100ull + static_cast<uint64_t>(k * 41));
					if (k == 0)
						est.NoteEvent(C::EstimatorEvent::Cut, 7);
					clk.Set(9000.0 + static_cast<double>(k) * 1500.0);
					ChainFx::Feed(est, f, static_cast<uint64_t>(7 + k), 9.0 + static_cast<double>(k) * 1.5);
				}
				Assert::IsTrue(det.staticRankCliffDetected[0]);

				est.NoteDiscontinuity("seek");
				const ChainFx::Plane afterSeek = RenderGrainy8BitFrame(cleanUp2x, kUhdW, kUhdH, 0.0, 0.0, 32.0, 0x8999ull);
				clk.Set(16000.0);
				ChainFx::Feed(est, afterSeek, 1, 0.0);
				Assert::IsFalse(det.staticRankCliffDetected[0],
					L"Control (c2): NoteDiscontinuity must reset staticRankWs back to single-frame WarmingUp");
				est.Stop();
			}
		}

		// 3. SymmetricJacobiEigen8 exact spectral decomposition, non-finite / geometry-change guards, and Original pathway.
		TEST_METHOD(Sota4JacobiEigenAccuracyNonFiniteGuardsPixelImmutabilityAndOriginalPathway)
		{
			using namespace Sota4StaticRankFx;

			// Exact 8x8 symmetric matrix eigendecomposition reconstruction A == V * diag(evals) * V^T and V^T * V == I:
			double A[C::kStaticPatchTaps][C::kStaticPatchTaps] = {};
			for (int i = 0; i < C::kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < C::kStaticPatchTaps; ++j)
					A[i][j] = (i == j ? 12.0 + static_cast<double>(i) : 0.0) + 2.5 / (1.0 + std::abs(i - j));
			}
			double evals[C::kStaticPatchTaps] = {};
			double evecs[C::kStaticPatchTaps][C::kStaticPatchTaps] = {};
			C::SymmetricJacobiEigen8(A, evals, evecs);

			for (int i = 0; i + 1 < C::kStaticPatchTaps; ++i)
				Assert::IsTrue(evals[i] >= evals[i + 1], L"eigenvalues must be sorted descending");

			for (int i = 0; i < C::kStaticPatchTaps; ++i)
			{
				for (int j = 0; j < C::kStaticPatchTaps; ++j)
				{
					double dot = 0.0;
					double recon = 0.0;
					for (int k = 0; k < C::kStaticPatchTaps; ++k)
					{
						dot += evecs[k][i] * evecs[k][j];
						recon += evecs[i][k] * evals[k] * evecs[j][k];
					}
					Assert::AreEqual(i == j ? 1.0 : 0.0, dot, 1e-11, L"eigenvectors must be orthonormal");
					Assert::AreEqual(A[i][j], recon, 1e-10, L"V * diag(evals) * V^T must reconstruct A");
				}
			}

			// Non-finite sample & cross-coordinate mismatch / geometry reset checks:
			C::LineSet ls;
			ls.axis = 0;
			ls.axisLen = 512;
			ls.origin = 16;
			ls.Resize(8, 480);
			SplitMix64 rng(0x9001ull);
			for (int j = 0; j < ls.count; ++j)
			{
				ls.cross[static_cast<size_t>(j)] = 32 + j * 40;
				double* row = ls.Line(j);
				for (int i = 0; i < ls.length; ++i)
					row[i] = 512.0 + 40.0 * rng.FastUnitZeroMean();
			}

			C::StaticPatchRankSettings onCfg;
			onCfg.enabled = true;
			C::StaticPatchWorkspace ws;
			Assert::IsTrue(C::SubmitFrame(ls, 1.0, onCfg, ws).reason == C::StaticRankReason::WarmingUp);

			C::LineSet nanLs = ls;
			nanLs.Line(2)[60] = std::numeric_limits<double>::quiet_NaN();
			const C::StaticRankResult rNan = C::SubmitFrame(nanLs, 1.0, onCfg, ws);
			Assert::IsFalse(rNan.valid);
			Assert::IsTrue(rNan.reason == C::StaticRankReason::InvalidInput);

			// Geometry change triggers automatic workspace reset and increments ws.geometryResets:
			C::LineSet resizedLs = ls;
			resizedLs.axisLen = 640;
			const C::StaticRankResult rResized = C::SubmitFrame(resizedLs, 1.0, onCfg, ws);
			Assert::IsTrue(rResized.reason == C::StaticRankReason::WarmingUp);
			Assert::AreEqual(1, ws.geometryResets);

			// Original pathway forces OFF:
			ChainFx::ManualClock clk;
			clk.Set(1000.0);
			C::EstimatorSettings cfg;
			cfg.staticPatchRank.enabled = true;
			C::ChainEstimator est(cfg, clk.Fn());
			est.SetSynchronous(true);
			est.SetMode(C::EstimatorMode::On);
			est.SetOriginalPathway(true);
			Assert::IsFalse(est.Running());
			Assert::AreEqual(uint64_t(0), est.GetCounters().staticRankEvals);
			est.Stop();
		}
	};
}
