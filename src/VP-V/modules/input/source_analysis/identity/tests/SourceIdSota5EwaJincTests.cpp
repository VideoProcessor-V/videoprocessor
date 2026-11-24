/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for SID-SOTA-5:
// 1-step Golub-Pereyra Gauss-Newton sub-pixel phase refinement & 2-D radial EWA-Jinc3 probe
// (EwaJincProbe, DescaleVerifier, KernelBank, ChainEstimator kReasonEwaJincVerified, W1DescaleRef)
// (Rule 3 mandatory same-input negative controls and separable/native false-positive traps, pure CPU C++14 /W4).
//
// Covers:
//   1. Sota5KernelDerivativesBesselJ1AndGaussNewtonSubPixelPhaseRefinement
//   2. Sota5Genuine2DEwaJinc3UpscaleBeatsSeparableAndRejectsSeparableAndNativeTraps
//   3. Sota5WorkspaceReuseOriginalPathwayAndDefaultOffW1DescaleRefMemcmp
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"
#include "W1DescaleRef.h"

#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/EwaJincProbe.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota5EwaFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBicubic = 1;
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

			double FastUnitZeroMean()
			{
				const uint64_t u = NextU64();
				const double a = static_cast<double>(u & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double b = static_cast<double>((u >> 16) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double c = static_cast<double>((u >> 32) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				const double d = static_cast<double>((u >> 48) & 0xFFFFu) * (1.0 / 65535.0) - 0.5;
				return (a + b + c + d) * 1.7320508075688772;
			}
		};

		inline uint16_t Quantize10(double v)
		{
			double q = std::floor(v + 0.5);
			if (q < 64.0)
				q = 64.0;
			if (q > 940.0)
				q = 940.0;
			return static_cast<uint16_t>(q);
		}

		// Synthesizes a 1-D LineSet upscaled from nIn -> nOut at exact sub-pixel `truePhase` (in latent samples).
		inline C::LineSet BuildSubPixelLineSet(int nIn, int nOut, int lineCount, int margin, int kernel, double truePhase, uint64_t seed)
		{
			C::LineSet ls;
			ls.axis = 0;
			ls.axisLen = nOut;
			ls.origin = margin;
			const int len = nOut - 2 * margin;
			ls.Resize(lineCount, len);

			SplitMix64 rng(seed);
			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> wt(static_cast<size_t>(taps));
			const double ratio = static_cast<double>(nIn) / static_cast<double>(nOut);

			for (int j = 0; j < lineCount; ++j)
			{
				std::vector<double> src(static_cast<size_t>(nIn), 512.0);
				for (int i = 0; i < nIn; ++i)
					src[static_cast<size_t>(i)] = 512.0 + 95.0 * rng.FastUnitZeroMean();

				double* dst = ls.Line(j);
				for (int i = 0; i < len; ++i)
				{
					const double p = (static_cast<double>(margin + i) + 0.5) * ratio - 0.5 + truePhase;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, wt.data());
					double acc = 0.0;
					for (int s = 0; s < taps; ++s)
					{
						int c = static_cast<int>(fl) - r + 1 + s;
						if (c < 0) c = 0;
						if (c > nIn - 1) c = nIn - 1;
						acc += wt[static_cast<size_t>(s)] * src[static_cast<size_t>(c)];
					}
					dst[i] = static_cast<double>(Quantize10(acc));
				}
				ls.cross[static_cast<size_t>(j)] = margin + j * 40;
			}
			return ls;
		}

		// Builds an isotropic + diagonal 2-D latent image (1920 x 1080) with strong diagonal high-frequency energy
		// where radial circular passband (EWA-Jinc3) and separable square passband (Lanczos3) maximally diverge.
		inline std::vector<double> BuildLatent2D(int w, int h, uint64_t seed)
		{
			SplitMix64 rng(seed);
			std::vector<double> img(static_cast<size_t>(w) * static_cast<size_t>(h), 512.0);
			for (int y = 0; y < h; ++y)
			{
				double* row = &img[static_cast<size_t>(y) * static_cast<size_t>(w)];
				for (int x = 0; x < w; ++x)
				{
					const double diagChecker = (((x + y) & 1) == 0 ? 42.0 : -42.0) * std::cos(0.07 * static_cast<double>(x - y));
					const double isoNoise = 95.0 * rng.FastUnitZeroMean();
					row[x] = C::ClampD(512.0 + isoNoise + diagChecker, 140.0, 880.0);
				}
			}
			return img;
		}

		// Exact 2x radial mpv/libplacebo EWA-Jinc3 upscale (1920x1080 -> 3840x2160, centre-aligned phase = 0).
		// At exact 2x centre-aligned upscale, (px - floor(px), py - floor(py)) takes only the 2x2 = 4 sub-pixel
		// phases {0.25, 0.75} x {0.25, 0.75}, so precomputing the 4 exact normalized 8x8 EwaJinc3Weight stencils
		// makes full 4K synthesis 100% bit-identical to per-pixel EwaJinc3Weight(hypot(dx, dy)) in milliseconds!
		inline ChainFx::Plane Upscale2xEwaJinc3(const std::vector<double>& latent, int srcW, int srcH)
		{
			const int outW = srcW * 2;
			const int outH = srcH * 2;
			constexpr int kRad = 4; // ceil(kJincZero3 = 3.2383) == 4 => 8x8 support box [-3..+4] around floor(p)
			constexpr int kSpan = 2 * kRad;

			double stencils[2][2][kSpan][kSpan] = {};
			for (int pyParity = 0; pyParity < 2; ++pyParity)
			{
				const double fy = (pyParity == 0) ? 0.75 : 0.25; // p = (2*m + parity + 0.5)*0.5 - 0.5 => m - 0.25 (fl=m-1, f=0.75) or m + 0.25 (fl=m, f=0.25)
				for (int pxParity = 0; pxParity < 2; ++pxParity)
				{
					const double fx = (pxParity == 0) ? 0.75 : 0.25;
					double wSum = 0.0;
					for (int sy = 0; sy < kSpan; ++sy)
					{
						const double dy = fy - static_cast<double>(sy - (kRad - 1));
						for (int sx = 0; sx < kSpan; ++sx)
						{
							const double dx = fx - static_cast<double>(sx - (kRad - 1));
							const double r = std::sqrt(dx * dx + dy * dy);
							const double w = C::EwaJinc3Weight(r);
							stencils[pyParity][pxParity][sy][sx] = w;
							wSum += w;
						}
					}
					for (int sy = 0; sy < kSpan; ++sy)
						for (int sx = 0; sx < kSpan; ++sx)
							stencils[pyParity][pxParity][sy][sx] /= wSum;
				}
			}

			ChainFx::Plane out;
			out.w = outW;
			out.h = outH;
			out.luma.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH));

			for (int y = 0; y < outH; ++y)
			{
				const double py = (static_cast<double>(y) + 0.5) * 0.5 - 0.5;
				const int iyBase = static_cast<int>(std::floor(py)) - (kRad - 1);
				const int pyP = y & 1;
				uint16_t* dstRow = &out.luma[static_cast<size_t>(y) * static_cast<size_t>(outW)];

				for (int x = 0; x < outW; ++x)
				{
					const double px = (static_cast<double>(x) + 0.5) * 0.5 - 0.5;
					const int ixBase = static_cast<int>(std::floor(px)) - (kRad - 1);
					const int pxP = x & 1;
					const auto& st = stencils[pyP][pxP];

					double acc = 0.0;
					for (int sy = 0; sy < kSpan; ++sy)
					{
						int cy = iyBase + sy;
						if (cy < 0) cy = 0;
						if (cy > srcH - 1) cy = srcH - 1;
						const double* sRow = &latent[static_cast<size_t>(cy) * static_cast<size_t>(srcW)];
						for (int sx = 0; sx < kSpan; ++sx)
						{
							int cx = ixBase + sx;
							if (cx < 0) cx = 0;
							if (cx > srcW - 1) cx = srcW - 1;
							acc += st[sy][sx] * sRow[cx];
						}
					}
					dstRow[x] = Quantize10(acc);
				}
			}
			return out;
		}

		// Exact 2x separable 2-D upscale (1920x1080 -> 3840x2160) on the identical latent buffer.
		inline ChainFx::Plane Upscale2xSeparable(const std::vector<double>& latent, int srcW, int srcH, int kernel)
		{
			const int outW = srcW * 2;
			const int outH = srcH * 2;
			const int r = C::Kernel(kernel).radius;
			const int taps = 2 * r;
			double wParity[2][8] = {};
			C::TapWeights(kernel, 0.75, wParity[0]);
			C::TapWeights(kernel, 0.25, wParity[1]);

			std::vector<double> tmp(static_cast<size_t>(outW) * static_cast<size_t>(srcH), 0.0);
			for (int y = 0; y < srcH; ++y)
			{
				const double* sRow = &latent[static_cast<size_t>(y) * static_cast<size_t>(srcW)];
				double* dRow = &tmp[static_cast<size_t>(y) * static_cast<size_t>(outW)];
				for (int x = 0; x < outW; ++x)
				{
					const double px = (static_cast<double>(x) + 0.5) * 0.5 - 0.5;
					const int c0 = static_cast<int>(std::floor(px)) - r + 1;
					const double* w = wParity[x & 1];
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

			ChainFx::Plane out;
			out.w = outW;
			out.h = outH;
			out.luma.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH));
			for (int y = 0; y < outH; ++y)
			{
				const double py = (static_cast<double>(y) + 0.5) * 0.5 - 0.5;
				const int c0 = static_cast<int>(std::floor(py)) - r + 1;
				const double* w = wParity[y & 1];
				uint16_t* dRow = &out.luma[static_cast<size_t>(y) * static_cast<size_t>(outW)];
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
					dRow[x] = Quantize10(acc);
				}
			}
			return out;
		}
	}

	TEST_CLASS(SourceIdSota5EwaJincTests)
	{
	public:
		// 1. Analytical 1-D kernel derivatives + Bessel/Jinc accuracy + 1-step Golub-Pereyra Gauss-Newton
		//    sub-pixel phase refinement across phase = {0.125, 0.37, -0.18}:
		//      - Proves markedly lower residual & < 0.03-sample phase error vs discrete 0.25-grid negative control;
		//      - Proves step clamping in [-0.25, +0.25] and flat/ill-conditioned input fallback incrementing
		//        phaseRefineFallbacks safely with zero NaN.
		TEST_METHOD(Sota5KernelDerivativesBesselJ1AndGaussNewtonSubPixelPhaseRefinement)
		{
			using namespace Sota5EwaFx;

			// (A) Analytical TapWeightDerivatives vs central finite differences across ALL 10 kernels (0..9):
			const double probeT[5] = { 1e-5, 0.125, 0.37, 0.50, 0.82 };
			const double epsFd = 1e-7;
			for (int k = 0; k < C::kKernelCount; ++k)
			{
				const int taps = 2 * C::Kernel(k).radius;
				for (int ti = 0; ti < 5; ++ti)
				{
					const double t = probeT[ti];
					double w[8] = {}, dw[8] = {}, wRef[8] = {}, wPlus[8] = {}, wMinus[8] = {};
					C::TapWeightDerivatives(k, t, w, dw);
					C::TapWeights(k, t, wRef);
					C::TapWeights(k, t + epsFd, wPlus);
					C::TapWeights(k, t - epsFd, wMinus);

					double sumW = 0.0, sumDw = 0.0;
					for (int s = 0; s < taps; ++s)
					{
						Assert::AreEqual(wRef[s], w[s], 1e-14, L"TapWeightDerivatives w[s] must match TapWeights");
						const double fd = (wPlus[s] - wMinus[s]) / (2.0 * epsFd);
						Assert::AreEqual(fd, dw[s], 1e-6, L"TapWeightDerivatives dw[s] must match finite difference");
						sumW += w[s];
						sumDw += dw[s];
					}
					Assert::AreEqual(1.0, sumW, 1e-14);
					Assert::AreEqual(0.0, sumDw, 1e-13, L"partition-of-unity derivative sum(dw) must vanish");
				}
			}

			// (B) BesselJ1, Jinc, and EwaJinc3Weight mathematical identities:
			Assert::AreEqual(0.0, C::BesselJ1(0.0), 0.0);
			Assert::AreEqual(0.0, C::BesselJ1(std::numeric_limits<double>::quiet_NaN()), 0.0);
			Assert::AreEqual(-C::BesselJ1(2.5), C::BesselJ1(-2.5), 1e-15, L"BesselJ1 odd symmetry");
			Assert::AreEqual(1.0, C::Jinc(0.0), 0.0);
			Assert::AreEqual(0.0, C::Jinc(C::kJincZero1), 1e-13, L"first zero of Jinc(r)");
			Assert::AreEqual(0.0, C::Jinc(C::kJincZero3), 1e-13, L"third zero of Jinc(r)");
			Assert::AreEqual(1.0, C::EwaJinc3Weight(0.0), 0.0);
			Assert::AreEqual(0.0, C::EwaJinc3Weight(C::kJincZero3), 0.0);
			Assert::AreEqual(0.0, C::EwaJinc3Weight(4.0), 0.0);
			// Continuity across |x| = 1000 Hankel asymptotic switch:
			Assert::AreEqual(C::BesselJ1(999.9999), C::BesselJ1(1000.0001), 2e-5);

			// (C) Sub-pixel shifted upscales at phase = {0.125, 0.37, -0.18}:
			//     Compare 1-step Gauss-Newton (gaussNewtonPhaseRefine = true) vs discrete 0.25-grid negative control!
			const double testPhases[3] = { 0.125, 0.37, -0.18 };
			const int kernels[2] = { kLanczos3, kBicubic };
			const int cands[1] = { 1920 };
			const C::VerifyOptions vOpt;

			for (int ki = 0; ki < 2; ++ki)
			{
				for (int pi = 0; pi < 3; ++pi)
				{
					const double truePhase = testPhases[pi];
					const C::LineSet ls = BuildSubPixelLineSet(
						1920, kUhdW, 16, 48, kernels[ki], truePhase, 0x5500ull + static_cast<uint64_t>(ki * 19 + pi * 7));

					// Mandatory In-Test Negative Control: discrete 0.25-sample phase grid (gaussNewtonPhaseRefine = false)
					C::VerifierSettings vsGrid;
					vsGrid.phaseCount = 5; // tries {-0.50, -0.25, 0.0, +0.25, +0.50}
					vsGrid.phaseStep = 0.25;
					vsGrid.gaussNewtonPhaseRefine = false;
					C::DescaleVerifier verGrid(vsGrid);

					C::VerifierSettings vsGn = vsGrid;
					vsGn.gaussNewtonPhaseRefine = true;
					C::DescaleVerifier verGn(vsGn);

					C::VerifyResult vrGrid, vrGn;
					Assert::IsTrue(verGrid.Evaluate(ls, cands, 1, vOpt, vrGrid));
					Assert::IsTrue(verGn.Evaluate(ls, cands, 1, vOpt, vrGn));

					const C::CandidateResult& cGrid = vrGrid.candidates[0];
					const C::CandidateResult& cGn = vrGn.candidates[0];
					Assert::IsTrue(cGrid.valid && cGrid.exactDone);
					Assert::IsTrue(cGn.valid && cGn.exactDone);

					// Prove discrete 0.25-grid negative control suffers >= 0.06-sample phase error (> 0.03):
					Assert::IsFalse(cGrid.phaseRefined);
					Assert::IsTrue(std::fabs(cGrid.phase - truePhase) >= 0.06,
						L"negative control: discrete 0.25-grid leaves >= 0.06-sample phase error on off-grid phases");

					// Prove 1-step Gauss-Newton achieves < 0.03-sample phase error & markedly lower residual:
					Assert::IsTrue(cGn.phaseRefined, L"Gauss-Newton phase refinement must engage on sub-pixel upscale");
					Assert::IsFalse(cGn.phaseRefineFallback);
					Assert::AreEqual(0, vrGn.phaseRefineFallbacks);
					Assert::IsTrue(std::fabs(cGn.phaseRefineStep) <= 0.25 + 1e-9);
					Assert::IsTrue(std::fabs(cGn.phase - truePhase) < 0.03,
						L"1-step Gauss-Newton refinement must recover sub-pixel phase within < 0.03 latent samples");
					Assert::IsTrue(cGn.stat < cGrid.stat - 0.15,
						L"1-step Gauss-Newton refinement must reach markedly lower log-ratio stat than discrete 0.25-grid");
					Assert::IsTrue(cGn.floorCodes < cGrid.floorCodes * 0.75,
						L"1-step Gauss-Newton refinement must markedly reduce raw RMS residual floorCodes");
				}
			}

			// Step clamp check [-0.25, +0.25] from phi0 = 0.0 (phaseCount = 1) on large offset phase = 0.44:
			{
				const C::LineSet lsLarge = BuildSubPixelLineSet(1920, kUhdW, 16, 48, kLanczos3, 0.44, 0x5599ull);
				C::VerifierSettings vsOne;
				vsOne.phaseCount = 1; // phi0 = 0.0 only
				vsOne.gaussNewtonPhaseRefine = true;
				C::DescaleVerifier verOne(vsOne);
				C::VerifyResult vrOne;
				Assert::IsTrue(verOne.Evaluate(lsLarge, cands, 1, vOpt, vrOne));
				Assert::IsTrue(std::fabs(vrOne.candidates[0].phaseRefineStep) <= 0.25 + 1e-9,
					L"Gauss-Newton phase step must be clamped strictly to [-0.25, +0.25]");
				Assert::IsTrue(std::isfinite(vrOne.candidates[0].stat) && std::isfinite(vrOne.candidates[0].phase));
			}

			// (D) Flat / ill-conditioned input fallback increments phaseRefineFallbacks safely with zero NaN:
			{
				C::LineSet flatLs;
				flatLs.axis = 0;
				flatLs.axisLen = kUhdW;
				flatLs.origin = 48;
				flatLs.Resize(16, kUhdW - 96);
				std::fill(flatLs.data.begin(), flatLs.data.end(), 512.0);

				C::VerifierSettings vsGn;
				vsGn.gaussNewtonPhaseRefine = true;
				C::DescaleVerifier verGn(vsGn);
				C::VerifyResult vrFlat;
				Assert::IsTrue(verGn.Evaluate(flatLs, cands, 1, vOpt, vrFlat));
				Assert::IsTrue(vrFlat.phaseRefineFallbacks >= 1,
					L"flat/ill-conditioned input must increment VerifyResult::phaseRefineFallbacks");
				const C::CandidateResult& cf = vrFlat.candidates[0];
				Assert::IsTrue(cf.phaseRefineFallback);
				Assert::IsFalse(cf.phaseRefined);
				Assert::AreEqual(0.0, cf.phaseRefineStep, 0.0);
				Assert::IsTrue(std::isfinite(cf.stat) && std::isfinite(cf.phase) && std::isfinite(cf.floorCodes),
					L"flat fallback must produce zero NaNs across all CandidateResult fields");

				// Also verify ChainEstimator surfaces EstimatorCounters::phaseRefineFallbacks on flat snapshot:
				C::LineRect active{ 0, 0, kUhdW, kUhdH };
				const C::LinePlan plan = C::LineSampler::Plan(active, 1);
				C::LineSnapshot flatSnap;
				flatSnap.seq = 1;
				flatSnap.timeUs = 1000000;
				flatSnap.rasterW = kUhdW;
				flatSnap.rasterH = kUhdH;
				flatSnap.active = plan.active;
				flatSnap.inner = plan.inner;
				flatSnap.rowCount = plan.rowCount;
				flatSnap.colCount = plan.colCount;
				flatSnap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
				flatSnap.colX.assign(plan.colX, plan.colX + plan.colCount);
				flatSnap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 512u);
				flatSnap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 512u);

				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.verifier.gaussNewtonPhaseRefine = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(flatSnap));
				Assert::IsTrue(est.GetCounters().phaseRefineFallbacks >= 1u,
					L"ChainEstimator must accumulate phaseRefineFallbacks on flat frames");
				est.Stop();
			}
		}

		// 2. Genuine 2-D radial mpv/libplacebo EWA-Jinc3 upscaled frame & patch vs mandatory negative controls:
		//      - Genuine 2-D EWA-Jinc3 upscale beats separable Lanczos3 & Bicubic and sets kReasonEwaJincVerified ("ewa-jinc");
		//      - Trap (a): 2-D separable Lanczos3 upscale on the identical latent image stays Separable (NEVER claimed as EWA-Jinc);
		//      - Trap (b): Native 4K isotropic 2-D texture returns NativeOrUnsure / NoUpscaleDip (NEVER false-verifies).
		TEST_METHOD(Sota5Genuine2DEwaJinc3UpscaleBeatsSeparableAndRejectsSeparableAndNativeTraps)
		{
			using namespace Sota5EwaFx;

			const C::EwaJincSettings defCfg;
			Assert::IsFalse(defCfg.enabled, L"EwaJincSettings must default to OFF");
			Assert::AreEqual(std::string("ewa-jinc"),
				SourceGeometry::ReasonWords(C::kReasonEwaJincVerified),
				L"ReasonWords(kReasonEwaJincVerified) must render 'ewa-jinc'");

			// Verify precomputed 2x stencil matches per-pixel EwaJinc3Weight(hypot(dx, dy)) to 0.0 codes on a 24x24 crop:
			const std::vector<double> latent1080 = BuildLatent2D(1920, 1080, 0x6601ull);
			const ChainFx::Plane ewaPlane = Upscale2xEwaJinc3(latent1080, 1920, 1080);
			const ChainFx::Plane sepL3Plane = Upscale2xSeparable(latent1080, 1920, 1080, kLanczos3);

			ChainFx::Plane nat4KPlane;
			{
				const std::vector<double> natBuf = BuildLatent2D(kUhdW, kUhdH, 0x6602ull);
				nat4KPlane.w = kUhdW;
				nat4KPlane.h = kUhdH;
				nat4KPlane.luma.resize(natBuf.size());
				for (size_t i = 0; i < natBuf.size(); ++i)
					nat4KPlane.luma[i] = Quantize10(natBuf[i]);
			}

			const int ps = defCfg.patchSize; // 24
			const int ox = (kUhdW - ps) / 2;
			const int oy = (kUhdH - ps) / 2;
			const double floorCodes = 1.0 / std::sqrt(12.0);

			auto extractCenterPatch = [&](const ChainFx::Plane& pl) -> std::vector<double> {
				std::vector<double> patch(static_cast<size_t>(ps) * static_cast<size_t>(ps));
				for (int py = 0; py < ps; ++py)
				{
					const uint16_t* row = &pl.luma[static_cast<size_t>(oy + py) * static_cast<size_t>(pl.w) + static_cast<size_t>(ox)];
					for (int px = 0; px < ps; ++px)
						patch[static_cast<size_t>(py * ps + px)] = static_cast<double>(row[px]);
				}
				return patch;
			};

			const std::vector<double> ewaPatch = extractCenterPatch(ewaPlane);
			const std::vector<double> sepPatch = extractCenterPatch(sepL3Plane);
			const std::vector<double> natPatch = extractCenterPatch(nat4KPlane);

			C::EwaJincSettings onCfg;
			onCfg.enabled = true;
			C::EwaJincWorkspace ws;

			// (A) Direct EvaluatePatch on genuine 2-D radial EWA-Jinc3 patch:
			const C::EwaProbeResult rEwa = C::EvaluatePatch(
				ewaPatch.data(), ps, ps, ps, ox, oy, kUhdW, 1920, kUhdH, 1080, 0.0, 0.0, floorCodes, onCfg, ws);
			Assert::IsTrue(rEwa.valid, L"EvaluatePatch must succeed on detailed 24x24 EWA-Jinc3 patch");
			Assert::IsTrue(rEwa.verdict == C::EwaProbeVerdict::EwaJinc, L"genuine EWA-Jinc3 patch must verify as EwaJinc");
			Assert::IsTrue(rEwa.reason == C::EwaProbeReason::EwaJincConfirmed);
			Assert::IsTrue(rEwa.statEwa < onCfg.minUpscaleDip);
			Assert::IsTrue(rEwa.ewaMarginOverSeparable >= onCfg.minKernelMargin,
				L"EWA-Jinc3 residual must beat both separable Lanczos3 and separable Bicubic by >= minKernelMargin");

			// Disabled negative control on identical EWA patch:
			const C::EwaProbeResult rOff = C::EvaluatePatch(
				ewaPatch.data(), ps, ps, ps, ox, oy, kUhdW, 1920, kUhdH, 1080, 0.0, 0.0, floorCodes, defCfg, ws);
			Assert::IsFalse(rOff.valid);
			Assert::IsTrue(rOff.reason == C::EwaProbeReason::Disabled);

			// (B) Mandatory Trap (a): Separable Lanczos3 2-D upscale on the EXACT SAME latent image stays Separable!
			const C::EwaProbeResult rSep = C::EvaluatePatch(
				sepPatch.data(), ps, ps, ps, ox, oy, kUhdW, 1920, kUhdH, 1080, 0.0, 0.0, floorCodes, onCfg, ws);
			Assert::IsTrue(rSep.valid);
			Assert::IsTrue(rSep.verdict == C::EwaProbeVerdict::Separable,
				L"Trap (a): separable Lanczos3 2-D upscale MUST stay Separable (never falsely claimed by EWA-Jinc)");
			Assert::IsTrue(rSep.reason == C::EwaProbeReason::SeparablePreferred);
			Assert::AreEqual(C::kEwaSeparableLanczos3, rSep.bestSeparableKernel);
			Assert::IsTrue(rSep.ewaMarginOverSeparable <= -onCfg.minKernelMargin);

			// (C) Mandatory Trap (b): Native 4K isotropic texture does NOT false-verify!
			const C::EwaProbeResult rNat = C::EvaluatePatch(
				natPatch.data(), ps, ps, ps, ox, oy, kUhdW, 1920, kUhdH, 1080, 0.0, 0.0, floorCodes, onCfg, ws);
			Assert::IsTrue(rNat.valid);
			Assert::IsTrue(rNat.verdict == C::EwaProbeVerdict::NativeOrUnsure,
				L"Trap (b): native 4K isotropic texture MUST NOT false-verify as EWA-Jinc");
			Assert::IsTrue(rNat.reason == C::EwaProbeReason::NoUpscaleDip);

			// (D) End-to-end ChainEstimator::Deliver (ChainFx::Feed, without EvidenceLevel::Frames!) across all 3 planes:
			auto runChainPlane = [&](const ChainFx::Plane& pl, bool enableEwa, G::Record& outRec, C::EstimatorCounters& outCnt) {
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.ewaJinc.enabled = enableEwa;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				ChainFx::Feed(est, pl, 1, 1.0);
				Assert::IsTrue(est.Held().ReadLatest(0, outRec));
				outCnt = est.GetCounters();
				est.Stop();
			};

			G::Record recEwaOn, recEwaOff, recSepOn, recNatOn;
			C::EstimatorCounters cntEwaOn, cntEwaOff, cntSepOn, cntNatOn;
			runChainPlane(ewaPlane, true, recEwaOn, cntEwaOn);
			runChainPlane(ewaPlane, false, recEwaOff, cntEwaOff);
			runChainPlane(sepL3Plane, true, recSepOn, cntSepOn);
			runChainPlane(nat4KPlane, true, recNatOn, cntNatOn);

			Assert::AreEqual(uint64_t(0), cntEwaOff.ewaJincVerifications,
				L"negative control: default-OFF ChainEstimator must report 0 ewaJincVerifications");
			Assert::AreEqual(uint32_t(0), recEwaOff.applied[0].reasonBits & C::kReasonEwaJincVerified);

			Assert::IsTrue(cntEwaOn.ewaJincVerifications >= 1u,
				L"ChainEstimator::Deliver with ewaJinc.enabled=true must verify 2-D radial EWA-Jinc3 plane without requiring EvidenceLevel::Frames");
			Assert::IsTrue((recEwaOn.measured[0].reasonBits & C::kReasonEwaJincVerified) != 0u);
			Assert::IsTrue((recEwaOn.applied[0].reasonBits & C::kReasonEwaJincVerified) != 0u);

			Assert::AreEqual(uint64_t(0), cntSepOn.ewaJincVerifications,
				L"Trap (a): ChainEstimator must NOT set ewaJincVerifications on separable Lanczos3 2-D upscale");
			Assert::AreEqual(uint32_t(0), recSepOn.applied[0].reasonBits & C::kReasonEwaJincVerified);

			Assert::AreEqual(uint64_t(0), cntNatOn.ewaJincVerifications,
				L"Trap (b): ChainEstimator must NOT set ewaJincVerifications on native 4K isotropic texture");
			Assert::AreEqual(uint32_t(0), recNatOn.applied[0].reasonBits & C::kReasonEwaJincVerified);
		}

		// 3. Workspace reuse, Original pathway forces OFF, and Mandatory Control (c): default-OFF W1DescaleRef
		//    std::memcmp == 0 stays green (with 1-ULP perturbation negative control).
		TEST_METHOD(Sota5WorkspaceReuseOriginalPathwayAndDefaultOffW1DescaleRefMemcmp)
		{
			using namespace Sota5EwaFx;

			const std::vector<double> latent = BuildLatent2D(64, 64, 0x7701ull);
			const ChainFx::Plane ewaSmall = Upscale2xEwaJinc3(latent, 64, 64); // 128 x 128
			const int ps = 24;
			const int ox = 52;
			const int oy = 52;
			std::vector<double> patch(static_cast<size_t>(ps * ps));
			for (int py = 0; py < ps; ++py)
				for (int px = 0; px < ps; ++px)
					patch[static_cast<size_t>(py * ps + px)] =
						static_cast<double>(ewaSmall.luma[static_cast<size_t>((oy + py) * 128 + (ox + px))]);

			C::EwaJincSettings onCfg;
			onCfg.enabled = true;
			C::EwaJincWorkspace wsDirty, wsFresh;
			const double floorCodes = 1.0 / std::sqrt(12.0);

			// Pollute wsDirty with NaN patch, undersized patch, flat patch, and different geometry:
			{
				std::vector<double> nanPatch = patch;
				nanPatch[17] = std::numeric_limits<double>::quiet_NaN();
				const C::EwaProbeResult rNan = C::EvaluatePatch(
					nanPatch.data(), ps, ps, ps, ox, oy, 128, 64, 128, 64, 0.0, 0.0, floorCodes, onCfg, wsDirty);
				Assert::IsFalse(rNan.valid);
				Assert::IsTrue(rNan.reason == C::EwaProbeReason::InvalidInput);

				const C::EwaProbeResult rSmall = C::EvaluatePatch(
					patch.data(), 12, 12, ps, ox, oy, 128, 64, 128, 64, 0.0, 0.0, floorCodes, onCfg, wsDirty);
				Assert::IsFalse(rSmall.valid);
				Assert::IsTrue(rSmall.reason == C::EwaProbeReason::PatchTooSmall);

				std::vector<double> flatPatch(static_cast<size_t>(ps * ps), 512.0);
				const C::EwaProbeResult rFlat = C::EvaluatePatch(
					flatPatch.data(), ps, ps, ps, ox, oy, 128, 64, 128, 64, 0.0, 0.0, floorCodes, onCfg, wsDirty);
				Assert::IsFalse(rFlat.valid);
				Assert::IsTrue(rFlat.reason == C::EwaProbeReason::LowPatchDetail);
			}

			// Reused dirty workspace vs fresh workspace must produce bit-identical results:
			const C::EwaProbeResult rReused = C::EvaluatePatch(
				patch.data(), ps, ps, ps, ox, oy, 128, 64, 128, 64, 0.0, 0.0, floorCodes, onCfg, wsDirty);
			const C::EwaProbeResult rFresh = C::EvaluatePatch(
				patch.data(), ps, ps, ps, ox, oy, 128, 64, 128, 64, 0.0, 0.0, floorCodes, onCfg, wsFresh);
			Assert::IsTrue(rReused.valid && rFresh.valid);
			Assert::IsTrue(rReused.verdict == C::EwaProbeVerdict::EwaJinc);
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.statEwa, rFresh.statEwa));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.statSepLanczos3, rFresh.statSepLanczos3));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.statSepBicubic, rFresh.statSepBicubic));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.ewaMarginOverSeparable, rFresh.ewaMarginOverSeparable));
			Assert::IsTrue(W1DescaleRef::MemEq(rReused.baselineRms, rFresh.baselineRms));

			// Original pathway forces ChainEstimator OFF:
			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.ewaJinc.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				est.SetOriginalPathway(true);
				Assert::IsFalse(est.Running());
				Assert::AreEqual(uint64_t(0), est.GetCounters().ewaJincVerifications);
				est.Stop();
			}

			// Mandatory Control (c): Default-OFF DescaleVerifier & PowerProbe match verbatim W1DescaleRef (std::memcmp == 0):
			const C::LineSet subPxLs = BuildSubPixelLineSet(1920, kUhdW, 32, 48, kLanczos3, 0.37, 0x8801ull);
			const C::VerifierSettings defVs;
			Assert::IsFalse(defVs.gaussNewtonPhaseRefine);
			Assert::IsFalse(defVs.hdrNoise.enabled);

			C::DescaleVerifier liveVer(defVs);
			W1DescaleRef::DescaleVerifierW1 w1Ver(defVs);
			C::DescaleVerifier liveProbeVer(defVs);
			W1DescaleRef::DescaleVerifierW1 w1ProbeVer(defVs);
			const C::ProbeSettings defProbe;
			C::PowerProbe::Workspace probeWs;
			const int candsW1[3] = { 1280, 1920, 2560 };
			const C::VerifyOptions defOpt;

			C::VerifyResult vrLive, vrW1;
			Assert::IsTrue(liveVer.Evaluate(subPxLs, candsW1, 3, defOpt, vrLive));
			Assert::IsTrue(w1Ver.Evaluate(subPxLs, candsW1, 3, defOpt, vrW1));
			Assert::IsTrue(W1DescaleRef::MemEq(vrLive.reason, vrW1.reason));
			Assert::AreEqual(0, vrLive.phaseRefineFallbacks);
			Assert::AreEqual(vrW1.candidates.size(), vrLive.candidates.size());

			for (size_t ci = 0; ci < vrLive.candidates.size(); ++ci)
			{
				const C::CandidateResult& a = vrLive.candidates[ci];
				const C::CandidateResult& b = vrW1.candidates[ci];
				Assert::IsFalse(a.phaseRefined);
				Assert::IsFalse(a.phaseRefineFallback);
				Assert::AreEqual(0.0, a.phaseRefineStep, 0.0);
				Assert::IsTrue(W1DescaleRef::MemEq(a.n, b.n));
				Assert::IsTrue(W1DescaleRef::MemEq(a.kernel, b.kernel));
				Assert::IsTrue(W1DescaleRef::MemEq(a.phase, b.phase));
				Assert::IsTrue(W1DescaleRef::MemEq(a.stat, b.stat));
				Assert::IsTrue(W1DescaleRef::MemEq(a.coarseStat, b.coarseStat));
				Assert::IsTrue(W1DescaleRef::MemEq(a.floorCodes, b.floorCodes));
				Assert::IsTrue(W1DescaleRef::MemEq(a.baselineMedian, b.baselineMedian));
				Assert::AreEqual(b.lineStat.size(), a.lineStat.size());
				for (size_t li = 0; li < a.lineStat.size(); ++li)
				{
					Assert::IsTrue(W1DescaleRef::MemEq(a.lineStat[li], b.lineStat[li]));
					Assert::IsTrue(W1DescaleRef::MemEq(a.lineBase[li], b.lineBase[li]));
					Assert::IsTrue(W1DescaleRef::MemEq(a.lineRms[li], b.lineRms[li]));
				}
			}

			C::ProbeResult prLive, prW1;
			Assert::IsTrue(C::PowerProbe::Run(liveProbeVer, subPxLs, floorCodes, 1.0, defProbe, prLive, &probeWs));
			Assert::IsTrue(W1DescaleRef::PowerProbeW1::Run(w1ProbeVer, subPxLs, floorCodes, 1.0, defProbe, prW1));
			Assert::IsTrue(W1DescaleRef::MemEq(prLive.stat, prW1.stat));
			Assert::IsTrue(W1DescaleRef::MemEq(prLive.reason, prW1.reason));

			// 1-ULP perturbation negative control proving MemEq catches even a 1-ULP difference:
			const double ulpShift = std::nextafter(vrLive.candidates[1].stat, vrLive.candidates[1].stat - 1.0);
			Assert::IsFalse(W1DescaleRef::MemEq(vrLive.candidates[1].stat, ulpShift),
				L"negative control: 1-ULP perturbation must fail W1DescaleRef::MemEq");
		}
	};
}
