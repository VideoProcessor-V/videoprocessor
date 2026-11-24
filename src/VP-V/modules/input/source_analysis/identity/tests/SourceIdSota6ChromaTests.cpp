/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Cross-Model Red-Team adversarial synthetic-physics unit tests for SID-SOTA-6
// (Upscaled-Chroma Identification in ChainChromaEstimator.h):
//   1. DeliveredChromaSampling::{Yuv422, Yuv420} HDMI transport origin separation
//      (player-upsampled 4:2:0->4:2:2 Left/TopLeft vs true native 4:2:2 vs flat vs native 4:2:0)
//   2. Opt-in detectBoxAndMpegChroma algebraic Point/Nearest-Neighbor (with 1-sigma noise + odd crop)
//      and MPEG-2/4 [3,1]/4 centered & [1,2,1]/4 cosited linear FIR probes + false-positive traps
//   3. Opt-in knownLumaNH / knownLumaNV arbitrary luma-coupled 4:2:0 chroma grid corroboration
//      (720p->2160p & 1440p->2160p vs native 4K 4:4:4 & 4:2:0 negative controls)
//
// Pure CPU C++14, synthetic physics only, deterministic SplitMix64 PRNG, zero clocks/sleeps.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainChromaEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace Sota6ChromaFx
	{
		using namespace SourceIdentity::Chain;

		// 10-bit uniform rounding quantization std: 1 / sqrt(12).
		constexpr double kFloor10Bit = 0.28867513459481288;

		inline ChromaEstimatorSettings EnabledBase()
		{
			ChromaEstimatorSettings s;
			s.enabled = true;
			s.detectBoxAndMpegChroma = false;
			return s;
		}

		inline ChromaEstimatorSettings EnabledBoxMpeg()
		{
			ChromaEstimatorSettings s;
			s.enabled = true;
			s.detectBoxAndMpegChroma = true;
			return s;
		}

		struct SplitMixRng
		{
			uint64_t state;
			explicit SplitMixRng(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double UniformCentered()
			{
				const double u = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				return u - 0.5;
			}

			double Normal01()
			{
				double u1 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				const double u2 = static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
				if (u1 < 1e-12)
					u1 = 1e-12;
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};

		inline LineSet MakeLineSet(int axis, int axisLen, int lines, int origin = 0, int length = -1)
		{
			const int actualLen = (length > 0) ? length : (axisLen - origin);
			LineSet ls;
			ls.axis = axis;
			ls.axisLen = axisLen;
			ls.origin = origin;
			ls.Resize(lines, actualLen);
			for (int j = 0; j < lines; ++j)
				ls.cross[static_cast<size_t>(j)] = (j + 1) * ((std::max)(axisLen, lines + 2) / (lines + 1));
			return ls;
		}

		inline std::vector<double> Quantize10Bit(const std::vector<double>& x)
		{
			std::vector<double> q(x.size(), 0.0);
			for (size_t i = 0; i < x.size(); ++i)
				q[i] = ClampD(std::floor(x[i] + 0.5), 0.0, 1023.0);
			return q;
		}

		inline std::vector<double> MakeBroadbandChroma(int n, SplitMixRng& rng, double amp = 90.0)
		{
			std::vector<double> z(static_cast<size_t>(n), 512.0);
			double level = 512.0;
			int nextEdge = 8 + static_cast<int>(rng.NextU64() % 19ull);
			for (int i = 0; i < n; ++i)
			{
				if (i >= nextEdge)
				{
					level = 512.0 + 2.2 * amp * rng.UniformCentered();
					nextEdge = i + 7 + static_cast<int>(rng.NextU64() % 23ull);
				}
				z[static_cast<size_t>(i)] = ClampD(level + 0.65 * amp * rng.Normal01(), 72.0, 952.0);
			}
			return z;
		}

		// Piecewise-constant + subtle micro-texture colored profile evaluated at continuous coordinate x.
		inline double EvalEdgeProfile(double x, const std::vector<double>& edges, const std::vector<double>& levels)
		{
			size_t idx = 0;
			while (idx < edges.size() && x >= edges[idx])
				++idx;
			return levels[idx];
		}

		// Classical separable 1D resampler using exact centre-aligned mapping p_i = (i + 0.5) * n / N - 0.5 + phase.
		inline std::vector<double> UpscaleClassical1D(const std::vector<double>& src, int N, int kernel, double phase)
		{
			const int n = static_cast<int>(src.size());
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			std::vector<double> out(static_cast<size_t>(N), 0.0);
			double w[8] = {};
			for (int i = 0; i < N; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(n) / static_cast<double>(N) - 0.5 + phase;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, w);
				double acc = 0.0;
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					if (c < 0)
						c = 0;
					else if (c >= n)
						c = n - 1;
					acc += w[s] * src[static_cast<size_t>(c)];
				}
				out[static_cast<size_t>(i)] = acc;
			}
			return out;
		}

		// Direct hardware zero-order hold (Point / Nearest-Neighbor 2x duplication):
		//   out[2*m]     = src[m]
		//   out[2*m + 1] = src[m]
		// Independent 1-sigma noise is added per output sample by the caller so adjacent duplicates are NOT bit-equal.
		inline std::vector<double> UpsamplePointNearest2x(const std::vector<double>& src)
		{
			const size_t n = src.size();
			std::vector<double> out(2u * n, 512.0);
			for (size_t m = 0; m < n; ++m)
			{
				out[2u * m] = src[m];
				out[2u * m + 1u] = src[m];
			}
			return out;
		}

		// Direct MPEG-2 / MPEG-4 centered 2-tap [1, 3]/4 & [3, 1]/4 linear chroma upsampler (2x):
		//   out[2*m]     = (src[m - 1] + 3 * src[m]) / 4
		//   out[2*m + 1] = (3 * src[m] + src[m + 1]) / 4
		inline std::vector<double> UpsampleMpeg31Centered2x(const std::vector<double>& src)
		{
			const int n = static_cast<int>(src.size());
			std::vector<double> out(static_cast<size_t>(2 * n), 512.0);
			for (int m = 0; m < n; ++m)
			{
				const int mPrev = (m > 0) ? (m - 1) : 0;
				const int mNext = (m + 1 < n) ? (m + 1) : (n - 1);
				out[static_cast<size_t>(2 * m)] =
					0.25 * src[static_cast<size_t>(mPrev)] + 0.75 * src[static_cast<size_t>(m)];
				out[static_cast<size_t>(2 * m + 1)] =
					0.75 * src[static_cast<size_t>(m)] + 0.25 * src[static_cast<size_t>(mNext)];
			}
			return out;
		}

		// Direct MPEG-2 / MPEG-4 / H.264 cosited [1, 2, 1]/4 tent linear chroma upsampler (2x zero-stuffed * 2 * [1,2,1]/4):
		//   out[2*m]     = src[m]
		//   out[2*m + 1] = 0.5 * (src[m] + src[m + 1])
		inline std::vector<double> UpsampleMpeg121Cosited2x(const std::vector<double>& src)
		{
			const int n = static_cast<int>(src.size());
			std::vector<double> out(static_cast<size_t>(2 * n), 512.0);
			for (int m = 0; m < n; ++m)
			{
				const int mNext = (m + 1 < n) ? (m + 1) : (n - 1);
				out[static_cast<size_t>(2 * m)] = src[static_cast<size_t>(m)];
				out[static_cast<size_t>(2 * m + 1)] =
					0.5 * (src[static_cast<size_t>(m)] + src[static_cast<size_t>(mNext)]);
			}
			return out;
		}

		// Builds co-located native luma (length N) and latent n = N/2 chroma from shared colored edges,
		// upsampling chroma via classical KernelBank kernel at exact siting phase (0.0 = centered, +0.25 = cosited).
		inline void BuildClassicalSitedAxis(
			int axis,
			int N,
			int lines,
			int kernel,
			double phase,
			SplitMixRng& rng,
			LineSet& lumaOut,
			LineSet& cbOut,
			LineSet& crOut)
		{
			lumaOut = MakeLineSet(axis, N, lines);
			cbOut = MakeLineSet(axis, N, lines);
			crOut = MakeLineSet(axis, N, lines);
			const int n = N / 2;
			const double xOffset = 0.5 - 2.0 * phase;

			for (int li = 0; li < lines; ++li)
			{
				std::vector<double> edges;
				std::vector<double> yLev;
				std::vector<double> cbLev;
				std::vector<double> crLev;
				yLev.push_back(512.0 + 180.0 * rng.UniformCentered());
				cbLev.push_back(512.0 + 170.0 * rng.UniformCentered());
				crLev.push_back(512.0 + 170.0 * rng.UniformCentered());
				double pos = 18.0 + 12.0 * (rng.UniformCentered() + 0.5);
				while (pos < static_cast<double>(N - 20))
				{
					edges.push_back(pos);
					yLev.push_back(512.0 + 220.0 * rng.UniformCentered());
					cbLev.push_back(512.0 + 210.0 * rng.UniformCentered());
					crLev.push_back(512.0 + 210.0 * rng.UniformCentered());
					pos += 15.0 + 22.0 * (rng.UniformCentered() + 0.5);
				}

				std::vector<double> yLine(static_cast<size_t>(N), 512.0);
				for (int i = 0; i < N; ++i)
				{
					const double v = EvalEdgeProfile(static_cast<double>(i), edges, yLev) + 10.0 * rng.Normal01();
					yLine[static_cast<size_t>(i)] = ClampD(std::floor(v + 0.5), 40.0, 980.0);
				}

				std::vector<double> cbLat(static_cast<size_t>(n), 512.0);
				std::vector<double> crLat(static_cast<size_t>(n), 512.0);
				for (int c = 0; c < n; ++c)
				{
					const double xc = 2.0 * static_cast<double>(c) + xOffset;
					cbLat[static_cast<size_t>(c)] = ClampD(EvalEdgeProfile(xc, edges, cbLev) + 8.0 * rng.Normal01(), 64.0, 960.0);
					crLat[static_cast<size_t>(c)] = ClampD(EvalEdgeProfile(xc, edges, crLev) + 8.0 * rng.Normal01(), 64.0, 960.0);
				}

				const std::vector<double> cbUp = Quantize10Bit(UpscaleClassical1D(cbLat, N, kernel, phase));
				const std::vector<double> crUp = Quantize10Bit(UpscaleClassical1D(crLat, N, kernel, phase));
				std::copy(yLine.begin(), yLine.end(), lumaOut.Line(li));
				std::copy(cbUp.begin(), cbUp.end(), cbOut.Line(li));
				std::copy(crUp.begin(), crUp.end(), crOut.Line(li));
			}
		}

		// Builds co-located native luma (length N) and MPEG linear FIR upsampled chroma:
		//   cosited == true  -> [1, 2, 1]/4 tent cosited at x_c = 2*c
		//   cosited == false -> [3, 1]/4 centered at x_c = 2*c + 0.5
		// Uses crisp piecewise-constant color plateaus + gentle sensor micro-texture so [3,1]/4 preserves
		// true zero-overshoot 2-tap step geometry (unlike negative-lobe Bicubic/Lanczos).
		inline void BuildMpegFirSitedAxis(
			int axis,
			int N,
			int lines,
			bool cosited,
			SplitMixRng& rng,
			LineSet& lumaOut,
			LineSet& cbOut,
			LineSet& crOut)
		{
			lumaOut = MakeLineSet(axis, N, lines);
			cbOut = MakeLineSet(axis, N, lines);
			crOut = MakeLineSet(axis, N, lines);
			const int n = N / 2;
			const double xOffset = cosited ? 0.0 : 0.5;

			for (int li = 0; li < lines; ++li)
			{
				std::vector<double> edges;
				std::vector<double> yLev;
				std::vector<double> cbLev;
				std::vector<double> crLev;
				yLev.push_back(512.0 + 190.0 * rng.UniformCentered());
				cbLev.push_back(512.0 + 185.0 * rng.UniformCentered());
				crLev.push_back(512.0 + 185.0 * rng.UniformCentered());
				double pos = 16.0 + 10.0 * (rng.UniformCentered() + 0.5);
				while (pos < static_cast<double>(N - 18))
				{
					edges.push_back(pos);
					yLev.push_back(512.0 + 230.0 * rng.UniformCentered());
					cbLev.push_back(512.0 + 220.0 * rng.UniformCentered());
					crLev.push_back(512.0 + 220.0 * rng.UniformCentered());
					pos += 14.0 + 18.0 * (rng.UniformCentered() + 0.5);
				}

				std::vector<double> yLine(static_cast<size_t>(N), 512.0);
				for (int i = 0; i < N; ++i)
				{
					const double v = EvalEdgeProfile(static_cast<double>(i), edges, yLev) + 6.0 * rng.Normal01();
					yLine[static_cast<size_t>(i)] = ClampD(std::floor(v + 0.5), 40.0, 980.0);
				}

				std::vector<double> cbLat(static_cast<size_t>(n), 512.0);
				std::vector<double> crLat(static_cast<size_t>(n), 512.0);
				for (int c = 0; c < n; ++c)
				{
					const double xc = 2.0 * static_cast<double>(c) + xOffset;
					// Realistic clean studio 4:2:0 chroma plateaus + low-amplitude camera chroma grain.
					cbLat[static_cast<size_t>(c)] = ClampD(EvalEdgeProfile(xc, edges, cbLev) + 8.0 * rng.Normal01(), 64.0, 960.0);
					crLat[static_cast<size_t>(c)] = ClampD(EvalEdgeProfile(xc, edges, crLev) + 8.0 * rng.Normal01(), 64.0, 960.0);
				}

				const std::vector<double> cbUp = Quantize10Bit(
					cosited ? UpsampleMpeg121Cosited2x(cbLat) : UpsampleMpeg31Centered2x(cbLat));
				const std::vector<double> crUp = Quantize10Bit(
					cosited ? UpsampleMpeg121Cosited2x(crLat) : UpsampleMpeg31Centered2x(crLat));
				std::copy(yLine.begin(), yLine.end(), lumaOut.Line(li));
				std::copy(cbUp.begin(), cbUp.end(), cbOut.Line(li));
				std::copy(crUp.begin(), crUp.end(), crOut.Line(li));
			}
		}

		inline double MeanDescaleRms(const LineSet& ls, int n, int kernel, double phase)
		{
			DescaleOperator op;
			if (!op.Build(ls.axisLen, n, kernel, phase, ls.origin, ls.length))
				return 0.0;
			std::vector<double> scratch;
			double segSum[kMaxSegs];
			int segRows[kMaxSegs];
			double acc = 0.0;
			for (int li = 0; li < ls.count; ++li)
			{
				double rss = 0.0;
				int rows = 0;
				op.Residual(ls.Line(li), scratch, rss, rows, segSum, segRows);
				acc += (rows > 0) ? std::sqrt(rss / static_cast<double>(rows)) : 0.0;
			}
			return (ls.count > 0) ? (acc / static_cast<double>(ls.count)) : 0.0;
		}

		inline double ComputeAxisDip2x(const LineSet& ls, int kernel)
		{
			const int N = ls.axisLen;
			const int n = N / 2;
			const double sCand = MeanDescaleRms(ls, n, kernel, 0.0);
			const double sLo = MeanDescaleRms(ls, RoundToInt(0.92 * static_cast<double>(n)), kernel, 0.0);
			const double sHi = MeanDescaleRms(ls, RoundToInt(1.08 * static_cast<double>(n)), kernel, 0.0);
			const double base = 0.5 * (sLo + sHi);
			return std::log((sCand + 1e-6) / (base + 1e-6));
		}

		inline void ComputeParityDiffRms(const LineSet& ls, double& outSameRms, double& outCrossRms)
		{
			double eoSq = 0.0;
			double oeSq = 0.0;
			int64_t eoCnt = 0;
			int64_t oeCnt = 0;
			for (int li = 0; li < ls.count; ++li)
			{
				const double* y = ls.Line(li);
				for (int j = 0; j + 1 < ls.length; ++j)
				{
					const double d = y[j + 1] - y[j];
					const int parity = (((ls.origin + j) % 2) + 2) % 2;
					if (parity == 0)
					{
						eoSq += d * d;
						++eoCnt;
					}
					else
					{
						oeSq += d * d;
						++oeCnt;
					}
				}
			}
			const double r0 = (eoCnt > 0) ? std::sqrt(eoSq / static_cast<double>(eoCnt)) : 0.0;
			const double r1 = (oeCnt > 0) ? std::sqrt(oeSq / static_cast<double>(oeCnt)) : 0.0;
			outSameRms = (std::min)(r0, r1);
			outCrossRms = (std::max)(r0, r1);
		}
	}

	TEST_CLASS(SourceIdSota6ChromaTests)
	{
	public:
		TEST_METHOD(DeliveredYuv422DistinguishesUpsampled420FromTrueNative422AndFlat)
		{
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings on = EnabledBase();

			constexpr int kN = 512;
			constexpr int kLines = 16;
			constexpr int kCatmullRom = 1;
			constexpr int kMitchell = 3;
			constexpr int kLanczos3 = 5;
			constexpr int kSpline36 = 8;
			SplitMixRng rng(2026112461ull);

			// 1. Player-upsampled 4:2:0 -> 4:2:2 over HDMI Yuv422 transport:
			//    (a) Left siting (centered vertical phase = 0.0) and (b) TopLeft siting (cosited vertical phase = +0.25)
			//    across uniquely identifiable single-axis families: Mitchell Bicubic (3), Lanczos3 (5), Spline36 (8).
			const int kernels[3] = { kMitchell, kLanczos3, kSpline36 };
			for (int ki = 0; ki < 3; ++ki)
			{
				const int k = kernels[ki];

				ChromaSampleInput inLeft422;
				inLeft422.floorCodes = kFloor10Bit;
				inLeft422.deliveredSampling = DeliveredChromaSampling::Yuv422;
				BuildClassicalSitedAxis(1, kN, kLines, k, 0.0, rng, inLeft422.lumaV, inLeft422.cbV, inLeft422.crV);

				const ChromaChainEstimate estLeft = EstimateChromaChain(inLeft422, on);
				Assert::IsTrue(estLeft.valid, L"4:2:0->4:2:2 Left siting must be valid");
				Assert::IsTrue(estLeft.origin == ChromaOrigin::Subsampled420, L"4:2:0->4:2:2 Left must report Subsampled420");
				Assert::IsTrue(estLeft.playerUpsampled, L"4:2:0->4:2:2 Left must report playerUpsampled=true");
				Assert::IsTrue(estLeft.siting == ChromaGridSiting::Left, L"Centered V on 4:2:2 transport must map to Left siting");
				Assert::IsTrue(estLeft.reason == ChromaEstimatorReason::PlayerUpsampled420Confirmed);
				Assert::AreEqual(static_cast<int>(Kernel(k).family), estLeft.kernelFamily);
				Assert::AreEqual(2, static_cast<int>(estLeft.ToProductChromaChain().playerUpsampled));

				ChromaSampleInput inTopLeft422;
				inTopLeft422.floorCodes = kFloor10Bit;
				inTopLeft422.deliveredSampling = DeliveredChromaSampling::Yuv422;
				BuildClassicalSitedAxis(1, kN, kLines, k, 0.25, rng, inTopLeft422.lumaV, inTopLeft422.cbV, inTopLeft422.crV);

				const ChromaChainEstimate estTopLeft = EstimateChromaChain(inTopLeft422, on);
				Assert::IsTrue(estTopLeft.valid, L"4:2:0->4:2:2 TopLeft siting must be valid");
				Assert::IsTrue(estTopLeft.origin == ChromaOrigin::Subsampled420, L"4:2:0->4:2:2 TopLeft must report Subsampled420");
				Assert::IsTrue(estTopLeft.playerUpsampled, L"4:2:0->4:2:2 TopLeft must report playerUpsampled=true");
				Assert::IsTrue(estTopLeft.siting == ChromaGridSiting::TopLeft, L"Cosited V on 4:2:2 transport must map to TopLeft siting");
				Assert::IsTrue(estTopLeft.reason == ChromaEstimatorReason::PlayerUpsampled420Confirmed);
				Assert::AreEqual(static_cast<int>(Kernel(k).family), estTopLeft.kernelFamily);

				// Operator cache on Yuv422 must match uncached bit-exact.
				ChromaOperatorCache cache(2);
				const ChromaChainEstimate estCached = EstimateChromaChain(inTopLeft422, on, &cache);
				Assert::AreEqual(estTopLeft.valid, estCached.valid);
				Assert::IsTrue(estTopLeft.origin == estCached.origin);
				Assert::AreEqual(estTopLeft.playerUpsampled, estCached.playerUpsampled);
				Assert::IsTrue(estTopLeft.siting == estCached.siting);
				Assert::IsTrue(std::fabs(estTopLeft.dipV - estCached.dipV) < 1e-12);
			}

			// Exact linear-algebra boundary check on Catmull-Rom (k = 1) over single-axis Yuv422:
			//   - On Left (centered V, phi = 0.0), negative-lobe step overshoot separates Catmull-Rom from [3,1]/4 -> Bicubic!
			//   - On TopLeft (cosited V, t = 0.5), Catmull-Rom and Lanczos2 both equal [-1/16, 9/16, 9/16, -1/16] bit-for-bit,
			//     so single-axis Yuv422 TopLeft must confirm Subsampled420 + TopLeft siting while honestly reporting KernelUnsure!
			{
				ChromaSampleInput crLeft422;
				crLeft422.floorCodes = kFloor10Bit;
				crLeft422.deliveredSampling = DeliveredChromaSampling::Yuv422;
				BuildClassicalSitedAxis(1, kN, kLines, kCatmullRom, 0.0, rng, crLeft422.lumaV, crLeft422.cbV, crLeft422.crV);
				const ChromaChainEstimate estCrLeft = EstimateChromaChain(crLeft422, on);
				Assert::IsTrue(estCrLeft.valid && estCrLeft.origin == ChromaOrigin::Subsampled420 && estCrLeft.playerUpsampled);
				Assert::IsTrue(estCrLeft.siting == ChromaGridSiting::Left);
				Assert::IsTrue(estCrLeft.reason == ChromaEstimatorReason::PlayerUpsampled420Confirmed);
				Assert::AreEqual(static_cast<int>(Family::Bicubic), estCrLeft.kernelFamily);

				ChromaSampleInput crTopLeft422;
				crTopLeft422.floorCodes = kFloor10Bit;
				crTopLeft422.deliveredSampling = DeliveredChromaSampling::Yuv422;
				BuildClassicalSitedAxis(1, kN, kLines, kCatmullRom, 0.25, rng, crTopLeft422.lumaV, crTopLeft422.cbV, crTopLeft422.crV);
				const ChromaChainEstimate estCrTl = EstimateChromaChain(crTopLeft422, on);
				Assert::IsTrue(estCrTl.valid && estCrTl.origin == ChromaOrigin::Subsampled420 && estCrTl.playerUpsampled);
				Assert::IsTrue(estCrTl.siting == ChromaGridSiting::TopLeft);
				Assert::IsTrue(estCrTl.reason == ChromaEstimatorReason::KernelUnsure);
			}

			// 2. CRITICAL NEGATIVE CONTROL: True Native 4:2:2 over HDMI Yuv422 transport!
			//    Genuine full-height vertical chroma detail MUST confirm Subsampled422, playerUpsampled=false,
			//    Native422Confirmed — NEVER misclassified as upsampled 4:2:0!
			int false420Count = 0;
			for (int scene = 0; scene < 4; ++scene)
			{
				ChromaSampleInput nat422;
				nat422.floorCodes = kFloor10Bit;
				nat422.deliveredSampling = DeliveredChromaSampling::Yuv422;
				nat422.lumaV = MakeLineSet(1, kN, kLines);
				nat422.cbV = MakeLineSet(1, kN, kLines);
				nat422.crV = MakeLineSet(1, kN, kLines);

				for (int li = 0; li < kLines; ++li)
				{
					std::vector<double> yv(static_cast<size_t>(kN), 512.0);
					std::vector<double> cbv(static_cast<size_t>(kN), 512.0);
					std::vector<double> crv(static_cast<size_t>(kN), 512.0);

					if (scene == 0)
					{
						// Broadband camera/graphics vertical chroma detail.
						yv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 100.0));
						cbv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 92.0));
						crv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 92.0));
					}
					else if (scene == 1)
					{
						// Sharp 1-px colored horizontal hairlines (vertical high-frequency spikes).
						for (int i = 0; i < kN; ++i)
						{
							const bool stroke = ((i + li * 5) % 11 == 0) || ((i + li * 3) % 13 == 2);
							yv[static_cast<size_t>(i)] = stroke ? 810.0 : (260.0 + 12.0 * rng.Normal01());
							cbv[static_cast<size_t>(i)] = stroke ? 770.0 : (512.0 + 24.0 * rng.Normal01());
							crv[static_cast<size_t>(i)] = stroke ? 250.0 : (512.0 + 24.0 * rng.Normal01());
						}
						yv = Quantize10Bit(yv);
						cbv = Quantize10Bit(cbv);
						crv = Quantize10Bit(crv);
					}
					else if (scene == 2)
					{
						// Saturated 3-row and 5-row vertical color stripes (non-2x periodic).
						for (int i = 0; i < kN; ++i)
						{
							const double s3 = ((i / 3 + li) & 1) ? 1.0 : -1.0;
							const double s5 = ((i / 5 + li * 2) & 1) ? 1.0 : -1.0;
							yv[static_cast<size_t>(i)] = 512.0 + 120.0 * s3 + 55.0 * s5;
							cbv[static_cast<size_t>(i)] = 512.0 + 135.0 * s5 + 35.0 * rng.Normal01();
							crv[static_cast<size_t>(i)] = 512.0 - 135.0 * s3 + 35.0 * rng.Normal01();
						}
						yv = Quantize10Bit(yv);
						cbv = Quantize10Bit(cbv);
						crv = Quantize10Bit(crv);
					}
					else
					{
						// Adversarial trap: 2:1 vertically upscaled luma paired with crisp NATIVE 4:2:2 vertical chroma!
						yv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(kN / 2, rng, 105.0), kN, kLanczos3, 0.0));
						cbv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
						crv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
					}

					std::copy(yv.begin(), yv.end(), nat422.lumaV.Line(li));
					std::copy(cbv.begin(), cbv.end(), nat422.cbV.Line(li));
					std::copy(crv.begin(), crv.end(), nat422.crV.Line(li));
				}

				// Mandatory same-input negative control on Scene 3: vertical luma has a deep 2:1 upscale dip (< -0.5),
				// proving any naive luma-driven heuristic would misclassify this 4:2:2 stream as upsampled 4:2:0!
				if (scene == 3)
				{
					const double lumaVDip = ComputeAxisDip2x(nat422.lumaV, kLanczos3);
					Assert::IsTrue(lumaVDip < -0.5, L"Scene 3 lumaV must exhibit strong 2:1 upscale dip hazard");
				}

				const ChromaChainEstimate estNat = EstimateChromaChain(nat422, EnabledBoxMpeg());
				if (estNat.origin == ChromaOrigin::Subsampled420 || estNat.playerUpsampled)
					++false420Count;

				Assert::IsTrue(estNat.valid, L"True native 4:2:2 must be valid");
				Assert::IsTrue(estNat.origin == ChromaOrigin::Subsampled422, L"True native 4:2:2 must report Subsampled422");
				Assert::IsFalse(estNat.playerUpsampled, L"True native 4:2:2 must report playerUpsampled=false");
				Assert::IsTrue(estNat.upsamplerFamily == ChromaUpsamplerFamily::Unknown);
				Assert::IsTrue(estNat.reason == ChromaEstimatorReason::Native422Confirmed);
				Assert::AreEqual(1, static_cast<int>(estNat.ToProductChromaChain().playerUpsampled));
			}
			Assert::AreEqual(0, false420Count);

			// 3. Flat / Achromatic 4:2:2 fail-closed negative control (even with strong vertical luma detail):
			ChromaSampleInput flat422;
			flat422.floorCodes = kFloor10Bit;
			flat422.deliveredSampling = DeliveredChromaSampling::Yuv422;
			flat422.lumaV = MakeLineSet(1, kN, kLines);
			flat422.cbV = MakeLineSet(1, kN, kLines);
			flat422.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> yv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 115.0));
				std::copy(yv.begin(), yv.end(), flat422.lumaV.Line(li));
				for (int i = 0; i < kN; ++i)
				{
					flat422.cbV.Line(li)[i] = 512.0;
					flat422.crV.Line(li)[i] = 512.0;
				}
			}
			const ChromaChainEstimate estFlat = EstimateChromaChain(flat422, on);
			Assert::IsFalse(estFlat.valid);
			Assert::IsTrue(estFlat.origin == ChromaOrigin::Unknown);
			Assert::IsFalse(estFlat.playerUpsampled);
			Assert::IsTrue(estFlat.reason == ChromaEstimatorReason::LowChromaDetail);

			// 4. Delivered Yuv420 direct transport: Subsampled420, playerUpsampled=false, Native420Delivered.
			ChromaSampleInput direct420;
			direct420.floorCodes = kFloor10Bit;
			direct420.deliveredSampling = DeliveredChromaSampling::Yuv420;
			direct420.cbH = MakeLineSet(0, kN / 2, kLines);
			direct420.cbV = MakeLineSet(1, kN / 2, kLines);
			direct420.crH = MakeLineSet(0, kN / 2, kLines);
			direct420.crV = MakeLineSet(1, kN / 2, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbh = Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 85.0));
				const std::vector<double> cbv = Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 85.0));
				const std::vector<double> crh = Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 85.0));
				const std::vector<double> crv = Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 85.0));
				std::copy(cbh.begin(), cbh.end(), direct420.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), direct420.cbV.Line(li));
				std::copy(crh.begin(), crh.end(), direct420.crH.Line(li));
				std::copy(crv.begin(), crv.end(), direct420.crV.Line(li));
			}
			const ChromaChainEstimate est420 = EstimateChromaChain(direct420, on);
			Assert::IsTrue(est420.valid);
			Assert::IsTrue(est420.origin == ChromaOrigin::Subsampled420);
			Assert::IsFalse(est420.playerUpsampled);
			Assert::IsTrue(est420.upsamplerFamily == ChromaUpsamplerFamily::Unknown);
			Assert::IsTrue(est420.reason == ChromaEstimatorReason::Native420Delivered);
			Assert::AreEqual(1, static_cast<int>(est420.ToProductChromaChain().playerUpsampled));
		}

		TEST_METHOD(Delivered422And420AcceptPhysicalFullResLumaLines)
		{
			// Adversarial physical-frame boundary test:
			// In a real W x H Yuv422 frame (YUY2/P210), luma is W x H while chroma is (W/2) x H (or cbH omitted).
			// In a real W x H Yuv420 frame (NV12/P010), luma is W x H while chroma is (W/2) x (H/2).
			// Passing full-resolution lumaH/lumaV alongside subsampled transport chroma must NOT fail with InvalidInput!
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings on = EnabledBase();

			constexpr int kW = 512;
			constexpr int kH = 512;
			constexpr int kLines = 12;
			SplitMixRng rng(2026112462ull);

			ChromaSampleInput phys422;
			phys422.floorCodes = kFloor10Bit;
			phys422.deliveredSampling = DeliveredChromaSampling::Yuv422;
			phys422.lumaH = MakeLineSet(0, kW, kLines);
			phys422.lumaV = MakeLineSet(1, kH, kLines);
			phys422.cbH = MakeLineSet(0, kW / 2, kLines);
			phys422.cbV = MakeLineSet(1, kH, kLines);
			phys422.crH = MakeLineSet(0, kW / 2, kLines);
			phys422.crV = MakeLineSet(1, kH, kLines);

			ChromaSampleInput phys420;
			phys420.floorCodes = kFloor10Bit;
			phys420.deliveredSampling = DeliveredChromaSampling::Yuv420;
			phys420.lumaH = MakeLineSet(0, kW, kLines);
			phys420.lumaV = MakeLineSet(1, kH, kLines);
			phys420.cbH = MakeLineSet(0, kW / 2, kLines);
			phys420.cbV = MakeLineSet(1, kH / 2, kLines);
			phys420.crH = MakeLineSet(0, kW / 2, kLines);
			phys420.crV = MakeLineSet(1, kH / 2, kLines);

			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> yh = Quantize10Bit(MakeBroadbandChroma(kW, rng, 95.0));
				const std::vector<double> yv = Quantize10Bit(MakeBroadbandChroma(kH, rng, 95.0));
				const std::vector<double> cbhHalf = Quantize10Bit(MakeBroadbandChroma(kW / 2, rng, 90.0));
				const std::vector<double> crhHalf = Quantize10Bit(MakeBroadbandChroma(kW / 2, rng, 90.0));
				const std::vector<double> cbvFull = Quantize10Bit(MakeBroadbandChroma(kH, rng, 90.0));
				const std::vector<double> crvFull = Quantize10Bit(MakeBroadbandChroma(kH, rng, 90.0));
				const std::vector<double> cbvHalf = Quantize10Bit(MakeBroadbandChroma(kH / 2, rng, 90.0));
				const std::vector<double> crvHalf = Quantize10Bit(MakeBroadbandChroma(kH / 2, rng, 90.0));

				std::copy(yh.begin(), yh.end(), phys422.lumaH.Line(li));
				std::copy(yv.begin(), yv.end(), phys422.lumaV.Line(li));
				std::copy(cbhHalf.begin(), cbhHalf.end(), phys422.cbH.Line(li));
				std::copy(crhHalf.begin(), crhHalf.end(), phys422.crH.Line(li));
				std::copy(cbvFull.begin(), cbvFull.end(), phys422.cbV.Line(li));
				std::copy(crvFull.begin(), crvFull.end(), phys422.crV.Line(li));

				std::copy(yh.begin(), yh.end(), phys420.lumaH.Line(li));
				std::copy(yv.begin(), yv.end(), phys420.lumaV.Line(li));
				std::copy(cbhHalf.begin(), cbhHalf.end(), phys420.cbH.Line(li));
				std::copy(crhHalf.begin(), crhHalf.end(), phys420.crH.Line(li));
				std::copy(cbvHalf.begin(), cbvHalf.end(), phys420.cbV.Line(li));
				std::copy(crvHalf.begin(), crvHalf.end(), phys420.crV.Line(li));
			}

			const ChromaChainEstimate estPhys422 = EstimateChromaChain(phys422, on);
			Assert::IsTrue(estPhys422.valid, L"Physical Yuv422 with W lumaH and W/2 cbH must succeed");
			Assert::IsTrue(estPhys422.origin == ChromaOrigin::Subsampled422);
			Assert::IsFalse(estPhys422.playerUpsampled);
			Assert::IsTrue(estPhys422.reason == ChromaEstimatorReason::Native422Confirmed);

			const ChromaChainEstimate estPhys420 = EstimateChromaChain(phys420, on);
			Assert::IsTrue(estPhys420.valid, L"Physical Yuv420 with WxH luma and (W/2)x(H/2) chroma must succeed");
			Assert::IsTrue(estPhys420.origin == ChromaOrigin::Subsampled420);
			Assert::IsFalse(estPhys420.playerUpsampled);
			Assert::IsTrue(estPhys420.reason == ChromaEstimatorReason::Native420Delivered);
		}

		TEST_METHOD(PointNearestDetectsNoisyLineDuplicationAndRejectsNegativeControls)
		{
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings sOn = EnabledBoxMpeg();
			const ChromaEstimatorSettings sOff = EnabledBase(); // detectBoxAndMpegChroma = false

			constexpr int kN = 512;
			constexpr int kLines = 16;
			constexpr double kSigmaNoise = 0.75; // Explicit 1-sigma post-upsample dither/sensor noise in 10-bit code units
			SplitMixRng rng(2026112463ull);

			auto addOneSigmaNoise = [&](const std::vector<double>& clean) -> std::vector<double> {
				std::vector<double> noisy(clean.size(), 512.0);
				for (size_t i = 0; i < clean.size(); ++i)
					noisy[i] = ClampD(clean[i] + kSigmaNoise * rng.Normal01(), 0.0, 1023.0);
				return noisy;
			};

			// 1. Line-duplicated 4:2:0 -> 4:2:2 over Yuv422 HDMI with 1-sigma independent pixel noise.
			ChromaSampleInput point422;
			point422.floorCodes = kSigmaNoise;
			point422.deliveredSampling = DeliveredChromaSampling::Yuv422;
			point422.cbV = MakeLineSet(1, kN, kLines);
			point422.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbDup = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				const std::vector<double> crDup = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				std::copy(cbDup.begin(), cbDup.end(), point422.cbV.Line(li));
				std::copy(crDup.begin(), crDup.end(), point422.crV.Line(li));
			}

			// In-test naive exact-duplicate negative control: adjacent duplicated samples differ by ~sqrt(2)*sigma > 0,
			// so a naive zero-difference check would miss every line!
			double sameRms422 = 0.0;
			double crossRms422 = 0.0;
			ComputeParityDiffRms(point422.cbV, sameRms422, crossRms422);
			Assert::IsTrue(sameRms422 > 0.9 * kSigmaNoise, L"Noisy duplicate pairs must have non-zero diff RMS");
			Assert::IsTrue(crossRms422 > 6.0 * sameRms422, L"Cross-pair steps must strongly exceed noisy same-pair diffs");

			const ChromaChainEstimate estPoint422 = EstimateChromaChain(point422, sOn);
			Assert::IsTrue(estPoint422.valid);
			Assert::IsTrue(estPoint422.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estPoint422.playerUpsampled);
			Assert::IsTrue(estPoint422.upsamplerFamily == ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estPoint422.reason == ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			// Mandatory same-input default-OFF negative control (detectBoxAndMpegChroma = false):
			const ChromaChainEstimate estPoint422Off = EstimateChromaChain(point422, sOff);
			Assert::IsTrue(estPoint422Off.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estPoint422Off.reason != ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			// 2. Pixel & line-duplicated 4:2:0 -> 4:4:4 over Yuv444 with 1-sigma noise (both origin=0 and odd crop origin=17).
			ChromaSampleInput point444Full;
			point444Full.floorCodes = kSigmaNoise;
			point444Full.deliveredSampling = DeliveredChromaSampling::Yuv444;
			point444Full.cbH = MakeLineSet(0, kN, kLines);
			point444Full.cbV = MakeLineSet(1, kN, kLines);
			point444Full.crH = MakeLineSet(0, kN, kLines);
			point444Full.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbh = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				const std::vector<double> cbv = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				const std::vector<double> crh = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				const std::vector<double> crv = addOneSigmaNoise(UpsamplePointNearest2x(Quantize10Bit(MakeBroadbandChroma(kN / 2, rng, 95.0))));
				std::copy(cbh.begin(), cbh.end(), point444Full.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), point444Full.cbV.Line(li));
				std::copy(crh.begin(), crh.end(), point444Full.crH.Line(li));
				std::copy(crv.begin(), crv.end(), point444Full.crV.Line(li));
			}

			const ChromaChainEstimate estPoint444 = EstimateChromaChain(point444Full, sOn);
			Assert::IsTrue(estPoint444.valid);
			Assert::IsTrue(estPoint444.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estPoint444.playerUpsampled);
			Assert::IsTrue(estPoint444.upsamplerFamily == ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estPoint444.reason == ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			const ChromaChainEstimate estPoint444Off = EstimateChromaChain(point444Full, sOff);
			Assert::IsTrue(estPoint444Off.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estPoint444Off.reason != ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			// Odd-origin crop (origin = 17, length = 480) flips duplicate pair parity from even->odd to odd->even.
			auto sliceOdd = [](const LineSet& src, int orig, int len) -> LineSet {
				LineSet sub = MakeLineSet(src.axis, src.axisLen, src.count, orig, len);
				for (int li = 0; li < src.count; ++li)
					std::copy(src.Line(li) + orig, src.Line(li) + orig + len, sub.Line(li));
				return sub;
			};
			ChromaSampleInput point444Odd;
			point444Odd.floorCodes = kSigmaNoise;
			point444Odd.cbH = sliceOdd(point444Full.cbH, 17, 480);
			point444Odd.cbV = sliceOdd(point444Full.cbV, 17, 480);
			point444Odd.crH = sliceOdd(point444Full.crH, 17, 480);
			point444Odd.crV = sliceOdd(point444Full.crV, 17, 480);

			const ChromaChainEstimate estPointOdd = EstimateChromaChain(point444Odd, sOn);
			Assert::IsTrue(estPointOdd.valid);
			Assert::IsTrue(estPointOdd.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estPointOdd.upsamplerFamily == ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estPointOdd.reason == ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			// 3. MANDATORY ADVERSARIAL NEGATIVE CONTROLS (detectBoxAndMpegChroma = true):
			//    (a) Smooth chroma gradient ramp + 1-sigma noise: same-pair diff IS <= sameGate (fooling naive same-pair-only
			//        detectors!), yet cross/same separation is ~1.0 so PointNearest MUST NOT fire!
			ChromaSampleInput rampIn;
			rampIn.floorCodes = kSigmaNoise;
			rampIn.cbH = MakeLineSet(0, kN, kLines);
			rampIn.cbV = MakeLineSet(1, kN, kLines);
			rampIn.crH = MakeLineSet(0, kN, kLines);
			rampIn.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				for (int i = 0; i < kN; ++i)
				{
					const double t = static_cast<double>(i) / static_cast<double>(kN - 1);
					rampIn.cbH.Line(li)[i] = 440.0 + 140.0 * t + kSigmaNoise * rng.Normal01();
					rampIn.cbV.Line(li)[i] = 440.0 + 140.0 * t + kSigmaNoise * rng.Normal01();
					rampIn.crH.Line(li)[i] = 580.0 - 140.0 * t + kSigmaNoise * rng.Normal01();
					rampIn.crV.Line(li)[i] = 580.0 - 140.0 * t + kSigmaNoise * rng.Normal01();
				}
			}
			double rampSame = 0.0;
			double rampCross = 0.0;
			ComputeParityDiffRms(rampIn.cbH, rampSame, rampCross);
			Assert::IsTrue(rampSame < 1.5 * std::sqrt(2.0) * kSigmaNoise, L"Ramp trap must pass naive sameGate");
			const ChromaChainEstimate estRamp = EstimateChromaChain(rampIn, sOn);
			Assert::IsTrue(estRamp.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estRamp.upsamplerFamily != ChromaUpsamplerFamily::MpegLinear);

			//    (b) Classical 2:1 Bilinear and 2:1 Lanczos3 upscales MUST NOT fire PointNearest!
			ChromaSampleInput bilIn;
			bilIn.floorCodes = kFloor10Bit;
			BuildClassicalSitedAxis(0, kN, kLines, 0, 0.25, rng, bilIn.lumaH, bilIn.cbH, bilIn.crH);
			BuildClassicalSitedAxis(1, kN, kLines, 0, 0.0, rng, bilIn.lumaV, bilIn.cbV, bilIn.crV);
			const ChromaChainEstimate estBil = EstimateChromaChain(bilIn, sOn);
			Assert::IsTrue(estBil.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);
			Assert::IsTrue(estBil.reason != ChromaEstimatorReason::PointNearestUpsampledConfirmed);

			ChromaSampleInput lanIn;
			lanIn.floorCodes = kFloor10Bit;
			BuildClassicalSitedAxis(0, kN, kLines, 5, 0.25, rng, lanIn.lumaH, lanIn.cbH, lanIn.crH);
			BuildClassicalSitedAxis(1, kN, kLines, 5, 0.0, rng, lanIn.lumaV, lanIn.cbV, lanIn.crV);
			const ChromaChainEstimate estLan = EstimateChromaChain(lanIn, sOn);
			Assert::IsTrue(estLan.valid);
			Assert::IsTrue(estLan.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estLan.upsamplerFamily == ChromaUpsamplerFamily::Lanczos);
			Assert::IsTrue(estLan.reason == ChromaEstimatorReason::PlayerUpsampled420Confirmed);

			//    (c) Native 4:4:4 broadband chroma MUST NOT fire PointNearest or MpegLinear!
			ChromaSampleInput nat444;
			nat444.floorCodes = kFloor10Bit;
			nat444.cbH = MakeLineSet(0, kN, kLines);
			nat444.cbV = MakeLineSet(1, kN, kLines);
			nat444.crH = MakeLineSet(0, kN, kLines);
			nat444.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbh = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
				const std::vector<double> cbv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
				const std::vector<double> crh = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
				const std::vector<double> crv = Quantize10Bit(MakeBroadbandChroma(kN, rng, 95.0));
				std::copy(cbh.begin(), cbh.end(), nat444.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), nat444.cbV.Line(li));
				std::copy(crh.begin(), crh.end(), nat444.crH.Line(li));
				std::copy(crv.begin(), crv.end(), nat444.crV.Line(li));
			}
			const ChromaChainEstimate estNat444 = EstimateChromaChain(nat444, sOn);
			Assert::IsTrue(estNat444.valid);
			Assert::IsTrue(estNat444.origin == ChromaOrigin::Native444);
			Assert::IsFalse(estNat444.playerUpsampled);
			Assert::IsTrue(estNat444.upsamplerFamily == ChromaUpsamplerFamily::Unknown);
			Assert::IsTrue(estNat444.reason == ChromaEstimatorReason::Native444Confirmed);
		}

		TEST_METHOD(MpegLinearFirDetectsCosited121AndCentered31WithAccurateSiting)
		{
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings sOn = EnabledBoxMpeg();
			const ChromaEstimatorSettings sOff = EnabledBase(); // detectBoxAndMpegChroma = false

			constexpr int kN = 512;
			constexpr int kLines = 16;
			SplitMixRng rng(2026112464ull);

			// 1. MPEG [1, 2, 1]/4 cosited H + [1, 2, 1]/4 cosited V on Yuv444 -> MpegLinear + TopLeft siting.
			ChromaSampleInput mpegTopLeft;
			mpegTopLeft.floorCodes = kFloor10Bit;
			BuildMpegFirSitedAxis(0, kN, kLines, true, rng, mpegTopLeft.lumaH, mpegTopLeft.cbH, mpegTopLeft.crH);
			BuildMpegFirSitedAxis(1, kN, kLines, true, rng, mpegTopLeft.lumaV, mpegTopLeft.cbV, mpegTopLeft.crV);

			const ChromaChainEstimate estTlOn = EstimateChromaChain(mpegTopLeft, sOn);
			Assert::IsTrue(estTlOn.valid);
			Assert::IsTrue(estTlOn.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estTlOn.playerUpsampled);
			Assert::IsTrue(estTlOn.upsamplerFamily == ChromaUpsamplerFamily::MpegLinear, L"Cosited [1,2,1]/4 must classify as MpegLinear");
			Assert::IsTrue(estTlOn.siting == ChromaGridSiting::TopLeft, L"Cosited [1,2,1]/4 H+V must report TopLeft siting");
			Assert::IsTrue(estTlOn.reason == ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			// Same-input default-OFF negative control:
			const ChromaChainEstimate estTlOff = EstimateChromaChain(mpegTopLeft, sOff);
			Assert::IsTrue(estTlOff.upsamplerFamily != ChromaUpsamplerFamily::MpegLinear);
			Assert::IsTrue(estTlOff.reason != ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			// 2. Standard MPEG-2 / H.264 / HEVC Left siting: [1, 2, 1]/4 cosited H + [3, 1]/4 centered V on Yuv444.
			ChromaSampleInput mpegLeft;
			mpegLeft.floorCodes = kFloor10Bit;
			BuildMpegFirSitedAxis(0, kN, kLines, true, rng, mpegLeft.lumaH, mpegLeft.cbH, mpegLeft.crH);
			BuildMpegFirSitedAxis(1, kN, kLines, false, rng, mpegLeft.lumaV, mpegLeft.cbV, mpegLeft.crV);

			const ChromaChainEstimate estLeftOn = EstimateChromaChain(mpegLeft, sOn);
			Assert::IsTrue(estLeftOn.valid);
			Assert::IsTrue(estLeftOn.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estLeftOn.playerUpsampled);
			Assert::IsTrue(estLeftOn.upsamplerFamily == ChromaUpsamplerFamily::MpegLinear, L"MPEG Left ([1,2,1]/4 H + [3,1]/4 V) must classify as MpegLinear");
			Assert::IsTrue(estLeftOn.siting == ChromaGridSiting::Left, L"MPEG Left ([1,2,1]/4 H + [3,1]/4 V) must report Left siting");
			Assert::IsTrue(estLeftOn.reason == ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			// 3. MPEG-1 / JPEG Center siting: [3, 1]/4 centered H + [3, 1]/4 centered V on Yuv444.
			ChromaSampleInput mpegCenter;
			mpegCenter.floorCodes = kFloor10Bit;
			BuildMpegFirSitedAxis(0, kN, kLines, false, rng, mpegCenter.lumaH, mpegCenter.cbH, mpegCenter.crH);
			BuildMpegFirSitedAxis(1, kN, kLines, false, rng, mpegCenter.lumaV, mpegCenter.cbV, mpegCenter.crV);

			const ChromaChainEstimate estCenterOn = EstimateChromaChain(mpegCenter, sOn);
			Assert::IsTrue(estCenterOn.valid);
			Assert::IsTrue(estCenterOn.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estCenterOn.playerUpsampled);
			Assert::IsTrue(estCenterOn.upsamplerFamily == ChromaUpsamplerFamily::MpegLinear, L"MPEG Center ([3,1]/4 H+V) must classify as MpegLinear");
			Assert::IsTrue(estCenterOn.siting == ChromaGridSiting::Center, L"MPEG Center ([3,1]/4 H+V) must report Center siting");
			Assert::IsTrue(estCenterOn.reason == ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			// Same-input default-OFF negative control on Center [3,1]/4 preserves Stage S5 KernelUnsure contract!
			const ChromaChainEstimate estCenterOff = EstimateChromaChain(mpegCenter, sOff);
			Assert::IsTrue(estCenterOff.upsamplerFamily != ChromaUpsamplerFamily::MpegLinear);
			Assert::IsTrue(estCenterOff.reason == ChromaEstimatorReason::KernelUnsure);

			// 4. Delivered Yuv422 HDMI with vertical MPEG [3, 1]/4 centered vs [1, 2, 1]/4 cosited.
			ChromaSampleInput mpeg422Left;
			mpeg422Left.floorCodes = kFloor10Bit;
			mpeg422Left.deliveredSampling = DeliveredChromaSampling::Yuv422;
			mpeg422Left.lumaV = mpegLeft.lumaV;
			mpeg422Left.cbV = mpegLeft.cbV;
			mpeg422Left.crV = mpegLeft.crV;
			const ChromaChainEstimate est422Left = EstimateChromaChain(mpeg422Left, sOn);
			Assert::IsTrue(est422Left.valid);
			Assert::IsTrue(est422Left.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(est422Left.playerUpsampled);
			Assert::IsTrue(est422Left.upsamplerFamily == ChromaUpsamplerFamily::MpegLinear, L"Yuv422 vertical [3,1]/4 must classify as MpegLinear");
			Assert::IsTrue(est422Left.siting == ChromaGridSiting::Left);
			Assert::IsTrue(est422Left.reason == ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			ChromaSampleInput mpeg422TopLeft;
			mpeg422TopLeft.floorCodes = kFloor10Bit;
			mpeg422TopLeft.deliveredSampling = DeliveredChromaSampling::Yuv422;
			mpeg422TopLeft.lumaV = mpegTopLeft.lumaV;
			mpeg422TopLeft.cbV = mpegTopLeft.cbV;
			mpeg422TopLeft.crV = mpegTopLeft.crV;
			const ChromaChainEstimate est422Tl = EstimateChromaChain(mpeg422TopLeft, sOn);
			Assert::IsTrue(est422Tl.valid);
			Assert::IsTrue(est422Tl.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(est422Tl.playerUpsampled);
			Assert::IsTrue(est422Tl.upsamplerFamily == ChromaUpsamplerFamily::MpegLinear);
			Assert::IsTrue(est422Tl.siting == ChromaGridSiting::TopLeft);
			Assert::IsTrue(est422Tl.reason == ChromaEstimatorReason::MpegLinearUpsampledConfirmed);

			// 5. Adversarial higher-order & multi-stage negative controls (MUST NOT misclassify as MpegLinear!):
			//    (a) Bicubic (k=1) & Lanczos3 (k=5) 2:1 upscales at Left & Center sitings
			ChromaSampleInput bicLeft;
			bicLeft.floorCodes = kFloor10Bit;
			BuildClassicalSitedAxis(0, kN, kLines, 1, 0.25, rng, bicLeft.lumaH, bicLeft.cbH, bicLeft.crH);
			BuildClassicalSitedAxis(1, kN, kLines, 1, 0.0, rng, bicLeft.lumaV, bicLeft.cbV, bicLeft.crV);
			const ChromaChainEstimate estBicLeft = EstimateChromaChain(bicLeft, sOn);
			Assert::IsTrue(estBicLeft.valid);
			Assert::IsTrue(estBicLeft.upsamplerFamily == ChromaUpsamplerFamily::Bicubic, L"Bicubic Left must NOT misclassify as MpegLinear");

			ChromaSampleInput bicCenter;
			bicCenter.floorCodes = kFloor10Bit;
			BuildClassicalSitedAxis(0, kN, kLines, 1, 0.0, rng, bicCenter.lumaH, bicCenter.cbH, bicCenter.crH);
			BuildClassicalSitedAxis(1, kN, kLines, 1, 0.0, rng, bicCenter.lumaV, bicCenter.cbV, bicCenter.crV);
			const ChromaChainEstimate estBicCenter = EstimateChromaChain(bicCenter, sOn);
			Assert::IsTrue(estBicCenter.valid);
			Assert::IsTrue(estBicCenter.upsamplerFamily != ChromaUpsamplerFamily::MpegLinear, L"Catmull-Rom Bicubic Center must NOT misclassify as MpegLinear");
			Assert::IsTrue(estBicCenter.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);

			//    (b) 4:1 Bilinear chroma upscale (1080p-in-4K 4:2:0) -> QuarterCorroborated, NOT MpegLinear or PointNearest!
			ChromaSampleInput bil4x;
			bil4x.floorCodes = kFloor10Bit;
			bil4x.cbH = MakeLineSet(0, kN, kLines);
			bil4x.cbV = MakeLineSet(1, kN, kLines);
			bil4x.crH = MakeLineSet(0, kN, kLines);
			bil4x.crV = MakeLineSet(1, kN, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbh = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(kN / 4, rng, 95.0), kN, 0, 0.0));
				const std::vector<double> cbv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(kN / 4, rng, 95.0), kN, 0, 0.0));
				const std::vector<double> crh = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(kN / 4, rng, 95.0), kN, 0, 0.0));
				const std::vector<double> crv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(kN / 4, rng, 95.0), kN, 0, 0.0));
				std::copy(cbh.begin(), cbh.end(), bil4x.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), bil4x.cbV.Line(li));
				std::copy(crh.begin(), crh.end(), bil4x.crH.Line(li));
				std::copy(crv.begin(), crv.end(), bil4x.crV.Line(li));
			}
			const ChromaChainEstimate estBil4x = EstimateChromaChain(bil4x, sOn);
			Assert::IsTrue(estBil4x.valid);
			Assert::IsTrue(estBil4x.reason == ChromaEstimatorReason::QuarterCorroborated);
			Assert::IsTrue(estBil4x.upsamplerFamily != ChromaUpsamplerFamily::MpegLinear);
			Assert::IsTrue(estBil4x.upsamplerFamily != ChromaUpsamplerFamily::PointNearest);
		}

		TEST_METHOD(LumaCoupledChromaGridCorroborates720pAnd1440pIn2160pAndRejectsNative)
		{
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings on = EnabledBoxMpeg();

			// Exact UHD 3840x2160 raster dimensions.
			constexpr int kW = 3840;
			constexpr int kH = 2160;
			constexpr int kLines = 10;
			constexpr int kLanczos3 = 5;
			SplitMixRng rng(2026112465ull);

			auto buildScaled420InUhd = [&](int lumaW, int lumaH, double sitingPhaseH, double sitingPhaseV) -> ChromaSampleInput {
				const int chromaW = lumaW / 2;
				const int chromaH = lumaH / 2;
				ChromaSampleInput in;
				in.floorCodes = kFloor10Bit;
				in.knownLumaNH = lumaW;
				in.knownLumaNV = lumaH;
				in.lumaH = MakeLineSet(0, kW, kLines);
				in.lumaV = MakeLineSet(1, kH, kLines);
				in.cbH = MakeLineSet(0, kW, kLines);
				in.cbV = MakeLineSet(1, kH, kLines);
				in.crH = MakeLineSet(0, kW, kLines);
				in.crV = MakeLineSet(1, kH, kLines);

				for (int li = 0; li < kLines; ++li)
				{
					const std::vector<double> yh = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(lumaW, rng, 100.0), kW, kLanczos3, 0.0));
					const std::vector<double> yv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(lumaH, rng, 100.0), kH, kLanczos3, 0.0));
					const std::vector<double> cbh = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(chromaW, rng, 95.0), kW, kLanczos3, sitingPhaseH));
					const std::vector<double> cbv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(chromaH, rng, 95.0), kH, kLanczos3, sitingPhaseV));
					const std::vector<double> crh = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(chromaW, rng, 95.0), kW, kLanczos3, sitingPhaseH));
					const std::vector<double> crv = Quantize10Bit(UpscaleClassical1D(MakeBroadbandChroma(chromaH, rng, 95.0), kH, kLanczos3, sitingPhaseV));

					std::copy(yh.begin(), yh.end(), in.lumaH.Line(li));
					std::copy(yv.begin(), yv.end(), in.lumaV.Line(li));
					std::copy(cbh.begin(), cbh.end(), in.cbH.Line(li));
					std::copy(cbv.begin(), cbv.end(), in.cbV.Line(li));
					std::copy(crh.begin(), crh.end(), in.crH.Line(li));
					std::copy(crv.begin(), crv.end(), in.crV.Line(li));
				}
				return in;
			};

			// 1. 720p -> 2160p (1280x720 luma -> 640x360 4:2:0 chroma upsampled 6x to 3840x2160, Left siting +0.25 H / 0.0 V).
			ChromaSampleInput in720 = buildScaled420InUhd(1280, 720, 0.25, 0.0);
			const ChromaChainEstimate est720 = EstimateChromaChain(in720, on);
			Assert::IsTrue(est720.valid);
			Assert::IsTrue(est720.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(est720.playerUpsampled);
			Assert::IsTrue(est720.reason == ChromaEstimatorReason::LumaCoupledChromaCorroborated);
			Assert::IsTrue(est720.lumaCoupled.valid);
			Assert::IsTrue(est720.lumaCoupled.corroborated);
			Assert::AreEqual(640, est720.lumaCoupled.expectedChromaNH);
			Assert::AreEqual(360, est720.lumaCoupled.expectedChromaNV);
			Assert::IsTrue(est720.lumaCoupled.dipH <= -0.20);
			Assert::IsTrue(est720.lumaCoupled.dipV <= -0.20);
			Assert::IsTrue(est720.lumaCoupled.reason == ChromaEstimatorReason::LumaCoupledChromaCorroborated);

			// Mandatory same-input uncoupled negative control (knownLumaNH = 0, knownLumaNV = 0 on identical 720p-in-2160p lines):
			ChromaSampleInput in720Uncoupled = in720;
			in720Uncoupled.knownLumaNH = 0;
			in720Uncoupled.knownLumaNV = 0;
			const ChromaChainEstimate est720Unc = EstimateChromaChain(in720Uncoupled, on);
			Assert::IsFalse(est720Unc.lumaCoupled.corroborated);
			Assert::IsTrue(est720Unc.lumaCoupled.reason == ChromaEstimatorReason::Disabled);
			Assert::IsFalse(est720Unc.quarter.supportsTwoToOneLumaUpscale);
			Assert::IsTrue(est720Unc.reason != ChromaEstimatorReason::LumaCoupledChromaCorroborated);

			// 2. 1440p -> 2160p (2560x1440 luma -> 1280x720 4:2:0 chroma upsampled 3x to 3840x2160, Left siting +0.25 H / 0.0 V).
			ChromaSampleInput in1440 = buildScaled420InUhd(2560, 1440, 0.25, 0.0);
			const ChromaChainEstimate est1440 = EstimateChromaChain(in1440, on);
			Assert::IsTrue(est1440.valid);
			Assert::IsTrue(est1440.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(est1440.playerUpsampled);
			Assert::IsTrue(est1440.reason == ChromaEstimatorReason::LumaCoupledChromaCorroborated);
			Assert::IsTrue(est1440.lumaCoupled.valid);
			Assert::IsTrue(est1440.lumaCoupled.corroborated);
			Assert::AreEqual(1280, est1440.lumaCoupled.expectedChromaNH);
			Assert::AreEqual(720, est1440.lumaCoupled.expectedChromaNV);
			Assert::IsTrue(est1440.lumaCoupled.dipH <= -0.20);
			Assert::IsTrue(est1440.lumaCoupled.dipV <= -0.20);
			Assert::IsTrue(est1440.lumaCoupled.reason == ChromaEstimatorReason::LumaCoupledChromaCorroborated);

			// 3. CRITICAL NEGATIVE CONTROL: Native 4K 4:4:4 broadband chroma passed WITH knownLumaN = 1280x720!
			//    Must reject luma-coupled corroboration (corroborated == false, LumaCoupledChromaNotSupported)
			//    and confirm Native444!
			ChromaSampleInput nat4K444;
			nat4K444.floorCodes = kFloor10Bit;
			nat4K444.knownLumaNH = 1280;
			nat4K444.knownLumaNV = 720;
			nat4K444.lumaH = in720.lumaH; // Even with genuine 720p-upscaled lumaH/V!
			nat4K444.lumaV = in720.lumaV;
			nat4K444.cbH = MakeLineSet(0, kW, kLines);
			nat4K444.cbV = MakeLineSet(1, kH, kLines);
			nat4K444.crH = MakeLineSet(0, kW, kLines);
			nat4K444.crV = MakeLineSet(1, kH, kLines);
			for (int li = 0; li < kLines; ++li)
			{
				const std::vector<double> cbh = Quantize10Bit(MakeBroadbandChroma(kW, rng, 95.0));
				const std::vector<double> cbv = Quantize10Bit(MakeBroadbandChroma(kH, rng, 95.0));
				const std::vector<double> crh = Quantize10Bit(MakeBroadbandChroma(kW, rng, 95.0));
				const std::vector<double> crv = Quantize10Bit(MakeBroadbandChroma(kH, rng, 95.0));
				std::copy(cbh.begin(), cbh.end(), nat4K444.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), nat4K444.cbV.Line(li));
				std::copy(crh.begin(), crh.end(), nat4K444.crH.Line(li));
				std::copy(crv.begin(), crv.end(), nat4K444.crV.Line(li));
			}

			const ChromaChainEstimate estNat444 = EstimateChromaChain(nat4K444, on);
			Assert::IsFalse(estNat444.lumaCoupled.corroborated);
			Assert::IsTrue(estNat444.lumaCoupled.reason == ChromaEstimatorReason::LumaCoupledChromaNotSupported);
			Assert::IsTrue(estNat444.valid);
			Assert::IsTrue(estNat444.origin == ChromaOrigin::Native444);
			Assert::IsFalse(estNat444.playerUpsampled);
			Assert::IsTrue(estNat444.reason == ChromaEstimatorReason::Native444Confirmed);

			// 4. Native 4K 4:2:0 (1920x1080 chroma upsampled 2x to 3840x2160) passed WITH wrong knownLumaN = 1280x720:
			//    Must reject 640x360 corroboration while still confirming 2:1 player-upsampled 4:2:0!
			ChromaSampleInput nat4K420WrongHint = buildScaled420InUhd(3840, 2160, 0.25, 0.0);
			nat4K420WrongHint.knownLumaNH = 1280;
			nat4K420WrongHint.knownLumaNV = 720;
			const ChromaChainEstimate estWrongHint = EstimateChromaChain(nat4K420WrongHint, on);
			Assert::IsFalse(estWrongHint.lumaCoupled.corroborated);
			Assert::IsTrue(estWrongHint.lumaCoupled.reason == ChromaEstimatorReason::LumaCoupledChromaNotSupported);
			Assert::IsTrue(estWrongHint.valid);
			Assert::IsTrue(estWrongHint.origin == ChromaOrigin::Subsampled420);
			Assert::IsTrue(estWrongHint.playerUpsampled);
		}

		TEST_METHOD(InputValidationAndEnumTextGuardsFailClosedVisibly)
		{
			using namespace Sota6ChromaFx;
			const ChromaEstimatorSettings on = EnabledBoxMpeg();

			// Enum string tables must cover all SID-SOTA-6 additions with non-empty text.
			Assert::IsTrue(std::strlen(DeliveredChromaSamplingName(DeliveredChromaSampling::Yuv444)) > 0u);
			Assert::IsTrue(std::strlen(DeliveredChromaSamplingName(DeliveredChromaSampling::Yuv422)) > 0u);
			Assert::IsTrue(std::strlen(DeliveredChromaSamplingName(DeliveredChromaSampling::Yuv420)) > 0u);
			Assert::IsTrue(std::strlen(ChromaUpsamplerFamilyName(ChromaUpsamplerFamily::PointNearest)) > 0u);
			Assert::IsTrue(std::strlen(ChromaUpsamplerFamilyName(ChromaUpsamplerFamily::MpegLinear)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::Native422Confirmed)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::Native420Delivered)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::PointNearestUpsampledConfirmed)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::MpegLinearUpsampledConfirmed)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::LumaCoupledChromaCorroborated)) > 0u);
			Assert::IsTrue(std::strlen(ChromaEstimatorReasonText(ChromaEstimatorReason::LumaCoupledChromaNotSupported)) > 0u);

			// Missing vertical chroma on Yuv422 must fail closed with InvalidInput.
			ChromaSampleInput missingV422;
			missingV422.floorCodes = kFloor10Bit;
			missingV422.deliveredSampling = DeliveredChromaSampling::Yuv422;
			missingV422.cbH = MakeLineSet(0, 256, 8);
			const ChromaChainEstimate estMissingV = EstimateChromaChain(missingV422, on);
			Assert::IsFalse(estMissingV.valid);
			Assert::IsTrue(estMissingV.reason == ChromaEstimatorReason::InvalidInput);

			// Non-finite NaN sample on Yuv422 cbV must fail closed with InvalidInput.
			ChromaSampleInput nan422;
			nan422.floorCodes = kFloor10Bit;
			nan422.deliveredSampling = DeliveredChromaSampling::Yuv422;
			nan422.cbV = MakeLineSet(1, 256, 8);
			nan422.cbV.Line(3)[42] = std::numeric_limits<double>::quiet_NaN();
			const ChromaChainEstimate estNan = EstimateChromaChain(nan422, on);
			Assert::IsFalse(estNan.valid);
			Assert::IsTrue(estNan.reason == ChromaEstimatorReason::InvalidInput);

			// Out-of-range DeliveredChromaSampling enum value must fail closed with InvalidInput.
			ChromaSampleInput badEnum;
			badEnum.floorCodes = kFloor10Bit;
			badEnum.deliveredSampling = static_cast<DeliveredChromaSampling>(77);
			badEnum.cbH = MakeLineSet(0, 256, 8);
			badEnum.cbV = MakeLineSet(1, 256, 8);
			const ChromaChainEstimate estBadEnum = EstimateChromaChain(badEnum, on);
			Assert::IsFalse(estBadEnum.valid);
			Assert::IsTrue(estBadEnum.reason == ChromaEstimatorReason::InvalidInput);

			// Out-of-range knownLumaNH/NV (too small < 32 -> target < 16, or >= 2*N -> target >= N) fails closed on lumaCoupled.
			SplitMixRng rng(2026112466ull);
			ChromaSampleInput badHint;
			badHint.floorCodes = kFloor10Bit;
			badHint.knownLumaNH = 16;   // targetH = 8 < 16
			badHint.knownLumaNV = 1024; // targetV = 512 >= N=512
			badHint.cbH = MakeLineSet(0, 512, 8);
			badHint.cbV = MakeLineSet(1, 512, 8);
			for (int li = 0; li < 8; ++li)
			{
				const std::vector<double> cbh = Quantize10Bit(MakeBroadbandChroma(512, rng, 90.0));
				const std::vector<double> cbv = Quantize10Bit(MakeBroadbandChroma(512, rng, 90.0));
				std::copy(cbh.begin(), cbh.end(), badHint.cbH.Line(li));
				std::copy(cbv.begin(), cbv.end(), badHint.cbV.Line(li));
			}
			const ChromaChainEstimate estBadHint = EstimateChromaChain(badHint, on);
			Assert::IsFalse(estBadHint.lumaCoupled.corroborated);
			Assert::IsTrue(estBadHint.lumaCoupled.reason == ChromaEstimatorReason::LumaCoupledChromaNotSupported);
		}
	};
}
