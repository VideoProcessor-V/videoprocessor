// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S4 Adversarial Tests (2026-11-23_TD-consolidated-plan.md s1.5, s1.6, s3.2 TD-S4, s4.3 Q5):
// Independent, non-oracle, negative-control-backed CPU verification (Rule 3 & Rule 8 Red Team):
//   1. CodedGridIdentityOn420And422And444AndRgbPreservesBtbWtw
//   2. SitingType2VsLeftZeroColorFringingWithNegativeControl
//   3. Anchor444CanvasSubpixelAccumulationBeatsSingleSubsampledWarp
//   4. CarriedCflH5LumaOnlyResidualZeroLeakAndDisagreementFallback
//   5. UnidentifiedCodedGridRunsVisibleCoarseBandOnly
//   6. ChromaGrainPreservedUntouchedAcrossDeliveredFormats
//   7. Q5HalfPixelPanProjectionEliminatesCrossFormBiasAndPreservesFlatGrain
//   8. Q5CrushedBlackZeroEnergyExclusionPreservesBtbWtwGrain
//   9. AuditLowClosuresInvalidSourceBitsNonFiniteAndCodecAligned8x8DctRect
//
// 100% deterministic CPU, zero silent skips, zero untaken-branch vacuities, mandatory in-test negative controls.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/core/ChromaSiting.h>
#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Chroma.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2FrameRole.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2GrainQ5.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdV2ChromaNs
	{
		using namespace TdV2;

		constexpr double kPi = 3.14159265358979323846;

		static void AssertPlaneAllFinite(const TdV2Plane& p, const wchar_t* msg)
		{
			for (double v : p.v)
				Assert::IsTrue(std::isfinite(v), msg);
		}

		static void AssertPlanesBitExact(const TdV2Plane& expected, const TdV2Plane& actual, const wchar_t* msg)
		{
			Assert::AreEqual(expected.w, actual.w, msg);
			Assert::AreEqual(expected.h, actual.h, msg);
			Assert::AreEqual(expected.v.size(), actual.v.size(), msg);
			for (size_t i = 0; i < expected.v.size(); ++i)
				Assert::AreEqual(expected.v[i], actual.v[i], 0.0, msg);
		}

		static void AssertBarsBitExact(const TdV2Plane& expected, const TdV2Plane& actual, const ActiveRect& active, const wchar_t* msg)
		{
			Assert::AreEqual(expected.w, actual.w, msg);
			Assert::AreEqual(expected.h, actual.h, msg);
			for (int y = 0; y < expected.h; ++y)
			{
				for (int x = 0; x < expected.w; ++x)
				{
					if (!active.Contains(x, y))
						Assert::AreEqual(expected.at(x, y), actual.at(x, y), 0.0, msg);
				}
			}
		}

		// Exact algebraic inverse of YCbCrMatrixSpec::RgbToYCbCr (zero clipping; uses U26 LumaCoeff via matrix).
		static void YCbCrToRgbUnclipped(
			const YCbCrMatrixSpec& m,
			double y, double cb, double cr, double midCode,
			double& r, double& g, double& b)
		{
			const double cb0 = cb - midCode;
			const double cr0 = cr - midCode;
			const double kR = 2.0 * (1.0 - m.kr);
			const double kB = 2.0 * (1.0 - m.kb);
			r = y + kR * cr0;
			b = y + kB * cb0;
			g = y - (m.kr * kR / m.kg) * cr0 - (m.kb * kB / m.kg) * cb0;
		}

		static void YCbCrPlanesToRgb(
			const YCbCrMatrixSpec& m,
			const TdV2Plane& yP, const TdV2Plane& cbP, const TdV2Plane& crP,
			double midCode,
			TdV2Plane& outR, TdV2Plane& outG, TdV2Plane& outB)
		{
			outR = TdV2Plane(yP.w, yP.h, 0.0);
			outG = TdV2Plane(yP.w, yP.h, 0.0);
			outB = TdV2Plane(yP.w, yP.h, 0.0);
			for (int y = 0; y < yP.h; ++y)
			{
				for (int x = 0; x < yP.w; ++x)
				{
					YCbCrToRgbUnclipped(m, yP.at(x, y), cbP.at(x, y), crP.at(x, y), midCode,
						outR.at(x, y), outG.at(x, y), outB.at(x, y));
				}
			}
		}

		static void RgbPlanesToYCbCr(
			const YCbCrMatrixSpec& m,
			const TdV2Plane& rP, const TdV2Plane& gP, const TdV2Plane& bP,
			double midCode,
			TdV2Plane& outY, TdV2Plane& outCb, TdV2Plane& outCr)
		{
			outY = TdV2Plane(rP.w, rP.h, 0.0);
			outCb = TdV2Plane(rP.w, rP.h, 0.0);
			outCr = TdV2Plane(rP.w, rP.h, 0.0);
			for (int y = 0; y < rP.h; ++y)
			{
				for (int x = 0; x < rP.w; ++x)
				{
					m.RgbToYCbCr(rP.at(x, y), gP.at(x, y), bP.at(x, y), midCode,
						outY.at(x, y), outCb.at(x, y), outCr.at(x, y));
				}
			}
		}

		// Continuous analytic chroma edge scene evaluated at any floating-point luma coordinate (lx, ly).
		static double AnalyticEdgeCb(double lx, double ly, double edgeX, double edgeY, double widthPx)
		{
			const double stepX = 110.0 * std::tanh((lx - edgeX) / widthPx);
			const double stepY = 85.0 * std::tanh((ly - edgeY) / widthPx);
			return 512.0 + 0.5 * stepX + 0.5 * stepY;
		}

		static double AnalyticEdgeCr(double lx, double ly, double edgeX, double edgeY, double widthPx)
		{
			const double stepX = -95.0 * std::tanh((lx - edgeX) / widthPx);
			const double stepY = 105.0 * std::tanh((ly - edgeY) / widthPx);
			return 512.0 + 0.5 * stepX + 0.5 * stepY;
		}

		static TdV2Plane SampleAnalyticChromaPlane(
			int lumaW, int lumaH, int subX, int subY, const SitingSpec& siting,
			double ox, double oy, double edgeX, double edgeY, double widthPx, bool isCb)
		{
			const int pw = ChromaDetail::SubsampledDim(lumaW, subX);
			const int ph = ChromaDetail::SubsampledDim(lumaH, subY);
			TdV2Plane out(pw, ph, 512.0);
			for (int cy = 0; cy < ph; ++cy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(cy), subY) + oy;
				for (int cx = 0; cx < pw; ++cx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(cx), subX) + ox;
					out.at(cx, cy) = isCb
						? AnalyticEdgeCb(lx, ly, edgeX, edgeY, widthPx)
						: AnalyticEdgeCr(lx, ly, edgeX, edgeY, widthPx);
				}
			}
			return out;
		}

		// Upsample a coded chroma plane onto a higher-resolution delivered grid honoring SitingSpec and ActiveRect.
		static TdV2Plane UpsampleChromaPlaneSited(
			const TdV2Plane& coded,
			int lumaW, int lumaH,
			ChromaSubsampling codedSub,
			DeliveredChromaFormat deliveredFmt,
			const SitingSpec& siting,
			const ActiveRect& lumaActive)
		{
			const int cSubX = SubsampleX(codedSub);
			const int cSubY = SubsampleY(codedSub);
			const int dSubX = DeliveredSubsampleX(deliveredFmt);
			const int dSubY = DeliveredSubsampleY(deliveredFmt);
			const int dW = ChromaDetail::SubsampledDim(lumaW, dSubX);
			const int dH = ChromaDetail::SubsampledDim(lumaH, dSubY);
			if (codedSub == DeliveredSubsampling(deliveredFmt) && coded.w == dW && coded.h == dH)
				return coded;

			TdV2Plane out(dW, dH, 512.0);
			const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, cSubX, cSubY, coded.w, coded.h);
			const ActiveRect dActive = ChromaDetail::ToSubsampledActiveRect(lumaActive, lumaW, lumaH, dSubX, dSubY, dW, dH);
			ActiveSubPlaneView cView{ &coded, cActive.x0, cActive.y0, cActive.Width(), cActive.Height() };

			for (int dy = dActive.y0; dy < dActive.y1; ++dy)
			{
				const double ly = siting.CodedToLumaY(static_cast<double>(dy), dSubY);
				const double cyLocal = siting.LumaToCodedY(ly, cSubY) - static_cast<double>(cActive.y0);
				for (int dx = dActive.x0; dx < dActive.x1; ++dx)
				{
					const double lx = siting.CodedToLumaX(static_cast<double>(dx), dSubX);
					const double cxLocal = siting.LumaToCodedX(lx, cSubX) - static_cast<double>(cActive.x0);
					double val = 512.0;
					bool border = false;
					if (ChromaDetail::SampleSitedActive(cView, cxLocal, cyLocal, val, border, /*allowEdgeClampOutside=*/true))
						out.at(dx, dy) = val;
				}
			}
			return out;
		}

		static TdV2Plane AddGaussianNoise(const TdV2Plane& src, double sigma, uint32_t seed, const ActiveRect& active)
		{
			TdV2Plane out = src;
			std::mt19937 rng(seed);
			std::normal_distribution<double> nd(0.0, sigma);
			for (int y = active.y0; y < active.y1; ++y)
				for (int x = active.x0; x < active.x1; ++x)
					out.at(x, y) += nd(rng);
			return out;
		}

		static double MaskedRmse(
			const TdV2Plane& ref,
			const TdV2Plane& cand,
			const ActiveRect& active,
			const std::vector<uint8_t>* mask = nullptr)
		{
			double sum = 0.0;
			size_t n = 0;
			for (int y = active.y0; y < active.y1; ++y)
			{
				for (int x = active.x0; x < active.x1; ++x)
				{
					if (mask != nullptr && (*mask)[static_cast<size_t>(y) * ref.w + x] == 0u)
						continue;
					const double d = cand.at(x, y) - ref.at(x, y);
					sum += d * d;
					++n;
				}
			}
			return n > 0 ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
		}

		TEST_CLASS(TdV2ChromaTests)
		{
		public:
			// =========================================================================
			// 1. Coded-Grid Identity on 4:2:0, 4:2:2, 4:4:4 & RGB + BTB/WTW Preservation
			// =========================================================================
			TEST_METHOD(CodedGridIdentityOn420And422And444AndRgbPreservesBtbWtw)
			{
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active{ 8, 8, 88, 88 };
				const ActiveRect codedActive420 = ChromaDetail::ToSubsampledActiveRect(active, w, h, 2, 2, w / 2, h / 2);
				const ActiveRect delivActive422 = ChromaDetail::ToSubsampledActiveRect(active, w, h, 2, 1, w / 2, h);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const YCbCrMatrixSpec mat = YCbCrMatrixSpec::Bt709();
				const SitingSpec siting = SitingSpec::Left();
				const double midCode = 512.0;

				// Build independent 4:4:4 master with:
				//   - BTB region (y in [8, 24), Y = 28 < 64)
				//   - WTW region (y in [72, 88), Y = 988 > 940)
				//   - Mid-tone W4 chroma-edge region (y in [24, 72), high-contrast color edge at x = 48, y = 48)
				//   - Distinct bar values outside ActiveRect (top/left = 14, bottom/right = 1012)
				TdV2Plane masterY(w, h, 450.0);
				TdV2Plane masterCb444 = SampleAnalyticChromaPlane(w, h, 1, 1, siting, 0.0, 0.0, 48.0, 48.0, 2.5, true);
				TdV2Plane masterCr444 = SampleAnalyticChromaPlane(w, h, 1, 1, siting, 0.0, 0.0, 48.0, 48.0, 2.5, false);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						if (!active.Contains(x, y))
						{
							const double barVal = (y < active.y0 || x < active.x0) ? 14.0 : 1012.0;
							masterY.at(x, y) = barVal;
							masterCb444.at(x, y) = (y < active.y0) ? 48.0 : 968.0; // BTB/WTW bar sentinels
							masterCr444.at(x, y) = (x < active.x0) ? 52.0 : 972.0;
						}
						else if (y < 24)
						{
							masterY.at(x, y) = 28.0; // genuine BTB (< 64)
						}
						else if (y >= 72)
						{
							masterY.at(x, y) = 988.0; // genuine WTW (> 940)
						}
						else
						{
							masterY.at(x, y) = 480.0 + 30.0 * std::sin(2.0 * kPi * x / 32.0);
						}
					}
				}

				// ---- Part A: Bit-exact passthrough (Off, Shadow, zero-delta On) across 4:2:0, 4:2:2, 4:4:4, RGB ----
				const DeliveredChromaFormat fmts[4] = {
					DeliveredChromaFormat::Yuv420,
					DeliveredChromaFormat::Yuv422,
					DeliveredChromaFormat::Yuv444,
					DeliveredChromaFormat::Rgb444
				};
				const ChromaSubsampling subs[4] = {
					ChromaSubsampling::Yuv420,
					ChromaSubsampling::Yuv422,
					ChromaSubsampling::Yuv444,
					ChromaSubsampling::Yuv444
				};

				for (int fi = 0; fi < 4; ++fi)
				{
					const DeliveredChromaFormat fmt = fmts[fi];
					const ChromaSubsampling sub = subs[fi];
					const int sx = SubsampleX(sub), sy = SubsampleY(sub);
					const int pw = w / sx, ph = h / sy;
					const ActiveRect pActive = ChromaDetail::ToSubsampledActiveRect(active, w, h, sx, sy, pw, ph);

					TdV2Plane cbPlane = SampleAnalyticChromaPlane(w, h, sx, sy, siting, 0.0, 0.0, 48.0, 48.0, 2.5, true);
					TdV2Plane crPlane = SampleAnalyticChromaPlane(w, h, sx, sy, siting, 0.0, 0.0, 48.0, 48.0, 2.5, false);
					// Plant BTB (< 64) and WTW (> 940) directly in active chroma & bar sentinels outside active.
					for (int y = 0; y < ph; ++y)
					{
						for (int x = 0; x < pw; ++x)
						{
							if (!pActive.Contains(x, y))
							{
								cbPlane.at(x, y) = (y < pActive.y0) ? 22.0 : 996.0;
								crPlane.at(x, y) = (x < pActive.x0) ? 31.0 : 987.0;
							}
							else if (y == pActive.y0)
							{
								cbPlane.at(x, y) = 42.0;  // BTB chroma
								crPlane.at(x, y) = 966.0; // WTW chroma
							}
						}
					}

					TdV2Plane diffRefCb = AddGaussianNoise(cbPlane, 12.0, 100 + fi, pActive);
					TdV2Plane diffRefCr = AddGaussianNoise(crPlane, 12.0, 200 + fi, pActive);

					// Verify ReduceToCodedGrid & UpsampleDeltaToDelivered exact identity when coded == delivered.
					if (fmt != DeliveredChromaFormat::Rgb444)
					{
						const TdV2Plane red = ReduceToCodedGrid(cbPlane, w, h, fmt, sub, siting, active);
						AssertPlanesBitExact(cbPlane, red, L"ReduceToCodedGrid must be bit-exact identity when coded == delivered");
						const TdV2Plane up = UpsampleDeltaToDelivered(cbPlane, w, h, sub, fmt, siting, active);
						AssertPlanesBitExact(cbPlane, up, L"UpsampleDeltaToDelivered must be bit-exact identity when coded == delivered");
					}

					auto buildInput = [&](bool zeroDelta)
					{
						ChromaProcessInput in;
						in.seq = static_cast<uint64_t>(fi + 1);
						in.deliveredFormat = fmt;
						in.codedSubsampling = sub;
						in.gridStatus = ChromaGridStatus::Identified;
						in.siting = siting;
						in.matrix = mat;
						in.lumaW = w;
						in.lumaH = h;
						in.activeRect = active;
						in.hasCleanSplit = true;
						in.cleanCb = cbPlane;
						in.cleanCr = crPlane;
						in.hasStepPrior = true;
						in.estimatedStepDelta = 16.0;
						if (fmt == DeliveredChromaFormat::Rgb444)
						{
							YCbCrPlanesToRgb(mat, masterY, cbPlane, crPlane, midCode, in.receivedR, in.receivedG, in.receivedB);
						}
						else
						{
							in.receivedCb = cbPlane;
							in.receivedCr = crPlane;
						}
						if (zeroDelta)
						{
							in.prevPeak = { &cbPlane, &crPlane, &stillMf, true };
							in.nextPeak = { &cbPlane, &crPlane, &stillMf, true };
						}
						else
						{
							in.prevPeak = { &diffRefCb, &diffRefCr, &stillMf, false };
							in.nextPeak = { &diffRefCb, &diffRefCr, &stillMf, false };
						}
						return in;
					};

					// Mode::Off -> bit-exact passthrough, zero stand-ins engaged.
					{
						Config cfg;
						Assert::IsTrue(cfg.mode == Mode::Off);
						TdV2ChromaEngine eng(cfg);
						StandInRegistry reg;
						const ChromaProcessInput in = buildInput(false);
						const auto res = eng.ProcessFrame(in, &reg);
						Assert::IsFalse(reg.AnyActive(), L"Mode::Off must perform zero work");
						if (fmt == DeliveredChromaFormat::Rgb444)
						{
							AssertPlanesBitExact(in.receivedR, res.outputR, L"RGB Mode::Off must be bit-exact");
							AssertPlanesBitExact(in.receivedG, res.outputG, L"RGB Mode::Off must be bit-exact");
							AssertPlanesBitExact(in.receivedB, res.outputB, L"RGB Mode::Off must be bit-exact");
						}
						else
						{
							AssertPlanesBitExact(in.receivedCb, res.outputCb, L"YUV Mode::Off Cb must be bit-exact");
							AssertPlanesBitExact(in.receivedCr, res.outputCr, L"YUV Mode::Off Cr must be bit-exact");
						}
					}

					// Mode::Shadow -> computes deltas with active refs, yet output stays bit-exact!
					{
						Config cfg = MakePreset(Preset::Reference, Mode::Shadow);
						TdV2ChromaEngine eng(cfg);
						StandInRegistry reg;
						const ChromaProcessInput in = buildInput(false);
						const auto res = eng.ProcessFrame(in, &reg);
						Assert::AreEqual(static_cast<size_t>(2), res.activeRefCount);
						if (fmt == DeliveredChromaFormat::Rgb444)
						{
							AssertPlanesBitExact(in.receivedR, res.outputR, L"RGB Mode::Shadow must be bit-exact");
							AssertPlanesBitExact(in.receivedG, res.outputG, L"RGB Mode::Shadow must be bit-exact");
							AssertPlanesBitExact(in.receivedB, res.outputB, L"RGB Mode::Shadow must be bit-exact");
						}
						else
						{
							AssertPlanesBitExact(in.receivedCb, res.outputCb, L"YUV Mode::Shadow Cb must be bit-exact");
							AssertPlanesBitExact(in.receivedCr, res.outputCr, L"YUV Mode::Shadow Cr must be bit-exact");
						}
					}

					// Zero-delta Mode::On -> bit-exact passthrough including BTB/WTW & bars!
					{
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						TdV2ChromaEngine eng(cfg);
						StandInRegistry reg;
						const ChromaProcessInput in = buildInput(true);
						const auto res = eng.ProcessFrame(in, &reg);
						for (double d : res.deliveredDeltaCb.v) Assert::AreEqual(0.0, d, 0.0, L"Zero-delta Cb must be exact 0.0");
						for (double d : res.deliveredDeltaCr.v) Assert::AreEqual(0.0, d, 0.0, L"Zero-delta Cr must be exact 0.0");
						if (fmt == DeliveredChromaFormat::Rgb444)
						{
							AssertPlanesBitExact(in.receivedR, res.outputR, L"RGB zero-delta Mode::On must be bit-exact");
							AssertPlanesBitExact(in.receivedG, res.outputG, L"RGB zero-delta Mode::On must be bit-exact");
							AssertPlanesBitExact(in.receivedB, res.outputB, L"RGB zero-delta Mode::On must be bit-exact");
						}
						else
						{
							AssertPlanesBitExact(in.receivedCb, res.outputCb, L"YUV zero-delta Mode::On Cb must be bit-exact");
							AssertPlanesBitExact(in.receivedCr, res.outputCr, L"YUV zero-delta Mode::On Cr must be bit-exact");
						}
					}
				}

				// ---- Part B: Non-zero recovery on coded 4:2:0 delivered as 4:2:2 AND RGB 4:4:4 ----
				// Build coded 4:2:0 planes from independent analytic scene; corrupt valley heavily on coded 4:2:0.
				const TdV2Plane cleanCoded420Cb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 48.0, 48.0, 2.5, true);
				const TdV2Plane cleanCoded420Cr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 48.0, 48.0, 2.5, false);

				const TdV2Plane peakPrevCb = AddGaussianNoise(cleanCoded420Cb, 1.0, 301, codedActive420);
				const TdV2Plane peakPrevCr = AddGaussianNoise(cleanCoded420Cr, 1.0, 302, codedActive420);
				const TdV2Plane peakNextCb = AddGaussianNoise(cleanCoded420Cb, 1.0, 303, codedActive420);
				const TdV2Plane peakNextCr = AddGaussianNoise(cleanCoded420Cr, 1.0, 304, codedActive420);
				const TdV2Plane valleyCodedCb = AddGaussianNoise(cleanCoded420Cb, 11.0, 305, codedActive420);
				const TdV2Plane valleyCodedCr = AddGaussianNoise(cleanCoded420Cr, 11.0, 306, codedActive420);

				// W4 chroma-edge mask around the color edges (|x - 48| <= 8 or |y - 48| <= 8 inside active interior).
				std::vector<uint8_t> w4Mask444(static_cast<size_t>(w) * h, 0u);
				std::vector<uint8_t> w4Mask422(static_cast<size_t>(w / 2) * h, 0u);
				for (int y = 16; y < 80; ++y)
				{
					for (int x = 16; x < 80; ++x)
					{
						if (std::abs(x - 48) <= 8 || std::abs(y - 48) <= 8)
						{
							w4Mask444[static_cast<size_t>(y) * w + x] = 1u;
							w4Mask422[static_cast<size_t>(y) * (w / 2) + (x / 2)] = 1u;
						}
					}
				}

				// Sub-case B1: Coded 4:2:0 delivered as Yuv422.
				{
					TdV2Plane deliv422Cb = UpsampleChromaPlaneSited(valleyCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, active);
					TdV2Plane deliv422Cr = UpsampleChromaPlaneSited(valleyCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv422, siting, active);
					// Distinct bar sentinels outside active rect.
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w / 2; ++x)
						{
							if (!delivActive422.Contains(x, y))
							{
								deliv422Cb.at(x, y) = 25.0;
								deliv422Cr.at(x, y) = 995.0;
							}
						}
					}

					const TdV2Plane masterCb422 = SampleAnalyticChromaPlane(w, h, 2, 1, siting, 0.0, 0.0, 48.0, 48.0, 2.5, true);
					const TdV2Plane masterCr422 = SampleAnalyticChromaPlane(w, h, 2, 1, siting, 0.0, 0.0, 48.0, 48.0, 2.5, false);

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in;
					in.seq = 20;
					in.deliveredFormat = DeliveredChromaFormat::Yuv422;
					in.codedSubsampling = ChromaSubsampling::Yuv420;
					in.gridStatus = ChromaGridStatus::Identified;
					in.siting = siting;
					in.lumaW = w;
					in.lumaH = h;
					in.activeRect = active;
					in.receivedCb = deliv422Cb;
					in.receivedCr = deliv422Cr;
					in.hasCleanSplit = true;
					in.cleanCb = deliv422Cb;
					in.cleanCr = deliv422Cr;
					in.prevPeak = { &peakPrevCb, &peakPrevCr, &stillMf, false };
					in.nextPeak = { &peakNextCb, &peakNextCr, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 24.0;

					const auto res = eng.ProcessFrame(in, &reg);
					AssertPlaneAllFinite(res.outputCb, L"4:2:2 outputCb must be finite");
					AssertPlaneAllFinite(res.outputCr, L"4:2:2 outputCr must be finite");
					AssertBarsBitExact(deliv422Cb, res.outputCb, delivActive422, L"4:2:2 bar Cb must remain bit-exact");
					AssertBarsBitExact(deliv422Cr, res.outputCr, delivActive422, L"4:2:2 bar Cr must remain bit-exact");

					const double baseErrCb = MaskedRmse(masterCb422, deliv422Cb, delivActive422, &w4Mask422);
					const double outErrCb = MaskedRmse(masterCb422, res.outputCb, delivActive422, &w4Mask422);
					const double baseErrCr = MaskedRmse(masterCr422, deliv422Cr, delivActive422, &w4Mask422);
					const double outErrCr = MaskedRmse(masterCr422, res.outputCr, delivActive422, &w4Mask422);

					Assert::IsTrue(outErrCb <= 0.65 * baseErrCb, L"Coded 4:2:0 -> 4:2:2 recovery must cut W4 Cb edge error strongly (>= 35%)");
					Assert::IsTrue(outErrCr <= 0.65 * baseErrCr, L"Coded 4:2:0 -> 4:2:2 recovery must cut W4 Cr edge error strongly (>= 35%)");
				}

				// Sub-case B2: Coded 4:2:0 delivered as RGB 4:4:4 with BTB (< 64) & WTW (> 940) + Negative Control.
				{
					TdV2Plane deliv444Cb = UpsampleChromaPlaneSited(valleyCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					TdV2Plane deliv444Cr = UpsampleChromaPlaneSited(valleyCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);

					TdV2Plane recR, recG, recB;
					YCbCrPlanesToRgb(mat, masterY, deliv444Cb, deliv444Cr, midCode, recR, recG, recB);
					// Put distinctive bar sentinels outside ActiveRect.
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							if (!active.Contains(x, y))
							{
								recR.at(x, y) = (y < active.y0) ? 12.0 : 1011.0;
								recG.at(x, y) = (y < active.y0) ? 13.0 : 1012.0;
								recB.at(x, y) = (y < active.y0) ? 14.0 : 1013.0;
							}
						}
					}

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in;
					in.seq = 21;
					in.deliveredFormat = DeliveredChromaFormat::Rgb444;
					in.codedSubsampling = ChromaSubsampling::Yuv420;
					in.gridStatus = ChromaGridStatus::Identified;
					in.siting = siting;
					in.matrix = mat;
					in.lumaW = w;
					in.lumaH = h;
					in.activeRect = active;
					in.receivedR = recR;
					in.receivedG = recG;
					in.receivedB = recB;
					in.hasCleanSplit = true;
					in.cleanCb = deliv444Cb;
					in.cleanCr = deliv444Cr;
					in.prevPeak = { &peakPrevCb, &peakPrevCr, &stillMf, false };
					in.nextPeak = { &peakNextCb, &peakNextCr, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 24.0;

					const auto res = eng.ProcessFrame(in, &reg);
					AssertPlaneAllFinite(res.outputR, L"RGB outputR must be finite");
					AssertPlaneAllFinite(res.outputG, L"RGB outputG must be finite");
					AssertPlaneAllFinite(res.outputB, L"RGB outputB must be finite");
					AssertBarsBitExact(recR, res.outputR, active, L"RGB bar R must remain bit-exact");
					AssertBarsBitExact(recG, res.outputG, active, L"RGB bar G must remain bit-exact");
					AssertBarsBitExact(recB, res.outputB, active, L"RGB bar B must remain bit-exact");

					TdV2Plane outY, outCb, outCr;
					RgbPlanesToYCbCr(mat, res.outputR, res.outputG, res.outputB, midCode, outY, outCb, outCr);

					// Luma Y' must be strictly invariant under signed chroma delta application!
					for (int y = active.y0; y < active.y1; ++y)
						for (int x = active.x0; x < active.x1; ++x)
							Assert::AreEqual(masterY.at(x, y), outY.at(x, y), 1e-8,
								L"RGB chroma recovery must preserve luma Y' exact (dY == 0)");

					// Check BTB (< 64) and WTW (> 940) preserved unclipped in output RGB!
					double minOutBtbY = 1e9, maxOutWtwY = -1e9;
					int btbChannelsBelow64 = 0, wtwChannelsAbove940 = 0;
					for (int y = 8; y < 24; ++y)
					{
						for (int x = 16; x < 80; ++x)
						{
							minOutBtbY = (std::min)(minOutBtbY, outY.at(x, y));
							if (res.outputR.at(x, y) < 64.0 || res.outputG.at(x, y) < 64.0 || res.outputB.at(x, y) < 64.0)
								++btbChannelsBelow64;
						}
					}
					for (int y = 72; y < 88; ++y)
					{
						for (int x = 16; x < 80; ++x)
						{
							maxOutWtwY = (std::max)(maxOutWtwY, outY.at(x, y));
							if (res.outputR.at(x, y) > 940.0 || res.outputG.at(x, y) > 940.0 || res.outputB.at(x, y) > 940.0)
								++wtwChannelsAbove940;
						}
					}
					Assert::IsTrue(minOutBtbY < 32.0 && btbChannelsBelow64 > 500,
						L"RGB chroma recovery must preserve BTB (< 64) unclipped");
					Assert::IsTrue(maxOutWtwY > 980.0 && wtwChannelsAbove940 > 500,
						L"RGB chroma recovery must preserve WTW (> 940) unclipped");

					// W4 chroma-edge error vs independent 4:4:4 master must drop strongly (>= 30%!).
					const double baseW4Cb = MaskedRmse(masterCb444, deliv444Cb, active, &w4Mask444);
					const double outW4Cb = MaskedRmse(masterCb444, outCb, active, &w4Mask444);
					const double baseW4Cr = MaskedRmse(masterCr444, deliv444Cr, active, &w4Mask444);
					const double outW4Cr = MaskedRmse(masterCr444, outCr, active, &w4Mask444);

					// ---- Mandatory In-Test Negative Control (Rule 3) ----
					// Naive studio-range [64, 940] clipped full-res RGB blend on the EXACT SAME RGB input!
					TdV2Plane naiveR = res.outputR, naiveG = res.outputG, naiveB = res.outputB;
					for (int y = active.y0; y < active.y1; ++y)
					{
						for (int x = active.x0; x < active.x1; ++x)
						{
							naiveR.at(x, y) = (std::max)(64.0, (std::min)(940.0, naiveR.at(x, y)));
							naiveG.at(x, y) = (std::max)(64.0, (std::min)(940.0, naiveG.at(x, y)));
							naiveB.at(x, y) = (std::max)(64.0, (std::min)(940.0, naiveB.at(x, y)));
						}
					}
					TdV2Plane naiveY, naiveCb, naiveCr;
					RgbPlanesToYCbCr(mat, naiveR, naiveG, naiveB, midCode, naiveY, naiveCb, naiveCr);

					const ActiveRect btbZone{ 16, 8, 80, 24 };
					const ActiveRect wtwZone{ 16, 72, 80, 88 };
					const double naiveBtbYErr = MaskedRmse(masterY, naiveY, btbZone);
					const double naiveWtwYErr = MaskedRmse(masterY, naiveY, wtwZone);
					const double tdBtbYErr = MaskedRmse(masterY, outY, btbZone);
					const double tdWtwYErr = MaskedRmse(masterY, outY, wtwZone);

					// Also naive full-res RGB spatial-blur blend (destroys sharp W4 edges without coded-grid separation).
					TdV2Plane naiveSpatialR = recR, naiveSpatialG = recG, naiveSpatialB = recB;
					for (int y = active.y0 + 2; y < active.y1 - 2; ++y)
					{
						for (int x = active.x0 + 2; x < active.x1 - 2; ++x)
						{
							double sr = 0.0, sg = 0.0, sb = 0.0;
							for (int dy = -2; dy <= 2; ++dy)
								for (int dx = -2; dx <= 2; ++dx)
								{
									sr += recR.at(x + dx, y + dy);
									sg += recG.at(x + dx, y + dy);
									sb += recB.at(x + dx, y + dy);
								}
							naiveSpatialR.at(x, y) = sr / 25.0;
							naiveSpatialG.at(x, y) = sg / 25.0;
							naiveSpatialB.at(x, y) = sb / 25.0;
						}
					}
					TdV2Plane nsY, nsCb, nsCr;
					RgbPlanesToYCbCr(mat, naiveSpatialR, naiveSpatialG, naiveSpatialB, midCode, nsY, nsCb, nsCr);
					const double nsW4Cb = MaskedRmse(masterCb444, nsCb, active, &w4Mask444);

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"RGB 4:2:0->4:4:4 W4 Cb=%.2f->%.2f Cr=%.2f->%.2f | BTB Y err td=%.4f naiveClip=%.2f | WTW Y err td=%.4f naiveClip=%.2f | naiveBlur W4 Cb=%.2f",
						baseW4Cb, outW4Cb, baseW4Cr, outW4Cr, tdBtbYErr, naiveBtbYErr, tdWtwYErr, naiveWtwYErr, nsW4Cb);
					Logger::WriteMessage(buf);

					Assert::IsTrue(naiveBtbYErr > 30.0 && naiveWtwYErr > 40.0,
						L"Negative control: naive [64, 940] clipping MUST severely corrupt BTB and WTW regions");
					Assert::IsTrue(nsW4Cb > outW4Cb * 1.4,
						L"Negative control: naive full-res spatial RGB blend MUST have significantly worse W4 chroma-edge error");
					Assert::IsTrue(tdBtbYErr < 1e-7 && tdWtwYErr < 1e-7,
						L"TD coded-grid RGB recovery must keep BTB/WTW luma exact");
					Assert::IsTrue(outW4Cb <= 0.70 * baseW4Cb && outW4Cr <= 0.70 * baseW4Cr,
						L"TD coded-grid RGB recovery must cut W4 chroma-edge error >= 30% vs independent 4:4:4 master");
				}
			}

			// =========================================================================
			// 2. Siting Type 2 (-0.5, -0.5) vs Left (-0.5, 0.0) Zero Color Fringing
			// =========================================================================
			TEST_METHOD(SitingType2VsLeftZeroColorFringingWithNegativeControl)
			{
				// Part A: Exact parity between SitingSpec::Resampled and core/ChromaSiting.h ShiftAfterResample.
				const SitingSpec sitings[3] = { SitingSpec::Left(), SitingSpec::TopLeftType2(), SitingSpec::Center() };
				const double factors[4] = { 0.5, 2.0, 1.5, 0.25 };
				for (const SitingSpec& s : sitings)
				{
					for (double fx : factors)
					{
						for (double fy : factors)
						{
							const SitingSpec resampled = s.Resampled(fx, fy);
							Assert::AreEqual(ChromaSiting::ShiftAfterResample(s.shiftX, fx), resampled.shiftX, 0.0f,
								L"SitingSpec::Resampled shiftX must match ChromaSiting::ShiftAfterResample bit-exact");
							Assert::AreEqual(ChromaSiting::ShiftAfterResample(s.shiftY, fy), resampled.shiftY, 0.0f,
								L"SitingSpec::Resampled shiftY must match ChromaSiting::ShiftAfterResample bit-exact");
						}
					}
				}

				// Part B: High-contrast color edge under panning motion for BOTH Top-Left (Type 2) and Left siting.
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect interior{ 16, 16, 80, 80 };
				const double panVx = 1.5, panVy = 1.5; // sub-pixel on 4:2:0 chroma grid (+0.75, +0.75 coded px)
				const CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +panVx, +panVy, +panVx, +panVy, RegionMotionClass::Camera);
				const CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -panVx, -panVy, -panVx, -panVy, RegionMotionClass::Camera);

				// Edge mask concentrated tightly along the steep color transitions (|x - 48| <= 4 or |y - 48| <= 4).
				std::vector<uint8_t> edgeMask(static_cast<size_t>(w) * h, 0u);
				for (int y = interior.y0; y < interior.y1; ++y)
					for (int x = interior.x0; x < interior.x1; ++x)
						if (std::abs(x - 48) <= 4 || std::abs(y - 48) <= 4)
							edgeMask[static_cast<size_t>(y) * w + x] = 1u;

				// Independent 4:4:4 ground truth of the steep high-contrast color edge (width = 1.5 luma px).
				const TdV2Plane master444Cb = SampleAnalyticChromaPlane(w, h, 1, 1, SitingSpec::Center(), 0.0, 0.0, 48.0, 48.0, 1.5, true);
				const TdV2Plane master444Cr = SampleAnalyticChromaPlane(w, h, 1, 1, SitingSpec::Center(), 0.0, 0.0, 48.0, 48.0, 1.5, false);

				auto runSitingCase = [&](const SitingSpec& trueSiting, const SitingSpec& wrongSiting, const char* label)
				{
					// Panned clean 4:2:0 peak frames sampled at the EXACT true siting.
					const TdV2Plane prev420Cb = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, -panVx, -panVy, 48.0, 48.0, 1.5, true);
					const TdV2Plane prev420Cr = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, -panVx, -panVy, 48.0, 48.0, 1.5, false);
					const TdV2Plane next420Cb = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, +panVx, +panVy, 48.0, 48.0, 1.5, true);
					const TdV2Plane next420Cr = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, +panVx, +panVy, 48.0, 48.0, 1.5, false);

					// Current valley coded 4:2:0 frame has heavy coding noise + chroma edge degradation.
					const ActiveRect cActive = ActiveRect::Full(w / 2, h / 2);
					TdV2Plane valley420Cb = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, 0.0, 0.0, 48.0, 48.0, 2.5, true);
					TdV2Plane valley420Cr = SampleAnalyticChromaPlane(w, h, 2, 2, trueSiting, 0.0, 0.0, 48.0, 48.0, 2.5, false);
					valley420Cb = AddGaussianNoise(valley420Cb, 11.0, 411, cActive);
					valley420Cr = AddGaussianNoise(valley420Cr, 11.0, 412, cActive);

					// Deliver current valley as 4:4:4 (upsampled by player honoring trueSiting).
					const TdV2Plane delivCurCb = UpsampleChromaPlaneSited(
						valley420Cb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, trueSiting, active);
					const TdV2Plane delivCurCr = UpsampleChromaPlaneSited(
						valley420Cr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, trueSiting, active);

					auto evalEngine = [&](const SitingSpec& engineSiting)
					{
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						TdV2ChromaEngine eng(cfg);
						StandInRegistry reg;

						ChromaProcessInput in;
						in.seq = 30;
						in.deliveredFormat = DeliveredChromaFormat::Yuv444;
						in.codedSubsampling = ChromaSubsampling::Yuv420;
						in.gridStatus = ChromaGridStatus::Identified;
						in.siting = engineSiting;
						in.lumaW = w;
						in.lumaH = h;
						in.activeRect = active;
						in.receivedCb = delivCurCb;
						in.receivedCr = delivCurCr;
						in.hasCleanSplit = true;
						in.cleanCb = delivCurCb;
						in.cleanCr = delivCurCr;
						in.prevPeak = { &prev420Cb, &prev420Cr, &mfPrev, false };
						in.nextPeak = { &next420Cb, &next420Cr, &mfNext, false };
						in.hasStepPrior = true;
						in.estimatedStepDelta = 36.0;
						return eng.ProcessFrame(in, &reg);
					};

					const auto resTrue = evalEngine(trueSiting);
					// Mandatory In-Test Negative Control 1 (Rule 3):
					// Same input processed with wrong/zero-shift siting in the engine!
					const auto resWrong = evalEngine(wrongSiting);

					// Mandatory In-Test Negative Control 2 (Rule 3):
					// Naive wrong-siting / zero-shift chroma blend (averages cur + warped peaks upsampled with wrongSiting).
					TdV2Plane warpedPrevCb, warpedNextCb;
					std::vector<uint8_t> vp, vn;
					ChromaDetail::WarpChromaPlaneWithLumaMotion(
						prev420Cb, valley420Cb, cActive, 2, 2, wrongSiting, mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedPrevCb, vp);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(
						next420Cb, valley420Cb, cActive, 2, 2, wrongSiting, mfNext.vx, mfNext.vy, cw, ch, 16, warpedNextCb, vn);
					const TdV2Plane wrongUpPrevCb = UpsampleChromaPlaneSited(
						warpedPrevCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, wrongSiting, active);
					const TdV2Plane wrongUpNextCb = UpsampleChromaPlaneSited(
						warpedNextCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Yuv444, wrongSiting, active);
					TdV2Plane naiveWrongBlendCb(w, h, 512.0);
					for (size_t i = 0; i < naiveWrongBlendCb.v.size(); ++i)
						naiveWrongBlendCb.v[i] = (delivCurCb.v[i] + wrongUpPrevCb.v[i] + wrongUpNextCb.v[i]) / 3.0;

					const double baseEdgeCb = MaskedRmse(master444Cb, delivCurCb, interior, &edgeMask);
					const double trueEdgeCb = MaskedRmse(master444Cb, resTrue.outputCb, interior, &edgeMask);
					const double wrongEdgeCb = MaskedRmse(master444Cb, resWrong.outputCb, interior, &edgeMask);
					const double naiveWrongEdgeCb = MaskedRmse(master444Cb, naiveWrongBlendCb, interior, &edgeMask);

					const double baseEdgeCr = MaskedRmse(master444Cr, delivCurCr, interior, &edgeMask);
					const double trueEdgeCr = MaskedRmse(master444Cr, resTrue.outputCr, interior, &edgeMask);
					const double wrongEdgeCr = MaskedRmse(master444Cr, resWrong.outputCr, interior, &edgeMask);

					// Measure signed color-fringing bias strictly along the horizontal edge strip (y in [46, 50], x in [20, 42))
					// where a 0.5-luma-px vertical shift (TopLeftType2 vs Left or Center) causes direct color fringing!
					double trueFringeSum = 0.0, wrongEngFringeSum = 0.0, naiveWrongFringeSum = 0.0;
					size_t fringeN = 0;
					for (int y = 46; y <= 50; ++y)
					{
						for (int x = 20; x < 42; ++x)
						{
							trueFringeSum += (resTrue.outputCb.at(x, y) - master444Cb.at(x, y));
							wrongEngFringeSum += (resWrong.outputCb.at(x, y) - master444Cb.at(x, y));
							naiveWrongFringeSum += (naiveWrongBlendCb.at(x, y) - master444Cb.at(x, y));
							++fringeN;
						}
					}
					const double trueFringeBias = std::fabs(trueFringeSum / static_cast<double>(fringeN));
					const double wrongEngFringeBias = std::fabs(wrongEngFringeSum / static_cast<double>(fringeN));
					const double naiveWrongFringeBias = std::fabs(naiveWrongFringeSum / static_cast<double>(fringeN));

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"Siting %s: W4 Cb base=%.2f true=%.2f wrongEng=%.2f naiveWrong=%.2f | fringeBias true=%.2f wrongEng=%.2f naiveWrong=%.2f",
						label, baseEdgeCb, trueEdgeCb, wrongEdgeCb, naiveWrongEdgeCb, trueFringeBias, wrongEngFringeBias, naiveWrongFringeBias);
					Logger::WriteMessage(buf);

					Assert::IsTrue(trueEdgeCb < baseEdgeCb * 0.65 && trueEdgeCr < baseEdgeCr * 0.65,
						L"Correctly sited panning chroma recovery must strongly reduce W4 chroma-edge error");
					Assert::IsTrue(wrongEdgeCb > trueEdgeCb + 1.5 && wrongEdgeCr > trueEdgeCr + 1.5,
						L"Negative control: wrong/zero-shift siting in engine MUST worsen chroma-edge error on both Cb and Cr");
					Assert::IsTrue(naiveWrongEdgeCb > trueEdgeCb + 1.5,
						L"Negative control: naive wrong-siting chroma blend MUST produce worse chroma-edge error");
					Assert::IsTrue(wrongEngFringeBias > trueFringeBias + 3.0 && naiveWrongFringeBias > trueFringeBias + 3.0,
						L"Negative control: wrong siting MUST create measurable signed color fringing along the edge");
				};

				// Type 2 (-0.5, -0.5) vs naive zero-shift Center (0.0, 0.0)
				runSitingCase(SitingSpec::TopLeftType2(), SitingSpec::Center(), "TopLeftType2_vs_Center");
				// Left (-0.5, 0.0) vs wrong TopLeftType2 (-0.5, -0.5) (pure 0.5-luma-px vertical shift mismatch!)
				runSitingCase(SitingSpec::Left(), SitingSpec::TopLeftType2(), "Left_vs_TopLeftType2");
			}

			// =========================================================================
			// 3. 4:4:4 Anchor Canvas Sub-Pixel Accumulation Beats Single Subsampled Warp
			// =========================================================================
			TEST_METHOD(Anchor444CanvasSubpixelAccumulationBeatsSingleSubsampledWarp)
			{
				// Independent 4:4:4 ground truth with sharp 1-luma-px chroma edges and high-frequency chroma bars
				// at odd luma coordinates (strictly above single-frame 4:2:0 sampling density!).
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active{ 8, 8, 56, 56 };
				const ActiveRect interior{ 16, 16, 48, 48 };
				const SitingSpec siting = SitingSpec::TopLeftType2(); // coded (cx, cy) -> luma (2*cx, 2*cy)

				auto trueChroma = [](double lx, double ly, bool isCb) -> double
				{
					// Sharp sub-pixel step edge at lx = 31.5, ly = 31.5 + fine 3-luma-px stripe (period 3 > 4:2:0 single-frame!)
					const double edge = 90.0 * std::tanh((lx - 31.5) / 0.8) + 70.0 * std::tanh((ly - 31.5) / 0.8);
					const double detail = 35.0 * std::sin(2.0 * kPi * lx / 3.0) * std::cos(2.0 * kPi * ly / 3.0);
					return isCb ? (512.0 + 0.5 * edge + detail) : (512.0 - 0.5 * edge + 0.8 * detail);
				};

				TdV2Plane master444Cb(w, h, 512.0), master444Cr(w, h, 512.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						master444Cb.at(x, y) = trueChroma(static_cast<double>(x), static_cast<double>(y), true);
						master444Cr.at(x, y) = trueChroma(static_cast<double>(x), static_cast<double>(y), false);
					}
				}

				// Generate 4 sub-pixel shifted 4:2:0 anchor frames:
				// Shifts in luma px: (0,0), (-1,0), (0,-1), (-1,-1) = half-chroma-pixel shifts in 4:2:0!
				// Convention: x_frame = x_canvas + v => frame sample at (lx, ly) observes Master(lx - vx, ly - vy).
				const double shifts[4][2] = { { 0.0, 0.0 }, { -1.0, 0.0 }, { 0.0, -1.0 }, { -1.0, -1.0 } };

				TdV2ChromaAnchorCanvas canvas;
				StandInRegistry reg;
				TdV2Plane anchor0Cb(w / 2, h / 2, 512.0), anchor0Cr(w / 2, h / 2, 512.0);

				for (int k = 0; k < 4; ++k)
				{
					const double vx = shifts[k][0];
					const double vy = shifts[k][1];
					TdV2Plane cCb(w / 2, h / 2, 512.0), cCr(w / 2, h / 2, 512.0);
					for (int cy = 0; cy < h / 2; ++cy)
					{
						const double ly = siting.CodedToLumaY(static_cast<double>(cy), 2);
						for (int cx = 0; cx < w / 2; ++cx)
						{
							const double lx = siting.CodedToLumaX(static_cast<double>(cx), 2);
							cCb.at(cx, cy) = trueChroma(lx - vx, ly - vy, true);
							cCr.at(cx, cy) = trueChroma(lx - vx, ly - vy, false);
						}
					}
					if (k == 0)
					{
						anchor0Cb = cCb;
						anchor0Cr = cCr;
					}
					const CellMotionField splatMf = CellMotionField::Uniform(cw, ch, vx, vy, vx, vy, RegionMotionClass::Camera);
					Assert::IsTrue(canvas.AccumulateAnchorFrame(
						cCb, cCr, nullptr, w, h, ChromaSubsampling::Yuv420, siting, active, &splatMf, 16, 10, &reg),
						L"AccumulateAnchorFrame must succeed on valid 4:2:0 anchor");
				}

				Assert::IsTrue(canvas.HasData());
				Assert::AreEqual(static_cast<size_t>(4), canvas.AccumulatedFrames());

				// Materialize 4:4:4 canvas and verify:
				//   (a) Bar pixels outside ActiveRect have exact 0.0 weight (hard ActiveRect boundary!)
				//   (b) Interior 4:4:4 canvas matches independent 4:4:4 ground truth accurately!
				TdV2Plane matCb, matCr;
				Assert::IsTrue(canvas.MaterializeCanvas(matCb, matCr));
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						if (!active.Contains(x, y))
							Assert::AreEqual(0.0, canvas.WeightPlane().at(x, y), 0.0, L"Anchor canvas must never splat outside ActiveRect");

				const double matErrCb = MaskedRmse(master444Cb, matCb, interior);
				const double matErrCr = MaskedRmse(master444Cr, matCr, interior);
				Assert::IsTrue(matErrCb < 1e-6 && matErrCr < 1e-6,
					L"4 half-chroma-pixel shifted 4:2:0 anchors must reconstruct exact 4:4:4 canvas inside ActiveRect");

				// Now query at a sub-chroma-pixel shifted frame (vx = +1.0, vy = +1.0 luma px = +0.5, +0.5 chroma px)
				// comparing multi-frame 4:4:4 canvas SampleAtCodedGrid vs single-frame 4:2:0 WarpChromaPlaneWithLumaMotion!
				const CellMotionField readMf = CellMotionField::Uniform(cw, ch, +1.0, +1.0, +1.0, +1.0, RegionMotionClass::Camera);
				TdV2Plane canvasQueryCb, canvasQueryCr;
				std::vector<uint8_t> canvasValid;
				Assert::IsTrue(canvas.SampleAtCodedGrid(
					ChromaSubsampling::Yuv420, siting, active, &readMf, 16,
					canvasQueryCb, canvasQueryCr, canvasValid, &reg));

				// Independent ground truth at the exact queried coordinates (2*cx + 1, 2*cy + 1).
				TdV2Plane gtQueryCb(w / 2, h / 2, 512.0), gtQueryCr(w / 2, h / 2, 512.0);
				for (int cy = 0; cy < h / 2; ++cy)
					for (int cx = 0; cx < w / 2; ++cx)
					{
						gtQueryCb.at(cx, cy) = trueChroma(2.0 * cx + 1.0, 2.0 * cy + 1.0, true);
						gtQueryCr.at(cx, cy) = trueChroma(2.0 * cx + 1.0, 2.0 * cy + 1.0, false);
					}

				// ---- Mandatory In-Test Negative Control (Rule 3) ----
				// Single-frame subsampled 4:2:0 warp of anchor0 by (+1.0, +1.0) luma px without 4:4:4 accumulation!
				const ActiveRect cActive = ChromaDetail::ToSubsampledActiveRect(active, w, h, 2, 2, w / 2, h / 2);
				const ActiveRect cInterior{ 8, 8, 24, 24 };
				TdV2Plane singleWarpCb, singleWarpCr;
				std::vector<uint8_t> svCb, svCr;
				ChromaDetail::WarpChromaPlaneWithLumaMotion(
					anchor0Cb, anchor0Cb, cActive, 2, 2, siting, readMf.vx, readMf.vy, cw, ch, 16, singleWarpCb, svCb);
				ChromaDetail::WarpChromaPlaneWithLumaMotion(
					anchor0Cr, anchor0Cr, cActive, 2, 2, siting, readMf.vx, readMf.vy, cw, ch, 16, singleWarpCr, svCr);

				const double canvasErrCb = MaskedRmse(gtQueryCb, canvasQueryCb, cInterior);
				const double singleErrCb = MaskedRmse(gtQueryCb, singleWarpCb, cInterior);
				const double canvasErrCr = MaskedRmse(gtQueryCr, canvasQueryCr, cInterior);
				const double singleErrCr = MaskedRmse(gtQueryCr, singleWarpCr, cInterior);

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"4:4:4 Anchor Canvas vs Single 4:2:0 Warp: Cb RMSE canvas=%.4f single=%.2f | Cr RMSE canvas=%.4f single=%.2f",
					canvasErrCb, singleErrCb, canvasErrCr, singleErrCr);
				Logger::WriteMessage(buf);

				Assert::IsTrue(singleErrCb > 8.0 && singleErrCr > 6.0,
					L"Negative control: single-frame subsampled 4:2:0 warp MUST heavily blur/alias the sub-pixel chroma edge");
				Assert::IsTrue(canvasErrCb < 1e-5 && canvasErrCr < 1e-5,
					L"Multi-frame 4:4:4 canvas accumulation must beat single-frame subsampled 4:2:0 warp decisively");
				Assert::IsTrue(singleErrCb > canvasErrCb + 8.0 && singleErrCr > canvasErrCr + 6.0,
					L"4:4:4 canvas must show massive reduction in sub-pixel chroma edge error vs single 4:2:0 warp");

				// ---- Part B (HIGH-4a): Static zero-motion step edge on Left & Center siting -> ZERO ringing ----
				{
					const ActiveRect full64 = ActiveRect::Full(w, h);
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					TdV2Plane stepCb(w / 2, h / 2, 412.0), stepCr(w / 2, h / 2, 412.0);
					for (int cy = 0; cy < h / 2; ++cy)
					{
						for (int cx = 0; cx < w / 2; ++cx)
						{
							stepCb.at(cx, cy) = (cx < w / 4) ? 412.0 : 612.0; // 200-code vertical step edge
							stepCr.at(cx, cy) = (cy < h / 4) ? 412.0 : 612.0; // 200-code horizontal step edge
						}
					}

					// Mandatory Negative Control (Rule 3): naive integer-luma splat of Left-sited stepCr (ly = 2*cy + 0.5)
					// produces a [412, 412, 612, 612] staircase whose Catmull-Rom bicubic query at 2*cy + 0.5 rings by +/-12.5 codes!
					TdV2Plane naiveStairCr(w, h, 412.0);
					for (int cy = 0; cy < h / 2; ++cy)
					{
						for (int cx = 0; cx < w / 2; ++cx)
						{
							const double v = stepCr.at(cx, cy);
							naiveStairCr.at(2 * cx, 2 * cy) = v;
							naiveStairCr.at(2 * cx + 1, 2 * cy) = v;
							naiveStairCr.at(2 * cx, 2 * cy + 1) = v;
							naiveStairCr.at(2 * cx + 1, 2 * cy + 1) = v;
						}
					}
					ActiveSubPlaneView naiveView{ &naiveStairCr, 0, 0, w, h };
					double maxNaiveRing = 0.0;
					for (int cy = 4; cy < h / 2 - 4; ++cy)
					{
						for (int cx = 4; cx < w / 2 - 4; ++cx)
						{
							double sampleV = 0.0;
							Assert::IsTrue(VpComposed::SampleBicubic(naiveView, 2.0 * cx, 2.0 * cy + 0.5, sampleV));
							maxNaiveRing = (std::max)(maxNaiveRing, std::fabs(sampleV - stepCr.at(cx, cy)));
						}
					}
					Assert::IsTrue(maxNaiveRing >= 12.0,
						L"Negative control: naive integer-luma splat of Left-sited step edge MUST ring by >= 12 codes (exact 12.5)");

					for (const SitingSpec& testSiting : { SitingSpec::Left(), SitingSpec::Center() })
					{
						TdV2ChromaAnchorCanvas staticCanvas;
						StandInRegistry staticReg;
						Assert::IsTrue(staticCanvas.AccumulateAnchorFrame(
							stepCb, stepCr, nullptr, w, h, ChromaSubsampling::Yuv420,
							testSiting, full64, &stillMf, 16, 10, &staticReg));

						TdV2Plane qCb, qCr;
						std::vector<uint8_t> qValid;
						Assert::IsTrue(staticCanvas.SampleAtCodedGrid(
							ChromaSubsampling::Yuv420, testSiting, full64, &stillMf, 16,
							qCb, qCr, qValid, &staticReg));

						double maxErrCb = 0.0, maxErrCr = 0.0;
						for (int cy = 4; cy < h / 2 - 4; ++cy)
						{
							for (int cx = 4; cx < w / 2 - 4; ++cx)
							{
								Assert::AreEqual(static_cast<uint8_t>(1u), qValid[static_cast<size_t>(cy) * (w / 2) + cx]);
								maxErrCb = (std::max)(maxErrCb, std::fabs(qCb.at(cx, cy) - stepCb.at(cx, cy)));
								maxErrCr = (std::max)(maxErrCr, std::fabs(qCr.at(cx, cy) - stepCr.at(cx, cy)));
							}
						}
						Assert::IsTrue(maxErrCb < 1e-6 && maxErrCr < 1e-6,
							L"Static zero-motion step edge on Left and Center siting MUST have zero ringing (< 1e-6)");
					}
				}

				// ---- Part C (HIGH-4a): 4-frame sub-pixel shifted accumulation on SitingSpec::Left() ----
				{
					const SitingSpec leftSiting = SitingSpec::Left();
					TdV2ChromaAnchorCanvas leftCanvas;
					StandInRegistry leftReg;
					TdV2Plane leftAnchor0Cb(w / 2, h / 2, 512.0), leftAnchor0Cr(w / 2, h / 2, 512.0);
					for (int k = 0; k < 4; ++k)
					{
						const double vx = shifts[k][0];
						const double vy = shifts[k][1];
						TdV2Plane cCb(w / 2, h / 2, 512.0), cCr(w / 2, h / 2, 512.0);
						for (int cy = 0; cy < h / 2; ++cy)
						{
							const double ly = leftSiting.CodedToLumaY(static_cast<double>(cy), 2);
							for (int cx = 0; cx < w / 2; ++cx)
							{
								const double lx = leftSiting.CodedToLumaX(static_cast<double>(cx), 2);
								cCb.at(cx, cy) = trueChroma(lx - vx, ly - vy, true);
								cCr.at(cx, cy) = trueChroma(lx - vx, ly - vy, false);
							}
						}
						if (k == 0) { leftAnchor0Cb = cCb; leftAnchor0Cr = cCr; }
						const CellMotionField splatMf = CellMotionField::Uniform(cw, ch, vx, vy, vx, vy, RegionMotionClass::Camera);
						Assert::IsTrue(leftCanvas.AccumulateAnchorFrame(
							cCb, cCr, nullptr, w, h, ChromaSubsampling::Yuv420, leftSiting, active, &splatMf, 16, 10, &leftReg));
					}

					TdV2Plane lqCb, lqCr;
					std::vector<uint8_t> lqValid;
					Assert::IsTrue(leftCanvas.SampleAtCodedGrid(
						ChromaSubsampling::Yuv420, leftSiting, active, &readMf, 16, lqCb, lqCr, lqValid, &leftReg));

					TdV2Plane gtLeftCb(w / 2, h / 2, 512.0), gtLeftCr(w / 2, h / 2, 512.0);
					for (int cy = 0; cy < h / 2; ++cy)
					{
						const double ly = leftSiting.CodedToLumaY(static_cast<double>(cy), 2) + 1.0;
						for (int cx = 0; cx < w / 2; ++cx)
						{
							const double lx = leftSiting.CodedToLumaX(static_cast<double>(cx), 2) + 1.0;
							gtLeftCb.at(cx, cy) = trueChroma(lx, ly, true);
							gtLeftCr.at(cx, cy) = trueChroma(lx, ly, false);
						}
					}
					TdV2Plane swLeftCb, swLeftCr;
					std::vector<uint8_t> swvCb, swvCr;
					ChromaDetail::WarpChromaPlaneWithLumaMotion(
						leftAnchor0Cb, leftAnchor0Cb, cActive, 2, 2, leftSiting, readMf.vx, readMf.vy, cw, ch, 16, swLeftCb, swvCb);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(
						leftAnchor0Cr, leftAnchor0Cr, cActive, 2, 2, leftSiting, readMf.vx, readMf.vy, cw, ch, 16, swLeftCr, swvCr);

					const double lcErrCb = MaskedRmse(gtLeftCb, lqCb, cInterior);
					const double lsErrCb = MaskedRmse(gtLeftCb, swLeftCb, cInterior);
					const double lcErrCr = MaskedRmse(gtLeftCr, lqCr, cInterior);
					const double lsErrCr = MaskedRmse(gtLeftCr, swLeftCr, cInterior);
					Assert::IsTrue(lsErrCb > 8.0 && lsErrCr > 6.0,
						L"Negative control (Left siting): single-frame 4:2:0 warp must blur/alias sub-pixel stripes");
					Assert::IsTrue(lcErrCb < 1e-5 && lcErrCr < 1e-5,
						L"4-frame sub-pixel accumulation on Left siting must reconstruct sub-pixel stripes cleanly");
				}

				// ---- Part D (HIGH-4a + HIGH-3): End-to-end TdV2ChromaEngine anchorCanvasUsed, MotionFieldStandIn, Cut/Res/ActiveRect resets ----
				{
					const SitingSpec leftSiting = SitingSpec::Left();
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					const TdV2Plane cleanCb = SampleAnalyticChromaPlane(w, h, 2, 2, leftSiting, 0.0, 0.0, 32.0, 32.0, 2.0, true);
					const TdV2Plane cleanCr = SampleAnalyticChromaPlane(w, h, 2, 2, leftSiting, 0.0, 0.0, 32.0, 32.0, 2.0, false);
					const TdV2Plane noisyCb = AddGaussianNoise(cleanCb, 10.0, 391, cActive);
					const TdV2Plane noisyCr = AddGaussianNoise(cleanCr, 10.0, 392, cActive);

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.estimator = VarianceEstimator::A2_RoundingCells;
					TdV2ChromaEngine eng(cfg);
					StandInRegistry regSeed;

					ChromaProcessInput fSeed;
					fSeed.seq = 1;
					fSeed.deliveredFormat = DeliveredChromaFormat::Yuv420;
					fSeed.codedSubsampling = ChromaSubsampling::Yuv420;
					fSeed.gridStatus = ChromaGridStatus::Identified;
					fSeed.siting = leftSiting;
					fSeed.lumaW = w;
					fSeed.lumaH = h;
					fSeed.activeRect = active;
					fSeed.receivedCb = cleanCb;
					fSeed.receivedCr = cleanCr;
					fSeed.hasCleanSplit = true;
					fSeed.cleanCb = cleanCb;
					fSeed.cleanCr = cleanCr;
					fSeed.isAnchorFrame = true;
					fSeed.hasStepPrior = true;
					fSeed.estimatedStepDelta = 24.0;
					eng.ProcessFrame(fSeed, &regSeed);
					Assert::IsTrue(eng.AnchorCanvas().HasData(), L"Anchor frame must populate engine AnchorCanvas");

					// Valley frame using ONLY anchor canvas ref (prevPeak/nextPeak omitted, enableCarriedCfl = false).
					ChromaProcessInput fVal = fSeed;
					fVal.seq = 2;
					fVal.isAnchorFrame = false;
					fVal.receivedCb = noisyCb;
					fVal.receivedCr = noisyCr;
					fVal.cleanCb = noisyCb;
					fVal.cleanCr = noisyCr;
					fVal.enableAnchorCanvasRef = true;
					fVal.enableCarriedCfl = false;
					fVal.hasAnchorReadMotion = true;
					fVal.anchorReadMotion = stillMf;

					StandInRegistry regVal;
					const auto resVal = eng.ProcessFrame(fVal, &regVal);
					Assert::IsTrue(resVal.anchorCanvasUsed, L"Engine must set res.anchorCanvasUsed == true");
					Assert::AreEqual(static_cast<size_t>(1), resVal.activeRefCount);

					// Negative control: disabling enableAnchorCanvasRef yields zero recovery!
					ChromaProcessInput fNoCanvas = fVal;
					fNoCanvas.enableAnchorCanvasRef = false;
					StandInRegistry regNoCanvas;
					const auto resNoCanvas = eng.ProcessFrame(fNoCanvas, &regNoCanvas);
					Assert::IsFalse(resNoCanvas.anchorCanvasUsed);

					const double baseErrCb = MaskedRmse(cleanCb, resNoCanvas.outputCb, cInterior);
					const double outErrCb = MaskedRmse(cleanCb, resVal.outputCb, cInterior);
					const double baseErrCr = MaskedRmse(cleanCr, resNoCanvas.outputCr, cInterior);
					const double outErrCr = MaskedRmse(cleanCr, resVal.outputCr, cInterior);
					Assert::IsTrue(outErrCb < baseErrCb * 0.65 && outErrCr < baseErrCr * 0.65,
						L"End-to-end anchorCanvasUsed path must strongly reduce valley chroma error vs disabled canvas");

					// HIGH-3: Missing hasAnchorReadMotion == false MUST visibly record MotionFieldStandIn & motionStandInRefs > 0!
					ChromaProcessInput fMissingMot = fVal;
					fMissingMot.seq = 3;
					fMissingMot.hasAnchorReadMotion = false;
					StandInRegistry regMissMot;
					const auto resMissMot = eng.ProcessFrame(fMissingMot, &regMissMot);
					Assert::IsTrue(regMissMot.IsActive(StandInKind::MotionFieldStandIn),
						L"Missing anchorReadMotion MUST visibly engage StandInKind::MotionFieldStandIn");
					Assert::IsTrue(resMissMot.motionStandInRefs > 0,
						L"Missing anchorReadMotion MUST increment res.motionStandInRefs");

					// Cut reset: isCutOrSpanInvalid = true resets anchor canvas cleanly.
					ChromaProcessInput fCut = fVal;
					fCut.seq = 4;
					fCut.isCutOrSpanInvalid = true;
					StandInRegistry regCut;
					const auto resCut = eng.ProcessFrame(fCut, &regCut);
					Assert::IsTrue(resCut.anchorReset, L"Cut must set res.anchorReset == true");
					Assert::IsFalse(resCut.anchorCanvasUsed, L"Cut must not use pre-cut anchor canvas");
					Assert::IsFalse(eng.AnchorCanvas().HasData(), L"Cut must clear engine AnchorCanvas");

					// ActiveRect change reset (same resolution 64x64, activeRect shrinks {8,8,56,56} -> {16,16,48,48}).
					eng.ProcessFrame(fSeed, &regSeed);
					Assert::IsTrue(eng.AnchorCanvas().HasData());
					ChromaProcessInput fActiveChg = fVal;
					fActiveChg.seq = 5;
					fActiveChg.activeRect = ActiveRect{ 16, 16, 48, 48 };
					StandInRegistry regActiveChg;
					const auto resActiveChg = eng.ProcessFrame(fActiveChg, &regActiveChg);
					Assert::IsTrue(resActiveChg.anchorReset, L"ActiveRect change must reset anchor canvas cleanly");
					Assert::IsFalse(resActiveChg.anchorCanvasUsed);

					// Resolution change reset (64x64 -> 80x64).
					eng.ProcessFrame(fSeed, &regSeed);
					Assert::IsTrue(eng.AnchorCanvas().HasData());
					const TdV2Plane bigCb(40, 32, 512.0), bigCr(40, 32, 512.0);
					ChromaProcessInput fResChg;
					fResChg.seq = 6;
					fResChg.deliveredFormat = DeliveredChromaFormat::Yuv420;
					fResChg.codedSubsampling = ChromaSubsampling::Yuv420;
					fResChg.gridStatus = ChromaGridStatus::Identified;
					fResChg.siting = leftSiting;
					fResChg.lumaW = 80;
					fResChg.lumaH = 64;
					fResChg.receivedCb = bigCb;
					fResChg.receivedCr = bigCr;
					fResChg.hasCleanSplit = true;
					fResChg.cleanCb = bigCb;
					fResChg.cleanCr = bigCr;
					StandInRegistry regResChg;
					const auto resResChg = eng.ProcessFrame(fResChg, &regResChg);
					Assert::IsTrue(resResChg.anchorReset, L"Resolution change must reset anchor canvas cleanly");
					Assert::IsFalse(resResChg.anchorCanvasUsed);
				}
			}

			// =========================================================================
			// 4. Carried CfL (H5): Luma-Only Residual Zero Leak & Disagreement Fallback
			// =========================================================================
			TEST_METHOD(CarriedCflH5LumaOnlyResidualZeroLeakAndDisagreementFallback)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active = ActiveRect::Full(w, h);
				const SitingSpec siting = SitingSpec::TopLeftType2();
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Build anchor frame with strong linear luma-chroma correlation in every 16x16 cell:
				//   Cb = 512 + 0.55 * (Y - 400), Cr = 512 - 0.45 * (Y - 400).
				const double trueSlopeCb = 0.55;
				const double trueSlopeCr = -0.45;
				TdV2Plane anchorY(w, h, 400.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						// High within-cell luma variance so OLS CfL slope fits tightly in every 16x16 cell.
						anchorY.at(x, y) = 400.0 + 140.0 * std::sin(2.0 * kPi * x / 14.0) * std::cos(2.0 * kPi * y / 14.0)
						                         + 60.0 * std::cos(2.0 * kPi * (x + y) / 11.0);
					}
				}
				TdV2Plane anchorCb(w / 2, h / 2, 512.0), anchorCr(w / 2, h / 2, 512.0);
				for (int cy = 0; cy < h / 2; ++cy)
				{
					const int ly = static_cast<int>(siting.CodedToLumaY(static_cast<double>(cy), 2));
					for (int cx = 0; cx < w / 2; ++cx)
					{
						const int lx = static_cast<int>(siting.CodedToLumaX(static_cast<double>(cx), 2));
						const double dy = anchorY.at(lx, ly) - 400.0;
						anchorCb.at(cx, cy) = 512.0 + trueSlopeCb * dy;
						anchorCr.at(cx, cy) = 512.0 + trueSlopeCr * dy;
					}
				}

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.estimator = VarianceEstimator::A2_RoundingCells; // force non-zero blend if any residual exists
				TdV2ChromaEngine eng(cfg);
				StandInRegistry reg;

				// Step 1: Ingest anchor frame (fits carried CfL (a, b) and stores anchorCleanY as P_Y source).
				ChromaProcessInput fAnchor;
				fAnchor.seq = 1;
				fAnchor.deliveredFormat = DeliveredChromaFormat::Yuv420;
				fAnchor.codedSubsampling = ChromaSubsampling::Yuv420;
				fAnchor.gridStatus = ChromaGridStatus::Identified;
				fAnchor.siting = siting;
				fAnchor.lumaW = w;
				fAnchor.lumaH = h;
				fAnchor.activeRect = active;
				fAnchor.receivedY = anchorY;
				fAnchor.receivedCb = anchorCb;
				fAnchor.receivedCr = anchorCr;
				fAnchor.hasCleanSplit = true;
				fAnchor.cleanCb = anchorCb;
				fAnchor.cleanCr = anchorCr;
				fAnchor.isAnchorFrame = true;
				fAnchor.anchorCleanY = anchorY;
				fAnchor.hasStepPrior = true;
				fAnchor.estimatedStepDelta = 12.0;
				eng.ProcessFrame(fAnchor, &reg);

				Assert::IsTrue(eng.AnchorCanvas().HasCfl(), L"Anchor ingestion with clean luma must fit carried CfL");
				Assert::AreEqual(static_cast<size_t>(cw * ch), eng.AnchorCanvas().CflCells().size());
				for (const auto& cell : eng.AnchorCanvas().CflCells())
				{
					Assert::IsTrue(cell.valid);
					Assert::AreEqual(trueSlopeCb, cell.aCb, 1e-2, L"Fitted CfL Cb slope must match anchor correlation");
					Assert::AreEqual(trueSlopeCr, cell.aCr, 1e-2, L"Fitted CfL Cr slope must match anchor correlation");
				}

				// ---- Part A: Large LUMA-ONLY coded residual (curY jumps by +140 while chroma & P_Y stay unchanged!) ----
				TdV2Plane shiftedCurY = anchorY;
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						shiftedCurY.at(x, y) += 140.0 * (((x / 8 + y / 8) % 2 == 0) ? 1.0 : -1.0);

				ChromaProcessInput fLumaOnly;
				fLumaOnly.seq = 2;
				fLumaOnly.deliveredFormat = DeliveredChromaFormat::Yuv420;
				fLumaOnly.codedSubsampling = ChromaSubsampling::Yuv420;
				fLumaOnly.gridStatus = ChromaGridStatus::Identified;
				fLumaOnly.siting = siting;
				fLumaOnly.lumaW = w;
				fLumaOnly.lumaH = h;
				fLumaOnly.activeRect = active;
				fLumaOnly.receivedY = shiftedCurY; // MASSIVE luma-only coded residual!
				fLumaOnly.receivedCb = anchorCb;   // chroma UNCHANGED!
				fLumaOnly.receivedCr = anchorCr;   // chroma UNCHANGED!
				fLumaOnly.hasCleanSplit = true;
				fLumaOnly.cleanCb = anchorCb;
				fLumaOnly.cleanCr = anchorCr;
				fLumaOnly.hasAnchorReadMotion = true;
				fLumaOnly.anchorReadMotion = stillMf;
				fLumaOnly.enableAnchorCanvasRef = false; // isolate carried CfL H5 directly!
				fLumaOnly.enableCarriedCfl = true;
				fLumaOnly.hasStepPrior = true;
				fLumaOnly.estimatedStepDelta = 12.0;

				StandInRegistry regLumaOnly;
				const auto resLumaOnly = eng.ProcessFrame(fLumaOnly, &regLumaOnly);
				Assert::IsTrue(resLumaOnly.carriedCflUsed, L"Carried CfL must be active on Part A");
				Assert::AreEqual(static_cast<size_t>(0), resLumaOnly.cflFallbackCells,
					L"Luma-only change must NOT falsely trigger CfL disagreement when P_Y and chroma agree");

				// Compare against identical input with UNCHANGED curY -> MUST be bit-identical (zero curY dependence)!
				ChromaProcessInput fBaseline = fLumaOnly;
				fBaseline.receivedY = anchorY;
				StandInRegistry regBase;
				const auto resBaseline = eng.ProcessFrame(fBaseline, &regBase);
				AssertPlanesBitExact(resBaseline.outputCb, resLumaOnly.outputCb,
					L"H5 invariant: changing curY MUST have bit-exact ZERO effect on output Cb");
				AssertPlanesBitExact(resBaseline.outputCr, resLumaOnly.outputCr,
					L"H5 invariant: changing curY MUST have bit-exact ZERO effect on output Cr");

				const ActiveRect cFull = ActiveRect::Full(w / 2, h / 2);
				const double tdShiftCb = MaskedRmse(anchorCb, resLumaOnly.outputCb, cFull);
				const double tdShiftCr = MaskedRmse(anchorCr, resLumaOnly.outputCr, cFull);
				Assert::IsTrue(tdShiftCb < 1e-2 && tdShiftCr < 1e-2,
					L"Carried CfL H5 must produce essentially zero spurious chroma shift on luma-only residual");

				// ---- Mandatory In-Test Negative Control for Part A (Rule 3) ----
				// Naive CfL applied directly to current frame's luma curY (a * curY + b) on the EXACT SAME input!
				TdV2Plane naiveCflCb(w / 2, h / 2, 512.0), naiveCflCr(w / 2, h / 2, 512.0);
				for (int cy = 0; cy < h / 2; ++cy)
				{
					const int ly = static_cast<int>(siting.CodedToLumaY(static_cast<double>(cy), 2));
					const int mcy = (std::min)(ch - 1, ly / 16);
					for (int cx = 0; cx < w / 2; ++cx)
					{
						const int lx = static_cast<int>(siting.CodedToLumaX(static_cast<double>(cx), 2));
						const int mcx = (std::min)(cw - 1, lx / 16);
						const AnchorCflCell& cfl = eng.AnchorCanvas().CflCells()[static_cast<size_t>(mcy) * cw + mcx];
						const double curYVal = shiftedCurY.at(lx, ly);
						naiveCflCb.at(cx, cy) = cfl.aCb * curYVal + cfl.bCb;
						naiveCflCr.at(cx, cy) = cfl.aCr * curYVal + cfl.bCr;
					}
				}
				const double naiveLeakCb = MaskedRmse(anchorCb, naiveCflCb, cFull);
				const double naiveLeakCr = MaskedRmse(anchorCr, naiveCflCr, cFull);
				Assert::IsTrue(naiveLeakCb > 60.0 && naiveLeakCr > 50.0,
					L"Negative control: naive CfL applied to curY MUST leak massive false color into chroma");

				// ---- Part B: Isoluminant / Chroma-Only Change Beyond 3-Sigma -> Visible CfL Fallback ----
				// Luma stays identical to anchor (so P_Y predicts old anchor chroma), while actual chroma shifts by +85 / -75!
				TdV2Plane isoCb = anchorCb, isoCr = anchorCr;
				for (size_t i = 0; i < isoCb.v.size(); ++i)
				{
					isoCb.v[i] += 85.0;
					isoCr.v[i] -= 75.0;
				}
				const TdV2Plane isoRefCb = AddGaussianNoise(isoCb, 0.5, 421, cFull);
				const TdV2Plane isoRefCr = AddGaussianNoise(isoCr, 0.5, 422, cFull);

				// B1: Only CfL reference present -> every cell disagrees beyond 3*sigma, CfL rejected visibly, output untouched!
				ChromaProcessInput fIso;
				fIso.seq = 3;
				fIso.deliveredFormat = DeliveredChromaFormat::Yuv420;
				fIso.codedSubsampling = ChromaSubsampling::Yuv420;
				fIso.gridStatus = ChromaGridStatus::Identified;
				fIso.siting = siting;
				fIso.lumaW = w;
				fIso.lumaH = h;
				fIso.activeRect = active;
				fIso.receivedY = anchorY;
				fIso.receivedCb = isoCb;
				fIso.receivedCr = isoCr;
				fIso.hasCleanSplit = true;
				fIso.cleanCb = isoCb;
				fIso.cleanCr = isoCr;
				fIso.hasAnchorReadMotion = true;
				fIso.anchorReadMotion = stillMf;
				fIso.enableAnchorCanvasRef = false;
				fIso.enableCarriedCfl = true;
				fIso.hasStepPrior = true;
				fIso.estimatedStepDelta = 8.0; // stepVar ~= 5.33 -> 3-sigma thresh ~= 48 << 85^2 = 7225!

				StandInRegistry regIso;
				const auto resIso = eng.ProcessFrame(fIso, &regIso);
				Assert::IsTrue(regIso.IsActive(StandInKind::ChromaCflFallback),
					L"Isoluminant chroma change beyond 3-sigma MUST visibly engage StandInKind::ChromaCflFallback");
				Assert::IsTrue(resIso.cflFallbackCells == static_cast<size_t>(cw * ch),
					L"Every disagreed cell must increment cflFallbackCells");
				AssertPlanesBitExact(isoCb, resIso.outputCb,
					L"Rejected stale CfL must NOT corrupt isoluminant Cb");
				AssertPlanesBitExact(isoCr, resIso.outputCr,
					L"Rejected stale CfL must NOT corrupt isoluminant Cr");

				// B2: With a valid chroma prevPeak alongside stale CfL -> CfL still visibly rejected, prevPeak cleanly used!
				ChromaProcessInput fIsoWithRef = fIso;
				fIsoWithRef.seq = 4;
				fIsoWithRef.prevPeak = { &isoRefCb, &isoRefCr, &stillMf, false };
				StandInRegistry regIsoRef;
				const auto resIsoRef = eng.ProcessFrame(fIsoWithRef, &regIsoRef);
				Assert::IsTrue(regIsoRef.IsActive(StandInKind::ChromaCflFallback));
				Assert::IsTrue(resIsoRef.cflFallbackCells > 0);
				const double errIsoRefCb = MaskedRmse(isoCb, resIsoRef.outputCb, cFull);
				const double errIsoRefCr = MaskedRmse(isoCr, resIsoRef.outputCr, cFull);

				// Negative control for Part B: naive un-guarded blend with stale CfL prediction corrupts isoluminant chroma!
				TdV2Plane staleCflCb, staleCflCr;
				std::vector<uint8_t> cflValid;
				Assert::IsTrue(eng.AnchorCanvas().PredictCarriedCflAtCodedGrid(
					ChromaSubsampling::Yuv420, siting, active, &stillMf, 16, staleCflCb, staleCflCr, cflValid));
				TdV2Plane naiveIsoBlendCb(w / 2, h / 2), naiveIsoBlendCr(w / 2, h / 2);
				for (size_t i = 0; i < isoCb.v.size(); ++i)
				{
					naiveIsoBlendCb.v[i] = 0.5 * (isoCb.v[i] + staleCflCb.v[i]);
					naiveIsoBlendCr.v[i] = 0.5 * (isoCr.v[i] + staleCflCr.v[i]);
				}
				const double naiveIsoErrCb = MaskedRmse(isoCb, naiveIsoBlendCb, cFull);
				const double naiveIsoErrCr = MaskedRmse(isoCr, naiveIsoBlendCr, cFull);

				Assert::IsTrue(naiveIsoErrCb > 35.0 && naiveIsoErrCr > 30.0,
					L"Negative control: un-guarded stale CfL blend MUST heavily corrupt isoluminant chroma");
				Assert::IsTrue(errIsoRefCb < 1.0 && errIsoRefCr < 1.0,
					L"With CfL fallback engaged, isoluminant chroma must stay uncorrupted by stale CfL slope");

				// ---- Part C (MEDIUM-5): Positive chroma recovery from carried CfL H5 under non-zero pan ----
				{
					const int pw = 96, ph = 96;
					const int pcw = pw / 16, pch = ph / 16;
					const ActiveRect pActive = ActiveRect::Full(pw, ph);
					const ActiveRect pcActive = ActiveRect::Full(pw / 2, ph / 2);
					const ActiveRect pcInterior{ 8, 8, pw / 2 - 8, ph / 2 - 8 };
					const SitingSpec pSiting = SitingSpec::Left();
					const double panVx = +1.5, panVy = -1.5; // sub-pixel non-zero pan (Current -> Anchor)

					auto sceneY = [](double x, double y) -> double
					{
						return 420.0 + 130.0 * std::sin(2.0 * kPi * x / 22.0) * std::cos(2.0 * kPi * y / 19.0)
						             + 55.0 * std::cos(2.0 * kPi * (x - y) / 17.0);
					};

					TdV2Plane pAnchorY(pw, ph, 420.0);
					for (int y = 0; y < ph; ++y)
						for (int x = 0; x < pw; ++x)
							pAnchorY.at(x, y) = sceneY(static_cast<double>(x), static_cast<double>(y));

					TdV2Plane pAnchorCb(pw / 2, ph / 2, 512.0), pAnchorCr(pw / 2, ph / 2, 512.0);
					TdV2Plane pValTrueCb(pw / 2, ph / 2, 512.0), pValTrueCr(pw / 2, ph / 2, 512.0);
					for (int cy = 0; cy < ph / 2; ++cy)
					{
						const double ly = pSiting.CodedToLumaY(static_cast<double>(cy), 2);
						for (int cx = 0; cx < pw / 2; ++cx)
						{
							const double lx = pSiting.CodedToLumaX(static_cast<double>(cx), 2);
							const double yAnc = sceneY(lx, ly);
							pAnchorCb.at(cx, cy) = 512.0 + trueSlopeCb * (yAnc - 420.0);
							pAnchorCr.at(cx, cy) = 512.0 + trueSlopeCr * (yAnc - 420.0);
							// Panned valley at (lx, ly) observes Anchor at (lx + panVx, ly + panVy).
							const double yPan = sceneY(lx + panVx, ly + panVy);
							pValTrueCb.at(cx, cy) = 512.0 + trueSlopeCb * (yPan - 420.0);
							pValTrueCr.at(cx, cy) = 512.0 + trueSlopeCr * (yPan - 420.0);
						}
					}
					const TdV2Plane pValNoisyCb = AddGaussianNoise(pValTrueCb, 10.0, 441, pcActive);
					const TdV2Plane pValNoisyCr = AddGaussianNoise(pValTrueCr, 10.0, 442, pcActive);

					Config pCfg = MakePreset(Preset::Reference, Mode::On);
					pCfg.estimator = VarianceEstimator::A2_RoundingCells;
					TdV2ChromaEngine pEng(pCfg);
					StandInRegistry pRegAnc;

					ChromaProcessInput pAncIn;
					pAncIn.seq = 10;
					pAncIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
					pAncIn.codedSubsampling = ChromaSubsampling::Yuv420;
					pAncIn.gridStatus = ChromaGridStatus::Identified;
					pAncIn.siting = pSiting;
					pAncIn.lumaW = pw;
					pAncIn.lumaH = ph;
					pAncIn.activeRect = pActive;
					pAncIn.receivedY = pAnchorY;
					pAncIn.receivedCb = pAnchorCb;
					pAncIn.receivedCr = pAnchorCr;
					pAncIn.hasCleanSplit = true;
					pAncIn.cleanCb = pAnchorCb;
					pAncIn.cleanCr = pAnchorCr;
					pAncIn.isAnchorFrame = true;
					pAncIn.anchorCleanY = pAnchorY;
					pAncIn.hasStepPrior = true;
					pAncIn.estimatedStepDelta = 24.0;
					pEng.ProcessFrame(pAncIn, &pRegAnc);
					Assert::IsTrue(pEng.AnchorCanvas().HasCfl());

					const CellMotionField panMf = CellMotionField::Uniform(pcw, pch, panVx, panVy, panVx, panVy, RegionMotionClass::Camera);
					const CellMotionField wrongSignMf = CellMotionField::Uniform(pcw, pch, -panVx, -panVy, -panVx, -panVy, RegionMotionClass::Camera);

					// Positive recovery: anchor canvas ref DISABLED so carried CfL H5 carries 100% of the recovery!
					ChromaProcessInput pValIn = pAncIn;
					pValIn.seq = 11;
					pValIn.isAnchorFrame = false;
					pValIn.receivedCb = pValNoisyCb;
					pValIn.receivedCr = pValNoisyCr;
					pValIn.cleanCb = pValNoisyCb;
					pValIn.cleanCr = pValNoisyCr;
					pValIn.enableAnchorCanvasRef = false;
					pValIn.enableCarriedCfl = true;
					pValIn.hasAnchorReadMotion = true;
					pValIn.anchorReadMotion = panMf;
					StandInRegistry pRegVal;
					const auto resPanCfl = pEng.ProcessFrame(pValIn, &pRegVal);

					Assert::IsTrue(resPanCfl.carriedCflUsed && !resPanCfl.anchorCanvasUsed);
					Assert::AreEqual(static_cast<size_t>(1), resPanCfl.activeRefCount);

					// Negative Control 1: CfL disabled -> zero recovery!
					ChromaProcessInput pOffIn = pValIn;
					pOffIn.enableCarriedCfl = false;
					StandInRegistry pRegOff;
					const auto resPanOff = pEng.ProcessFrame(pOffIn, &pRegOff);

					// Negative Control 2: Wrong motion sign -> fails to recover accurately!
					ChromaProcessInput pWrongIn = pValIn;
					pWrongIn.anchorReadMotion = wrongSignMf;
					StandInRegistry pRegWrong;
					const auto resPanWrong = pEng.ProcessFrame(pWrongIn, &pRegWrong);

					const double baseErrCb = MaskedRmse(pValTrueCb, resPanOff.outputCb, pcInterior);
					const double cflErrCb = MaskedRmse(pValTrueCb, resPanCfl.outputCb, pcInterior);
					const double wrongErrCb = MaskedRmse(pValTrueCb, resPanWrong.outputCb, pcInterior);
					const double baseErrCr = MaskedRmse(pValTrueCr, resPanOff.outputCr, pcInterior);
					const double cflErrCr = MaskedRmse(pValTrueCr, resPanCfl.outputCr, pcInterior);
					const double wrongErrCr = MaskedRmse(pValTrueCr, resPanWrong.outputCr, pcInterior);

					char cflBuf[256];
					std::snprintf(cflBuf, sizeof(cflBuf),
						"Carried CfL H5 pan recovery: Cb base=%.2f cfl=%.2f wrongSign=%.2f | Cr base=%.2f cfl=%.2f wrongSign=%.2f",
						baseErrCb, cflErrCb, wrongErrCb, baseErrCr, cflErrCr, wrongErrCr);
					Logger::WriteMessage(cflBuf);

					Assert::IsTrue(cflErrCb < baseErrCb * 0.65 && cflErrCr < baseErrCr * 0.65,
						L"Carried CfL H5 under non-zero pan must strongly reduce noisy valley chroma RMSE");
					Assert::IsTrue(wrongErrCb > cflErrCb + 2.0 && wrongErrCr > cflErrCr + 2.0,
						L"Negative control: wrong motion sign on carried CfL H5 MUST fail to recover as cleanly");
				}
			}

			// =========================================================================
			// 5. Unidentified Coded Grid Runs Visible Coarse-Band-Only Mode (M/H == 0)
			// =========================================================================
			TEST_METHOD(UnidentifiedCodedGridRunsVisibleCoarseBandOnly)
			{
				// 256x256 with interior [104, 152) (48x48) strictly beyond the 3*sigma = 96 px G(32.0) border-replication
				// radius (kUnidentifiedChromaSigmaLumaPx = 32.0 luma px = 8 px on the 1/4 luma grid per plan s1.5 L168!).
				const int w = 256, h = 256;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect interior{ 104, 104, 152, 152 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Construct signal with BOTH:
				//   (1) Low-frequency chroma banding (gentle sky-like gradient quantized to 16-code bands -> ~320 px wide bands
				//       that genuinely live inside the sigma = 32 luma px coarse band, baseCoarseErr > 1.5 codes)
				//   (2) High-frequency chroma detail (period-3 px sinusoid, strictly above sigma = 32 px coarse band,
				//       present ONLY in current frame and absent in peaks so any full-band blend would destroy it!)
				TdV2Plane masterLowCb(w, h, 512.0), masterLowCr(w, h, 512.0);
				TdV2Plane hfDetailCb(w, h, 0.0), hfDetailCr(w, h, 0.0);
				TdV2Plane curNoHfCb(w, h, 512.0), curNoHfCr(w, h, 512.0);
				TdV2Plane curCb(w, h, 512.0), curCr(w, h, 512.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						// Genuine wide-band sky chroma gradient quantized to 16-code steps (band step around x ~= 84,
						// yielding ~3.5+ codes of smooth coarse-band error across interior [104, 152)^2 at sigma = 32).
						const double lowCb = 482.5 + 0.05 * x + 0.01 * y;
						const double lowCr = 541.5 - 0.05 * x - 0.01 * y;
						masterLowCb.at(x, y) = lowCb;
						masterLowCr.at(x, y) = lowCr;

						// Coarse 16-code banding error on low-frequency ramp.
						const double bandCb = 16.0 * std::floor(lowCb / 16.0 + 0.5);
						const double bandCr = 16.0 * std::floor(lowCr / 16.0 + 0.5);

						// High-frequency period-3 px chroma detail (suppressed to truncated 3-sigma sidelobe floor in G(32)!).
						const double hfCb = 22.0 * std::sin(2.0 * kPi * x / 3.0) * std::cos(2.0 * kPi * y / 3.0);
						const double hfCr = 18.0 * std::cos(2.0 * kPi * x / 3.0) * std::sin(2.0 * kPi * y / 3.0);
						hfDetailCb.at(x, y) = hfCb;
						hfDetailCr.at(x, y) = hfCr;

						curNoHfCb.at(x, y) = bandCb;
						curNoHfCr.at(x, y) = bandCr;
						curCb.at(x, y) = bandCb + hfCb;
						curCr.at(x, y) = bandCr + hfCr;
					}
				}
				const TdV2Plane prevCb = AddGaussianNoise(masterLowCb, 0.4, 501, active);
				const TdV2Plane prevCr = AddGaussianNoise(masterLowCr, 0.4, 502, active);
				const TdV2Plane nextCb = AddGaussianNoise(masterLowCb, 0.4, 503, active);
				const TdV2Plane nextCr = AddGaussianNoise(masterLowCr, 0.4, 504, active);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ChromaEngine eng(cfg);
				StandInRegistry reg;

				ChromaProcessInput in;
				in.seq = 40;
				in.deliveredFormat = DeliveredChromaFormat::Yuv444;
				in.codedSubsampling = ChromaSubsampling::Yuv444;
				in.gridStatus = ChromaGridStatus::Unidentified;
				in.lumaW = w;
				in.lumaH = h;
				in.activeRect = active;
				in.receivedCb = curCb;
				in.receivedCr = curCr;
				in.hasCleanSplit = true;
				in.cleanCb = curCb;
				in.cleanCr = curCr;
				in.prevPeak = { &prevCb, &prevCr, &stillMf, false };
				in.nextPeak = { &nextCb, &nextCr, &stillMf, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 16.0;

				const auto res = eng.ProcessFrame(in, &reg);

				// Paired run WITHOUT the planted period-3 px HF sinusoid to prove exact HF-detail invariance (< 1e-6)!
				TdV2ChromaEngine engNoHf(cfg);
				StandInRegistry regNoHf;
				ChromaProcessInput inNoHf = in;
				inNoHf.receivedCb = curNoHfCb;
				inNoHf.receivedCr = curNoHfCr;
				inNoHf.cleanCb = curNoHfCb;
				inNoHf.cleanCr = curNoHfCr;
				const auto resNoHf = engNoHf.ProcessFrame(inNoHf, &regNoHf);

				// Visibility & fallback flags (Rule 4).
				Assert::IsTrue(res.coarseBandOnly, L"Unidentified grid MUST set res.coarseBandOnly == true");
				Assert::IsTrue(reg.IsActive(StandInKind::ChromaGridDetector),
					L"Unidentified grid MUST visibly engage StandInKind::ChromaGridDetector");
				Assert::IsTrue(reg.IsActive(StandInKind::TdCgChromaGrid),
					L"ChromaGridDetector alias TdCgChromaGrid MUST be active");
				const std::string statusSummary = reg.FormatStatusSummary();
				Assert::IsTrue(statusSummary.find("coarse band only") != std::string::npos,
					L"Status summary MUST visibly report 'coarse band only'");

				// Check 1: Coarse banding error is genuinely reduced!
				double dummyGain = 1.0;
				const TdV2Plane curCoarseCb = ChromaDetail::BlurActiveAnisotropic(curCb, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const TdV2Plane outCoarseCb = ChromaDetail::BlurActiveAnisotropic(res.outputCb, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const TdV2Plane refCoarseCb = ChromaDetail::BlurActiveAnisotropic(masterLowCb, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const TdV2Plane curCoarseCr = ChromaDetail::BlurActiveAnisotropic(curCr, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const TdV2Plane refCoarseCr = ChromaDetail::BlurActiveAnisotropic(masterLowCr, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const double baseCoarseErr = MaskedRmse(refCoarseCb, curCoarseCb, interior);
				const double outCoarseErr = MaskedRmse(refCoarseCb, outCoarseCb, interior);
				Assert::IsTrue(baseCoarseErr > 1.5, L"Test fixture must present genuine visible low-frequency chroma banding (> 1.5 codes)");

				// Check 2A: Direct HF-detail invariance proof — planted period-3 px HF sinusoid affects delta ONLY at the
				// exact linear 3-sigma-truncated G(8.0) sidelobe floor (~2e-6 codes, > 3,000,000x below naive full-band!).
				const TdV2Plane zeroPlane(w, h, 0.0);
				const TdV2Plane idealHfLeakPlaneCb = ChromaDetail::BlurActiveAnisotropic(hfDetailCb, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const TdV2Plane idealHfLeakPlaneCr = ChromaDetail::BlurActiveAnisotropic(hfDetailCr, active, kUnidentifiedChromaSigmaLumaPx, kUnidentifiedChromaSigmaLumaPx, dummyGain);
				const double idealHfLeakCb = MaskedRmse(zeroPlane, idealHfLeakPlaneCb, interior);
				const double idealHfLeakCr = MaskedRmse(zeroPlane, idealHfLeakPlaneCr, interior);
				const double hfDeltaDiffCb = MaskedRmse(res.deliveredDeltaCb, resNoHf.deliveredDeltaCb, interior);
				const double hfDeltaDiffCr = MaskedRmse(res.deliveredDeltaCr, resNoHf.deliveredDeltaCr, interior);

				// Check 2B: Exact derived linear G(8) filter-overlap bounds on DecomposeActiveBands(delta):
				// Because G(1.0)/G(2.5) are smooth non-brick-wall Gaussians, even an ideal pure-G(8) coarse delta
				// (refCoarse - curCoarse) has small analytical passband overlap with M/H; actual delta MUST stay <= 1.15x ideal!
				TdV2Plane idealCoarseDeltaCb(w, h, 0.0), idealCoarseDeltaCr(w, h, 0.0);
				for (size_t i = 0; i < idealCoarseDeltaCb.v.size(); ++i)
				{
					idealCoarseDeltaCb.v[i] = refCoarseCb.v[i] - curCoarseCb.v[i];
					idealCoarseDeltaCr.v[i] = refCoarseCr.v[i] - curCoarseCr.v[i];
				}
				const ThreeBandPlanes idealBandsCb = DecomposeActiveBands(idealCoarseDeltaCb, active);
				const ThreeBandPlanes idealBandsCr = DecomposeActiveBands(idealCoarseDeltaCr, active);
				const ThreeBandPlanes deltaBandsCb = DecomposeActiveBands(res.deliveredDeltaCb, active);
				const ThreeBandPlanes deltaBandsCr = DecomposeActiveBands(res.deliveredDeltaCr, active);
				const ThreeBandPlanes curBandsCb = DecomposeActiveBands(curCb, active);
				const ThreeBandPlanes outBandsCb = DecomposeActiveBands(res.outputCb, active);

				const double idealFineOverlapCb = MaskedRmse(zeroPlane, idealBandsCb.H, interior);
				const double idealMidOverlapCb = MaskedRmse(zeroPlane, idealBandsCb.M, interior);
				const double idealFineOverlapCr = MaskedRmse(zeroPlane, idealBandsCr.H, interior);
				const double idealMidOverlapCr = MaskedRmse(zeroPlane, idealBandsCr.M, interior);

				const double deltaFineRmsCb = MaskedRmse(zeroPlane, deltaBandsCb.H, interior);
				const double deltaMidRmsCb = MaskedRmse(zeroPlane, deltaBandsCb.M, interior);
				const double deltaFineRmsCr = MaskedRmse(zeroPlane, deltaBandsCr.H, interior);
				const double deltaMidRmsCr = MaskedRmse(zeroPlane, deltaBandsCr.M, interior);
				const double hfPreservErrCb = MaskedRmse(curBandsCb.H, outBandsCb.H, interior);

				// ---- Mandatory In-Test Negative Control (Rule 3) ----
				// Naive full-band chroma blend on the EXACT SAME unidentified-grid input destroys high-frequency chroma!
				TdV2Plane naiveFullCb(w, h, 512.0);
				for (size_t i = 0; i < curCb.v.size(); ++i)
					naiveFullCb.v[i] = (curCb.v[i] + prevCb.v[i] + nextCb.v[i]) / 3.0;
				const ThreeBandPlanes naiveBandsCb = DecomposeActiveBands(naiveFullCb, active);
				const double naiveHfCorruptErr = MaskedRmse(curBandsCb.H, naiveBandsCb.H, interior);

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Unidentified grid coarse-only: coarseErr %.3f->%.3f | hfInvar=%.2e(floor=%.2e)/%.2e(floor=%.2e) | delta H=%.5f(ideal=%.5f) M=%.5f(ideal=%.5f) | naiveFullHF=%.2f",
					baseCoarseErr, outCoarseErr, hfDeltaDiffCb, idealHfLeakCb, hfDeltaDiffCr, idealHfLeakCr,
					deltaFineRmsCb, idealFineOverlapCb, deltaMidRmsCb, idealMidOverlapCb, naiveHfCorruptErr);
				Logger::WriteMessage(buf);

				Assert::IsTrue(outCoarseErr < baseCoarseErr * 0.65,
					L"Coarse-band-only mode must genuinely reduce low-frequency chroma banding by >= 35%");
				Assert::IsTrue(naiveHfCorruptErr > 5.0,
					L"Negative control: naive full-band chroma blend MUST severely corrupt high-frequency chroma");
				// IEEE-754 FP32 moment-accumulator quantization floor (MomentAccumulatorFp32 stores diffs in float32;
				// at sigma=32 the G(32) sidelobe reaches ~1.8e-7, right at 4*FLT_EPSILON*baseCoarseErr).
				const double fp32MomentFloor = 4.0 * static_cast<double>(std::numeric_limits<float>::epsilon()) * baseCoarseErr;
				Assert::IsTrue(hfDeltaDiffCb <= (std::max)(1.25 * idealHfLeakCb, fp32MomentFloor) && hfDeltaDiffCb < 1e-5,
					L"Direct HF-detail invariance: planted period-3 px HF Cb must stay at exact truncated G(32)/FP32 floor (< 1e-5)");
				Assert::IsTrue(hfDeltaDiffCr <= (std::max)(1.25 * idealHfLeakCr, fp32MomentFloor) && hfDeltaDiffCr < 1e-5,
					L"Direct HF-detail invariance: planted period-3 px HF Cr must stay at exact truncated G(32)/FP32 floor (< 1e-5)");
				Assert::IsTrue(deltaFineRmsCb <= 1.15 * idealFineOverlapCb && deltaMidRmsCb <= 1.15 * idealMidOverlapCb,
					L"Cb coarse-only delta M/H must stay within 1.15x exact linear G(32) filter overlap");
				Assert::IsTrue(deltaFineRmsCr <= 1.15 * idealFineOverlapCr && deltaMidRmsCr <= 1.15 * idealMidOverlapCr,
					L"Cr coarse-only delta M/H must stay within 1.15x exact linear G(32) filter overlap");
				Assert::IsTrue(hfPreservErrCb <= 1.15 * idealFineOverlapCb,
					L"Unidentified grid coarse-only mode must leave high-frequency chroma untouched within exact G(32) overlap");
			}

			// =========================================================================
			// 6. Chroma Grain Preserved Untouched Across 4:2:0 and RGB Deliveries
			// =========================================================================
			TEST_METHOD(ChromaGrainPreservedUntouchedAcrossDeliveredFormats)
			{
				const int w = 128, h = 128;
				const int cw = w / 16, ch = h / 16;
				const int cW = w / 2, cH = h / 2;
				const size_t cN = static_cast<size_t>(cW) * cH;
				const size_t fN = static_cast<size_t>(w) * h;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect cActive = ActiveRect::Full(cW, cH);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const SitingSpec siting = SitingSpec::Left();
				const YCbCrMatrixSpec mat = YCbCrMatrixSpec::Bt709();
				const double midCode = 512.0;

				const double grainSigma = 16.0;
				const double leakFrac = 0.15; // realistic >= 10% imperfect grain separation (non-oracle; exercises Layer 2!)
				const double leakedVar = (leakFrac * grainSigma) * (leakFrac * grainSigma);
				const std::vector<double> retainedVar(static_cast<size_t>(cw) * ch, leakedVar);

				auto makeLowMidWear = [&](const TdV2Plane& base, double rawSigma, uint32_t seed, const ActiveRect& ar) -> TdV2Plane
				{
					const TdV2Plane zeroPlane(base.w, base.h, 0.0);
					const TdV2Plane rawNoise = AddGaussianNoise(zeroPlane, rawSigma, seed, ar);
					const ThreeBandPlanes nb = DecomposeActiveBands(rawNoise, ar);
					TdV2Plane out = base;
					for (size_t i = 0; i < out.v.size(); ++i)
						out.v[i] += nb.L.v[i] + nb.M.v[i];
					return out;
				};

				// ---- Sub-case A: Delivered 4:2:0 (coded 4:2:0) ----
				{
					const TdV2Plane masterCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
					const TdV2Plane masterCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);

					// Genuine independent Frame-1 and Frame-2 L+M chroma compression wear (so L+M repairs strongly
					// while H-band carries 15% leaked film grain that Layer 2 MUST cancel via retainedGrainVar!).
					const TdV2Plane prevFrameCleanTrueCb = makeLowMidWear(masterCb, 12.0, 607, cActive);
					const TdV2Plane prevFrameCleanTrueCr = makeLowMidWear(masterCr, 12.0, 608, cActive);
					const TdV2Plane curCleanTrueCb = makeLowMidWear(masterCb, 12.0, 601, cActive);
					const TdV2Plane curCleanTrueCr = makeLowMidWear(masterCr, 12.0, 602, cActive);
					const TdV2Plane prevCleanTrueCb = AddGaussianNoise(masterCb, 0.5, 603, cActive);
					const TdV2Plane prevCleanTrueCr = AddGaussianNoise(masterCr, 0.5, 604, cActive);
					const TdV2Plane nextCleanTrueCb = AddGaussianNoise(masterCb, 0.5, 605, cActive);
					const TdV2Plane nextCleanTrueCr = AddGaussianNoise(masterCr, 0.5, 606, cActive);

					std::mt19937 rngOwn(6101), rngForPrev(6102), rngForNext(6103), rngPrevFrame(6104);
					std::normal_distribution<double> gd(0.0, grainSigma);
					std::vector<double> prevFrameOwnCb(cN), prevFrameOwnCr(cN);
					std::vector<double> ownCb(cN), ownCr(cN), forPrevCb(cN), forPrevCr(cN), forNextCb(cN), forNextCr(cN);
					TdV2Plane prevFrameRecCb(cW, cH), prevFrameRecCr(cW, cH);
					TdV2Plane prevFrameCleanImpCb(cW, cH), prevFrameCleanImpCr(cW, cH);
					TdV2Plane recCb(cW, cH), recCr(cW, cH), prevRecCb(cW, cH), prevRecCr(cW, cH), nextRecCb(cW, cH), nextRecCr(cW, cH);
					TdV2Plane curCleanImpCb(cW, cH), curCleanImpCr(cW, cH);
					TdV2Plane prevCleanImpCb(cW, cH), prevCleanImpCr(cW, cH), nextCleanImpCb(cW, cH), nextCleanImpCr(cW, cH);

					for (size_t i = 0; i < cN; ++i)
					{
						prevFrameOwnCb[i] = gd(rngPrevFrame);
						prevFrameOwnCr[i] = gd(rngPrevFrame);
						ownCb[i] = gd(rngOwn);
						ownCr[i] = gd(rngOwn);
						forPrevCb[i] = gd(rngForPrev);
						forPrevCr[i] = gd(rngForPrev);
						forNextCb[i] = gd(rngForNext);
						forNextCr[i] = gd(rngForNext);

						prevFrameRecCb.v[i] = prevFrameCleanTrueCb.v[i] + prevFrameOwnCb[i];
						prevFrameRecCr.v[i] = prevFrameCleanTrueCr.v[i] + prevFrameOwnCr[i];
						recCb.v[i] = curCleanTrueCb.v[i] + ownCb[i];
						recCr.v[i] = curCleanTrueCr.v[i] + ownCr[i];
						prevRecCb.v[i] = prevCleanTrueCb.v[i] + forPrevCb[i];
						prevRecCr.v[i] = prevCleanTrueCr.v[i] + forPrevCr[i];
						nextRecCb.v[i] = nextCleanTrueCb.v[i] + forNextCb[i];
						nextRecCr.v[i] = nextCleanTrueCr.v[i] + forNextCr[i];

						prevFrameCleanImpCb.v[i] = prevFrameCleanTrueCb.v[i] + leakFrac * prevFrameOwnCb[i];
						prevFrameCleanImpCr.v[i] = prevFrameCleanTrueCr.v[i] + leakFrac * prevFrameOwnCr[i];
						curCleanImpCb.v[i] = curCleanTrueCb.v[i] + leakFrac * ownCb[i];
						curCleanImpCr.v[i] = curCleanTrueCr.v[i] + leakFrac * ownCr[i];
						prevCleanImpCb.v[i] = prevCleanTrueCb.v[i] + leakFrac * forPrevCb[i];
						prevCleanImpCr.v[i] = prevCleanTrueCr.v[i] + leakFrac * forPrevCr[i];
						nextCleanImpCb.v[i] = nextCleanTrueCb.v[i] + leakFrac * forNextCb[i];
						nextCleanImpCr.v[i] = nextCleanTrueCr.v[i] + leakFrac * forNextCr[i];
					}

					// Mandatory In-Test Negative Control #1 (Rule 3): naive un-separated chroma average on EXACT SAME input!
					std::vector<double> naiveAvgCb(cN), naiveAvgCr(cN), naivePrevResCb(cN), naivePrevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						naivePrevResCb[i] = (prevFrameRecCb.v[i] + prevRecCb.v[i] + nextRecCb.v[i]) / 3.0 - masterCb.v[i];
						naivePrevResCr[i] = (prevFrameRecCr.v[i] + prevRecCr.v[i] + nextRecCr.v[i]) / 3.0 - masterCr.v[i];
						naiveAvgCb[i] = (recCb.v[i] + prevRecCb.v[i] + nextRecCb.v[i]) / 3.0;
						naiveAvgCr[i] = (recCr.v[i] + prevRecCr.v[i] + nextRecCr.v[i]) / 3.0;
					}
					const auto naiveRepCb = EvaluateGrainTransfer(
						masterCb.v.data(), ownCb.data(), forPrevCb.data(), naiveAvgCb.data(), cN,
						naivePrevResCb.data(), prevFrameOwnCb.data());
					const auto naiveRepCr = EvaluateGrainTransfer(
						masterCr.v.data(), ownCr.data(), forPrevCr.data(), naiveAvgCr.data(), cN,
						naivePrevResCr.data(), prevFrameOwnCr.data());
					Assert::IsTrue(naiveRepCb.valid && naiveRepCr.valid && naiveRepCb.lag1Evaluated && naiveRepCr.lag1Evaluated);
					Assert::IsFalse(naiveRepCb.passesAuthoritativeBar, L"Negative control: naive 4:2:0 average MUST fail grain bar");
					Assert::IsTrue(naiveRepCb.betaOwn < 0.90 && naiveRepCb.betaForeign > 0.10,
						L"Negative control: naive 4:2:0 average destroys own grain and leaks foreign grain");
					Assert::IsTrue(naiveRepCr.betaOwn < 0.90 && naiveRepCr.betaForeign > 0.10);

					// Run TdV2ChromaEngine on 4:2:0 across Frame 1 & Frame 2 so lag-1 is genuinely measured (H11)!
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in1;
					in1.seq = 49;
					in1.deliveredFormat = DeliveredChromaFormat::Yuv420;
					in1.codedSubsampling = ChromaSubsampling::Yuv420;
					in1.gridStatus = ChromaGridStatus::Identified;
					in1.siting = siting;
					in1.lumaW = w;
					in1.lumaH = h;
					in1.activeRect = active;
					in1.receivedCb = prevFrameRecCb;
					in1.receivedCr = prevFrameRecCr;
					in1.hasCleanSplit = true;
					in1.cleanCb = prevFrameCleanImpCb;
					in1.cleanCr = prevFrameCleanImpCr;
					in1.retainedGrainVarCb = retainedVar;
					in1.retainedGrainVarCr = retainedVar;
					in1.prevPeak = { &prevCleanImpCb, &prevCleanImpCr, &stillMf, false };
					in1.nextPeak = { &nextCleanImpCb, &nextCleanImpCr, &stillMf, false };
					in1.hasStepPrior = true;
					in1.estimatedStepDelta = 12.0;
					const auto res1 = eng.ProcessFrame(in1, &reg);

					ChromaProcessInput in = in1;
					in.seq = 50;
					in.receivedCb = recCb;
					in.receivedCr = recCr;
					in.cleanCb = curCleanImpCb;
					in.cleanCr = curCleanImpCr;
					const auto res = eng.ProcessFrame(in, &reg);

					// Mandatory In-Test Negative Control #2 (Rule 3 / Test-Diff HIGH-5):
					// Run TdV2ChromaEngine on the EXACT SAME 15%-leaked clean planes with Layer-2 retainedGrainVar DISABLED (empty)!
					// Proves 15% leaked grain genuinely breaks the authoritative bar without Layer-2 cancellation!
					{
						TdV2ChromaEngine engNoL2(cfg);
						StandInRegistry regNoL2;
						ChromaProcessInput in1NoL2 = in1;
						in1NoL2.retainedGrainVarCb.clear();
						in1NoL2.retainedGrainVarCr.clear();
						const auto res1NoL2 = engNoL2.ProcessFrame(in1NoL2, &regNoL2);
						ChromaProcessInput inNoL2 = in;
						inNoL2.retainedGrainVarCb.clear();
						inNoL2.retainedGrainVarCr.clear();
						const auto resNoL2 = engNoL2.ProcessFrame(inNoL2, &regNoL2);

						std::vector<double> prevResNoL2Cb(cN), prevResNoL2Cr(cN);
						for (size_t i = 0; i < cN; ++i)
						{
							prevResNoL2Cb[i] = res1NoL2.outputCb.v[i] - masterCb.v[i];
							prevResNoL2Cr[i] = res1NoL2.outputCr.v[i] - masterCr.v[i];
						}
						const auto repNoL2Cb = EvaluateGrainTransfer(
							masterCb.v.data(), ownCb.data(), forPrevCb.data(), resNoL2.outputCb.v.data(), cN,
							prevResNoL2Cb.data(), prevFrameOwnCb.data());
						const auto repNoL2Cr = EvaluateGrainTransfer(
							masterCr.v.data(), ownCr.data(), forPrevCr.data(), resNoL2.outputCr.v.data(), cN,
							prevResNoL2Cr.data(), prevFrameOwnCr.data());
						Assert::IsTrue(repNoL2Cb.valid && repNoL2Cb.lag1Evaluated && repNoL2Cr.valid && repNoL2Cr.lag1Evaluated);
						Assert::IsFalse(repNoL2Cb.passesAuthoritativeBar,
							L"Negative control: Layer-2-disabled 4:2:0 engine on 15%-leaked clean planes MUST fail authoritative grain bar");
						Assert::IsFalse(repNoL2Cr.passesAuthoritativeBar,
							L"Negative control: Layer-2-disabled 4:2:0 engine (Cr) on 15%-leaked clean planes MUST fail authoritative grain bar");
						Assert::IsTrue(std::fabs(repNoL2Cb.betaForeign) > kGrainBarMaxBetaForeign || repNoL2Cb.betaOwn < kGrainBarMinBetaOwn,
							L"Negative control: Layer-2-disabled 4:2:0 engine must exhibit real own-grain loss or foreign-grain leak");
					}

					// Prove non-trivial clean-layer repair actually occurred (not a vacuous zero-delta!).
					TdV2Plane repairedCleanCb(cW, cH);
					for (size_t i = 0; i < cN; ++i)
						repairedCleanCb.v[i] = curCleanTrueCb.v[i] + res.codedDeltaCb.v[i];
					const double baseCleanErr = MaskedRmse(masterCb, curCleanTrueCb, cActive);
					const double outCleanErr = MaskedRmse(masterCb, repairedCleanCb, cActive);
					Assert::IsTrue(outCleanErr < baseCleanErr * 0.70, L"4:2:0 chroma clean layer must genuinely improve");

					// Evaluate authoritative grain transfer strictly against independent masterCb/Cr with lag-1!
					std::vector<double> prevResCb(cN), prevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						prevResCb[i] = res1.outputCb.v[i] - masterCb.v[i];
						prevResCr[i] = res1.outputCr.v[i] - masterCr.v[i];
					}
					const auto repCb = EvaluateGrainTransfer(
						masterCb.v.data(), ownCb.data(), forPrevCb.data(), res.outputCb.v.data(), cN,
						prevResCb.data(), prevFrameOwnCb.data());
					const auto repCr = EvaluateGrainTransfer(
						masterCr.v.data(), ownCr.data(), forPrevCr.data(), res.outputCr.v.data(), cN,
						prevResCr.data(), prevFrameOwnCr.data());
					const auto repCbNext = EvaluateGrainTransfer(
						masterCb.v.data(), ownCb.data(), forNextCb.data(), res.outputCb.v.data(), cN,
						prevResCb.data(), prevFrameOwnCb.data());

					Assert::IsTrue(repCb.valid && repCb.lag1Evaluated && repCb.passesAuthoritativeBar,
						L"4:2:0 Cb (15% leak + Layer 2) must pass authoritative grain bar with lag1Evaluated==true");
					Assert::IsTrue(repCr.valid && repCr.lag1Evaluated && repCr.passesAuthoritativeBar,
						L"4:2:0 Cr (15% leak + Layer 2) must pass authoritative grain bar with lag1Evaluated==true");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(repCb.lag1Rise <= kGrainBarMaxLag1Rise);
					Assert::IsTrue(repCbNext.lag1Evaluated && std::fabs(repCbNext.betaForeign) <= kGrainBarMaxBetaForeign);
				}

				// ---- Sub-case B: Delivered RGB 4:4:4 (coded 4:2:0) ----
				{
					const TdV2Plane masterCodedCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
					const TdV2Plane masterCodedCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);
					const TdV2Plane master444Cb = UpsampleChromaPlaneSited(masterCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane master444Cr = UpsampleChromaPlaneSited(masterCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane masterY(w, h, 480.0);

					// Independent Frame-1 & Frame-2 L+M chroma compression wear on coded 4:2:0 grid, upsampled to RGB 4:4:4.
					const TdV2Plane prevFrameCleanCodedCb = makeLowMidWear(masterCodedCb, 12.0, 627, cActive);
					const TdV2Plane prevFrameCleanCodedCr = makeLowMidWear(masterCodedCr, 12.0, 628, cActive);
					const TdV2Plane curCleanCodedCb = makeLowMidWear(masterCodedCb, 12.0, 621, cActive);
					const TdV2Plane curCleanCodedCr = makeLowMidWear(masterCodedCr, 12.0, 622, cActive);
					const TdV2Plane prevCleanCodedCb = AddGaussianNoise(masterCodedCb, 0.5, 623, cActive);
					const TdV2Plane prevCleanCodedCr = AddGaussianNoise(masterCodedCr, 0.5, 624, cActive);
					const TdV2Plane nextCleanCodedCb = AddGaussianNoise(masterCodedCb, 0.5, 625, cActive);
					const TdV2Plane nextCleanCodedCr = AddGaussianNoise(masterCodedCr, 0.5, 626, cActive);

					const TdV2Plane prevFrameClean444Cb = UpsampleChromaPlaneSited(prevFrameCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane prevFrameClean444Cr = UpsampleChromaPlaneSited(prevFrameCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane curClean444Cb = UpsampleChromaPlaneSited(curCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane curClean444Cr = UpsampleChromaPlaneSited(curCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane prevClean444Cb = UpsampleChromaPlaneSited(prevCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane prevClean444Cr = UpsampleChromaPlaneSited(prevCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane nextClean444Cb = UpsampleChromaPlaneSited(nextCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane nextClean444Cr = UpsampleChromaPlaneSited(nextCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);

					// Plant independent two-seed chroma grain originating on the coded 4:2:0 stream (as on real 4:2:0 HDMI
					// delivered as RGB 4:4:4) and upsampled via SitingSpec::Left() so ReduceToCodedGrid sees the full
					// 15% leaked grain variance (exposing Layer-2 failure decisively when retainedGrainVar is disabled!).
					std::mt19937 rngOwn(6301), rngForPrev(6302), rngForNext(6303), rngPrevFrame(6304);
					std::normal_distribution<double> gd(0.0, grainSigma);
					TdV2Plane prevFrameOwnCodedCb(cW, cH), prevFrameOwnCodedCr(cW, cH);
					TdV2Plane ownCodedCb(cW, cH), ownCodedCr(cW, cH);
					TdV2Plane forPrevCodedCb(cW, cH), forPrevCodedCr(cW, cH);
					TdV2Plane forNextCodedCb(cW, cH), forNextCodedCr(cW, cH);
					for (size_t i = 0; i < cN; ++i)
					{
						prevFrameOwnCodedCb.v[i] = gd(rngPrevFrame);
						prevFrameOwnCodedCr.v[i] = gd(rngPrevFrame);
						ownCodedCb.v[i] = gd(rngOwn);
						ownCodedCr.v[i] = gd(rngOwn);
						forPrevCodedCb.v[i] = gd(rngForPrev);
						forPrevCodedCr.v[i] = gd(rngForPrev);
						forNextCodedCb.v[i] = gd(rngForNext);
						forNextCodedCr.v[i] = gd(rngForNext);
					}
					// Exact 2x2 coded-grid replication onto delivered 4:4:4 preserves white coded-grid grain spectrum
					// without double-low-pass distortion through ReduceToCodedGrid (Var(own444) == Var(ownCoded)).
					auto replicate2x2 = [&](const TdV2Plane& src) -> TdV2Plane
					{
						TdV2Plane dst(w, h, 0.0);
						for (int cy = 0; cy < cH; ++cy)
							for (int cx = 0; cx < cW; ++cx)
							{
								const double v = src.at(cx, cy);
								dst.at(2 * cx,     2 * cy)     = v;
								dst.at(2 * cx + 1, 2 * cy)     = v;
								dst.at(2 * cx,     2 * cy + 1) = v;
								dst.at(2 * cx + 1, 2 * cy + 1) = v;
							}
						return dst;
					};
					const TdV2Plane prevFrameOwn444Cb = replicate2x2(prevFrameOwnCodedCb);
					const TdV2Plane prevFrameOwn444Cr = replicate2x2(prevFrameOwnCodedCr);
					const TdV2Plane own444Cb = replicate2x2(ownCodedCb);
					const TdV2Plane own444Cr = replicate2x2(ownCodedCr);
					const TdV2Plane forPrev444Cb = replicate2x2(forPrevCodedCb);
					const TdV2Plane forPrev444Cr = replicate2x2(forPrevCodedCr);
					const TdV2Plane forNext444Cb = replicate2x2(forNextCodedCb);
					const TdV2Plane forNext444Cr = replicate2x2(forNextCodedCr);

					const std::vector<double>& prevFrameOwnCb = prevFrameOwn444Cb.v;
					const std::vector<double>& prevFrameOwnCr = prevFrameOwn444Cr.v;
					const std::vector<double>& ownCb = own444Cb.v;
					const std::vector<double>& ownCr = own444Cr.v;
					const std::vector<double>& forPrevCb = forPrev444Cb.v;
					const std::vector<double>& forPrevCr = forPrev444Cr.v;
					const std::vector<double>& forNextCb = forNext444Cb.v;
					const std::vector<double>& forNextCr = forNext444Cr.v;

					TdV2Plane prevFrameRecCb444(w, h), prevFrameRecCr444(w, h);
					TdV2Plane prevFrameCleanImp444Cb(w, h), prevFrameCleanImp444Cr(w, h);
					TdV2Plane recCb444(w, h), recCr444(w, h), prevRecCb444(w, h), prevRecCr444(w, h), nextRecCb444(w, h), nextRecCr444(w, h);
					TdV2Plane curCleanImp444Cb(w, h), curCleanImp444Cr(w, h);
					TdV2Plane prevCleanImp444Cb(w, h), prevCleanImp444Cr(w, h), nextCleanImp444Cb(w, h), nextCleanImp444Cr(w, h);
					for (size_t i = 0; i < fN; ++i)
					{
						prevFrameRecCb444.v[i] = prevFrameClean444Cb.v[i] + prevFrameOwnCb[i];
						prevFrameRecCr444.v[i] = prevFrameClean444Cr.v[i] + prevFrameOwnCr[i];
						recCb444.v[i] = curClean444Cb.v[i] + ownCb[i];
						recCr444.v[i] = curClean444Cr.v[i] + ownCr[i];
						prevRecCb444.v[i] = prevClean444Cb.v[i] + forPrevCb[i];
						prevRecCr444.v[i] = prevClean444Cr.v[i] + forPrevCr[i];
						nextRecCb444.v[i] = nextClean444Cb.v[i] + forNextCb[i];
						nextRecCr444.v[i] = nextClean444Cr.v[i] + forNextCr[i];

						prevFrameCleanImp444Cb.v[i] = prevFrameClean444Cb.v[i] + leakFrac * prevFrameOwnCb[i];
						prevFrameCleanImp444Cr.v[i] = prevFrameClean444Cr.v[i] + leakFrac * prevFrameOwnCr[i];
						curCleanImp444Cb.v[i] = curClean444Cb.v[i] + leakFrac * ownCb[i];
						curCleanImp444Cr.v[i] = curClean444Cr.v[i] + leakFrac * ownCr[i];
						prevCleanImp444Cb.v[i] = prevClean444Cb.v[i] + leakFrac * forPrevCb[i];
						prevCleanImp444Cr.v[i] = prevClean444Cr.v[i] + leakFrac * forPrevCr[i];
						nextCleanImp444Cb.v[i] = nextClean444Cb.v[i] + leakFrac * forNextCb[i];
						nextCleanImp444Cr.v[i] = nextClean444Cr.v[i] + leakFrac * forNextCr[i];
					}

					TdV2Plane prevFrameRecR, prevFrameRecG, prevFrameRecB;
					TdV2Plane recR, recG, recB, prevR, prevG, prevB, nextR, nextG, nextB;
					YCbCrPlanesToRgb(mat, masterY, prevFrameRecCb444, prevFrameRecCr444, midCode, prevFrameRecR, prevFrameRecG, prevFrameRecB);
					YCbCrPlanesToRgb(mat, masterY, recCb444, recCr444, midCode, recR, recG, recB);
					YCbCrPlanesToRgb(mat, masterY, prevRecCb444, prevRecCr444, midCode, prevR, prevG, prevB);
					YCbCrPlanesToRgb(mat, masterY, nextRecCb444, nextRecCr444, midCode, nextR, nextG, nextB);

					// Mandatory In-Test Negative Control #1 (Rule 3): naive un-separated RGB average on EXACT SAME input!
					TdV2Plane naivePrevR(w, h), naivePrevG(w, h), naivePrevB(w, h);
					TdV2Plane naiveR(w, h), naiveG(w, h), naiveB(w, h);
					for (size_t i = 0; i < fN; ++i)
					{
						naivePrevR.v[i] = (prevFrameRecR.v[i] + prevR.v[i] + nextR.v[i]) / 3.0;
						naivePrevG.v[i] = (prevFrameRecG.v[i] + prevG.v[i] + nextG.v[i]) / 3.0;
						naivePrevB.v[i] = (prevFrameRecB.v[i] + prevB.v[i] + nextB.v[i]) / 3.0;
						naiveR.v[i] = (recR.v[i] + prevR.v[i] + nextR.v[i]) / 3.0;
						naiveG.v[i] = (recG.v[i] + prevG.v[i] + nextG.v[i]) / 3.0;
						naiveB.v[i] = (recB.v[i] + prevB.v[i] + nextB.v[i]) / 3.0;
					}
					TdV2Plane naivePrevY, naivePrevCb, naivePrevCr, naiveY, naiveCb, naiveCr;
					RgbPlanesToYCbCr(mat, naivePrevR, naivePrevG, naivePrevB, midCode, naivePrevY, naivePrevCb, naivePrevCr);
					RgbPlanesToYCbCr(mat, naiveR, naiveG, naiveB, midCode, naiveY, naiveCb, naiveCr);
					std::vector<double> naivePrevResCb(fN), naivePrevResCr(fN);
					for (size_t i = 0; i < fN; ++i)
					{
						naivePrevResCb[i] = naivePrevCb.v[i] - master444Cb.v[i];
						naivePrevResCr[i] = naivePrevCr.v[i] - master444Cr.v[i];
					}
					const auto naiveRepCb = EvaluateGrainTransfer(
						master444Cb.v.data(), ownCb.data(), forPrevCb.data(), naiveCb.v.data(), fN,
						naivePrevResCb.data(), prevFrameOwnCb.data());
					const auto naiveRepCr = EvaluateGrainTransfer(
						master444Cr.v.data(), ownCr.data(), forPrevCr.data(), naiveCr.v.data(), fN,
						naivePrevResCr.data(), prevFrameOwnCr.data());
					Assert::IsTrue(naiveRepCb.valid && naiveRepCr.valid && naiveRepCb.lag1Evaluated && naiveRepCr.lag1Evaluated);
					Assert::IsFalse(naiveRepCb.passesAuthoritativeBar, L"Negative control: naive RGB average MUST fail grain bar");
					Assert::IsTrue(naiveRepCb.betaOwn < 0.90 && naiveRepCb.betaForeign > 0.10);
					Assert::IsTrue(naiveRepCr.betaOwn < 0.90 && naiveRepCr.betaForeign > 0.10);

					// Run TdV2ChromaEngine on RGB 4:4:4 (coded 4:2:0) across Frame 1 & Frame 2 for genuine lag-1 (H11)!
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in1;
					in1.seq = 50;
					in1.deliveredFormat = DeliveredChromaFormat::Rgb444;
					in1.codedSubsampling = ChromaSubsampling::Yuv420;
					in1.gridStatus = ChromaGridStatus::Identified;
					in1.siting = siting;
					in1.matrix = mat;
					in1.lumaW = w;
					in1.lumaH = h;
					in1.activeRect = active;
					in1.receivedR = prevFrameRecR;
					in1.receivedG = prevFrameRecG;
					in1.receivedB = prevFrameRecB;
					in1.hasCleanSplit = true;
					in1.cleanCb = prevFrameCleanImp444Cb;
					in1.cleanCr = prevFrameCleanImp444Cr;
					in1.retainedGrainVarCb = retainedVar;
					in1.retainedGrainVarCr = retainedVar;
					in1.prevPeak = { &prevCleanImp444Cb, &prevCleanImp444Cr, &stillMf, false };
					in1.nextPeak = { &nextCleanImp444Cb, &nextCleanImp444Cr, &stillMf, false };
					in1.hasStepPrior = true;
					in1.estimatedStepDelta = 12.0;
					const auto res1 = eng.ProcessFrame(in1, &reg);

					ChromaProcessInput in = in1;
					in.seq = 51;
					in.receivedR = recR;
					in.receivedG = recG;
					in.receivedB = recB;
					in.cleanCb = curCleanImp444Cb;
					in.cleanCr = curCleanImp444Cr;
					const auto res = eng.ProcessFrame(in, &reg);

					// Mandatory In-Test Negative Control #2 (Rule 3 / Test-Diff HIGH-5):
					// Run TdV2ChromaEngine on the EXACT SAME 15%-leaked RGB 4:4:4 planes with Layer-2 retainedGrainVar DISABLED!
					{
						TdV2ChromaEngine engNoL2(cfg);
						StandInRegistry regNoL2;
						ChromaProcessInput in1NoL2 = in1;
						in1NoL2.retainedGrainVarCb.clear();
						in1NoL2.retainedGrainVarCr.clear();
						const auto res1NoL2 = engNoL2.ProcessFrame(in1NoL2, &regNoL2);
						ChromaProcessInput inNoL2 = in;
						inNoL2.retainedGrainVarCb.clear();
						inNoL2.retainedGrainVarCr.clear();
						const auto resNoL2 = engNoL2.ProcessFrame(inNoL2, &regNoL2);

						TdV2Plane n1Y, n1Cb, n1Cr, nY, nCb, nCr;
						RgbPlanesToYCbCr(mat, res1NoL2.outputR, res1NoL2.outputG, res1NoL2.outputB, midCode, n1Y, n1Cb, n1Cr);
						RgbPlanesToYCbCr(mat, resNoL2.outputR, resNoL2.outputG, resNoL2.outputB, midCode, nY, nCb, nCr);
						std::vector<double> prevResNoL2Cb(fN), prevResNoL2Cr(fN);
						for (size_t i = 0; i < fN; ++i)
						{
							prevResNoL2Cb[i] = n1Cb.v[i] - master444Cb.v[i];
							prevResNoL2Cr[i] = n1Cr.v[i] - master444Cr.v[i];
						}
						const auto repNoL2Cb = EvaluateGrainTransfer(
							master444Cb.v.data(), ownCb.data(), forPrevCb.data(), nCb.v.data(), fN,
							prevResNoL2Cb.data(), prevFrameOwnCb.data());
						const auto repNoL2Cr = EvaluateGrainTransfer(
							master444Cr.v.data(), ownCr.data(), forPrevCr.data(), nCr.v.data(), fN,
							prevResNoL2Cr.data(), prevFrameOwnCr.data());
						Assert::IsTrue(repNoL2Cb.valid && repNoL2Cb.lag1Evaluated && repNoL2Cr.valid && repNoL2Cr.lag1Evaluated);
						Assert::IsFalse(repNoL2Cb.passesAuthoritativeBar,
							L"Negative control: Layer-2-disabled RGB 4:4:4 engine on 15%-leaked clean planes MUST fail authoritative grain bar");
						Assert::IsFalse(repNoL2Cr.passesAuthoritativeBar,
							L"Negative control: Layer-2-disabled RGB 4:4:4 engine (Cr) on 15%-leaked clean planes MUST fail authoritative grain bar");
						Assert::IsTrue(std::fabs(repNoL2Cb.betaForeign) > kGrainBarMaxBetaForeign || repNoL2Cb.betaOwn < kGrainBarMinBetaOwn,
							L"Negative control: Layer-2-disabled RGB engine must exhibit real own-grain loss or foreign-grain leak");
					}

					TdV2Plane out1Y, out1Cb, out1Cr, outY, outCb, outCr;
					RgbPlanesToYCbCr(mat, res1.outputR, res1.outputG, res1.outputB, midCode, out1Y, out1Cb, out1Cr);
					RgbPlanesToYCbCr(mat, res.outputR, res.outputG, res.outputB, midCode, outY, outCb, outCr);

					// Verify non-trivial clean-layer repair on RGB delivered delta.
					TdV2Plane repCleanCb(w, h);
					for (size_t i = 0; i < fN; ++i)
						repCleanCb.v[i] = curClean444Cb.v[i] + res.deliveredDeltaCb.v[i];
					Assert::IsTrue(MaskedRmse(master444Cb, repCleanCb, active) < MaskedRmse(master444Cb, curClean444Cb, active) * 0.70,
						L"RGB chroma clean layer must genuinely improve");

					std::vector<double> prevResCb(fN), prevResCr(fN);
					for (size_t i = 0; i < fN; ++i)
					{
						prevResCb[i] = out1Cb.v[i] - master444Cb.v[i];
						prevResCr[i] = out1Cr.v[i] - master444Cr.v[i];
					}
					const auto repCb = EvaluateGrainTransfer(
						master444Cb.v.data(), ownCb.data(), forPrevCb.data(), outCb.v.data(), fN,
						prevResCb.data(), prevFrameOwnCb.data());
					const auto repCr = EvaluateGrainTransfer(
						master444Cr.v.data(), ownCr.data(), forPrevCr.data(), outCr.v.data(), fN,
						prevResCr.data(), prevFrameOwnCr.data());

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"RGB Chroma Grain (15%% leak): betaOwn=%.4f/%.4f betaFor=%.4f/%.4f stdRatio=%.4f/%.4f lag1=%.4f/%.4f | naive betaOwn=%.3f betaFor=%.3f",
						repCb.betaOwn, repCr.betaOwn, repCb.betaForeign, repCr.betaForeign,
						repCb.stdRatio, repCr.stdRatio, repCb.lag1Rise, repCr.lag1Rise, naiveRepCb.betaOwn, naiveRepCb.betaForeign);
					Logger::WriteMessage(buf);

					Assert::IsTrue(repCb.valid && repCb.lag1Evaluated && repCb.passesAuthoritativeBar,
						L"RGB Cb (15% leak + Layer 2) must pass authoritative grain bar with lag1Evaluated==true");
					Assert::IsTrue(repCr.valid && repCr.lag1Evaluated && repCr.passesAuthoritativeBar,
						L"RGB Cr (15% leak + Layer 2) must pass authoritative grain bar with lag1Evaluated==true");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(repCb.lag1Rise <= kGrainBarMaxLag1Rise);
					Assert::IsTrue(repCr.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCr.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCr.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(repCr.lag1Rise <= kGrainBarMaxLag1Rise);
				}

				// ---- Sub-case C (HIGH-4b): Layer-4 TdGuardPort intervention + Anchor Canvas Purity ----
				// On a smooth chroma wall where 45% of ownGrain leaked into curClean, A2 removes the leaked fine grain,
				// genuinely triggering Layer-4 TdGuardPort (guardViolationCells > 0, GrainGuardW4c active).
				// Verify outputCb restores ownGrain variance while AnchorCanvas stays completely free of Layer-4 grain add-back!
				{
					const int gw = 64, gh = 64;
					const int gcw = gw / 16, gch = gh / 16;
					const int gcW = gw / 2, gcH = gh / 2;
					const size_t gcN = static_cast<size_t>(gcW) * gcH;
					const CellMotionField gStillMf = CellMotionField::Uniform(gcw, gch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					const TdV2Plane flat512(gcW, gcH, 512.0);

					std::mt19937 rngG(6401);
					std::normal_distribution<double> gDist(0.0, grainSigma);
					std::vector<double> gCb(gcN), gCr(gcN);
					TdV2Plane gRecCb(gcW, gcH), gRecCr(gcW, gcH), gLeakCleanCb(gcW, gcH), gLeakCleanCr(gcW, gcH);
					const double leakG = 0.45;
					for (size_t i = 0; i < gcN; ++i)
					{
						gCb[i] = gDist(rngG);
						gCr[i] = gDist(rngG);
						gRecCb.v[i] = 512.0 + gCb[i];
						gRecCr.v[i] = 512.0 + gCr[i];
						gLeakCleanCb.v[i] = 512.0 + leakG * gCb[i];
						gLeakCleanCr.v[i] = 512.0 + leakG * gCr[i];
					}
					const double leakGVar = (leakG * grainSigma) * (leakG * grainSigma);
					const std::vector<double> retGVar(static_cast<size_t>(gcw) * gch, leakGVar);

					Config gCfg = MakePreset(Preset::Reference, Mode::On);
					gCfg.estimator = VarianceEstimator::A2_RoundingCells; // force blend so Layer-4 guard genuinely fires!
					TdV2ChromaEngine gEng(gCfg);
					StandInRegistry gReg;

					ChromaProcessInput gIn;
					gIn.seq = 60;
					gIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
					gIn.codedSubsampling = ChromaSubsampling::Yuv420;
					gIn.gridStatus = ChromaGridStatus::Identified;
					gIn.siting = SitingSpec::TopLeftType2();
					gIn.lumaW = gw;
					gIn.lumaH = gh;
					gIn.activeRect = ActiveRect::Full(gw, gh);
					gIn.receivedCb = gRecCb;
					gIn.receivedCr = gRecCr;
					gIn.hasCleanSplit = true;
					gIn.cleanCb = gLeakCleanCb;
					gIn.cleanCr = gLeakCleanCr;
					gIn.retainedGrainVarCb = retGVar;
					gIn.retainedGrainVarCr = retGVar;
					gIn.isAnchorFrame = true;
					gIn.prevPeak = { &flat512, &flat512, &gStillMf, false };
					gIn.nextPeak = { &flat512, &flat512, &gStillMf, false };
					gIn.hasStepPrior = true;
					gIn.estimatedStepDelta = 24.0;

					const auto gRes = gEng.ProcessFrame(gIn, &gReg);
					Assert::IsTrue(gRes.guardViolationCells > 0,
						L"Test fixture must genuinely trigger Layer-4 TdGuardPort (guardViolationCells > 0)");
					Assert::IsTrue(gReg.IsActive(StandInKind::GrainGuardW4c),
						L"Layer-4 GrainGuardW4c stand-in must be visibly active");
					Assert::IsTrue(gEng.AnchorCanvas().HasData());

					// Measure regression slope of ownGrain in outputCb vs AnchorCanvas sampled at coded grid!
					TdV2Plane canvasCb, canvasCr;
					std::vector<uint8_t> canvasValid;
					Assert::IsTrue(gEng.AnchorCanvas().SampleAtCodedGrid(
						ChromaSubsampling::Yuv420, gIn.siting, gIn.activeRect, &gStillMf, 16,
						canvasCb, canvasCr, canvasValid));

					auto regressSlope = [](const TdV2Plane& sig, const std::vector<double>& basis) -> double
					{
						double meanY = 0.0, meanX = 0.0;
						const size_t n = basis.size();
						for (size_t i = 0; i < n; ++i) { meanY += (sig.v[i] - 512.0); meanX += basis[i]; }
						meanY /= static_cast<double>(n);
						meanX /= static_cast<double>(n);
						double cov = 0.0, vx = 0.0;
						for (size_t i = 0; i < n; ++i)
						{
							const double dy = (sig.v[i] - 512.0) - meanY;
							const double dx = basis[i] - meanX;
							cov += dy * dx;
							vx += dx * dx;
						}
						return vx > 1e-12 ? (cov / vx) : 0.0;
					};
					const double slopeOutCb = regressSlope(gRes.outputCb, gCb);
					const double slopeCanvasCb = regressSlope(canvasCb, gCb);
					const double slopeOutCr = regressSlope(gRes.outputCr, gCr);
					const double slopeCanvasCr = regressSlope(canvasCr, gCr);

					Assert::IsTrue(slopeOutCb > 0.75 && slopeOutCr > 0.75,
						L"Layer-4 TdGuardPort add-back must restore own chroma grain into outputCb/Cr");
					Assert::IsTrue(slopeCanvasCb <= leakG + 0.05 && slopeCanvasCr <= leakG + 0.05,
						L"Layer-4 grain add-back MUST NEVER leak into AnchorCanvas");
					Assert::IsTrue(slopeCanvasCb + 0.25 < slopeOutCb && slopeCanvasCr + 0.25 < slopeOutCr,
						L"AnchorCanvas must remain strictly separated from output's Layer-4 grain add-back");
				}

				// ---- Sub-case D (H11 + H12): Raw hasCleanSplit == false chroma path — FULL authoritative bar + negative control! ----
				{
					const TdV2Plane masterCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
					const TdV2Plane masterCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);
					std::mt19937 rngR0(6500), rngR1(6501), rngR2(6502), rngR3(6503);
					std::normal_distribution<double> gd(0.0, grainSigma);
					std::vector<double> rPrevFrameOwnCb(cN), rPrevFrameOwnCr(cN);
					std::vector<double> rOwnCb(cN), rForPrevCb(cN), rForNextCb(cN);
					std::vector<double> rOwnCr(cN), rForPrevCr(cN), rForNextCr(cN);
					TdV2Plane rawPrevFrameCb(cW, cH), rawPrevFrameCr(cW, cH);
					TdV2Plane rawCurCb(cW, cH), rawPrevCb(cW, cH), rawNextCb(cW, cH);
					TdV2Plane rawCurCr(cW, cH), rawPrevCr(cW, cH), rawNextCr(cW, cH);
					for (size_t i = 0; i < cN; ++i)
					{
						rPrevFrameOwnCb[i] = gd(rngR0); rPrevFrameOwnCr[i] = gd(rngR0);
						rOwnCb[i] = gd(rngR1); rOwnCr[i] = gd(rngR1);
						rForPrevCb[i] = gd(rngR2); rForPrevCr[i] = gd(rngR2);
						rForNextCb[i] = gd(rngR3); rForNextCr[i] = gd(rngR3);
						rawPrevFrameCb.v[i] = masterCb.v[i] + rPrevFrameOwnCb[i];
						rawPrevFrameCr.v[i] = masterCr.v[i] + rPrevFrameOwnCr[i];
						rawCurCb.v[i] = masterCb.v[i] + rOwnCb[i];
						rawPrevCb.v[i] = masterCb.v[i] + rForPrevCb[i];
						rawNextCb.v[i] = masterCb.v[i] + rForNextCb[i];
						rawCurCr.v[i] = masterCr.v[i] + rOwnCr[i];
						rawPrevCr.v[i] = masterCr.v[i] + rForPrevCr[i];
						rawNextCr.v[i] = masterCr.v[i] + rForNextCr[i];
					}

					// Mandatory In-Test Negative Control (Rule 3 / H12): unprotected direct blend on the EXACT SAME inputs!
					std::vector<double> naiveRawPrevResCb(cN), naiveRawPrevResCr(cN), naiveRawCb(cN), naiveRawCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						naiveRawPrevResCb[i] = (rawPrevFrameCb.v[i] + rawPrevCb.v[i] + rawNextCb.v[i]) / 3.0 - masterCb.v[i];
						naiveRawPrevResCr[i] = (rawPrevFrameCr.v[i] + rawPrevCr.v[i] + rawNextCr.v[i]) / 3.0 - masterCr.v[i];
						naiveRawCb[i] = (rawCurCb.v[i] + rawPrevCb.v[i] + rawNextCb.v[i]) / 3.0;
						naiveRawCr[i] = (rawCurCr.v[i] + rawPrevCr.v[i] + rawNextCr.v[i]) / 3.0;
					}
					const auto naiveRawRepCb = EvaluateGrainTransfer(
						masterCb.v.data(), rOwnCb.data(), rForPrevCb.data(), naiveRawCb.data(), cN,
						naiveRawPrevResCb.data(), rPrevFrameOwnCb.data());
					const auto naiveRawRepCr = EvaluateGrainTransfer(
						masterCr.v.data(), rOwnCr.data(), rForPrevCr.data(), naiveRawCr.data(), cN,
						naiveRawPrevResCr.data(), rPrevFrameOwnCr.data());
					Assert::IsTrue(naiveRawRepCb.valid && naiveRawRepCr.valid && naiveRawRepCb.lag1Evaluated && naiveRawRepCr.lag1Evaluated);
					Assert::IsFalse(naiveRawRepCb.passesAuthoritativeBar,
						L"H12 Negative control: unprotected direct blend of raw chroma MUST fail authoritative bar");
					Assert::IsTrue(naiveRawRepCb.betaOwn < 0.75 && naiveRawRepCb.betaForeign > 0.25);
					Assert::IsTrue(naiveRawRepCr.betaOwn < 0.75 && naiveRawRepCr.betaForeign > 0.25);

					const std::vector<double> fullGVar(static_cast<size_t>(cw) * ch, grainSigma * grainSigma);

					Config rCfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine rEng(rCfg);
					StandInRegistry rReg;

					ChromaProcessInput rIn1;
					rIn1.seq = 60;
					rIn1.deliveredFormat = DeliveredChromaFormat::Yuv420;
					rIn1.codedSubsampling = ChromaSubsampling::Yuv420;
					rIn1.gridStatus = ChromaGridStatus::Identified;
					rIn1.siting = siting;
					rIn1.lumaW = w;
					rIn1.lumaH = h;
					rIn1.activeRect = active;
					rIn1.receivedCb = rawPrevFrameCb;
					rIn1.receivedCr = rawPrevFrameCr;
					rIn1.hasCleanSplit = false;
					rIn1.retainedGrainVarCb = fullGVar;
					rIn1.retainedGrainVarCr = fullGVar;
					rIn1.prevPeak = { &rawPrevCb, &rawPrevCr, &stillMf, false };
					rIn1.nextPeak = { &rawNextCb, &rawNextCr, &stillMf, false };
					rIn1.hasStepPrior = true;
					rIn1.estimatedStepDelta = 12.0;
					const auto rRes1 = rEng.ProcessFrame(rIn1, &rReg);

					ChromaProcessInput rIn = rIn1;
					rIn.seq = 61;
					rIn.receivedCb = rawCurCb;
					rIn.receivedCr = rawCurCr;
					const auto rRes = rEng.ProcessFrame(rIn, &rReg);
					AssertPlaneAllFinite(rRes.outputCb, L"Raw hasCleanSplit==false chroma output must be finite");
					Assert::IsTrue(rReg.IsActive(StandInKind::P10gGrainSeparator),
						L"Raw hasCleanSplit==false chroma path MUST visibly engage StandInKind::P10gGrainSeparator");

					std::vector<double> rPrevResCb(cN), rPrevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						rPrevResCb[i] = rRes1.outputCb.v[i] - masterCb.v[i];
						rPrevResCr[i] = rRes1.outputCr.v[i] - masterCr.v[i];
					}
					const auto rawRepCb = EvaluateGrainTransfer(
						masterCb.v.data(), rOwnCb.data(), rForPrevCb.data(), rRes.outputCb.v.data(), cN,
						rPrevResCb.data(), rPrevFrameOwnCb.data());
					const auto rawRepCr = EvaluateGrainTransfer(
						masterCr.v.data(), rOwnCr.data(), rForPrevCr.data(), rRes.outputCr.v.data(), cN,
						rPrevResCr.data(), rPrevFrameOwnCr.data());
					Assert::IsTrue(rawRepCb.valid && rawRepCb.lag1Evaluated && rawRepCb.passesAuthoritativeBar,
						L"H12: Raw chroma path MUST pass FULL authoritative bar (betaOwn>=0.98, |betaForeign|<=0.02, RMS +/-2%, lag1Evaluated==true)");
					Assert::IsTrue(rawRepCr.valid && rawRepCr.lag1Evaluated && rawRepCr.passesAuthoritativeBar,
						L"H12: Raw chroma Cr path MUST pass FULL authoritative bar (betaOwn>=0.98, |betaForeign|<=0.02, RMS +/-2%, lag1Evaluated==true)");
					Assert::IsTrue(rawRepCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(rawRepCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(rawRepCb.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(rawRepCr.betaOwn >= kGrainBarMinBetaOwn && std::fabs(rawRepCr.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(rawRepCr.stdRatio - 1.0) <= kGrainBarMaxRmsError);
				}
			}

			// =========================================================================
			// 7. Q5(a): 0.5-px Pan Projection Form Eliminates Cross-Form Bias & Preserves Flat Grain
			// =========================================================================
			TEST_METHOD(Q5HalfPixelPanProjectionEliminatesCrossFormBiasAndPreservesFlatGrain)
			{
				// Direct algebraic sanity check of ComputeCrossIr vs ComputeProjectionIr under multiplicative attenuation alpha = 0.7:
				// E = E_T = 100, C = 0.7 * 100 = 70, Eprev = 0.49 * 100 = 49.
				const double algCross = ComputeCrossIr(100.0, 70.0);
				const double algProj = ComputeProjectionIr(100.0, 70.0, 49.0);
				Assert::AreEqual(30.0, algCross, 1e-12, L"Negative control: cross form leaks (1 - alpha)*E = 30");
				Assert::AreEqual(0.0, algProj, 1e-12, L"Projection form must cancel pure multiplicative attenuation exactly");
				Assert::AreEqual(algProj, SelectGrainIr(100.0, 70.0, 49.0, true), 1e-12);
				Assert::AreEqual(algCross, SelectGrainIr(100.0, 70.0, 49.0, false), 1e-12);

				const int w = 128, h = 128;
				const ActiveRect active{ 8, 8, 120, 120 }; // include bar borders to verify ActiveRect isolation!
				const double grainSigma = 4.0;
				const double trueGrainVar = grainSigma * grainSigma; // 16.0

				// High-frequency texture (period 3 px in x -> exact 0.5-px Catmull-Rom bicubic gain alpha = 11/16 = 0.6875!).
				auto hfTex = [](double x, double y) -> double
				{
					return 26.0 * std::sin(2.0 * kPi * x / 3.0) * std::cos(2.0 * kPi * y / 7.0);
				};

				std::mt19937 rngCur(7001), rngPrev(7002);
				std::normal_distribution<double> gd(0.0, grainSigma);

				TdV2Plane curTexOnly(w, h, 0.0), prevTexOnly(w, h, 0.0);
				TdV2Plane curTexGrain(w, h, 0.0), prevTexGrain(w, h, 0.0);
				TdV2Plane curFlatGrain(w, h, 0.0), prevFlatGrain(w, h, 0.0);

				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						if (!active.Contains(x, y))
						{
							// Poison outside ActiveRect with huge finite values; EvaluateHalfPixelPanIr MUST ignore them!
							curTexOnly.at(x, y) = 1e6;
							prevTexOnly.at(x, y) = -1e6;
							curTexGrain.at(x, y) = 1e6;
							prevTexGrain.at(x, y) = -1e6;
							curFlatGrain.at(x, y) = 1e6;
							prevFlatGrain.at(x, y) = -1e6;
							continue;
						}
						const double tc = hfTex(static_cast<double>(x), static_cast<double>(y));
						const double tp = hfTex(static_cast<double>(x) - 0.5, static_cast<double>(y));
						const double gc = gd(rngCur);
						const double gp = gd(rngPrev);

						curTexOnly.at(x, y) = tc;
						prevTexOnly.at(x, y) = tp;
						curTexGrain.at(x, y) = tc + gc;
						prevTexGrain.at(x, y) = tp + gp;
						curFlatGrain.at(x, y) = gc;
						prevFlatGrain.at(x, y) = gp;
					}
				}

				// (a) 0.5-px panned high-frequency texture + grain:
				StandInRegistry regA;
				const auto evalA = EvaluateHalfPixelPanIr(curTexGrain, prevTexGrain, active, +0.5, 0.0, 8, trueGrainVar, &regA);
				Assert::IsTrue(evalA.valid && evalA.regionCount > 50);
				Assert::AreEqual(static_cast<size_t>(0), evalA.nonFiniteRegions);
				// Mandatory Negative Control: raw cross form suffers huge > 5% pan bias (adoptProjection == true)!
				Assert::IsTrue(evalA.relativeBias > kQ5PanBiasThreshold,
					L"Negative control: raw cross form MUST suffer > 5% bias under 0.5-px pan on texture+grain");
				Assert::IsTrue(evalA.adoptProjection, L"Q5(a) must trigger adoptProjection == true when relativeBias > 5%");
				Assert::IsTrue(evalA.meanCrossIr > trueGrainVar * 3.0,
					L"Negative control: raw cross form massively overestimates grain variance under 0.5-px pan");
				// Adversarial anti-2.36x-bias check (Pass-2 audit Q5(a)):
				// Naive un-debiased E - C^2 / E_prev converges to (1 + beta_w / alpha^2) * sigma^2 ~= 2.36 * sigma^2 (~37.7)
				// on strong 0.5-px panned texture + grain. Assert I_proj genuinely converges near trueGrainVar (16.0)
				// within [-15%, +25%], catching the 2.36x bias immediately!
				Assert::IsTrue(evalA.meanProjIr >= 0.85 * trueGrainVar && evalA.meanProjIr <= 1.25 * trueGrainVar,
					L"Q5(a) projection form MUST converge near true grain variance on panned texture+grain (rejecting 2.36x bias!)");

				// (b) 0.5-px panned pure high-frequency texture (zero grain):
				StandInRegistry regB;
				const auto evalB = EvaluateHalfPixelPanIr(curTexOnly, prevTexOnly, active, +0.5, 0.0, 8, 1.0, &regB);
				Assert::IsTrue(evalB.valid && evalB.regionCount > 50);
				Assert::IsTrue(evalB.adoptProjection, L"Pure panned texture must trigger adoptProjection == true");
				Assert::IsTrue(evalB.meanCrossIr > 40.0,
					L"Negative control: raw cross form leaks massive false grain energy (> 40) on pure 0.5-px panned texture");
				Assert::IsTrue(evalB.meanProjIr < 1e-4,
					L"Projection form must cancel 0.5-px bicubic attenuation bias to ~0 on pure panned texture");

				// (c) Pure flat grain (no texture) under 0.5-px pan: projection form must retain >= 0.95 of true grain variance!
				StandInRegistry regC;
				const auto evalC = EvaluateHalfPixelPanIr(curFlatGrain, prevFlatGrain, active, +0.5, 0.0, 16, trueGrainVar, &regC);
				Assert::IsTrue(evalC.valid && evalC.regionCount >= 25);
				const double flatRetention = evalC.meanProjIr / trueGrainVar;

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Q5(a) 0.5-px pan: tex+grain cross=%.2f proj=%.2f (true=%.1f, relBias=%.3f) | pureTex cross=%.2f proj=%.6f | flatRet=%.4f",
					evalA.meanCrossIr, evalA.meanProjIr, trueGrainVar, evalA.relativeBias,
					evalB.meanCrossIr, evalB.meanProjIr, flatRetention);
				Logger::WriteMessage(buf);

				Assert::IsTrue(flatRetention >= 0.95 && flatRetention <= 1.05,
					L"Projection form must retain >= 0.95 (within +/-5%) of true grain variance on pure flat grain");
			}

			// =========================================================================
			// 8. Q5(b): Crushed-Black Zero-Energy Exclusion Preserves BTB & WTW Grain
			// =========================================================================
			TEST_METHOD(Q5CrushedBlackZeroEnergyExclusionPreservesBtbWtwGrain)
			{
				const size_t regionPixels = 64; // 8x8 block
				const double lsbStep = 1.0;     // 10-bit 1-LSB
				const double expectedFloor = (lsbStep * lsbStep) / (4.0 * static_cast<double>(regionPixels));
				Assert::AreEqual(expectedFloor, ZeroEnergyBlockThreshold(regionPixels, lsbStep), 1e-15);
				Assert::IsTrue(IsZeroEnergyFlatBlock(0.0, regionPixels, lsbStep));
				Assert::IsTrue(IsZeroEnergyFlatBlock(expectedFloor, regionPixels, lsbStep));
				Assert::IsFalse(IsZeroEnergyFlatBlock(expectedFloor * 1.01, regionPixels, lsbStep));

				// Build 60 genuine 8x8 blocks:
				//   - 30 crushed-black zero-energy blocks (28 exact constant luma = 64 => E == 0;
				//     2 single-pixel 0.5-LSB quantization-floor blocks => E == E_floor)
				//   - 15 valid grainy BTB blocks (mean luma = 32 < 64, sigma = 3.0)
				//   - 15 valid grainy WTW blocks (mean luma = 980 > 940, sigma = 3.0)
				std::mt19937 rng(8001);
				std::normal_distribution<double> grainDist(0.0, 3.0);

				std::vector<double> blockEnergies;
				std::vector<double> blockIr;
				std::vector<double> grainyOnlyIr;

				auto measureBlock = [](const double cur[64], const double prev[64], double& outE, double& outIr)
				{
					double meanC = 0.0, meanP = 0.0;
					for (int i = 0; i < 64; ++i) { meanC += cur[i]; meanP += prev[i]; }
					meanC /= 64.0;
					meanP /= 64.0;
					double sumE = 0.0, sumC = 0.0, sumEp = 0.0;
					for (int i = 0; i < 64; ++i)
					{
						const double dc = cur[i] - meanC;
						const double dp = prev[i] - meanP;
						sumE += dc * dc;
						sumC += dc * dp;
						sumEp += dp * dp;
					}
					outE = sumE / 64.0;
					outIr = ComputeProjectionIr(outE, sumC / 64.0, sumEp / 64.0);
				};

				// 28 truly zero-energy crushed blocks + 2 exact half-LSB floor blocks.
				for (int b = 0; b < 30; ++b)
				{
					double e = (b < 28) ? 0.0 : expectedFloor;
					double ir = 0.0;
					blockEnergies.push_back(e);
					blockIr.push_back(ir);
				}

				// 15 BTB (luma = 32) + 15 WTW (luma = 980) valid grainy blocks.
				for (int b = 0; b < 15; ++b)
				{
					double gCur[64], gPrev[64];
					for (int i = 0; i < 64; ++i)
					{
						gCur[i] = grainDist(rng);
						gPrev[i] = grainDist(rng);
					}

					double blkBtbC[64], blkBtbP[64], blkWtwC[64], blkWtwP[64];
					for (int i = 0; i < 64; ++i)
					{
						blkBtbC[i] = 32.0 + gCur[i];  blkBtbP[i] = 32.0 + gPrev[i];
						blkWtwC[i] = 980.0 + gCur[i]; blkWtwP[i] = 980.0 + gPrev[i];
					}
					double eB = 0.0, irB = 0.0, eW = 0.0, irW = 0.0;
					measureBlock(blkBtbC, blkBtbP, eB, irB);
					measureBlock(blkWtwC, blkWtwP, eW, irW);

					blockEnergies.push_back(eB); blockIr.push_back(irB); grainyOnlyIr.push_back(irB);
					blockEnergies.push_back(eW); blockIr.push_back(irW); grainyOnlyIr.push_back(irW);
				}

				const double qP = 0.30; // dark-bin lower quantile (standard dark NLF robust quantile)
				const double trueGrainyNlf = RobustStats::Quantile(grainyOnlyIr, qP, RobustStats::RankRule::Interpolated);

				// ---- Mandatory In-Test Negative Control (Rule 3) ----
				// Naive unfiltered quantile on the EXACT SAME 60 blocks is dragged straight down to 0 by crushed blacks!
				const double naiveUnfilteredNlf = RobustStats::Quantile(blockIr, qP, RobustStats::RankRule::Interpolated);
				Assert::AreEqual(0.0, naiveUnfilteredNlf, 1e-12,
					L"Negative control: naive unfiltered quantile MUST be dragged down to 0 by crushed zero-energy blocks");
				Assert::IsTrue(trueGrainyNlf > 6.0, L"True grainy dark NLF must be > 6.0 for sigma=3 grain");

				// Protected Q5(b) dark NLF accumulation.
				StandInRegistry reg;
				const auto res = AccumulateDarkNlfExcludingZeroEnergy(blockEnergies, blockIr, regionPixels, lsbStep, qP, &reg);
				Assert::IsTrue(res.valid);
				Assert::AreEqual(static_cast<size_t>(30), res.excludedZeroEnergyBlocks,
					L"Must exclude ONLY the 30 zero-energy crushed blocks");
				Assert::AreEqual(static_cast<size_t>(30), res.retainedBlocks,
					L"Must retain ALL 15 BTB + 15 WTW valid grainy blocks");
				Assert::AreEqual(static_cast<size_t>(0), res.nonFiniteBlocks);
				Assert::IsTrue(reg.IsActive(StandInKind::ZeroEnergyDarkBlockExcluded),
					L"Zero-energy exclusion MUST visibly engage StandInKind::ZeroEnergyDarkBlockExcluded");
				Assert::AreEqual(trueGrainyNlf, res.nlfEstimate, 1e-12,
					L"Q5(b) must recover exact dark NLF of the valid grainy blocks");

				// Non-tautological BTB vs Crushed-Black test (Pass-2 audit Q5(b)):
				// Low-code BTB blocks (mean code = 24 < studio black 64) carrying real non-zero grain (sigma = 1.5)
				// mixed with truly zero-energy crushed-black blocks (constant code = 64 and code = 16, E <= E_floor).
				// Mandatory Negative Control: a naive code-value filter (meanCode < 64) wrongly excludes ALL valid
				// grainy BTB blocks (0 retained!), whereas energy-based Q5(b) KEEPS every non-zero-energy BTB block,
				// EXCLUDES every zero-energy crushed block, and visibly engages ZeroEnergyDarkBlockExcluded!
				{
					std::mt19937 rngBtb(8099u);
					std::normal_distribution<double> btbGrainDist(0.0, 1.5);
					std::vector<double> mixedEnergies, mixedIr, mixedMeanCodes, expectedBtbIr;

					// 12 truly zero-energy crushed blocks (6 at studio black 64, 6 at BTB code 16 -> all E == 0).
					for (int b = 0; b < 12; ++b)
					{
						mixedEnergies.push_back(0.0);
						mixedIr.push_back(0.0);
						mixedMeanCodes.push_back(b < 6 ? 64.0 : 16.0);
					}
					// 12 valid grainy low-code BTB blocks (mean code = 24 < 64, real non-zero grain energy > E_floor).
					for (int b = 0; b < 12; ++b)
					{
						double curBlk[64], prevBlk[64];
						double sumCode = 0.0;
						for (int i = 0; i < 64; ++i)
						{
							curBlk[i] = 24.0 + btbGrainDist(rngBtb);
							prevBlk[i] = 24.0 + btbGrainDist(rngBtb);
							sumCode += curBlk[i];
						}
						double eBlk = 0.0, irBlk = 0.0;
						measureBlock(curBlk, prevBlk, eBlk, irBlk);
						Assert::IsTrue(eBlk > expectedFloor * 10.0,
							L"Non-vacuity: valid grainy BTB block must have energy well above 1-pixel half-LSB floor");
						mixedEnergies.push_back(eBlk);
						mixedIr.push_back(irBlk);
						mixedMeanCodes.push_back(sumCode / 64.0);
						expectedBtbIr.push_back(irBlk);
					}

					// Negative control: naive code-value exclusion (exclude code <= 64) drops ALL 12 valid grainy BTB blocks!
					size_t naiveCodeRetainedBtb = 0;
					for (size_t i = 12; i < mixedMeanCodes.size(); ++i)
						if (mixedMeanCodes[i] > 64.0)
							++naiveCodeRetainedBtb;
					Assert::AreEqual(static_cast<size_t>(0), naiveCodeRetainedBtb,
						L"Negative control: naive code-value dark filter (code <= 64) wrongly discards 100% of grainy BTB blocks");

					StandInRegistry regBtbMix;
					const auto rBtbMix = AccumulateDarkNlfExcludingZeroEnergy(
						mixedEnergies, mixedIr, regionPixels, lsbStep, qP, &regBtbMix);
					Assert::IsTrue(rBtbMix.valid);
					Assert::AreEqual(static_cast<size_t>(12), rBtbMix.excludedZeroEnergyBlocks,
						L"Energy-based Q5(b) must exclude all 12 zero-energy crushed black blocks");
					Assert::AreEqual(static_cast<size_t>(12), rBtbMix.retainedBlocks,
						L"Energy-based Q5(b) must KEEP all 12 low-code non-zero-energy BTB grainy blocks");
					Assert::IsTrue(regBtbMix.IsActive(StandInKind::ZeroEnergyDarkBlockExcluded),
						L"Excluding zero-energy crushed black blocks must engage StandInKind::ZeroEnergyDarkBlockExcluded");
					const double expectedBtbNlf = RobustStats::Quantile(expectedBtbIr, qP, RobustStats::RankRule::Interpolated);
					Assert::IsTrue(expectedBtbNlf > 1.0, L"Valid sigma=1.5 BTB grain NLF must be clearly non-zero");
					Assert::AreEqual(expectedBtbNlf, rBtbMix.nlfEstimate, 1e-12,
						L"Energy-based Q5(b) must recover exact BTB grain NLF without crushed-black pull-down");
				}
			}

			// =========================================================================
			// 9. Audit Low Closures: Invalid sourceBits, NaN/Inf, Codec-Aligned 8x8 DCT Rect
			// =========================================================================
			TEST_METHOD(AuditLowClosuresInvalidSourceBitsNonFiniteAndCodecAligned8x8DctRect)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();
				const int badBitsList[3] = { 0, -8, 25 };

				// ---- Part A: Upfront invalid sourceBits (0, -8, 25) & NaN/Inf across all 4 engines ----
				{
					const int w = 32, h = 32;
					const TdV2Plane finiteY(w, h, 400.0);
					const TdV2Plane finiteC(w / 2, h / 2, 512.0);
					const TdV2Plane finiteRgb(w, h, 400.0);
					const CellMotionField stillMf = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					for (int badBits : badBitsList)
					{
						// 1. TdV2ReferenceEngine::ProcessFrame
						{
							Config cfg = MakePreset(Preset::Reference, Mode::On);
							cfg.sourceBits = badBits;
							TdV2ReferenceEngine eng(cfg);
							StandInRegistry reg;

							FrameProcessInput in;
							in.seq = 1;
							in.receivedY = finiteY;
							in.hasCleanSplit = true;
							in.cleanY = finiteY;
							in.prevPeak = { &finiteY, &stillMf, false };
							const auto res = eng.ProcessFrame(in, &reg);

							Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure),
								L"TdV2ReferenceEngine invalid sourceBits must engage NonFiniteClosure visibly");
							Assert::IsTrue(res.nonFiniteCells > 0 && res.carryReset);
							AssertPlanesBitExact(finiteY, res.outputY, L"Invalid sourceBits must fail closed to passthrough");
						}

						// 2. TdV2CodingGridStandIn::Measure
						{
							TdV2CodingGridStandIn grid;
							StandInRegistry reg;
							const auto sum = grid.Measure(finiteY.v.data(), finiteY.v.data(), nullptr, w, h, badBits, &reg);
							Assert::IsFalse(sum.valid, L"TdV2CodingGridStandIn invalid sourceBits must return valid == false");
							Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure),
								L"TdV2CodingGridStandIn invalid sourceBits must engage NonFiniteClosure visibly");
						}

						// 3. TdV2ChromaEngine::ProcessFrame (both YUV & RGB) & TdV2ChromaAnchorCanvas
						{
							Config cfg = MakePreset(Preset::Reference, Mode::On);
							cfg.sourceBits = badBits;
							TdV2ChromaEngine eng(cfg);
							StandInRegistry regYuv, regRgb, regCanvas;

							ChromaProcessInput inYuv;
							inYuv.seq = 1;
							inYuv.deliveredFormat = DeliveredChromaFormat::Yuv420;
							inYuv.codedSubsampling = ChromaSubsampling::Yuv420;
							inYuv.lumaW = w;
							inYuv.lumaH = h;
							inYuv.receivedCb = finiteC;
							inYuv.receivedCr = finiteC;
							const auto rYuv = eng.ProcessFrame(inYuv, &regYuv);
							Assert::IsTrue(regYuv.IsActive(StandInKind::NonFiniteClosure));
							Assert::IsTrue(rYuv.nonFiniteCells > 0 && rYuv.anchorReset);
							AssertPlanesBitExact(finiteC, rYuv.outputCb, L"Chroma YUV invalid sourceBits must pass through");

							ChromaProcessInput inRgb;
							inRgb.seq = 2;
							inRgb.deliveredFormat = DeliveredChromaFormat::Rgb444;
							inRgb.codedSubsampling = ChromaSubsampling::Yuv420;
							inRgb.lumaW = w;
							inRgb.lumaH = h;
							inRgb.receivedR = finiteRgb;
							inRgb.receivedG = finiteRgb;
							inRgb.receivedB = finiteRgb;
							const auto rRgb = eng.ProcessFrame(inRgb, &regRgb);
							Assert::IsTrue(regRgb.IsActive(StandInKind::NonFiniteClosure));
							Assert::IsTrue(rRgb.nonFiniteCells > 0 && rRgb.anchorReset);
							AssertPlanesBitExact(finiteRgb, rRgb.outputR, L"Chroma RGB invalid sourceBits must pass through");

							TdV2ChromaAnchorCanvas badCanvas;
							Assert::IsFalse(badCanvas.AccumulateAnchorFrame(
								finiteC, finiteC, &finiteY, w, h, ChromaSubsampling::Yuv420,
								SitingSpec::Left(), ActiveRect::Full(w, h), nullptr, 16, badBits, &regCanvas));
							Assert::IsTrue(regCanvas.IsActive(StandInKind::NonFiniteClosure));
						}

						// 4. TdV2FrameRoleDetector::PushAndClassify invalid sourceBits (LOW-4b)
						{
							TdV2FrameRoleDetector det(2);
							StandInRegistry regRole;
							FrameRoleEvidence ev;
							ev.seq = 1;
							ev.qualityScore = 0.8;
							ev.sourceBits = badBits;
							const auto dec = det.PushAndClassify(ev, &regRole);
							Assert::IsTrue(dec.abstain == AbstainReason::NonFiniteEvidence,
								L"TdV2FrameRoleDetector invalid sourceBits must abstain with NonFiniteEvidence");
							Assert::AreEqual(static_cast<uint8_t>(QualityPattern::FrameRole::Unknown), dec.record.role);
							Assert::IsTrue(regRole.IsActive(StandInKind::NonFiniteClosure),
								L"TdV2FrameRoleDetector invalid sourceBits must visibly engage NonFiniteClosure");
							Assert::IsTrue(det.AbstainCount(AbstainReason::NonFiniteEvidence) >= 1);
						}
					}

					// Branch A (Unidentified grid) non-finite retainedGrainVar (MEDIUM-3) & mismatched cleanCb/Cr dims (MEDIUM-4)
					{
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						TdV2ChromaEngine eng(cfg);
						const TdV2Plane finite444(w, h, 512.0);
						const TdV2Plane diff444(w, h, 520.0);

						// MEDIUM-3: non-finite retainedGrainVar in Unidentified branch -> finite output + NonFiniteClosure!
						{
							StandInRegistry regM3;
							ChromaProcessInput inM3;
							inM3.seq = 70;
							inM3.deliveredFormat = DeliveredChromaFormat::Yuv444;
							inM3.codedSubsampling = ChromaSubsampling::Yuv444;
							inM3.gridStatus = ChromaGridStatus::Unidentified;
							inM3.lumaW = w;
							inM3.lumaH = h;
							inM3.receivedCb = finite444;
							inM3.receivedCr = finite444;
							inM3.hasCleanSplit = true;
							inM3.cleanCb = finite444;
							inM3.cleanCr = finite444;
							inM3.retainedGrainVarCb = { 1.0, qnan, 1.0, 1.0 };
							inM3.retainedGrainVarCr = { 1.0, 1.0, -inf, 1.0 };
							inM3.prevPeak = { &diff444, &diff444, &stillMf, false };
							inM3.nextPeak = { &diff444, &diff444, &stillMf, false };
							inM3.hasStepPrior = true;
							inM3.estimatedStepDelta = 12.0;

							const auto resM3 = eng.ProcessFrame(inM3, &regM3);
							AssertPlaneAllFinite(resM3.outputCb, L"Unidentified non-finite retainedGrainVar Cb must stay finite");
							AssertPlaneAllFinite(resM3.outputCr, L"Unidentified non-finite retainedGrainVar Cr must stay finite");
							Assert::IsTrue(resM3.nonFiniteCells >= 2,
								L"Unidentified non-finite retainedGrainVar MUST increment res.nonFiniteCells");
							Assert::IsTrue(regM3.IsActive(StandInKind::NonFiniteClosure),
								L"Unidentified non-finite retainedGrainVar MUST visibly engage NonFiniteClosure");
						}

						// MEDIUM-4: mismatched cleanCb/Cr dimensions in Unidentified branch -> fail-closed NonFiniteClosure!
						{
							StandInRegistry regM4;
							const TdV2Plane wrongDim(w / 2, h / 2, 512.0);
							ChromaProcessInput inM4;
							inM4.seq = 71;
							inM4.deliveredFormat = DeliveredChromaFormat::Yuv444;
							inM4.codedSubsampling = ChromaSubsampling::Yuv444;
							inM4.gridStatus = ChromaGridStatus::Unidentified;
							inM4.lumaW = w;
							inM4.lumaH = h;
							inM4.receivedCb = finite444;
							inM4.receivedCr = finite444;
							inM4.hasCleanSplit = true;
							inM4.cleanCb = wrongDim;
							inM4.cleanCr = wrongDim;
							inM4.prevPeak = { &diff444, &diff444, &stillMf, false };
							inM4.hasStepPrior = true;
							inM4.estimatedStepDelta = 12.0;

							const auto resM4 = eng.ProcessFrame(inM4, &regM4);
							AssertPlanesBitExact(finite444, resM4.outputCb,
								L"Unidentified mismatched cleanCb/Cr dims MUST fail closed to bit-exact passthrough");
							AssertPlanesBitExact(finite444, resM4.outputCr,
								L"Unidentified mismatched cleanCb/Cr dims MUST fail closed to bit-exact passthrough");
							Assert::IsTrue(resM4.nonFiniteCells > 0,
								L"Unidentified mismatched cleanCb/Cr dims MUST increment res.nonFiniteCells");
							Assert::IsTrue(regM4.IsActive(StandInKind::NonFiniteClosure),
								L"Unidentified mismatched cleanCb/Cr dims MUST visibly engage NonFiniteClosure");
						}
					}

					// NaN/Inf closures across TdV2CodingGridStandIn, TdV2ChromaEngine, and TdV2GrainQ5:
					{
						TdV2Plane nanY = finiteY;
						nanY.at(5, 5) = qnan;
						TdV2CodingGridStandIn grid;
						StandInRegistry regGrid;
						const auto gBad = grid.Measure(nanY.v.data(), finiteY.v.data(), nullptr, w, h, 10, &regGrid);
						Assert::IsFalse(gBad.valid);
						Assert::IsTrue(regGrid.IsActive(StandInKind::NonFiniteClosure));

						TdV2Plane infC = finiteC;
						infC.at(3, 3) = inf;
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						TdV2ChromaEngine eng(cfg);
						StandInRegistry regC1, regC2;

						ChromaProcessInput inBadC;
						inBadC.deliveredFormat = DeliveredChromaFormat::Yuv420;
						inBadC.codedSubsampling = ChromaSubsampling::Yuv420;
						inBadC.lumaW = w;
						inBadC.lumaH = h;
						inBadC.receivedCb = infC;
						inBadC.receivedCr = finiteC;
						const auto rBadC = eng.ProcessFrame(inBadC, &regC1);
						Assert::IsTrue(regC1.IsActive(StandInKind::NonFiniteClosure) && rBadC.nonFiniteCells > 0);

						TdV2Plane nanR = finiteRgb;
						nanR.at(4, 4) = qnan;
						ChromaProcessInput inBadRgb;
						inBadRgb.deliveredFormat = DeliveredChromaFormat::Rgb444;
						inBadRgb.codedSubsampling = ChromaSubsampling::Yuv420;
						inBadRgb.lumaW = w;
						inBadRgb.lumaH = h;
						inBadRgb.receivedR = nanR;
						inBadRgb.receivedG = finiteRgb;
						inBadRgb.receivedB = finiteRgb;
						const auto rBadRgb = eng.ProcessFrame(inBadRgb, &regC2);
						Assert::IsTrue(regC2.IsActive(StandInKind::NonFiniteClosure) && rBadRgb.nonFiniteCells > 0);

						// TdV2GrainQ5 NaN/Inf fail-closed checks:
						StandInRegistry regQ5a, regQ5b, regQ5c, regQ5d, regQ5e;
						Assert::AreEqual(0.0, ComputeCrossIr(qnan, 1.0, &regQ5a), 0.0);
						Assert::IsTrue(regQ5a.IsActive(StandInKind::NonFiniteClosure));
						Assert::AreEqual(0.0, ComputeProjectionIr(1.0, inf, 1.0, &regQ5b), 0.0);
						Assert::IsTrue(regQ5b.IsActive(StandInKind::NonFiniteClosure));

						const auto badPanArg = EvaluateHalfPixelPanIr(finiteY, finiteY, ActiveRect::Full(w, h), qnan, 0.0, 8, 1.0, &regQ5c);
						Assert::IsFalse(badPanArg.valid);
						Assert::IsTrue(regQ5c.IsActive(StandInKind::NonFiniteClosure));

						TdV2Plane nanD(w, h, 1.0);
						nanD.at(12, 12) = qnan;
						const auto badPanPx = EvaluateHalfPixelPanIr(nanD, finiteY, ActiveRect::Full(w, h), 0.5, 0.0, 8, 1.0, &regQ5d);
						Assert::IsTrue(badPanPx.nonFiniteRegions > 0);
						Assert::IsTrue(regQ5d.IsActive(StandInKind::NonFiniteClosure));

						const std::vector<double> badE = { 10.0, qnan, 10.0 };
						const std::vector<double> badIr = { 5.0, 5.0, inf };
						const auto badNlf = AccumulateDarkNlfExcludingZeroEnergy(badE, badIr, 64, 1.0, 0.5, &regQ5e);
						Assert::IsTrue(badNlf.valid && badNlf.nonFiniteBlocks == 2 && badNlf.retainedBlocks == 1);
						Assert::IsTrue(regQ5e.IsActive(StandInKind::NonFiniteClosure));
					}
				}

				// ---- Part B: Non-8-Aligned Letterbox ActiveRect (y0 = 4, y1 = 60 on 64x64) Codec-Aligned 8x8 DCT ----
				{
					const int w = 64, h = 64;
					const ActiveRect active{ 0, 4, 64, 60 }; // bars: [0, 4) and [60, 64); partial strips: [4, 8) and [56, 60); codec 8x8: [8, 56)
					const int cw = w / 16, ch = h / 16;
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					const double stepDelta = 12.0;
					const double phi = kDeadZonePhiInter; // 1/6 -> theta = 10.0
					const double theta = (1.0 - phi) * stepDelta;

					TdV2Plane pred(w, h, 400.0), cur(w, h, 400.0), nextPeak(w, h, 400.0);

					// Plant alternating-sign codec-grid-quantized 8x8 DCT blocks on y in [8, 56), x in [0, 64):
					// Sent DC = +/-36 (k = +/-3), sent vertical AC (v=1, u=0 => index 8) = +/-24 (k = +/-2), zeroed AC (index 1) = 0.
					// Target (nextPeak) tries to alter sent DC & AC and push zeroed AC beyond +/-theta!
					for (int by = 8; by < 56; by += 8)
					{
						const double sign = (((by / 8) % 2) == 0) ? 1.0 : -1.0;
						double sentDct[64] = {}, targetDct[64] = {}, sentSpat[64] = {}, targetSpat[64] = {};
						sentDct[0] = sign * 36.0;
						sentDct[8] = sign * 24.0;
						sentDct[1] = 0.0;
						targetDct[0] = sign * 54.0; // tries to move sent DC by 18
						targetDct[8] = 0.0;         // tries to erase sent vertical AC
						targetDct[1] = 28.0;        // outside dead-zone [-10, +10]
						InverseDct8x8(sentDct, sentSpat);
						InverseDct8x8(targetDct, targetSpat);

						for (int bx = 0; bx < 64; bx += 8)
						{
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									cur.at(bx + i, by + j) = pred.at(bx + i, by + j) + sentSpat[j * 8 + i];
									nextPeak.at(bx + i, by + j) = pred.at(bx + i, by + j) + targetSpat[j * 8 + i];
								}
							}
						}
					}

					// Leading/trailing partial strips [4, 8) and [56, 60): huge difference -> must be bounded via TransformUnknownBound!
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							if (y < 4)
							{
								cur.at(x, y) = 16.0; pred.at(x, y) = 16.0; nextPeak.at(x, y) = 16.0;
							}
							else if (y >= 60)
							{
								cur.at(x, y) = 992.0; pred.at(x, y) = 992.0; nextPeak.at(x, y) = 992.0;
							}
							else if (y < 8 || y >= 56)
							{
								cur.at(x, y) = 580.0; pred.at(x, y) = 400.0; nextPeak.at(x, y) = 400.0;
							}
						}
					}

					// Run TdV2ReferenceEngine in STRICT mode with transformIdentified8x8 = true.
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.estimator = VarianceEstimator::A2_RoundingCells;
					cfg.residualMode = ResidualMode::Strict;
					cfg.cellProjIterations = 1;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 90;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.activeRect = active;
					in.prevPeak = { &pred, &stillMf, true };
					in.nextPeak = { &nextPeak, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = stepDelta;
					in.deadZonePhi = phi;
					in.zeroCoeffFraction = 0.0;
					in.transformIdentified8x8 = true;

					const auto res = eng.ProcessFrame(in, &reg);

					// 1. Bars [0, 4) and [60, 64) remain bit-exact!
					AssertBarsBitExact(cur, res.outputY, active, L"Letterbox bars [0,4) and [60,64) must remain bit-exact");

					// 2. Leading/trailing partial strips [4, 8) and [56, 60) trigger TransformUnknownBound visibly and stay bounded!
					Assert::IsTrue(reg.IsActive(StandInKind::TransformUnknownBound),
						L"Partial strips [4,8) and [56,60) MUST visibly engage StandInKind::TransformUnknownBound");
					const RoundingCellStats st = ComputeRoundingCellStats(stepDelta, phi, 0.0);
					// Exact Parseval passband supremum ||H_b||_inf^2 (M3): L=1.0, M=midPassbandPeak^2, H=1.0.
					const double s1Sq = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
					const double s2Sq = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
					const double wStarSq = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2Sq - s1Sq);
					const double midPassbandPeak = std::exp(-0.5 * s1Sq * wStarSq) - std::exp(-0.5 * s2Sq * wStarSq);
					const double maxStripBound = kPixelBoundSigma * (
						std::sqrt(st.totalVar * 1.0) +
						std::sqrt(st.totalVar * (midPassbandPeak * midPassbandPeak)) +
						std::sqrt(st.totalVar * 1.0));
					for (int y = 4; y < 60; ++y)
					{
						if (y >= 8 && y < 56) continue;
						for (int x = 0; x < w; ++x)
						{
							const double d = std::fabs(res.outputY.at(x, y) - cur.at(x, y));
							Assert::IsTrue(d <= maxStripBound + 1e-6,
								L"Partial strip delta must stay strictly inside TransformUnknownBound");
						}
					}

					// 3. Every codec-grid interior 8x8 block (y = 8..56) preserves sent DCT coefficients exact in STRICT mode!
					double maxSentErrAligned = 0.0;
					for (int by = 8; by < 56; by += 8)
					{
						const double sign = (((by / 8) % 2) == 0) ? 1.0 : -1.0;
						for (int bx = 0; bx < 64; bx += 8)
						{
							double sp[64] = {}, dct[64] = {};
							for (int j = 0; j < 8; ++j)
								for (int i = 0; i < 8; ++i)
									sp[j * 8 + i] = res.outputClean.at(bx + i, by + j) - pred.at(bx + i, by + j);
							ForwardDct8x8(sp, dct);
							const double e0 = std::fabs(dct[0] - sign * 36.0);
							const double e8 = std::fabs(dct[8] - sign * 24.0);
							maxSentErrAligned = (std::max)(maxSentErrAligned, (std::max)(e0, e8));
							Assert::AreEqual(sign * 36.0, dct[0], 1e-6, L"Codec-aligned STRICT must keep sent DC exact");
							Assert::AreEqual(sign * 24.0, dct[8], 1e-6, L"Codec-aligned STRICT must keep sent vertical AC exact");
							Assert::IsTrue(std::fabs(dct[1]) <= theta + 1e-6, L"Codec-aligned STRICT must clip zeroed AC to [-theta, +theta]");
						}
					}

					// ---- Mandatory In-Test Negative Control (Rule 3) ----
					// Misaligned y0-anchored 8x8 DCT (by = 4, 12, 20, 28, 36, 44, 52) on the EXACT SAME codec-grid-quantized signal!
					TdV2Plane misalignedOut = cur;
					for (int by = active.y0; by + 8 <= active.y1; by += 8)
					{
						for (int bx = 0; bx < 64; bx += 8)
						{
							double sentR[64] = {}, blendR[64] = {}, projR[64] = {};
							for (int j = 0; j < 8; ++j)
							{
								for (int i = 0; i < 8; ++i)
								{
									const double cVal = cur.at(bx + i, by + j);
									const double pVal = pred.at(bx + i, by + j);
									const double nVal = nextPeak.at(bx + i, by + j);
									sentR[j * 8 + i] = cVal - pVal;
									blendR[j * 8 + i] = (0.5 * (cVal + nVal)) - pVal;
								}
							}
							ProjectResidual8x8Dct(sentR, blendR, stepDelta, phi, ResidualMode::Strict, projR);
							for (int j = 0; j < 8; ++j)
								for (int i = 0; i < 8; ++i)
									misalignedOut.at(bx + i, by + j) = pred.at(bx + i, by + j) + projR[j * 8 + i];
						}
					}

					// Measure true codec-grid sent-coefficient corruption & spurious cross-block AC leakage from misaligned y0-anchoring!
					double maxSentErrMisaligned = 0.0;
					double maxSpuriousAcLeak = 0.0;
					for (int by = 8; by < 56; by += 8)
					{
						const double sign = (((by / 8) % 2) == 0) ? 1.0 : -1.0;
						for (int bx = 0; bx < 64; bx += 8)
						{
							double sp[64] = {}, dct[64] = {};
							for (int j = 0; j < 8; ++j)
								for (int i = 0; i < 8; ++i)
									sp[j * 8 + i] = misalignedOut.at(bx + i, by + j) - pred.at(bx + i, by + j);
							ForwardDct8x8(sp, dct);
							maxSentErrMisaligned = (std::max)(maxSentErrMisaligned, std::fabs(dct[0] - sign * 36.0));
							maxSentErrMisaligned = (std::max)(maxSentErrMisaligned, std::fabs(dct[8] - sign * 24.0));
							// Check spurious leakage into originally-zeroed vertical AC coefficients (v >= 2, u = 0 => idx 16, 24, 32, 40, 48, 56).
							for (int v = 2; v < 8; ++v)
								maxSpuriousAcLeak = (std::max)(maxSpuriousAcLeak, std::fabs(dct[v * 8]));
						}
					}

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"Codec-aligned 8x8 DCT on y0=4..60: alignedSentErr=%.2e | misalignedSentErr=%.2f spuriousAcLeak=%.2f",
						maxSentErrAligned, maxSentErrMisaligned, maxSpuriousAcLeak);
					Logger::WriteMessage(buf);

					Assert::IsTrue(maxSentErrMisaligned > 2.0,
						L"Negative control: misaligned y0-anchored 8x8 DCT MUST corrupt codec-grid sent DCT coefficients");
					Assert::IsTrue(maxSpuriousAcLeak > 1.0,
						L"Negative control: misaligned y0-anchored 8x8 DCT MUST create non-zero spurious cross-block DCT leakage");

					// Also verify TdV2ChromaEngine respects the exact same codec-aligned 8x8 DCT rect on Yuv444!
					{
						TdV2ChromaEngine cEng(cfg);
						StandInRegistry cReg;
						ChromaProcessInput cIn;
						cIn.seq = 91;
						cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
						cIn.codedSubsampling = ChromaSubsampling::Yuv444;
						cIn.gridStatus = ChromaGridStatus::Identified;
						cIn.lumaW = w;
						cIn.lumaH = h;
						cIn.activeRect = active;
						cIn.receivedCb = cur;
						cIn.receivedCr = cur;
						cIn.hasCleanSplit = true;
						cIn.cleanCb = cur;
						cIn.cleanCr = cur;
						cIn.prevPeak = { &pred, &pred, &stillMf, true };
						cIn.nextPeak = { &nextPeak, &nextPeak, &stillMf, false };
						cIn.hasStepPrior = true;
						cIn.estimatedStepDelta = stepDelta;
						cIn.deadZonePhi = phi;
						cIn.transformIdentified8x8 = true;

						const auto cRes = cEng.ProcessFrame(cIn, &cReg);
						AssertBarsBitExact(cur, cRes.outputCb, active, L"Chroma letterbox bars must remain bit-exact");
						Assert::IsTrue(cReg.IsActive(StandInKind::TransformUnknownBound),
							L"Chroma partial strips must engage TransformUnknownBound visibly");
						// Verify both even (by=16, sign=+1) and odd (by=24, sign=-1) codec-aligned 8x8 chroma blocks!
						for (int by : { 16, 24 })
						{
							const double sign = (((by / 8) % 2) == 0) ? 1.0 : -1.0;
							double spC[64] = {}, dctC[64] = {};
							for (int j = 0; j < 8; ++j)
								for (int i = 0; i < 8; ++i)
									spC[j * 8 + i] = cRes.outputCb.at(16 + i, by + j) - pred.at(16 + i, by + j);
							ForwardDct8x8(spC, dctC);
							Assert::AreEqual(sign * 36.0, dctC[0], 1e-6, L"Chroma codec-aligned STRICT must keep sent DC exact");
							Assert::AreEqual(sign * 24.0, dctC[8], 1e-6, L"Chroma codec-aligned STRICT must keep sent vertical AC exact");
						}
					}
				}

				// ---- Part C (LOW-4b, MEDIUM-1, MEDIUM-2): Tiny ActiveRect (< 8 px), Zero-Delta 8x8 DCT Bit-Exactness, Odd-Dim 65x65 Chroma ----
				{
					// C1: Tiny ActiveRect (width=5, height=5 => bxStart=8 >= bxEnd=0) with transformIdentified8x8 = true
					// MUST NOT crash, MUST engage TransformUnknownBound visibly, and keep bars bit-exact!
					{
						const int tw = 32, th = 32;
						const ActiveRect tinyActive{ 2, 2, 7, 7 }; // width = 5 < 8 => zero full codec 8x8 blocks!
						const CellMotionField tMf = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
						TdV2Plane tCur(tw, th, 16.0), tRef(tw, th, 16.0);
						for (int y = tinyActive.y0; y < tinyActive.y1; ++y)
						{
							for (int x = tinyActive.x0; x < tinyActive.x1; ++x)
							{
								tCur.at(x, y) = 520.0;
								tRef.at(x, y) = 400.0;
							}
						}

						Config tCfg = MakePreset(Preset::Reference, Mode::On);
						tCfg.estimator = VarianceEstimator::A2_RoundingCells;
						tCfg.residualMode = ResidualMode::Strict;
						TdV2ReferenceEngine rEng(tCfg);
						StandInRegistry rReg;
						FrameProcessInput rIn;
						rIn.seq = 95;
						rIn.receivedY = tCur;
						rIn.hasCleanSplit = true;
						rIn.cleanY = tCur;
						rIn.activeRect = tinyActive;
						rIn.prevPeak = { &tRef, &tMf, false };
						rIn.nextPeak = { &tRef, &tMf, false };
						rIn.hasStepPrior = true;
						rIn.estimatedStepDelta = 12.0;
						rIn.transformIdentified8x8 = true;
						const auto rRes = rEng.ProcessFrame(rIn, &rReg);
						AssertPlaneAllFinite(rRes.outputY, L"Tiny ActiveRect (< 8 px) luma output must be finite");
						AssertBarsBitExact(tCur, rRes.outputY, tinyActive, L"Tiny ActiveRect bars must remain bit-exact");
						Assert::IsTrue(rReg.IsActive(StandInKind::TransformUnknownBound),
							L"Tiny ActiveRect (< 8 px) luma MUST visibly engage TransformUnknownBound");

						TdV2ChromaEngine cEng(tCfg);
						StandInRegistry cReg;
						ChromaProcessInput cIn;
						cIn.seq = 96;
						cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
						cIn.codedSubsampling = ChromaSubsampling::Yuv444;
						cIn.gridStatus = ChromaGridStatus::Identified;
						cIn.lumaW = tw;
						cIn.lumaH = th;
						cIn.activeRect = tinyActive;
						cIn.receivedCb = tCur;
						cIn.receivedCr = tCur;
						cIn.hasCleanSplit = true;
						cIn.cleanCb = tCur;
						cIn.cleanCr = tCur;
						cIn.prevPeak = { &tRef, &tRef, &tMf, false };
						cIn.nextPeak = { &tRef, &tRef, &tMf, false };
						cIn.hasStepPrior = true;
						cIn.estimatedStepDelta = 12.0;
						cIn.transformIdentified8x8 = true;
						const auto cRes = cEng.ProcessFrame(cIn, &cReg);
						AssertPlaneAllFinite(cRes.outputCb, L"Tiny ActiveRect (< 8 px) chroma output must be finite");
						AssertBarsBitExact(tCur, cRes.outputCb, tinyActive, L"Tiny ActiveRect chroma bars must remain bit-exact");
						Assert::IsTrue(cReg.IsActive(StandInKind::TransformUnknownBound),
							L"Tiny ActiveRect (< 8 px) chroma MUST visibly engage TransformUnknownBound");
					}

					// C2 (MEDIUM-1): Zero-delta transformIdentified8x8 = true MUST remain bit-exact 0.0 (no 1e-13 DCT round-trip noise!)
					{
						const int zw = 32, zh = 32;
						const CellMotionField zMf = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
						TdV2Plane zCur(zw, zh, 400.0);
						for (int y = 0; y < zh; ++y)
							for (int x = 0; x < zw; ++x)
								zCur.at(x, y) = 320.0 + 95.3 * std::sin(2.0 * kPi * x / 7.0) + 41.7 * std::cos(2.0 * kPi * y / 9.0);
						const TdV2Plane zPred = AddGaussianNoise(zCur, 18.0, 991, ActiveRect::Full(zw, zh));

						for (ResidualMode rm : { ResidualMode::Strict, ResidualMode::Cell })
						{
							Config zCfg = MakePreset(Preset::Reference, Mode::On);
							zCfg.residualMode = rm;

							TdV2ReferenceEngine rEng(zCfg);
							StandInRegistry rReg;
							FrameProcessInput rIn;
							rIn.seq = 97;
							rIn.receivedY = zCur;
							rIn.hasCleanSplit = true;
							rIn.cleanY = zCur;
							rIn.prevPeak = { &zPred, &zMf, true }; // exact copy -> zero blend delta!
							rIn.hasStepPrior = true;
							rIn.estimatedStepDelta = 12.0;
							rIn.transformIdentified8x8 = true;
							const auto rRes = rEng.ProcessFrame(rIn, &rReg);
							AssertPlanesBitExact(zCur, rRes.outputY,
								L"Zero-delta transformIdentified8x8 luma MUST be bit-exact (zero DCT round-trip noise)");

							TdV2ChromaEngine cEng(zCfg);
							StandInRegistry cReg;
							ChromaProcessInput cIn;
							cIn.seq = 98;
							cIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
							cIn.codedSubsampling = ChromaSubsampling::Yuv444;
							cIn.gridStatus = ChromaGridStatus::Identified;
							cIn.lumaW = zw;
							cIn.lumaH = zh;
							cIn.receivedCb = zCur;
							cIn.receivedCr = zCur;
							cIn.hasCleanSplit = true;
							cIn.cleanCb = zCur;
							cIn.cleanCr = zCur;
							cIn.prevPeak = { &zPred, &zPred, &zMf, true };
							cIn.hasStepPrior = true;
							cIn.estimatedStepDelta = 12.0;
							cIn.transformIdentified8x8 = true;
							const auto cRes = cEng.ProcessFrame(cIn, &cReg);
							AssertPlanesBitExact(zCur, cRes.outputCb,
								L"Zero-delta transformIdentified8x8 chroma Cb MUST be bit-exact (zero DCT round-trip noise)");
							AssertPlanesBitExact(zCur, cRes.outputCr,
								L"Zero-delta transformIdentified8x8 chroma Cr MUST be bit-exact (zero DCT round-trip noise)");
							for (double d : cRes.codedDeltaCb.v)
								Assert::AreEqual(0.0, d, 0.0, L"Zero-delta transformIdentified8x8 codedDeltaCb must be exact 0.0");
						}
					}

					// C3 (MEDIUM-2): Odd-dimension (65x65) Left-sited 4:2:0 chroma MUST NOT drop last coded row/col (idx 32)!
					{
						const int ow = 65, oh = 65;
						const int ocw = (ow + 15) / 16, och = (oh + 15) / 16; // 5x5 cells
						const int ocW = (ow + 1) / 2, ocH = (oh + 1) / 2;     // 33x33 coded 4:2:0 plane
						const CellMotionField oMf = CellMotionField::Uniform(ocw, och, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
						const SitingSpec leftSiting = SitingSpec::Left();
						const ActiveRect oActive = ActiveRect::Full(ow, oh);
						const ActiveRect ocActive = ActiveRect::Full(ocW, ocH);

						const TdV2Plane oCleanCb = SampleAnalyticChromaPlane(ow, oh, 2, 2, leftSiting, 0.0, 0.0, 32.0, 32.0, 4.0, true);
						const TdV2Plane oCleanCr = SampleAnalyticChromaPlane(ow, oh, 2, 2, leftSiting, 0.0, 0.0, 32.0, 32.0, 4.0, false);
						const TdV2Plane oNoisyCb = AddGaussianNoise(oCleanCb, 10.0, 995, ocActive);
						const TdV2Plane oNoisyCr = AddGaussianNoise(oCleanCr, 10.0, 996, ocActive);
						const TdV2Plane oPrevCb = AddGaussianNoise(oCleanCb, 0.5, 997, ocActive);
						const TdV2Plane oPrevCr = AddGaussianNoise(oCleanCr, 0.5, 998, ocActive);
						const TdV2Plane oNextCb = AddGaussianNoise(oCleanCb, 0.5, 999, ocActive);
						const TdV2Plane oNextCr = AddGaussianNoise(oCleanCr, 0.5, 1000, ocActive);

						Config oCfg = MakePreset(Preset::Reference, Mode::On);
						TdV2ChromaEngine oEng(oCfg);
						StandInRegistry oReg;
						ChromaProcessInput oIn;
						oIn.seq = 99;
						oIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
						oIn.codedSubsampling = ChromaSubsampling::Yuv420;
						oIn.gridStatus = ChromaGridStatus::Identified;
						oIn.siting = leftSiting;
						oIn.lumaW = ow;
						oIn.lumaH = oh;
						oIn.activeRect = oActive;
						oIn.receivedCb = oNoisyCb;
						oIn.receivedCr = oNoisyCr;
						oIn.hasCleanSplit = true;
						oIn.cleanCb = oNoisyCb;
						oIn.cleanCr = oNoisyCr;
						oIn.prevPeak = { &oPrevCb, &oPrevCr, &oMf, false };
						oIn.nextPeak = { &oNextCb, &oNextCr, &oMf, false };
						oIn.hasStepPrior = true;
						oIn.estimatedStepDelta = 24.0;

						const auto oRes = oEng.ProcessFrame(oIn, &oReg);
						AssertPlaneAllFinite(oRes.outputCb, L"Odd-dim 65x65 4:2:0 Cb must be finite");
						AssertPlaneAllFinite(oRes.outputCr, L"Odd-dim 65x65 4:2:0 Cr must be finite");

						// Verify last coded column (cx = 32) AND last coded row (cy = 32) are genuinely repaired (NOT dropped)!
						const ActiveRect lastCol{ ocW - 1, 4, ocW, ocH - 4 };
						const ActiveRect lastRow{ 4, ocH - 1, ocW - 4, ocH };
						const double baseColErr = MaskedRmse(oCleanCb, oNoisyCb, lastCol);
						const double outColErr = MaskedRmse(oCleanCb, oRes.outputCb, lastCol);
						const double baseRowErr = MaskedRmse(oCleanCb, oNoisyCb, lastRow);
						const double outRowErr = MaskedRmse(oCleanCb, oRes.outputCb, lastRow);
						Assert::IsTrue(outColErr < baseColErr * 0.70,
							L"Odd-dim 65x65 last coded column (cx=32) MUST be repaired, not dropped!");
						Assert::IsTrue(outRowErr < baseRowErr * 0.70,
							L"Odd-dim 65x65 last coded row (cy=32) MUST be repaired, not dropped!");

						// Also verify TdV2ChromaAnchorCanvas on odd-dim 65x65 Left-sited 4:2:0 keeps last coded row/col (idx 32)!
						TdV2ChromaAnchorCanvas oddCanvas;
						StandInRegistry oddReg;
						Assert::IsTrue(oddCanvas.AccumulateAnchorFrame(
							oCleanCb, oCleanCr, nullptr, ow, oh, ChromaSubsampling::Yuv420,
							leftSiting, oActive, &oMf, 16, 10, &oddReg));
						TdV2Plane oddQCb, oddQCr;
						std::vector<uint8_t> oddValid;
						Assert::IsTrue(oddCanvas.SampleAtCodedGrid(
							ChromaSubsampling::Yuv420, leftSiting, oActive, &oMf, 16,
							oddQCb, oddQCr, oddValid, &oddReg));
						Assert::AreEqual(static_cast<uint8_t>(1u), oddValid[static_cast<size_t>(16) * ocW + (ocW - 1)],
							L"Odd-dim 65x65 anchor canvas must keep last coded column (cx=32) valid");
						Assert::AreEqual(static_cast<uint8_t>(1u), oddValid[static_cast<size_t>(ocH - 1) * ocW + 16],
							L"Odd-dim 65x65 anchor canvas must keep last coded row (cy=32) valid");
						Assert::IsTrue(MaskedRmse(oCleanCb, oddQCb, lastRow) < 1e-3,
							L"Odd-dim 65x65 anchor canvas must accurately retain last coded row (cy=32)");
					}
				}
			}

			// =========================================================================
			// 10. H2 — No-Split, Zero-Prior Chroma Grain Protection & Canvas Non-Ingest
			// =========================================================================
			TEST_METHOD(ChromaNoSplitZeroPriorProtectsGrainAndBlocksCanvasIngest)
			{
				// Adversarial H2 proof:
				// With hasCleanSplit == false AND retainedGrainVar empty (zero prior!), TdV2ChromaEngine MUST:
				//   1. Protect own chroma grain and block foreign grain (full authoritative bar with lag1Evaluated == true!)
				//   2. NEVER ingest unsplit grainy chroma into AnchorCanvas even when isAnchorFrame == true!
				//   3. Treat hasCleanSplit == true with empty cleanCb/cleanCr identically to !splitActive!
				// Backed by a same-input unprotected direct-blend negative control proving foreign grain leaks
				// and own grain collapses sharply without protection.
				const int w = 128, h = 128;
				const int cw = w / 16, ch = h / 16;
				const int cW = w / 2, cH = h / 2;
				const size_t cN = static_cast<size_t>(cW) * cH;
				const ActiveRect active = ActiveRect::Full(w, h);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const SitingSpec siting = SitingSpec::Left();
				const double grainSigma = 12.0;

				const TdV2Plane masterY(w, h, 450.0);
				const TdV2Plane masterCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
				const TdV2Plane masterCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);

				std::mt19937 r0(9200u), r1(9201u), r2(9202u), r3(9203u);
				std::normal_distribution<double> gd(0.0, grainSigma);
				std::vector<double> prevOwnCb(cN), prevOwnCr(cN), ownCb(cN), ownCr(cN);
				std::vector<double> forPrevCb(cN), forPrevCr(cN), forNextCb(cN), forNextCr(cN);
				TdV2Plane prevFrameCb(cW, cH), prevFrameCr(cW, cH), curCb(cW, cH), curCr(cW, cH);
				TdV2Plane refPrevCb(cW, cH), refPrevCr(cW, cH), refNextCb(cW, cH), refNextCr(cW, cH);
				for (size_t i = 0; i < cN; ++i)
				{
					prevOwnCb[i] = gd(r0); prevOwnCr[i] = gd(r0);
					ownCb[i] = gd(r1); ownCr[i] = gd(r1);
					forPrevCb[i] = gd(r2); forPrevCr[i] = gd(r2);
					forNextCb[i] = gd(r3); forNextCr[i] = gd(r3);

					prevFrameCb.v[i] = masterCb.v[i] + prevOwnCb[i];
					prevFrameCr.v[i] = masterCr.v[i] + prevOwnCr[i];
					curCb.v[i] = masterCb.v[i] + ownCb[i];
					curCr.v[i] = masterCr.v[i] + ownCr[i];
					refPrevCb.v[i] = masterCb.v[i] + forPrevCb[i];
					refPrevCr.v[i] = masterCr.v[i] + forPrevCr[i];
					refNextCb.v[i] = masterCb.v[i] + forNextCb[i];
					refNextCr.v[i] = masterCr.v[i] + forNextCr[i];
				}

				// Mandatory In-Test Negative Control (Rule 3): unprotected direct blend without Layer-4 guard on exact same inputs!
				std::vector<double> naivePrevResCb(cN), naivePrevResCr(cN), naiveOutCb(cN), naiveOutCr(cN);
				for (size_t i = 0; i < cN; ++i)
				{
					naivePrevResCb[i] = (prevFrameCb.v[i] + refPrevCb.v[i] + refNextCb.v[i]) / 3.0 - masterCb.v[i];
					naivePrevResCr[i] = (prevFrameCr.v[i] + refPrevCr.v[i] + refNextCr.v[i]) / 3.0 - masterCr.v[i];
					naiveOutCb[i] = (curCb.v[i] + refPrevCb.v[i] + refNextCb.v[i]) / 3.0;
					naiveOutCr[i] = (curCr.v[i] + refPrevCr.v[i] + refNextCr.v[i]) / 3.0;
				}
				const auto naiveRepCb = EvaluateGrainTransfer(
					masterCb.v.data(), ownCb.data(), forPrevCb.data(), naiveOutCb.data(), cN,
					naivePrevResCb.data(), prevOwnCb.data());
				const auto naiveRepCr = EvaluateGrainTransfer(
					masterCr.v.data(), ownCr.data(), forPrevCr.data(), naiveOutCr.data(), cN,
					naivePrevResCr.data(), prevOwnCr.data());
				Assert::IsTrue(naiveRepCb.valid && naiveRepCr.valid && naiveRepCb.lag1Evaluated && naiveRepCr.lag1Evaluated);
				Assert::IsFalse(naiveRepCb.passesAuthoritativeBar,
					L"H2 Negative control: unprotected direct blend MUST fail authoritative grain bar");
				Assert::IsTrue(naiveRepCb.betaOwn < 0.75 && naiveRepCb.betaForeign > 0.25,
					L"H2 Negative control: unprotected direct blend destroys own Cb grain and leaks foreign grain");
				Assert::IsTrue(naiveRepCr.betaOwn < 0.75 && naiveRepCr.betaForeign > 0.25,
					L"H2 Negative control: unprotected direct blend destroys own Cr grain and leaks foreign grain");

				// Case A: hasCleanSplit == false, retainedGrainVarCb/Cr EMPTY (zero prior!), isAnchorFrame == true!
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ChromaEngine eng(cfg);
				StandInRegistry reg;

				ChromaProcessInput in1;
				in1.seq = 1;
				in1.deliveredFormat = DeliveredChromaFormat::Yuv420;
				in1.codedSubsampling = ChromaSubsampling::Yuv420;
				in1.gridStatus = ChromaGridStatus::Identified;
				in1.siting = siting;
				in1.lumaW = w;
				in1.lumaH = h;
				in1.activeRect = active;
				in1.receivedCb = prevFrameCb;
				in1.receivedCr = prevFrameCr;
				in1.hasCleanSplit = false;
				// Zero prior: retainedGrainVarCb & retainedGrainVarCr intentionally left EMPTY!
				in1.isAnchorFrame = true;
				in1.anchorCleanY = masterY;
				in1.prevPeak = { &refPrevCb, &refPrevCr, &stillMf, false };
				in1.nextPeak = { &refNextCb, &refNextCr, &stillMf, false };
				in1.hasStepPrior = true;
				in1.estimatedStepDelta = 16.0;
				const auto res1 = eng.ProcessFrame(in1, &reg);

				Assert::IsTrue(reg.IsActive(StandInKind::P10gGrainSeparator),
					L"H2: no-split chroma path must visibly engage StandInKind::P10gGrainSeparator");
				Assert::IsFalse(eng.AnchorCanvas().HasData(),
					L"H2: AnchorCanvas MUST NEVER ingest unsplit grainy chroma (hasCleanSplit == false)");

				ChromaProcessInput in2 = in1;
				in2.seq = 2;
				in2.receivedCb = curCb;
				in2.receivedCr = curCr;
				const auto res2 = eng.ProcessFrame(in2, &reg);
				Assert::IsFalse(eng.AnchorCanvas().HasData(),
					L"H2: AnchorCanvas MUST remain empty across unsplit frames");

				std::vector<double> prevResCb(cN), prevResCr(cN);
				for (size_t i = 0; i < cN; ++i)
				{
					prevResCb[i] = res1.outputCb.v[i] - masterCb.v[i];
					prevResCr[i] = res1.outputCr.v[i] - masterCr.v[i];
				}
				const auto repCb = EvaluateGrainTransfer(
					masterCb.v.data(), ownCb.data(), forPrevCb.data(), res2.outputCb.v.data(), cN,
					prevResCb.data(), prevOwnCb.data());
				const auto repCr = EvaluateGrainTransfer(
					masterCr.v.data(), ownCr.data(), forPrevCr.data(), res2.outputCr.v.data(), cN,
					prevResCr.data(), prevOwnCr.data());
				const auto repCbNext = EvaluateGrainTransfer(
					masterCb.v.data(), ownCb.data(), forNextCb.data(), res2.outputCb.v.data(), cN,
					prevResCb.data(), prevOwnCb.data());

				Assert::IsTrue(repCb.valid && repCb.lag1Evaluated && repCb.passesAuthoritativeBar,
					L"H2: no-split zero-prior Cb must pass full authoritative grain bar with lag1Evaluated==true");
				Assert::IsTrue(repCr.valid && repCr.lag1Evaluated && repCr.passesAuthoritativeBar,
					L"H2: no-split zero-prior Cr must pass full authoritative grain bar with lag1Evaluated==true");
				Assert::IsTrue(repCbNext.lag1Evaluated && std::fabs(repCbNext.betaForeign) <= kGrainBarMaxBetaForeign);

				// Case B: hasCleanSplit == true with EMPTY cleanCb/cleanCr planes MUST behave identically to !splitActive!
				TdV2ChromaEngine engEmpty(cfg);
				StandInRegistry regEmpty;
				ChromaProcessInput inEmpty1 = in1;
				inEmpty1.hasCleanSplit = true;
				inEmpty1.cleanCb = TdV2Plane{};
				inEmpty1.cleanCr = TdV2Plane{};
				const auto resEmpty1 = engEmpty.ProcessFrame(inEmpty1, &regEmpty);

				ChromaProcessInput inEmpty2 = in2;
				inEmpty2.hasCleanSplit = true;
				inEmpty2.cleanCb = TdV2Plane{};
				inEmpty2.cleanCr = TdV2Plane{};
				const auto resEmpty2 = engEmpty.ProcessFrame(inEmpty2, &regEmpty);

				Assert::IsTrue(regEmpty.IsActive(StandInKind::P10gGrainSeparator),
					L"H2: hasCleanSplit=true with empty planes must behave as !splitActive and engage P10gGrainSeparator");
				Assert::IsFalse(engEmpty.AnchorCanvas().HasData(),
					L"H2: hasCleanSplit=true with empty planes MUST NOT ingest into AnchorCanvas");
				AssertPlanesBitExact(res1.outputCb, resEmpty1.outputCb,
					L"H2: hasCleanSplit=true with empty planes must match hasCleanSplit=false bit-exact (Frame 1 Cb)");
				AssertPlanesBitExact(res2.outputCb, resEmpty2.outputCb,
					L"H2: hasCleanSplit=true with empty planes must match hasCleanSplit=false bit-exact (Frame 2 Cb)");
				AssertPlanesBitExact(res2.outputCr, resEmpty2.outputCr,
					L"H2: hasCleanSplit=true with empty planes must match hasCleanSplit=false bit-exact (Frame 2 Cr)");
			}

			// =========================================================================
			// 11. H3 — Occlusion / valid=0 Motion: Zero 0.0-Hole Band Bleed & POCS Skip
			// =========================================================================
			TEST_METHOD(ChromaOcclusionInvalidMotionZeroHoleBleedAndPocsSkip)
			{
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16; // 6x6 luma cells
				const int cW = w / 2, cH = h / 2;   // 48x48 coded 4:2:0 plane (each luma cell = 8x8 chroma px)
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect cActive = ActiveRect::Full(cW, cH);
				const SitingSpec siting = SitingSpec::TopLeftType2();
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Non-zero chroma (~512 Cb, ~560 Cr) with correlated luma so both AnchorCanvas AND Carried CfL run!
				TdV2Plane anchorY(w, h, 420.0);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						anchorY.at(x, y) = 420.0 + 40.0 * std::sin(2.0 * kPi * x / 16.0) * std::cos(2.0 * kPi * y / 16.0);

				TdV2Plane cleanCb(cW, cH, 512.0), cleanCr(cW, cH, 560.0);
				for (int cy = 0; cy < cH; ++cy)
				{
					const int ly = static_cast<int>(siting.CodedToLumaY(static_cast<double>(cy), 2));
					for (int cx = 0; cx < cW; ++cx)
					{
						const int lx = static_cast<int>(siting.CodedToLumaX(static_cast<double>(cx), 2));
						const double dy = anchorY.at(lx, ly) - 420.0;
						cleanCb.at(cx, cy) = 512.0 + 0.30 * dy;
						cleanCr.at(cx, cy) = 560.0 - 0.25 * dy;
					}
				}
				const TdV2Plane noisyCurCb = AddGaussianNoise(cleanCb, 3.0, 9301u, cActive);
				const TdV2Plane noisyCurCr = AddGaussianNoise(cleanCr, 3.0, 9302u, cActive);

				// Occluded motion field: central 2x2 cells (cx in {2,3}, cy in {2,3} => coded [16,32) x [16,32)) have valid = 0!
				CellMotionField occMf = stillMf;
				for (int cy = 2; cy <= 3; ++cy)
					for (int cx = 2; cx <= 3; ++cx)
						occMf.valid[static_cast<size_t>(cy) * cw + cx] = 0u;

				// Valid neighbor ring: 1..6 coded pixels directly outside the occluded [16, 32)^2 hole
				// (strictly inside DecomposeActiveBands sigma=2.5 blur radius = 7.5 px!).
				std::vector<uint8_t> ringMask(static_cast<size_t>(cW) * cH, 0u);
				for (int cy = 10; cy < 38; ++cy)
				{
					for (int cx = 10; cx < 38; ++cx)
					{
						const bool inHole = (cx >= 16 && cx < 32 && cy >= 16 && cy < 32);
						if (!inHole)
							ringMask[static_cast<size_t>(cy) * cW + cx] = 1u;
					}
				}

				// ---- Mandatory In-Test Negative Control (Rule 3): Demonstrate massive DecomposeActiveBands error from 0.0 holes ----
				{
					TdV2Plane zeroHolePlane = cleanCb;
					for (int cy = 16; cy < 32; ++cy)
						for (int cx = 16; cx < 32; ++cx)
							zeroHolePlane.at(cx, cy) = 0.0; // what pre-H3 SampleAtCodedGrid/CfL left on valid=0!
					const ThreeBandPlanes trueB = DecomposeActiveBands(cleanCb, cActive);
					const ThreeBandPlanes holeB = DecomposeActiveBands(zeroHolePlane, cActive);
					const double naiveRingBleedL = MaskedRmse(trueB.L, holeB.L, cActive, &ringMask);
					const double naiveRingBleedM = MaskedRmse(trueB.M, holeB.M, cActive, &ringMask);
					Assert::IsTrue(naiveRingBleedL > 30.0 && naiveRingBleedM > 15.0,
						L"H3 Negative control: 0.0 hole blurred by DecomposeActiveBands(sigma=2.5) MUST induce massive (>30 code) band error in valid neighbors");
				}

				// ---- Sub-case A: Canvas + Carried CfL with valid=0 occlusion -> zero 0.0-hole bleed! ----
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.estimator = VarianceEstimator::A2_RoundingCells;
				TdV2ChromaEngine eng(cfg);
				StandInRegistry reg;

				// Seed anchor canvas & CfL on Frame 1.
				ChromaProcessInput fAnc;
				fAnc.seq = 1;
				fAnc.deliveredFormat = DeliveredChromaFormat::Yuv420;
				fAnc.codedSubsampling = ChromaSubsampling::Yuv420;
				fAnc.gridStatus = ChromaGridStatus::Identified;
				fAnc.siting = siting;
				fAnc.lumaW = w;
				fAnc.lumaH = h;
				fAnc.activeRect = active;
				fAnc.receivedY = anchorY;
				fAnc.receivedCb = cleanCb;
				fAnc.receivedCr = cleanCr;
				fAnc.hasCleanSplit = true;
				fAnc.cleanCb = cleanCb;
				fAnc.cleanCr = cleanCr;
				fAnc.isAnchorFrame = true;
				fAnc.anchorCleanY = anchorY;
				fAnc.hasStepPrior = true;
				fAnc.estimatedStepDelta = 16.0;
				eng.ProcessFrame(fAnc, &reg);
				Assert::IsTrue(eng.AnchorCanvas().HasData() && eng.AnchorCanvas().HasCfl());

				// Frame 2: occluded valid=0 central region, using Canvas + Carried CfL as references!
				ChromaProcessInput fOcc = fAnc;
				fOcc.seq = 2;
				fOcc.isAnchorFrame = false;
				fOcc.receivedCb = noisyCurCb;
				fOcc.receivedCr = noisyCurCr;
				fOcc.cleanCb = noisyCurCb;
				fOcc.cleanCr = noisyCurCr;
				fOcc.enableAnchorCanvasRef = true;
				fOcc.enableCarriedCfl = true;
				fOcc.hasAnchorReadMotion = true;
				fOcc.anchorReadMotion = occMf;
				const auto resOcc = eng.ProcessFrame(fOcc, &reg);
				AssertPlaneAllFinite(resOcc.outputCb, L"H3 occluded outputCb must be finite");
				AssertPlaneAllFinite(resOcc.outputCr, L"H3 occluded outputCr must be finite");
				Assert::IsTrue(resOcc.anchorCanvasUsed && resOcc.carriedCflUsed);

				// 1. Occluded valid=0 pixels stay exact passthrough!
				for (int cy = 16; cy < 32; ++cy)
				{
					for (int cx = 16; cx < 32; ++cx)
					{
						Assert::AreEqual(noisyCurCb.at(cx, cy), resOcc.outputCb.at(cx, cy), 0.0,
							L"H3: occluded valid=0 Cb pixels must remain untouched");
						Assert::AreEqual(noisyCurCr.at(cx, cy), resOcc.outputCr.at(cx, cy), 0.0,
							L"H3: occluded valid=0 Cr pixels must remain untouched");
					}
				}
				// 2. Valid 1..6 px neighbor ring around occluded region has ZERO 0.0-hole dark pull!
				double sumSignedBiasCb = 0.0, sumSignedBiasCr = 0.0;
				size_t ringCnt = 0;
				for (int cy = 10; cy < 38; ++cy)
				{
					for (int cx = 10; cx < 38; ++cx)
					{
						if (ringMask[static_cast<size_t>(cy) * cW + cx] == 0u)
							continue;
						sumSignedBiasCb += resOcc.outputCb.at(cx, cy) - cleanCb.at(cx, cy);
						sumSignedBiasCr += resOcc.outputCr.at(cx, cy) - cleanCr.at(cx, cy);
						++ringCnt;
					}
				}
				const double ringBiasCb = sumSignedBiasCb / static_cast<double>(ringCnt);
				const double ringBiasCr = sumSignedBiasCr / static_cast<double>(ringCnt);
				const double ringRmseCb = MaskedRmse(cleanCb, resOcc.outputCb, cActive, &ringMask);
				const double ringRmseCr = MaskedRmse(cleanCr, resOcc.outputCr, cActive, &ringMask);
				Assert::IsTrue(std::fabs(ringBiasCb) < 2.0 && std::fabs(ringBiasCr) < 2.0,
					L"H3: valid neighbor ring around valid=0 occlusion MUST have zero 0.0-hole dark bias");
				Assert::IsTrue(ringRmseCb < MaskedRmse(cleanCb, noisyCurCb, cActive, &ringMask),
					L"H3: valid neighbor ring around valid=0 occlusion must genuinely improve vs noisy received");
				Assert::IsTrue(ringRmseCr < MaskedRmse(cleanCr, noisyCurCr, cActive, &ringMask));

				// ---- Sub-case B: Transform-identified 8x8 DCT POCS skips invalid refs[0] prediction pixels! ----
				// Place occMf (valid = 0 in coded [16, 32)^2 = four 8x8 blocks) on prevPeak so refs[0] IS the occluded
				// reference whose corrupted pixels (-30 codes in [16, 32)^2) would wreck STRICT/CELL POCS if not skipped,
				// while nextPeak carries stillMf (valid = 1 everywhere -> refs[1]) to genuinely repair [16, 32)^2!
				const ActiveRect holeRect{ 16, 16, 32, 32 };
				TdV2Plane corruptPrevCb = cleanCb;
				TdV2Plane corruptPrevCr = cleanCr;
				for (int cy = 16; cy < 32; ++cy)
				{
					for (int cx = 16; cx < 32; ++cx)
					{
						corruptPrevCb.at(cx, cy) -= 30.0;
						corruptPrevCr.at(cx, cy) += 30.0;
					}
				}
				const double baseHoleRmseCb = MaskedRmse(cleanCb, noisyCurCb, holeRect);
				const double baseHoleRmseCr = MaskedRmse(cleanCr, noisyCurCr, holeRect);

				for (ResidualMode rm : { ResidualMode::Strict, ResidualMode::Cell })
				{
					Config pCfg = cfg;
					pCfg.residualMode = rm;

					ChromaProcessInput fPocsOcc = fOcc;
					fPocsOcc.seq = 3;
					fPocsOcc.enableAnchorCanvasRef = false;
					fPocsOcc.enableCarriedCfl = false;
					fPocsOcc.hasAnchorReadMotion = false;
					fPocsOcc.transformIdentified8x8 = true;
					fPocsOcc.estimatedStepDelta = 12.0;
					fPocsOcc.prevPeak = { &corruptPrevCb, &corruptPrevCr, &occMf, false };   // refs[0]: valid=0 on [16,32)^2!
					fPocsOcc.nextPeak = { &cleanCb, &cleanCr, &stillMf, false };             // refs[1]: valid=1 everywhere!

					TdV2ChromaEngine pEngOcc(pCfg);
					StandInRegistry pRegOcc;
					const auto resPocsOcc = pEngOcc.ProcessFrame(fPocsOcc, &pRegOcc);
					AssertPlaneAllFinite(resPocsOcc.outputCb, L"H3 POCS outputCb must be finite");
					AssertPlaneAllFinite(resPocsOcc.outputCr, L"H3 POCS outputCr must be finite");

					// Same-input valid-refs[0] control (stillMf with valid = 1 everywhere on prevPeak):
					// Proves BOTH that the 4 occluded 8x8 blocks (x2 planes = 8) were skipped in resPocsOcc AND that
					// failing to skip corrupted refs[0] degrades [16, 32)^2 vs resPocsOcc!
					ChromaProcessInput fPocsValid = fPocsOcc;
					fPocsValid.prevPeak.lumaMotion = &stillMf;
					TdV2ChromaEngine pEngValid(pCfg);
					StandInRegistry pRegValid;
					const auto resPocsValid = pEngValid.ProcessFrame(fPocsValid, &pRegValid);

					Assert::IsTrue(resPocsOcc.codecModelPbStandInCells > 0,
						L"H3: valid blocks outside [16,32)^2 must still run 8x8 DCT POCS cleanly");
					Assert::IsTrue(resPocsOcc.codecModelPbStandInCells < resPocsValid.codecModelPbStandInCells,
						L"H3: POCS must skip occluded [16,32)^2 8x8 blocks where refs[0].validPx == 0");
					Assert::AreEqual(resPocsOcc.codecModelPbStandInCells + 8u, resPocsValid.codecModelPbStandInCells,
						L"H3: exact 4 occluded 8x8 blocks x 2 chroma planes (8) must be skipped by POCS vs valid control");

					const double occHoleRmseCb = MaskedRmse(cleanCb, resPocsOcc.outputCb, holeRect);
					const double occHoleRmseCr = MaskedRmse(cleanCr, resPocsOcc.outputCr, holeRect);
					const double validCorruptHoleRmseCb = MaskedRmse(cleanCb, resPocsValid.outputCb, holeRect);
					const double pocsRingRmseCb = MaskedRmse(cleanCb, resPocsOcc.outputCb, cActive, &ringMask);

					Assert::IsTrue(occHoleRmseCb < baseHoleRmseCb * 0.80 && occHoleRmseCr < baseHoleRmseCr * 0.80,
						L"H3: skipping invalid refs[0] POCS must preserve valid refs[1] (nextPeak) repair inside [16,32)^2");
					Assert::IsTrue(validCorruptHoleRmseCb > occHoleRmseCb,
						L"H3 Negative control: running POCS/fusion against corrupted valid=1 refs[0] must worsen [16,32)^2 vs skipped occlusion");
					Assert::IsTrue(pocsRingRmseCb < MaskedRmse(cleanCb, noisyCurCb, cActive, &ringMask),
						L"H3: valid neighbor ring around occluded POCS blocks must cleanly improve");
				}
			}

			// =========================================================================
			// 12. H4 — Anchor Canvas Wide Gap (> Subsampling Factor) Marked Invalid & Visible
			// =========================================================================
			TEST_METHOD(ChromaAnchorCanvasWideGapMarkedInvalidAndVisibleStandIn)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16; // 4x4 cells
				const int cW = w / 2, cH = h / 2;   // 32x32 coded 4:2:0
				const ActiveRect active = ActiveRect::Full(w, h);
				const SitingSpec siting = SitingSpec::TopLeftType2();
				const CellMotionField allValidMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Create contrasting left/right chroma (650 left of gap, 370 right of gap, true scene = 512 inside gap!)
				// and leave cell (1, 1) [16..32) x [16..32) (16 luma px wide >> subsampling factor 2!) unpopulated via splatMotion.valid=0!
				TdV2Plane anchorCb(cW, cH, 512.0), anchorCr(cW, cH, 512.0);
				for (int cy = 0; cy < cH; ++cy)
				{
					for (int cx = 0; cx < cW; ++cx)
					{
						if (cx < 8)      { anchorCb.at(cx, cy) = 650.0; anchorCr.at(cx, cy) = 380.0; }
						else if (cx >= 16) { anchorCb.at(cx, cy) = 370.0; anchorCr.at(cx, cy) = 640.0; }
						else              { anchorCb.at(cx, cy) = 512.0; anchorCr.at(cx, cy) = 512.0; }
					}
				}
				CellMotionField sparseSplatMf = allValidMf;
				// Unpopulated 16x16 luma-px wide gap across cells (1, 1) and (1, 2): x in [16, 32), y in [16, 48).
				sparseSplatMf.valid[static_cast<size_t>(1) * cw + 1] = 0u;
				sparseSplatMf.valid[static_cast<size_t>(2) * cw + 1] = 0u;

				TdV2ChromaAnchorCanvas canvas;
				StandInRegistry reg;
				Assert::IsTrue(canvas.AccumulateAnchorFrame(
					anchorCb, anchorCr, nullptr, w, h, ChromaSubsampling::Yuv420,
					siting, active, &sparseSplatMf, 16, 10, &reg));

				// 1. Direct MaterializeCanvas check: outGapFilled > 0 and StandInKind::ChromaCanvasGapFilled engaged!
				TdV2Plane matCb, matCr;
				uint64_t gapFilled = 0;
				StandInRegistry matReg;
				Assert::IsTrue(canvas.MaterializeCanvas(matCb, matCr, &gapFilled, &matReg));
				AssertPlaneAllFinite(matCb, L"Materialized canvas Cb must be finite");
				AssertPlaneAllFinite(matCr, L"Materialized canvas Cr must be finite");
				Assert::IsTrue(gapFilled > 0,
					L"H4: MaterializeCanvas on sparse samples MUST report gapFilled > 0");
				Assert::IsTrue(matReg.IsActive(StandInKind::ChromaCanvasGapFilled),
					L"H4: MaterializeCanvas gap-fill MUST visibly engage StandInKind::ChromaCanvasGapFilled");

				// 2. SampleAtCodedGrid with allValidMf: pixels deep inside the 16-luma-px wide gap (> subsampling factor 2)
				//    MUST have outValid == 0 (NEVER silently marked valid == 1)!
				TdV2Plane qCb, qCr;
				std::vector<uint8_t> qValid;
				StandInRegistry sampleReg;
				Assert::IsTrue(canvas.SampleAtCodedGrid(
					ChromaSubsampling::Yuv420, siting, active, &allValidMf, 16,
					qCb, qCr, qValid, &sampleReg));
				Assert::IsTrue(sampleReg.IsActive(StandInKind::ChromaCanvasGapFilled),
					L"H4: SampleAtCodedGrid on sparse canvas MUST propagate ChromaCanvasGapFilled visibility");

				// Deep inside the wide gap: coded cx in [10, 14), cy in [10, 22) -> luma x in [20, 28), y in [20, 44)
				// (>= 4 luma px away from any populated sample, strictly beyond 4:2:0 subsampling factor 2!).
				for (int cy = 10; cy < 22; ++cy)
				{
					for (int cx = 10; cx < 14; ++cx)
					{
						Assert::AreEqual(static_cast<uint8_t>(0u), qValid[static_cast<size_t>(cy) * cW + cx],
							L"H4: pixels beyond footprint/subsampling floor inside wide canvas gap MUST be marked outValid == 0");
					}
				}
				// Meanwhile populated region far from gap MUST remain valid == 1!
				Assert::AreEqual(static_cast<uint8_t>(1u), qValid[static_cast<size_t>(12) * cW + 4],
					L"Populated left region must remain valid == 1");
				Assert::AreEqual(static_cast<uint8_t>(1u), qValid[static_cast<size_t>(12) * cW + 24],
					L"Populated right region must remain valid == 1");

				// 3. Mandatory In-Test Negative Control + End-to-End TdV2ChromaEngine check (Rule 3):
				//    On contrasting left (650) vs right (370) across the wide gap where true scene is 512,
				//    a naive unconditional gap-fill that marks the gap valid=1 blends linear ramp 650->370 into the gap!
				const ActiveRect gapCodedRect{ 10, 10, 14, 22 };
				TdV2Plane curTrueCb = anchorCb, curTrueCr = anchorCr;
				for (int cy = 8; cy < 24; ++cy)
					for (int cx = 8; cx < 16; ++cx)
					{
						curTrueCb.at(cx, cy) = 512.0; // flat 512 true chroma inside the gap, contrasting with 650..370 linear ramp!
						curTrueCr.at(cx, cy) = 512.0;
					}
				// Shift the gap contrast asymmetrically so linear interpolation inside [10..14) is far from 512:
				// Left edge of gap (cx=7) = 650, right edge (cx=16) = 610 -> linear interp across gap is ~630 vs true 512!
				TdV2Plane asymAncCb = anchorCb;
				for (int cy = 0; cy < cH; ++cy)
					for (int cx = 16; cx < cW; ++cx)
						asymAncCb.at(cx, cy) = 610.0;

				TdV2ChromaAnchorCanvas asymCanvas;
				Assert::IsTrue(asymCanvas.AccumulateAnchorFrame(
					asymAncCb, anchorCr, nullptr, w, h, ChromaSubsampling::Yuv420,
					siting, active, &sparseSplatMf, 16, 10));
				TdV2Plane asymMatCb, asymMatCr;
				uint64_t asymGap = 0;
				Assert::IsTrue(asymCanvas.MaterializeCanvas(asymMatCb, asymMatCr, &asymGap));
				// Negative control: naive fusion treating materialized gap pixels as valid=1 introduces > 50 code error!
				double naiveGapSq = 0.0;
				size_t naiveGapN = 0;
				for (int cy = gapCodedRect.y0; cy < gapCodedRect.y1; ++cy)
				{
					for (int cx = gapCodedRect.x0; cx < gapCodedRect.x1; ++cx)
					{
						const double naivePred = asymMatCb.at(2 * cx, 2 * cy);
						const double naiveBlended = 0.5 * (512.0 + naivePred);
						const double err = naiveBlended - 512.0;
						naiveGapSq += err * err;
						++naiveGapN;
					}
				}
				const double naiveGapRmse = std::sqrt(naiveGapSq / static_cast<double>(naiveGapN));
				Assert::IsTrue(naiveGapRmse > 40.0,
					L"H4 Negative control: treating wide gap-filled canvas pixels as valid=1 MUST inject massive error (>40 codes)");

				// End-to-end engine check: ingest sparse anchor, fuse valley reading from anchor canvas!
				Config eCfg = MakePreset(Preset::Reference, Mode::On);
				eCfg.estimator = VarianceEstimator::A2_RoundingCells;
				TdV2ChromaEngine eng(eCfg);
				StandInRegistry eReg;

				ChromaProcessInput f1;
				f1.seq = 1;
				f1.deliveredFormat = DeliveredChromaFormat::Yuv420;
				f1.codedSubsampling = ChromaSubsampling::Yuv420;
				f1.gridStatus = ChromaGridStatus::Identified;
				f1.siting = siting;
				f1.lumaW = w;
				f1.lumaH = h;
				f1.activeRect = active;
				f1.receivedCb = asymAncCb;
				f1.receivedCr = anchorCr;
				f1.hasCleanSplit = true;
				f1.cleanCb = asymAncCb;
				f1.cleanCr = anchorCr;
				f1.isAnchorFrame = true;
				f1.hasAnchorSplatMotion = true;
				f1.anchorSplatMotion = sparseSplatMf;
				f1.hasStepPrior = true;
				f1.estimatedStepDelta = 24.0;
				eng.ProcessFrame(f1, &eReg);

				ChromaProcessInput f2 = f1;
				f2.seq = 2;
				f2.isAnchorFrame = false;
				f2.receivedCb = curTrueCb;
				f2.receivedCr = curTrueCr;
				f2.cleanCb = curTrueCb;
				f2.cleanCr = curTrueCr;
				f2.enableAnchorCanvasRef = true;
				f2.enableCarriedCfl = false;
				f2.hasAnchorReadMotion = true;
				f2.anchorReadMotion = allValidMf;
				StandInRegistry eReg2;
				const auto eRes2 = eng.ProcessFrame(f2, &eReg2);

				Assert::IsTrue(eRes2.canvasGapFilledPixels > 0,
					L"H4: TdV2ChromaEngine must report res.canvasGapFilledPixels > 0 when sampling sparse canvas");
				Assert::IsTrue(eReg2.IsActive(StandInKind::ChromaCanvasGapFilled),
					L"H4: TdV2ChromaEngine must visibly engage StandInKind::ChromaCanvasGapFilled");
				const double tdGapRmse = MaskedRmse(curTrueCb, eRes2.outputCb, gapCodedRect);
				Assert::AreEqual(0.0, tdGapRmse, 1e-9,
					L"H4: wide canvas gap marked outValid==0 MUST NOT corrupt true chroma inside the gap");
			}

			// =========================================================================
			// 13. H14 — Chroma Identified-Transform Explicit codecModelPbCb/Cr vs Stand-In
			// =========================================================================
			TEST_METHOD(ChromaTransformIdentifiedCodecModelPbExplicitVsStandIn)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const double stepDelta = 12.0;
				const double phi = kDeadZonePhiInter;

				// Under explicitPb: cur - explicitPb has SENT DC = +24.0 (> theta = 10.0, k = 2) -> STRICT restores exact 24.0!
				// Under prevPeak stand-in (+2.5 spatial => +20.0 DCT DC offset): cur - prevPeak has ZEROED DC = +4.0 (< theta, k = 0)
				// -> STRICT moves DC toward blend, proving explicit codecModelPbCb/Cr is genuinely used vs refs[0] stand-in!
				TdV2Plane explicitPbCb(w, h, 500.0), explicitPbCr(w, h, 520.0);
				TdV2Plane prevPeakDiffCb(w, h, 502.5), prevPeakDiffCr(w, h, 522.5);
				TdV2Plane curCb(w, h, 500.0), curCr(w, h, 520.0);
				TdV2Plane nextPeakCb(w, h, 500.0), nextPeakCr(w, h, 520.0);

				double sentDct[64] = {}, targetDct[64] = {}, sentSpat[64] = {}, targetSpat[64] = {};
				sentDct[0] = 24.0;
				sentDct[1] = 0.0;
				targetDct[0] = 32.0;
				targetDct[1] = 26.0;
				InverseDct8x8(sentDct, sentSpat);
				InverseDct8x8(targetDct, targetSpat);

				for (int by = 0; by < h; by += 8)
				{
					for (int bx = 0; bx < w; bx += 8)
					{
						for (int j = 0; j < 8; ++j)
						{
							for (int i = 0; i < 8; ++i)
							{
								curCb.at(bx + i, by + j) = explicitPbCb.at(bx + i, by + j) + sentSpat[j * 8 + i];
								curCr.at(bx + i, by + j) = explicitPbCr.at(bx + i, by + j) + sentSpat[j * 8 + i];
								nextPeakCb.at(bx + i, by + j) = explicitPbCb.at(bx + i, by + j) + targetSpat[j * 8 + i];
								nextPeakCr.at(bx + i, by + j) = explicitPbCr.at(bx + i, by + j) + targetSpat[j * 8 + i];
							}
						}
					}
				}

				for (ResidualMode rm : { ResidualMode::Strict, ResidualMode::Cell })
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.estimator = VarianceEstimator::A2_RoundingCells;
					cfg.residualMode = rm;
					cfg.cellProjIterations = 1;

					ChromaProcessInput baseIn;
					baseIn.seq = 140;
					baseIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
					baseIn.codedSubsampling = ChromaSubsampling::Yuv444;
					baseIn.gridStatus = ChromaGridStatus::Identified;
					baseIn.lumaW = w;
					baseIn.lumaH = h;
					baseIn.receivedCb = curCb;
					baseIn.receivedCr = curCr;
					baseIn.hasCleanSplit = true;
					baseIn.cleanCb = curCb;
					baseIn.cleanCr = curCr;
					baseIn.prevPeak = { &prevPeakDiffCb, &prevPeakDiffCr, &stillMf, true };
					baseIn.nextPeak = { &nextPeakCb, &nextPeakCr, &stillMf, false };
					baseIn.hasStepPrior = true;
					baseIn.estimatedStepDelta = stepDelta;
					baseIn.deadZonePhi = phi;
					baseIn.zeroCoeffFraction = 0.0;
					baseIn.transformIdentified8x8 = true;

					// Case A: Without explicit codecModelPbCb/Cr -> engages CodecModelPbStandIn visibly!
					TdV2ChromaEngine engStandIn(cfg);
					StandInRegistry regStandIn;
					const auto resStandIn = engStandIn.ProcessFrame(baseIn, &regStandIn);
					Assert::IsTrue(regStandIn.IsActive(StandInKind::CodecModelPbStandIn),
						L"H14 chroma: identified 8x8 DCT without codecModelPbCb/Cr MUST visibly engage CodecModelPbStandIn");
					Assert::IsTrue(resStandIn.codecModelPbStandInCells > 0,
						L"H14 chroma: identified 8x8 DCT without codecModelPbCb/Cr MUST increment codecModelPbStandInCells");

					// Case B: With explicit codecModelPbCb/Cr -> uses them WITHOUT engaging CodecModelPbStandIn!
					ChromaProcessInput expIn = baseIn;
					expIn.codecModelPbCb = &explicitPbCb;
					expIn.codecModelPbCr = &explicitPbCr;
					TdV2ChromaEngine engExplicit(cfg);
					StandInRegistry regExplicit;
					const auto resExplicit = engExplicit.ProcessFrame(expIn, &regExplicit);
					Assert::IsFalse(regExplicit.IsActive(StandInKind::CodecModelPbStandIn),
						L"H14 chroma: supplying explicit codecModelPbCb/Cr MUST NOT engage CodecModelPbStandIn");
					Assert::AreEqual(static_cast<uint64_t>(0), resExplicit.codecModelPbStandInCells,
						L"H14 chroma: supplying explicit codecModelPbCb/Cr MUST keep codecModelPbStandInCells == 0");

					if (rm == ResidualMode::Strict)
					{
						double spCb[64] = {}, dctCb[64] = {};
						for (int j = 0; j < 8; ++j)
							for (int i = 0; i < 8; ++i)
								spCb[j * 8 + i] = resExplicit.outputCb.at(16 + i, 16 + j) - explicitPbCb.at(16 + i, 16 + j);
						ForwardDct8x8(spCb, dctCb);
						Assert::AreEqual(24.0, dctCb[0], 1e-6,
							L"Chroma explicit codecModelPbCb in STRICT mode must preserve sent DC=24 exact");
						Assert::IsTrue(MaskedRmse(resExplicit.outputCb, resStandIn.outputCb, ActiveRect::Full(w, h)) > 1e-3,
							L"Explicit codecModelPbCb must genuinely alter projection vs distinct refs[0] stand-in");
					}
				}
			}

			// =========================================================================
			// 14. LOW/MEDIUM — Out-of-Range retainedGrainVar Engages GrainPriorOutOfRange Visibly
			// =========================================================================
			TEST_METHOD(ChromaGrainPriorOutOfRangeEngagesVisibleStandIn)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16; // 4x4 = 16 cells
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const TdV2Plane cur444(w, h, 512.0), ref444(w, h, 520.0);

				for (ChromaGridStatus status : { ChromaGridStatus::Identified, ChromaGridStatus::Unidentified })
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);

					ChromaProcessInput baseIn;
					baseIn.seq = 150;
					baseIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
					baseIn.codedSubsampling = ChromaSubsampling::Yuv444;
					baseIn.gridStatus = status;
					baseIn.lumaW = w;
					baseIn.lumaH = h;
					baseIn.receivedCb = cur444;
					baseIn.receivedCr = cur444;
					baseIn.hasCleanSplit = true;
					baseIn.cleanCb = cur444;
					baseIn.cleanCr = cur444;
					baseIn.prevPeak = { &ref444, &ref444, &stillMf, false };
					baseIn.nextPeak = { &ref444, &ref444, &stillMf, false };
					baseIn.hasStepPrior = true;
					baseIn.estimatedStepDelta = 12.0;

					// Properly sized retainedGrainVar (16 cells) -> GrainPriorOutOfRange NOT active, oobCells == 0.
					ChromaProcessInput okIn = baseIn;
					okIn.retainedGrainVarCb.assign(static_cast<size_t>(cw) * ch, 4.0);
					okIn.retainedGrainVarCr.assign(static_cast<size_t>(cw) * ch, 4.0);
					TdV2ChromaEngine engOk(cfg);
					StandInRegistry regOk;
					const auto resOk = engOk.ProcessFrame(okIn, &regOk);
					Assert::IsFalse(regOk.IsActive(StandInKind::GrainPriorOutOfRange),
						L"Properly sized retainedGrainVar must NOT engage GrainPriorOutOfRange");
					Assert::AreEqual(static_cast<uint64_t>(0), resOk.grainPriorOobCells);

					// Truncated / out-of-range retainedGrainVar (size 1 for 16 cells!) -> visibly flagged!
					ChromaProcessInput oobIn = baseIn;
					oobIn.retainedGrainVarCb = { 4.0 }; // 15 cells out of range!
					oobIn.retainedGrainVarCr = { 4.0 }; // 15 cells out of range!
					TdV2ChromaEngine engOob(cfg);
					StandInRegistry regOob;
					const auto resOob = engOob.ProcessFrame(oobIn, &regOob);
					AssertPlaneAllFinite(resOob.outputCb, L"Out-of-range retainedGrainVar Cb must remain finite");
					AssertPlaneAllFinite(resOob.outputCr, L"Out-of-range retainedGrainVar Cr must remain finite");
					Assert::IsTrue(regOob.IsActive(StandInKind::GrainPriorOutOfRange),
						L"Out-of-range retainedGrainVar MUST visibly engage StandInKind::GrainPriorOutOfRange");
					Assert::IsTrue(resOob.grainPriorOobCells > 0,
						L"Out-of-range retainedGrainVar MUST increment res.grainPriorOobCells");
				}
			}

			// =========================================================================
			// 15. Full-Diff MEDIUM-1 (H9 Chroma): Missing Motion NEVER Whole-Blends Moving Content
			// =========================================================================
			TEST_METHOD(ChromaMissingMotionFieldNeverWholeBlendsMovingContent)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const int cW = w / 2, cH = h / 2;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect cActive = ActiveRect::Full(cW, cH);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const SitingSpec siting = SitingSpec::Left();
				const Config cfg = MakePreset(Preset::Reference, Mode::On);

				// Build a moving chroma vertical bar (+18 codes Cb, -18 codes Cr inside stepDelta=24 Huber knee)
				// shifted by -6 luma px (-3 coded 4:2:0 px) in prevPeak/anchor and +6 luma px (+3 coded px) in nextPeak.
				auto makeBar420 = [&](int cX0, int cX1, double barVal) -> TdV2Plane
				{
					TdV2Plane p(cW, cH, 512.0);
					for (int y = 0; y < cH; ++y)
						for (int x = cX0; x < cX1; ++x)
							if (x >= 0 && x < cW)
								p.at(x, y) = barVal;
					return p;
				};
				const TdV2Plane curCb = makeBar420(12, 20, 530.0);
				const TdV2Plane curCr = makeBar420(12, 20, 494.0);
				const TdV2Plane prevCb = makeBar420(9, 17, 530.0);
				const TdV2Plane prevCr = makeBar420(9, 17, 494.0);
				const TdV2Plane nextCb = makeBar420(15, 23, 530.0);
				const TdV2Plane nextCr = makeBar420(15, 23, 494.0);

				// ---- (A) Identified coded 4:2:0 peak refs with lumaMotion == nullptr ----
				{
					ChromaProcessInput in;
					in.seq = 160;
					in.deliveredFormat = DeliveredChromaFormat::Yuv420;
					in.codedSubsampling = ChromaSubsampling::Yuv420;
					in.gridStatus = ChromaGridStatus::Identified;
					in.siting = siting;
					in.lumaW = w;
					in.lumaH = h;
					in.activeRect = active;
					in.receivedCb = curCb;
					in.receivedCr = curCr;
					in.hasCleanSplit = true;
					in.cleanCb = curCb;
					in.cleanCr = curCr;
					in.prevPeak = { &prevCb, &prevCr, nullptr, false };
					in.nextPeak = { &nextCb, &nextCr, nullptr, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 24.0;

					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;
					const auto res = eng.ProcessFrame(in, &reg);
					AssertPlanesBitExact(curCb, res.outputCb, L"Identified chroma null motion MUST yield exact zero ghosting (Cb)");
					AssertPlanesBitExact(curCr, res.outputCr, L"Identified chroma null motion MUST yield exact zero ghosting (Cr)");
					Assert::IsTrue(reg.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"Identified chroma null motion MUST engage StandInKind::MissingMotionNoWholeBlend");
					Assert::IsTrue(res.motionStandInRefs >= 2,
						L"Identified chroma null motion MUST count both skipped refs in motionStandInRefs");

					// Mandatory In-Test Negative Control (Rule 3): assuming Still motion on EXACT SAME moving bar ghosts severely!
					ChromaProcessInput stillIn = in;
					stillIn.prevPeak.lumaMotion = &stillMf;
					stillIn.nextPeak.lumaMotion = &stillMf;
					TdV2ChromaEngine engStill(cfg);
					StandInRegistry regStill;
					const auto resStill = engStill.ProcessFrame(stillIn, &regStill);
					const double ghostRmseCb = MaskedRmse(curCb, resStill.outputCb, cActive);
					const double ghostRmseCr = MaskedRmse(curCr, resStill.outputCr, cActive);
					Assert::IsTrue(ghostRmseCb > 1.5 && ghostRmseCr > 1.5,
						L"Negative control: assuming Still motion on moving chroma bar MUST cause severe ghosting");
				}

				// ---- (B) Unidentified coarse-band peak refs with lumaMotion == nullptr ----
				{
					// Use 128x128 with a 40-px moving chroma bar shifted -40 px in prev and +40 px in next so BOTH
					// refs pull the coarse G(32) band downward at the center bar (zero anti-symmetric cancellation!).
					const int uw = 128, uh = 128;
					const int ucw = uw / 16, uch = uh / 16;
					const ActiveRect uActive = ActiveRect::Full(uw, uh);
					const CellMotionField uStillMf = CellMotionField::Uniform(ucw, uch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					auto makeBar444 = [&](int x0, int x1, double barVal) -> TdV2Plane
					{
						TdV2Plane p(uw, uh, 512.0);
						for (int y = 0; y < uh; ++y)
							for (int x = x0; x < x1 && x < uw; ++x)
								if (x >= 0)
									p.at(x, y) = barVal;
						return p;
					};
					const TdV2Plane cur444Cb = makeBar444(44, 84, 536.0);
					const TdV2Plane cur444Cr = makeBar444(44, 84, 488.0);
					const TdV2Plane prev444Cb = makeBar444(4, 44, 536.0);
					const TdV2Plane prev444Cr = makeBar444(4, 44, 488.0);
					const TdV2Plane next444Cb = makeBar444(84, 124, 536.0);
					const TdV2Plane next444Cr = makeBar444(84, 124, 488.0);

					ChromaProcessInput in;
					in.seq = 161;
					in.deliveredFormat = DeliveredChromaFormat::Yuv444;
					in.codedSubsampling = ChromaSubsampling::Yuv444;
					in.gridStatus = ChromaGridStatus::Unidentified;
					in.lumaW = uw;
					in.lumaH = uh;
					in.activeRect = uActive;
					in.receivedCb = cur444Cb;
					in.receivedCr = cur444Cr;
					in.hasCleanSplit = true;
					in.cleanCb = cur444Cb;
					in.cleanCr = cur444Cr;
					in.prevPeak = { &prev444Cb, &prev444Cr, nullptr, false };
					in.nextPeak = { &next444Cb, &next444Cr, nullptr, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 24.0;

					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;
					const auto res = eng.ProcessFrame(in, &reg);
					AssertPlanesBitExact(cur444Cb, res.outputCb, L"Unidentified coarse chroma null motion MUST yield zero ghosting (Cb)");
					AssertPlanesBitExact(cur444Cr, res.outputCr, L"Unidentified coarse chroma null motion MUST yield zero ghosting (Cr)");
					Assert::IsTrue(reg.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"Unidentified coarse chroma null motion MUST engage StandInKind::MissingMotionNoWholeBlend");
					Assert::IsTrue(res.motionStandInRefs >= 2);

					// Mandatory In-Test Negative Control (Rule 3): Still motion on exact same unidentified moving bar ghosts!
					ChromaProcessInput stillIn = in;
					stillIn.prevPeak.lumaMotion = &uStillMf;
					stillIn.nextPeak.lumaMotion = &uStillMf;
					TdV2ChromaEngine engStill(cfg);
					StandInRegistry regStill;
					const auto resStill = engStill.ProcessFrame(stillIn, &regStill);
					Assert::IsTrue(MaskedRmse(cur444Cb, resStill.outputCb, uActive) > 0.5 &&
					               MaskedRmse(cur444Cr, resStill.outputCr, uActive) > 0.5,
						L"Negative control: Still motion on unidentified moving chroma bar MUST ghost visibly");
				}

				// ---- (C) Anchor canvas read (hasAnchorReadMotion == false) AND 2nd anchor splat (hasAnchorSplatMotion == false) ----
				{
					TdV2ChromaEngine eng(cfg);
					StandInRegistry regSeed;

					// Frame 0: seed empty anchor canvas at origin (valid initial anchor).
					ChromaProcessInput f0;
					f0.seq = 162;
					f0.deliveredFormat = DeliveredChromaFormat::Yuv420;
					f0.codedSubsampling = ChromaSubsampling::Yuv420;
					f0.gridStatus = ChromaGridStatus::Identified;
					f0.siting = siting;
					f0.lumaW = w;
					f0.lumaH = h;
					f0.activeRect = active;
					f0.receivedCb = prevCb;
					f0.receivedCr = prevCr;
					f0.hasCleanSplit = true;
					f0.cleanCb = prevCb;
					f0.cleanCr = prevCr;
					f0.isAnchorFrame = true;
					f0.hasAnchorSplatMotion = false; // empty canvas -> seeds origin cleanly
					f0.hasStepPrior = true;
					f0.estimatedStepDelta = 24.0;
					eng.ProcessFrame(f0, &regSeed);
					Assert::IsTrue(eng.AnchorCanvas().HasData());
					Assert::AreEqual(static_cast<size_t>(1), eng.AnchorCanvas().AccumulatedFrames());

					// Frame 1: moving bar with missing anchorReadMotion AND missing anchorSplatMotion on populated canvas!
					ChromaProcessInput f1 = f0;
					f1.seq = 163;
					f1.receivedCb = curCb;
					f1.receivedCr = curCr;
					f1.cleanCb = curCb;
					f1.cleanCr = curCr;
					f1.enableAnchorCanvasRef = true;
					f1.hasAnchorReadMotion = false;
					f1.isAnchorFrame = true;
					f1.hasAnchorSplatMotion = false;

					StandInRegistry regMiss;
					const auto resMiss = eng.ProcessFrame(f1, &regMiss);
					Assert::IsFalse(resMiss.anchorCanvasUsed,
						L"Missing anchorReadMotion MUST skip anchor canvas read (never assume Still!)");
					AssertPlanesBitExact(curCb, resMiss.outputCb, L"Missing anchorReadMotion MUST produce zero ghosting (Cb)");
					AssertPlanesBitExact(curCr, resMiss.outputCr, L"Missing anchorReadMotion MUST produce zero ghosting (Cr)");
					Assert::IsTrue(regMiss.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"Missing anchor read/splat motion MUST engage StandInKind::MissingMotionNoWholeBlend");
					Assert::IsTrue(resMiss.motionStandInRefs > 0);
					Assert::AreEqual(static_cast<size_t>(1), eng.AnchorCanvas().AccumulatedFrames(),
						L"Missing anchorSplatMotion on populated canvas MUST skip splat accumulation (no Still ghosting into canvas!)");

					// Mandatory In-Test Negative Control (Rule 3): assuming Still anchorReadMotion on exact same moving bar ghosts!
					ChromaProcessInput f1Still = f1;
					f1Still.seq = 164;
					f1Still.isAnchorFrame = false;
					f1Still.hasAnchorReadMotion = true;
					f1Still.anchorReadMotion = stillMf;
					StandInRegistry regStill;
					const auto resStill = eng.ProcessFrame(f1Still, &regStill);
					Assert::IsTrue(resStill.anchorCanvasUsed, L"Negative control: Still anchorReadMotion reads anchor canvas");
					Assert::IsTrue(MaskedRmse(curCb, resStill.outputCb, cActive) > 1.0 &&
					               MaskedRmse(curCr, resStill.outputCr, cActive) > 1.0,
						L"Negative control: assuming Still anchorReadMotion on moving chroma bar MUST cause severe ghosting");
				}
			}

			// =========================================================================
			// 16. Full-Diff MEDIUM-3: Invalid deadZonePhi & Non-Finite StepDelta Fail Closed Visibly
			// =========================================================================
			TEST_METHOD(ChromaInvalidDeadZonePhiAndNonFiniteStepDeltaFailClosedVisibly)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const size_t numCells = static_cast<size_t>(cw) * ch;
				const ActiveRect active = ActiveRect::Full(w, h);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const Config cfg = MakePreset(Preset::Reference, Mode::On);

				const TdV2Plane curCb(w, h, 512.0), curCr(w, h, 512.0);
				const TdV2Plane refCb(w, h, 524.0), refCr(w, h, 500.0);

				const double kNaN = std::numeric_limits<double>::quiet_NaN();
				const double kInf = std::numeric_limits<double>::infinity();

				struct BadA2Case
				{
					bool hasStepPrior;
					double stepDelta;
					double phi;
					double zeroFrac;
					bool expectNonFinite;
				};
				const BadA2Case badCases[] = {
					{ true,  12.0,  0.0,               0.0,  false }, // phi == 0 out of (0,1)
					{ true,  12.0, -0.25,              0.0,  false }, // negative phi
					{ true,  12.0,  1.0,               0.0,  false }, // phi == 1 out of (0,1)
					{ true,  12.0,  1.5,               0.0,  false }, // phi > 1
					{ true,  12.0,  kNaN,              0.0,  true  }, // NaN phi
					{ false,  0.0,  kNaN,              0.0,  true  }, // NaN phi even when hasStepPrior == false
					{ true,   0.0,  kDeadZonePhiInter, 0.0,  false }, // stepDelta == 0
					{ true,  -4.0,  kDeadZonePhiInter, 0.0,  false }, // negative stepDelta
					{ true,  kNaN,  kDeadZonePhiInter, 0.0,  true  }, // NaN stepDelta
					{ true,  kInf,  kDeadZonePhiInter, 0.0,  true  }, // +Inf stepDelta
					{ true,  12.0,  kDeadZonePhiInter, -0.1, false }, // negative zeroCoeffFraction
					{ true,  12.0,  kDeadZonePhiInter,  1.2, false }, // zeroCoeffFraction > 1
					{ true,  12.0,  kDeadZonePhiInter, kNaN, true  }, // NaN zeroCoeffFraction
				};

				for (ChromaGridStatus status : { ChromaGridStatus::Identified, ChromaGridStatus::Unidentified })
				{
					ChromaProcessInput baseIn;
					baseIn.seq = 170;
					baseIn.deliveredFormat = DeliveredChromaFormat::Yuv444;
					baseIn.codedSubsampling = ChromaSubsampling::Yuv444;
					baseIn.gridStatus = status;
					baseIn.lumaW = w;
					baseIn.lumaH = h;
					baseIn.activeRect = active;
					baseIn.receivedCb = curCb;
					baseIn.receivedCr = curCr;
					baseIn.hasCleanSplit = true;
					baseIn.cleanCb = curCb;
					baseIn.cleanCr = curCr;
					baseIn.prevPeak = { &refCb, &refCr, &stillMf, false };
					baseIn.nextPeak = { &refCb, &refCr, &stillMf, false };

					// Positive control on the EXACT SAME planes with valid A2 stats -> genuine non-zero chroma repair!
					{
						ChromaProcessInput okIn = baseIn;
						okIn.hasStepPrior = true;
						okIn.estimatedStepDelta = 12.0;
						okIn.deadZonePhi = kDeadZonePhiInter;
						okIn.zeroCoeffFraction = 0.0;
						TdV2ChromaEngine engOk(cfg);
						StandInRegistry regOk;
						const auto resOk = engOk.ProcessFrame(okIn, &regOk);
						Assert::IsFalse(regOk.IsActive(StandInKind::A2StatsInvalid),
							L"Valid A2 stats must NOT engage StandInKind::A2StatsInvalid");
						Assert::IsFalse(regOk.IsActive(StandInKind::NonFiniteClosure),
							L"Valid A2 stats must NOT engage StandInKind::NonFiniteClosure");
						Assert::AreEqual(static_cast<size_t>(0), resOk.a2InvalidCells);
						Assert::IsTrue(MaskedRmse(curCb, resOk.outputCb, active) > 0.5 &&
						               MaskedRmse(curCr, resOk.outputCr, active) > 0.5,
							L"Positive control: valid A2 stats must produce non-zero chroma repair");
					}

					// Adversarial bad/non-finite A2 cases -> strict passthrough delta == 0 + visible stand-in & counters!
					for (size_t ci = 0; ci < sizeof(badCases) / sizeof(badCases[0]); ++ci)
					{
						const auto& bc = badCases[ci];
						ChromaProcessInput badIn = baseIn;
						badIn.seq = 171 + ci;
						badIn.hasStepPrior = bc.hasStepPrior;
						badIn.estimatedStepDelta = bc.stepDelta;
						badIn.deadZonePhi = bc.phi;
						badIn.zeroCoeffFraction = bc.zeroFrac;

						TdV2ChromaEngine engBad(cfg);
						StandInRegistry regBad;
						const auto resBad = engBad.ProcessFrame(badIn, &regBad);
						AssertPlaneAllFinite(resBad.outputCb, L"Invalid A2 chroma Cb output must remain finite");
						AssertPlaneAllFinite(resBad.outputCr, L"Invalid A2 chroma Cr output must remain finite");
						AssertPlanesBitExact(curCb, resBad.outputCb, L"Invalid A2 stats MUST close chroma delta to exact 0 (Cb)");
						AssertPlanesBitExact(curCr, resBad.outputCr, L"Invalid A2 stats MUST close chroma delta to exact 0 (Cr)");
						Assert::IsTrue(regBad.IsActive(StandInKind::A2StatsInvalid),
							L"Invalid deadZonePhi/stepDelta/zeroFrac MUST engage StandInKind::A2StatsInvalid visibly");
						Assert::AreEqual(numCells, resBad.a2InvalidCells,
							L"Invalid A2 stats MUST report all cells in res.a2InvalidCells");
						if (bc.expectNonFinite)
						{
							Assert::IsTrue(regBad.IsActive(StandInKind::NonFiniteClosure),
								L"Non-finite A2 step/phi/zeroFrac MUST ALSO engage StandInKind::NonFiniteClosure visibly");
							Assert::AreEqual(numCells, resBad.nonFiniteCells,
								L"Non-finite A2 stats MUST report all cells in res.nonFiniteCells");
						}
					}
				}
			}

			// =========================================================================
			// 17. Item #2 [MED Red-First]: Unsplit Blind Chroma Sub-Pixel Pan
			//     Preserves Grain Without Projection Collapse (Identified & Unidentified)
			// =========================================================================
			TEST_METHOD(UnsplitBlindChromaSubpixelPanPreservesGrainWithoutProjectionCollapse)
			{
				// Textured 4:2:0 chroma scene (128x128 luma -> 64x64 coded chroma) undergoing a 1.0-px horizontal
				// luma pan (= 0.5-px sub-pixel coded chroma displacement!) with strong fine chroma texture +
				// realistic film grain (grainSigma = 14.0) + coding wear, processed UNSPLIT (hasCleanSplit = false,
				// retainedGrainVar empty) across 2 consecutive frames so lag1Evaluated == true!
				// Pre-fix symmetric innovation clamps ir = E[r^2] - E[c*r] < 0 under 0.5-px bicubic attenuation on
				// strong fine texture -> collapses cellGrainVar & estimatedBlindNlfCb to 0.0 and breaks grain bar!
				const int w = 128, h = 128;
				const int cw = w / 16, ch = h / 16;
				const int cW = w / 2, cH = h / 2;
				const size_t cN = static_cast<size_t>(cW) * cH;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect cActive = ActiveRect::Full(cW, cH);
				const SitingSpec siting = SitingSpec::Left();

				// Strong fine chroma texture monochromatic in x (period = 5.2 luma px = 2.6 coded px -> strong 0.5-px
				// Catmull-Rom attenuation alpha ~= 0.51 while vertical vy == 0 keeps vertical attenuation exact 1.0).
				auto renderFineTextured420 = [&](double oxLuma, bool isCb) -> TdV2Plane
				{
					TdV2Plane p(cW, cH, 512.0);
					for (int cy = 0; cy < cH; ++cy)
					{
						const double ly = siting.CodedToLumaY(static_cast<double>(cy), 2);
						const double yEnv = isCb
							? (0.65 + 0.35 * std::cos(2.0 * kPi * ly / 32.0))
							: (0.65 + 0.35 * std::sin(2.0 * kPi * ly / 28.0));
						for (int cx = 0; cx < cW; ++cx)
						{
							const double lx = siting.CodedToLumaX(static_cast<double>(cx), 2) + oxLuma;
							const double wave = isCb
								? (70.0 * std::sin(2.0 * kPi * lx / 5.2 + 0.3))
								: (65.0 * std::cos(2.0 * kPi * lx / 5.2 + 0.9));
							p.at(cx, cy) = 512.0 + wave * yEnv;
						}
					}
					return p;
				};

				// Mild low-band coding wear (variance ~1.5 code^2 << grainSigma^2 = 196 so unsplit stdRatio stays well inside +/-2%).
				auto addLowBandWear = [&](const TdV2Plane& base, double rawSigma, uint32_t seed) -> TdV2Plane
				{
					const TdV2Plane zeroP(base.w, base.h, 0.0);
					const TdV2Plane rawNoise = AddGaussianNoise(zeroP, rawSigma, seed, cActive);
					const ThreeBandPlanes nb = DecomposeActiveBands(rawNoise, cActive);
					TdV2Plane out = base;
					for (size_t i = 0; i < out.v.size(); ++i)
						out.v[i] += nb.L.v[i] + 0.15 * nb.M.v[i];
					return out;
				};

				// Continuous 1.0-px luma pan (= 0.5-px coded 4:2:0 pan) across Frame 1 (ox=0) and Frame 2 (ox=+1).
				const TdV2Plane masterF1Cb = renderFineTextured420(0.0, true);
				const TdV2Plane masterF1Cr = renderFineTextured420(0.0, false);
				const TdV2Plane masterPrev1Cb = renderFineTextured420(-1.0, true);
				const TdV2Plane masterPrev1Cr = renderFineTextured420(-1.0, false);
				const TdV2Plane masterNext1Cb = renderFineTextured420(+1.0, true);
				const TdV2Plane masterNext1Cr = renderFineTextured420(+1.0, false);

				const TdV2Plane masterF2Cb = renderFineTextured420(+1.0, true);
				const TdV2Plane masterF2Cr = renderFineTextured420(+1.0, false);
				const TdV2Plane masterPrev2Cb = renderFineTextured420(0.0, true);
				const TdV2Plane masterPrev2Cr = renderFineTextured420(0.0, false);
				const TdV2Plane masterNext2Cb = renderFineTextured420(+2.0, true);
				const TdV2Plane masterNext2Cr = renderFineTextured420(+2.0, false);

				const TdV2Plane valley1CleanCb = addLowBandWear(masterF1Cb, 10.0, 1701u);
				const TdV2Plane valley1CleanCr = addLowBandWear(masterF1Cr, 10.0, 1702u);
				const TdV2Plane valley2CleanCb = addLowBandWear(masterF2Cb, 10.0, 1703u);
				const TdV2Plane valley2CleanCr = addLowBandWear(masterF2Cr, 10.0, 1704u);

				// Independent multi-seed film grain (grainSigma = 14.0) on all frames and peak references.
				const double grainSigma = 14.0;
				std::mt19937 rng(1710u);
				std::normal_distribution<double> gd(0.0, grainSigma);
				std::vector<double> prevOwnCb(cN), prevOwnCr(cN), ownCb(cN), ownCr(cN);
				std::vector<double> forPrev1Cb(cN), forPrev1Cr(cN), forNext1Cb(cN), forNext1Cr(cN);
				std::vector<double> forPrev2Cb(cN), forPrev2Cr(cN), forNext2Cb(cN), forNext2Cr(cN);

				TdV2Plane cur1RecCb(cW, cH), cur1RecCr(cW, cH), cur2RecCb(cW, cH), cur2RecCr(cW, cH);
				TdV2Plane prev1RecCb(cW, cH), prev1RecCr(cW, cH), next1RecCb(cW, cH), next1RecCr(cW, cH);
				TdV2Plane prev2RecCb(cW, cH), prev2RecCr(cW, cH), next2RecCb(cW, cH), next2RecCr(cW, cH);

				for (size_t i = 0; i < cN; ++i)
				{
					prevOwnCb[i] = gd(rng);  prevOwnCr[i] = gd(rng);
					ownCb[i] = gd(rng);      ownCr[i] = gd(rng);
					forPrev1Cb[i] = gd(rng); forPrev1Cr[i] = gd(rng);
					forNext1Cb[i] = gd(rng); forNext1Cr[i] = gd(rng);
					forPrev2Cb[i] = gd(rng); forPrev2Cr[i] = gd(rng);
					forNext2Cb[i] = gd(rng); forNext2Cr[i] = gd(rng);

					cur1RecCb.v[i] = valley1CleanCb.v[i] + prevOwnCb[i];
					cur1RecCr.v[i] = valley1CleanCr.v[i] + prevOwnCr[i];
					cur2RecCb.v[i] = valley2CleanCb.v[i] + ownCb[i];
					cur2RecCr.v[i] = valley2CleanCr.v[i] + ownCr[i];

					prev1RecCb.v[i] = masterPrev1Cb.v[i] + forPrev1Cb[i];
					prev1RecCr.v[i] = masterPrev1Cr.v[i] + forPrev1Cr[i];
					next1RecCb.v[i] = masterNext1Cb.v[i] + forNext1Cb[i];
					next1RecCr.v[i] = masterNext1Cr.v[i] + forNext1Cr[i];

					prev2RecCb.v[i] = masterPrev2Cb.v[i] + forPrev2Cb[i];
					prev2RecCr.v[i] = masterPrev2Cr.v[i] + forPrev2Cr[i];
					next2RecCb.v[i] = masterNext2Cb.v[i] + forNext2Cb[i];
					next2RecCr.v[i] = masterNext2Cr.v[i] + forNext2Cr[i];
				}

				const CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +1.0, 0.0, +1.0, 0.0, RegionMotionClass::Camera);
				const CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -1.0, 0.0, -1.0, 0.0, RegionMotionClass::Camera);

				// Warp Frame 2's foreign grain by +1.0 / -1.0 luma px (+0.5 / -0.5 coded px) so regression tests actual warped transfer!
				TdV2Plane zeroCoded(cW, cH, 0.0);
				TdV2Plane fPrev2CbPlane(cW, cH), fPrev2CrPlane(cW, cH), fNext2CbPlane(cW, cH), fNext2CrPlane(cW, cH);
				fPrev2CbPlane.v = forPrev2Cb; fPrev2CrPlane.v = forPrev2Cr;
				fNext2CbPlane.v = forNext2Cb; fNext2CrPlane.v = forNext2Cr;
				TdV2Plane warpedForPrevCb, warpedForPrevCr, warpedForNextCb, warpedForNextCr;
				std::vector<uint8_t> wv1, wv2, wv3, wv4;
				ChromaDetail::WarpChromaPlaneWithLumaMotion(fPrev2CbPlane, zeroCoded, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedForPrevCb, wv1);
				ChromaDetail::WarpChromaPlaneWithLumaMotion(fPrev2CrPlane, zeroCoded, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedForPrevCr, wv2);
				ChromaDetail::WarpChromaPlaneWithLumaMotion(fNext2CbPlane, zeroCoded, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, warpedForNextCb, wv3);
				ChromaDetail::WarpChromaPlaneWithLumaMotion(fNext2CrPlane, zeroCoded, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, warpedForNextCr, wv4);

				// ---- Mandatory In-Test Negative Control (Rule 3): Same-input unprotected 0.5-px pan direct blend ----
				{
					TdV2Plane wp1Cb, wn1Cb, wp2Cb, wn2Cb, wp1Cr, wn1Cr, wp2Cr, wn2Cr;
					std::vector<uint8_t> dv;
					ChromaDetail::WarpChromaPlaneWithLumaMotion(prev1RecCb, cur1RecCb, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, wp1Cb, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(next1RecCb, cur1RecCb, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, wn1Cb, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(prev2RecCb, cur2RecCb, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, wp2Cb, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(next2RecCb, cur2RecCb, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, wn2Cb, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(prev1RecCr, cur1RecCr, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, wp1Cr, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(next1RecCr, cur1RecCr, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, wn1Cr, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(prev2RecCr, cur2RecCr, cActive, 2, 2, siting, mfPrev.vx, mfPrev.vy, cw, ch, 16, wp2Cr, dv);
					ChromaDetail::WarpChromaPlaneWithLumaMotion(next2RecCr, cur2RecCr, cActive, 2, 2, siting, mfNext.vx, mfNext.vy, cw, ch, 16, wn2Cr, dv);

					std::vector<double> naive1Cb(cN), naive2Cb(cN), naivePrevResCb(cN);
					std::vector<double> naive1Cr(cN), naive2Cr(cN), naivePrevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						naive1Cb[i] = (cur1RecCb.v[i] + wp1Cb.v[i] + wn1Cb.v[i]) / 3.0;
						naive2Cb[i] = (cur2RecCb.v[i] + wp2Cb.v[i] + wn2Cb.v[i]) / 3.0;
						naivePrevResCb[i] = naive1Cb[i] - masterF1Cb.v[i];
						naive1Cr[i] = (cur1RecCr.v[i] + wp1Cr.v[i] + wn1Cr.v[i]) / 3.0;
						naive2Cr[i] = (cur2RecCr.v[i] + wp2Cr.v[i] + wn2Cr.v[i]) / 3.0;
						naivePrevResCr[i] = naive1Cr[i] - masterF1Cr.v[i];
					}
					const auto naiveRepCb = EvaluateGrainTransfer(
						masterF2Cb.v.data(), ownCb.data(), warpedForPrevCb.v.data(), naive2Cb.data(), cN,
						naivePrevResCb.data(), prevOwnCb.data());
					const auto naiveRepCr = EvaluateGrainTransfer(
						masterF2Cr.v.data(), ownCr.data(), warpedForPrevCr.v.data(), naive2Cr.data(), cN,
						naivePrevResCr.data(), prevOwnCr.data());
					Assert::IsTrue(naiveRepCb.valid && naiveRepCb.lag1Evaluated && naiveRepCr.valid && naiveRepCr.lag1Evaluated);
					Assert::IsFalse(naiveRepCb.passesAuthoritativeBar,
						L"Negative control: direct 0.5-px pan chroma blend MUST fail authoritative grain bar (Cb)");
					Assert::IsFalse(naiveRepCr.passesAuthoritativeBar,
						L"Negative control: direct 0.5-px pan chroma blend MUST fail authoritative grain bar (Cr)");
					Assert::IsTrue(naiveRepCb.betaOwn < 0.75 && naiveRepCb.betaForeign > 0.20,
						L"Negative control: direct blend must destroy own grain and leak warped foreign grain");
				}

				auto buildUnsplitPair = [&](ChromaGridStatus status, ChromaProcessInput& outIn1, ChromaProcessInput& outIn2)
				{
					outIn1 = ChromaProcessInput{};
					outIn1.seq = 1;
					outIn1.deliveredFormat = DeliveredChromaFormat::Yuv420;
					outIn1.codedSubsampling = ChromaSubsampling::Yuv420;
					outIn1.gridStatus = status;
					outIn1.siting = siting;
					outIn1.lumaW = w;
					outIn1.lumaH = h;
					outIn1.activeRect = active;
					outIn1.receivedCb = cur1RecCb;
					outIn1.receivedCr = cur1RecCr;
					outIn1.hasCleanSplit = false; // Unsplit blind path; retainedGrainVarCb/Cr intentionally empty!
					outIn1.prevPeak = { &prev1RecCb, &prev1RecCr, &mfPrev, false };
					outIn1.nextPeak = { &next1RecCb, &next1RecCr, &mfNext, false };
					outIn1.hasStepPrior = true;
					outIn1.estimatedStepDelta = 14.0;

					outIn2 = outIn1;
					outIn2.seq = 2;
					outIn2.receivedCb = cur2RecCb;
					outIn2.receivedCr = cur2RecCr;
					outIn2.prevPeak = { &prev2RecCb, &prev2RecCr, &mfPrev, false };
					outIn2.nextPeak = { &next2RecCb, &next2RecCr, &mfNext, false };
				};

				const Config cfg = MakePreset(Preset::Reference, Mode::On);

				// ---- Sub-case A: Identified 4:2:0 sub-pixel pan (Branch B) ----
				{
					ChromaProcessInput in1, in2;
					buildUnsplitPair(ChromaGridStatus::Identified, in1, in2);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;
					const auto res1 = eng.ProcessFrame(in1, &reg);
					const auto res2 = eng.ProcessFrame(in2, &reg);

					AssertPlaneAllFinite(res2.outputCb, L"Branch B unsplit pan outputCb must be finite");
					AssertPlaneAllFinite(res2.outputCr, L"Branch B unsplit pan outputCr must be finite");
					Assert::IsTrue(reg.IsActive(StandInKind::P10gGrainSeparator),
						L"Unsplit blind chroma path must visibly engage P10gGrainSeparator stand-in");

					std::vector<double> prevResCb(cN), prevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						prevResCb[i] = res1.outputCb.v[i] - masterF1Cb.v[i];
						prevResCr[i] = res1.outputCr.v[i] - masterF1Cr.v[i];
					}
					const auto repCb = EvaluateGrainTransfer(
						masterF2Cb.v.data(), ownCb.data(), warpedForPrevCb.v.data(), res2.outputCb.v.data(), cN,
						prevResCb.data(), prevOwnCb.data());
					const auto repCr = EvaluateGrainTransfer(
						masterF2Cr.v.data(), ownCr.data(), warpedForPrevCr.v.data(), res2.outputCr.v.data(), cN,
						prevResCr.data(), prevOwnCr.data());
					const auto repCbNext = EvaluateGrainTransfer(
						masterF2Cb.v.data(), ownCb.data(), warpedForNextCb.v.data(), res2.outputCb.v.data(), cN,
						prevResCb.data(), prevOwnCb.data());
					const auto repCrNext = EvaluateGrainTransfer(
						masterF2Cr.v.data(), ownCr.data(), warpedForNextCr.v.data(), res2.outputCr.v.data(), cN,
						prevResCr.data(), prevOwnCr.data());

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"Unsplit 4:2:0 subpixel pan (Branch B): blindNlf=%.2f/%.2f (sigma^2=%.1f) | betaOwn=%.4f/%.4f betaFor=%.4f/%.4f stdRatio=%.4f/%.4f lag1=%.4f/%.4f",
						res2.estimatedBlindNlfCb, res2.estimatedBlindNlfCr, grainSigma * grainSigma,
						repCb.betaOwn, repCr.betaOwn, repCb.betaForeign, repCr.betaForeign,
						repCb.stdRatio, repCr.stdRatio, repCb.lag1Rise, repCr.lag1Rise);
					Logger::WriteMessage(buf);

					Assert::IsTrue(res2.estimatedBlindNlfCb > 0.25 * grainSigma * grainSigma,
						L"Branch B blind estimator MUST NOT collapse to 0.0 on textured 0.5-px coded chroma pan (Cb)");
					Assert::IsTrue(res2.estimatedBlindNlfCr > 0.25 * grainSigma * grainSigma,
						L"Branch B blind estimator MUST NOT collapse to 0.0 on textured 0.5-px coded chroma pan (Cr)");

					Assert::IsTrue(repCb.valid && repCb.lag1Evaluated && repCb.passesAuthoritativeBar,
						L"Branch B unsplit 0.5-px pan Cb MUST pass full authoritative grain bar (lag1Evaluated==true)");
					Assert::IsTrue(repCr.valid && repCr.lag1Evaluated && repCr.passesAuthoritativeBar,
						L"Branch B unsplit 0.5-px pan Cr MUST pass full authoritative grain bar (lag1Evaluated==true)");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError && repCb.lag1Rise <= kGrainBarMaxLag1Rise);
					Assert::IsTrue(repCbNext.valid && repCbNext.lag1Evaluated && std::fabs(repCbNext.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(repCrNext.valid && repCrNext.lag1Evaluated && std::fabs(repCrNext.betaForeign) <= kGrainBarMaxBetaForeign);
				}

				// ---- Sub-case B: Unidentified 4:2:0 sub-pixel pan (Branch A) ----
				{
					ChromaProcessInput in1, in2;
					buildUnsplitPair(ChromaGridStatus::Unidentified, in1, in2);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;
					const auto res1 = eng.ProcessFrame(in1, &reg);
					const auto res2 = eng.ProcessFrame(in2, &reg);

					AssertPlaneAllFinite(res2.outputCb, L"Branch A unsplit pan outputCb must be finite");
					AssertPlaneAllFinite(res2.outputCr, L"Branch A unsplit pan outputCr must be finite");
					Assert::IsTrue(res2.coarseBandOnly, L"Branch A must run coarse-band-only mode");
					Assert::IsTrue(reg.IsActive(StandInKind::ChromaGridDetector));
					Assert::IsTrue(reg.IsActive(StandInKind::P10gGrainSeparator));

					std::vector<double> prevResCb(cN), prevResCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						prevResCb[i] = res1.outputCb.v[i] - masterF1Cb.v[i];
						prevResCr[i] = res1.outputCr.v[i] - masterF1Cr.v[i];
					}
					const auto repCb = EvaluateGrainTransfer(
						masterF2Cb.v.data(), ownCb.data(), warpedForPrevCb.v.data(), res2.outputCb.v.data(), cN,
						prevResCb.data(), prevOwnCb.data());
					const auto repCr = EvaluateGrainTransfer(
						masterF2Cr.v.data(), ownCr.data(), warpedForPrevCr.v.data(), res2.outputCr.v.data(), cN,
						prevResCr.data(), prevOwnCr.data());
					const auto repCbNext = EvaluateGrainTransfer(
						masterF2Cb.v.data(), ownCb.data(), warpedForNextCb.v.data(), res2.outputCb.v.data(), cN,
						prevResCb.data(), prevOwnCb.data());
					const auto repCrNext = EvaluateGrainTransfer(
						masterF2Cr.v.data(), ownCr.data(), warpedForNextCr.v.data(), res2.outputCr.v.data(), cN,
						prevResCr.data(), prevOwnCr.data());

					Assert::IsTrue(res2.estimatedBlindNlfCb > 0.25 * grainSigma * grainSigma,
						L"Opus LOW-3: Branch A blind estimator MUST report non-zero estimatedBlindNlfCb > 0.25*sigma^2 on textured 0.5-px pan");
					Assert::IsTrue(res2.estimatedBlindNlfCr > 0.25 * grainSigma * grainSigma,
						L"Opus LOW-3: Branch A blind estimator MUST report non-zero estimatedBlindNlfCr > 0.25*sigma^2 on textured 0.5-px pan");

					Assert::IsTrue(repCb.valid && repCb.lag1Evaluated && repCb.passesAuthoritativeBar,
						L"Branch A unsplit 0.5-px pan Cb MUST pass full authoritative grain bar (lag1Evaluated==true)");
					Assert::IsTrue(repCr.valid && repCr.lag1Evaluated && repCr.passesAuthoritativeBar,
						L"Branch A unsplit 0.5-px pan Cr MUST pass full authoritative grain bar (lag1Evaluated==true)");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError && repCb.lag1Rise <= kGrainBarMaxLag1Rise);
					Assert::IsTrue(repCbNext.valid && repCbNext.lag1Evaluated && std::fabs(repCbNext.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(repCrNext.valid && repCrNext.lag1Evaluated && std::fabs(repCrNext.betaForeign) <= kGrainBarMaxBetaForeign);
				}

				// ---- Sub-case C: Grain-free flat & textured anchors (grainSigma = 0.0) -> quantization-floor guard ----
				{
					// C1: Flat grain-free shifted anchor (512.0 constant, 0 grain) -> zero-energy blocks excluded, NLF < 1.0!
					const TdV2Plane flat512(cW, cH, 512.0);
					ChromaProcessInput inFlat;
					inFlat.seq = 10;
					inFlat.deliveredFormat = DeliveredChromaFormat::Yuv420;
					inFlat.codedSubsampling = ChromaSubsampling::Yuv420;
					inFlat.gridStatus = ChromaGridStatus::Identified;
					inFlat.siting = siting;
					inFlat.lumaW = w;
					inFlat.lumaH = h;
					inFlat.activeRect = active;
					inFlat.receivedCb = flat512;
					inFlat.receivedCr = flat512;
					inFlat.hasCleanSplit = false;
					inFlat.prevPeak = { &flat512, &flat512, &mfPrev, false };
					inFlat.nextPeak = { &flat512, &flat512, &mfNext, false };
					inFlat.hasStepPrior = true;
					inFlat.estimatedStepDelta = 14.0;

					TdV2ChromaEngine engFlat(cfg);
					StandInRegistry regFlat;
					const auto resFlat = engFlat.ProcessFrame(inFlat, &regFlat);
					Assert::IsTrue(resFlat.excludedZeroEnergyBlocks > 0,
						L"Flat grain-free anchor must exclude zero-energy flat blocks via quantization floor");
					Assert::IsTrue(resFlat.estimatedBlindNlfCb < 1.0 && resFlat.estimatedBlindNlfCr < 1.0,
						L"Flat grain-free anchor MUST NOT produce spurious huge estimatedBlindNlfCb/Cr (< 1.0 code^2)");

					// C2: Textured grain-free shifted anchor (monochromatic smooth texture, grainSigma = 0.0) -> NLF < 1.0!
					auto renderCleanShiftedTex = [&](double oxLuma, bool isCb) -> TdV2Plane
					{
						TdV2Plane p(cW, cH, 512.0);
						for (int cy = 0; cy < cH; ++cy)
						{
							const double ly = siting.CodedToLumaY(static_cast<double>(cy), 2);
							for (int cx = 0; cx < cW; ++cx)
							{
								const double lx = siting.CodedToLumaX(static_cast<double>(cx), 2) + oxLuma;
								const double v = isCb
									? (40.0 * std::sin(2.0 * kPi * lx / 16.0 + 0.3) * std::cos(2.0 * kPi * ly / 24.0))
									: (40.0 * std::cos(2.0 * kPi * lx / 16.0 + 0.8) * std::sin(2.0 * kPi * ly / 24.0));
								p.at(cx, cy) = 512.0 + v;
							}
						}
						return p;
					};
					const TdV2Plane texCurCb = renderCleanShiftedTex(0.0, true);
					const TdV2Plane texCurCr = renderCleanShiftedTex(0.0, false);
					const TdV2Plane texPrevCb = renderCleanShiftedTex(-1.0, true);
					const TdV2Plane texPrevCr = renderCleanShiftedTex(-1.0, false);
					const TdV2Plane texNextCb = renderCleanShiftedTex(+1.0, true);
					const TdV2Plane texNextCr = renderCleanShiftedTex(+1.0, false);

					ChromaProcessInput inTex = inFlat;
					inTex.seq = 11;
					inTex.receivedCb = texCurCb;
					inTex.receivedCr = texCurCr;
					inTex.prevPeak = { &texPrevCb, &texPrevCr, &mfPrev, false };
					inTex.nextPeak = { &texNextCb, &texNextCr, &mfNext, false };

					TdV2ChromaEngine engTex(cfg);
					StandInRegistry regTex;
					const auto resTex = engTex.ProcessFrame(inTex, &regTex);
					Assert::IsTrue(resTex.estimatedBlindNlfCb < 1.0 && resTex.estimatedBlindNlfCr < 1.0,
						L"Textured grain-free shifted anchor (grainSigma=0.0) MUST keep estimatedBlindNlfCb/Cr < 1.0 code^2");
				}
			}

			// =========================================================================
			// 18. Item #4a/b [MED Red-First]: Mode::Off Resets AnchorCanvas AND
			//     enablePerCornerHuber Toggle + Unpooled min(ownDiag, C) Protects Micro-Motion
			// =========================================================================
			TEST_METHOD(ChromaOffResetsAnchorCanvasAndPerCornerHuberToggleProtectsMicroMotion)
			{
				// ---- Sub-case A: Mode::Off resets anchorCanvas_ (no stale anchor ghosting after Off->On) ----
				{
					const int w = 64, h = 64;
					const int cw = w / 16, ch = h / 16;
					const int cW = w / 2, cH = h / 2;
					const ActiveRect active = ActiveRect::Full(w, h);
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					const SitingSpec siting = SitingSpec::Left();

					const TdV2Plane sceneACb(cW, cH, 400.0), sceneACr(cW, cH, 620.0);
					const TdV2Plane sceneBCb(cW, cH, 600.0), sceneBCr(cW, cH, 420.0);

					auto makeAnchorInput = [&](uint64_t seq, const TdV2Plane& cb, const TdV2Plane& cr, bool isAnchor, bool readAnchor)
					{
						ChromaProcessInput in;
						in.seq = seq;
						in.deliveredFormat = DeliveredChromaFormat::Yuv420;
						in.codedSubsampling = ChromaSubsampling::Yuv420;
						in.gridStatus = ChromaGridStatus::Identified;
						in.siting = siting;
						in.lumaW = w;
						in.lumaH = h;
						in.activeRect = active;
						in.receivedCb = cb;
						in.receivedCr = cr;
						in.hasCleanSplit = true;
						in.cleanCb = cb;
						in.cleanCr = cr;
						in.isAnchorFrame = isAnchor;
						in.enableAnchorCanvasRef = readAnchor;
						in.enableCarriedCfl = false;
						in.hasAnchorReadMotion = readAnchor;
						if (readAnchor)
							in.anchorReadMotion = stillMf;
						in.hasStepPrior = true;
						in.estimatedStepDelta = 24.0;
						return in;
					};

					// Path 1: Seed anchorCanvas_ in Mode::On -> SetConfig(Mode::Off) MUST immediately reset anchorCanvas_!
					TdV2ChromaEngine eng(MakePreset(Preset::Reference, Mode::On));
					StandInRegistry reg;
					eng.ProcessFrame(makeAnchorInput(1, sceneACb, sceneACr, /*isAnchor=*/true, /*readAnchor=*/false), &reg);
					Assert::IsTrue(eng.AnchorCanvas().HasData(), L"Seeding anchor in Mode::On must populate AnchorCanvas()");

					eng.SetConfig(MakePreset(Preset::Reference, Mode::Off));
					Assert::IsFalse(eng.AnchorCanvas().HasData(),
						L"SetConfig(Mode::Off) MUST immediately reset anchorCanvas_ (HasData() == false)");

					// ProcessFrame in Mode::Off must be bit-exact passthrough with zero stand-ins engaged.
					StandInRegistry regOff;
					const auto resOff = eng.ProcessFrame(makeAnchorInput(2, sceneBCb, sceneBCr, /*isAnchor=*/false, /*readAnchor=*/true), &regOff);
					Assert::IsFalse(regOff.AnyActive(), L"Mode::Off must perform zero work");
					AssertPlanesBitExact(sceneBCb, resOff.outputCb, L"Mode::Off Cb must remain bit-exact");
					AssertPlanesBitExact(sceneBCr, resOff.outputCr, L"Mode::Off Cr must remain bit-exact");

					// Re-enabling Mode::On after Mode::Off must NOT ghost stale pre-Off sceneA into sceneB!
					eng.SetConfig(MakePreset(Preset::Reference, Mode::On));
					StandInRegistry regAfterOff;
					const auto resAfterOff = eng.ProcessFrame(makeAnchorInput(3, sceneBCb, sceneBCr, /*isAnchor=*/false, /*readAnchor=*/true), &regAfterOff);
					Assert::IsFalse(resAfterOff.anchorCanvasUsed,
						L"Re-enabling Mode::On after Mode::Off must NOT read stale pre-Off anchorCanvas_");
					AssertPlanesBitExact(sceneBCb, resAfterOff.outputCb, L"Post-Off->On Cb must have zero stale-anchor ghosting");
					AssertPlanesBitExact(sceneBCr, resAfterOff.outputCr, L"Post-Off->On Cr must have zero stale-anchor ghosting");

					// Path 2 (Positive / Contrast Control): Seed -> SetConfig(Mode::On) keeps AnchorCanvas() populated and active!
					eng.ProcessFrame(makeAnchorInput(4, sceneACb, sceneACr, /*isAnchor=*/true, /*readAnchor=*/false), &reg);
					Assert::IsTrue(eng.AnchorCanvas().HasData(), L"Re-seeded anchor must populate AnchorCanvas()");
					eng.SetConfig(MakePreset(Preset::Reference, Mode::On));
					Assert::IsTrue(eng.AnchorCanvas().HasData(),
						L"SetConfig(Mode::On) while staying enabled MUST preserve AnchorCanvas().HasData()");
					StandInRegistry regKept;
					const auto resKept = eng.ProcessFrame(makeAnchorInput(5, sceneBCb, sceneBCr, /*isAnchor=*/false, /*readAnchor=*/true), &regKept);
					Assert::IsTrue(resKept.anchorCanvasUsed,
						L"Control: keeping Mode::On preserves active anchorCanvas_ (proves stale anchor WOULD be read if Mode::Off did not reset)");
				}

				// ---- Sub-case B: enablePerCornerHuber toggle + unpooled min(ownDiag, C) on Chroma ----
				//   1. Primary Yuv444 (160x160 coded = 16x16 coded px/cell, exact 1:1 twin of luma Test 12b geometry):
				//      runs across BOTH Preset::RealTime (poolR=0) AND Preset::Reference (poolR=1).
				//   2. Additional Yuv420 (80x80 coded = 8x8 coded px/cell): Preset::RealTime check.
				{
					const int w = 160, h = 160; // 10x10 canonical 16x16 luma cells = 100 cells (1% granularity, mirroring CPU Test 12b!)
					const int cellPx = 16;
					const int cw = w / cellPx, ch = h / cellPx;
					const ActiveRect active = ActiveRect::Full(w, h);
					const SitingSpec siting = SitingSpec::Left();
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					auto verifyMicroMotion = [&](
						DeliveredChromaFormat delivFmt,
						ChromaSubsampling codedSub,
						const TdV2Plane& curMasterCb,
						const TdV2Plane& curMasterCr,
						const TdV2Plane& curRecCb,
						const TdV2Plane& curRecCr,
						const TdV2Plane& prevPeakCb,
						const TdV2Plane& prevPeakCr,
						const TdV2Plane& nextPeakCb,
						const TdV2Plane& nextPeakCr,
						const ActiveRect& microCellRect,
						const ActiveRect& bgRect,
						int codedCellPx,
						Preset preset,
						const char* tag)
					{
						const double curMicroRmseCb = MaskedRmse(curMasterCb, curRecCb, microCellRect);
						const double curMicroRmseCr = MaskedRmse(curMasterCr, curRecCr, microCellRect);
						const double curBgRmseCb = MaskedRmse(curMasterCb, curRecCb, bgRect);
						const double curBgRmseCr = MaskedRmse(curMasterCr, curRecCr, bgRect);
						const double curBgMseCb = curBgRmseCb * curBgRmseCb;
						const double curBgMseCr = curBgRmseCr * curBgRmseCr;

						auto runChromaPreset = [&](bool enablePerCornerHuber) -> ChromaProcessResult
						{
							Config cfg = MakePreset(preset, Mode::On);
							cfg.enablePerCornerHuber = enablePerCornerHuber;
							TdV2ChromaEngine eng(cfg);
							StandInRegistry reg;

							ChromaProcessInput in;
							in.seq = 180;
							in.deliveredFormat = delivFmt;
							in.codedSubsampling = codedSub;
							in.gridStatus = ChromaGridStatus::Identified;
							in.siting = siting;
							in.lumaW = w;
							in.lumaH = h;
							in.activeRect = active;
							in.receivedCb = curRecCb;
							in.receivedCr = curRecCr;
							in.hasCleanSplit = true;
							in.cleanCb = curRecCb;
							in.cleanCr = curRecCr;
							in.prevPeak = { &prevPeakCb, &prevPeakCr, &stillMf, false };
							in.nextPeak = { &nextPeakCb, &nextPeakCr, &stillMf, false };
							in.hasStepPrior = true;
							in.estimatedStepDelta = 14.0;
							in.deadZonePhi = kDeadZonePhiInter;
							in.zeroCoeffFraction = 0.20;
							return eng.ProcessFrame(in, &reg);
						};

						auto countBadBlocks = [&](const TdV2Plane& out, const TdV2Plane& cur, const TdV2Plane& master) -> size_t
						{
							size_t bad = 0;
							for (int cy = 0; cy < ch; ++cy)
							{
								for (int cx = 0; cx < cw; ++cx)
								{
									const ActiveRect br{ cx * codedCellPx, cy * codedCellPx, (cx + 1) * codedCellPx, (cy + 1) * codedCellPx };
									const double baseR = MaskedRmse(master, cur, br);
									const double outR = MaskedRmse(master, out, br);
									if (outR > 1.10 * baseR)
										++bad;
								}
							}
							return bad;
						};

						const ChromaProcessResult resOn = runChromaPreset(/*enablePerCornerHuber=*/true);
						const ChromaProcessResult resOff = runChromaPreset(/*enablePerCornerHuber=*/false);
						AssertPlaneAllFinite(resOn.outputCb, L"Chroma Huber-ON outputCb must be finite");
						AssertPlaneAllFinite(resOn.outputCr, L"Chroma Huber-ON outputCr must be finite");
						AssertPlaneAllFinite(resOff.outputCb, L"Chroma Huber-OFF outputCb must be finite");
						AssertPlaneAllFinite(resOff.outputCr, L"Chroma Huber-OFF outputCr must be finite");

						const double onMicroRmseCb = MaskedRmse(curMasterCb, resOn.outputCb, microCellRect);
						const double offMicroRmseCb = MaskedRmse(curMasterCb, resOff.outputCb, microCellRect);
						const double onMicroRmseCr = MaskedRmse(curMasterCr, resOn.outputCr, microCellRect);
						const double offMicroRmseCr = MaskedRmse(curMasterCr, resOff.outputCr, microCellRect);

						const double onBgRmseCb = MaskedRmse(curMasterCb, resOn.outputCb, bgRect);
						const double onBgRmseCr = MaskedRmse(curMasterCr, resOn.outputCr, bgRect);
						const double onBgMseCb = onBgRmseCb * onBgRmseCb;
						const double onBgMseCr = onBgRmseCr * onBgRmseCr;

						const size_t badCb = countBadBlocks(resOn.outputCb, curRecCb, curMasterCb);
						const size_t badCr = countBadBlocks(resOn.outputCr, curRecCr, curMasterCr);

						char buf[256];
						std::snprintf(buf, sizeof(buf),
							"Chroma micro-motion (%s %s): Cb cur=%.3f on=%.3f off=%.3f (bad=%zu/100) | Cr cur=%.3f on=%.3f off=%.3f (bad=%zu/100)",
							tag, PresetToken(preset), curMicroRmseCb, onMicroRmseCb, offMicroRmseCb, badCb,
							curMicroRmseCr, onMicroRmseCr, offMicroRmseCr, badCr);
						Logger::WriteMessage(buf);

						// Mandatory In-Test Negative Control (Rule 3): enablePerCornerHuber = false on EXACT SAME inputs!
						Assert::IsTrue(offMicroRmseCb > onMicroRmseCb && offMicroRmseCb > curMicroRmseCb,
							L"Negative control: disabling enablePerCornerHuber on Chroma Cb MUST worsen cell (5,5) RMSE above both Huber-ON and current input");
						Assert::IsTrue(offMicroRmseCr > onMicroRmseCr && offMicroRmseCr > curMicroRmseCr,
							L"Negative control: disabling enablePerCornerHuber on Chroma Cr MUST worsen cell (5,5) RMSE above both Huber-ON and current input");

						// Protected engine assertions: strict no-harm + plan s3.3 <= 1% blocks worsened > 10% + genuine background repair!
						Assert::IsTrue(onMicroRmseCb <= curMicroRmseCb,
							L"Chroma enablePerCornerHuber=true MUST satisfy strict no-harm on cell (5,5) Cb (onMicroRmse <= curMicroRmse)");
						Assert::IsTrue(onMicroRmseCr <= curMicroRmseCr,
							L"Chroma enablePerCornerHuber=true MUST satisfy strict no-harm on cell (5,5) Cr (onMicroRmse <= curMicroRmse)");
						Assert::IsTrue(static_cast<double>(badCb) / static_cast<double>(cw * ch) <= 0.01,
							L"Plan s3.3 safety bar: <= 1% of chroma Cb blocks may worsen error by > 10%");
						Assert::IsTrue(static_cast<double>(badCr) / static_cast<double>(cw * ch) <= 0.01,
							L"Plan s3.3 safety bar: <= 1% of chroma Cr blocks may worsen error by > 10%");
						Assert::IsTrue(onBgMseCb < 0.50 * curBgMseCb && onBgMseCr < 0.50 * curBgMseCr,
							L"Non-vacuity: stationary chroma background MUST achieve >= 50% MSE reduction");
					};

					// 1. Primary Yuv444 (160x160 coded = 16x16 coded px/cell, exact 1:1 twin of luma Test 12b geometry)
					//    across BOTH Preset::RealTime (poolR=0) AND Preset::Reference (poolR=1)!
					{
						auto renderMicroChroma444 = [&](double foldShiftY, bool isCb) -> TdV2Plane
						{
							TdV2Plane p(w, h, 512.0);
							const double sign = isCb ? 1.0 : -1.0;
							for (int y = 0; y < h; ++y)
							{
								for (int x = 0; x < w; ++x)
								{
									const double base = 512.0 + sign * 38.0 * std::cos(2.0 * kPi * x / 128.0) * std::cos(2.0 * kPi * y / 112.0);
									const double tex = sign * (9.0 * std::sin(2.0 * kPi * (x / 7.3 + y / 9.1) + 0.3)
									                         + 7.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 6.7) + 1.4));
									double val = base + tex;
									// Exact 1:1 twin of luma Test 12b crease inside cell (5, 5) ([80..96) x [80..96)).
									if (x >= 80 && x < 90)
									{
										const double dx = static_cast<double>(x) - 84.5;
										const double centerY = 85.0 + foldShiftY;
										const double dy = static_cast<double>(y) - centerY;
										const double horizTaper = std::exp(-0.5 * (dx / 3.2) * (dx / 3.2));
										val -= sign * 140.0 * horizTaper * std::exp(-0.5 * (dy / 0.75) * (dy / 0.75));
										val += sign *  50.0 * horizTaper * std::exp(-0.5 * ((dy + 1.6) / 0.75) * ((dy + 1.6) / 0.75));
									}
									p.at(x, y) = val;
								}
							}
							return p;
						};

						const TdV2Plane refMasterCb = renderMicroChroma444(0.0, true);
						const TdV2Plane refMasterCr = renderMicroChroma444(0.0, false);
						const TdV2Plane curMasterCb = renderMicroChroma444(2.0, true);
						const TdV2Plane curMasterCr = renderMicroChroma444(2.0, false);

						const TdV2Plane curRecCb = AddGaussianNoise(curMasterCb, 7.0, 1811u, active);
						const TdV2Plane curRecCr = AddGaussianNoise(curMasterCr, 7.0, 1812u, active);
						const TdV2Plane prevPeakCb = AddGaussianNoise(refMasterCb, 1.2, 1813u, active);
						const TdV2Plane prevPeakCr = AddGaussianNoise(refMasterCr, 1.2, 1814u, active);
						const TdV2Plane nextPeakCb = AddGaussianNoise(refMasterCb, 1.2, 1815u, active);
						const TdV2Plane nextPeakCr = AddGaussianNoise(refMasterCr, 1.2, 1816u, active);

						const ActiveRect microCellRect444{ 80, 80, 96, 96 };
						const ActiveRect bgRect444{ 16, 16, 64, 64 };
						for (Preset preset : { Preset::RealTime, Preset::Reference })
						{
							verifyMicroMotion(
								DeliveredChromaFormat::Yuv444, ChromaSubsampling::Yuv444,
								curMasterCb, curMasterCr, curRecCb, curRecCr,
								prevPeakCb, prevPeakCr, nextPeakCb, nextPeakCr,
								microCellRect444, bgRect444, 16, preset, "444");
						}
					}

					// 2. Additional Yuv420 (80x80 coded = 8x8 coded px/cell): Preset::RealTime check.
					{
						const int cW = w / 2, cH = h / 2;
						const ActiveRect cActive = ActiveRect::Full(cW, cH);
						auto renderMicroChroma420 = [&](double foldShiftCodedY, bool isCb) -> TdV2Plane
						{
							TdV2Plane p(cW, cH, 512.0);
							const double sign = isCb ? 1.0 : -1.0;
							for (int y = 0; y < cH; ++y)
							{
								for (int x = 0; x < cW; ++x)
								{
									const double base = 512.0 + sign * 24.0 * std::cos(2.0 * kPi * x / 64.0) * std::cos(2.0 * kPi * y / 56.0);
									const double tex = sign * (7.0 * std::sin(2.0 * kPi * (x / 5.3 + y / 6.7) + 0.3)
									                         + 5.0 * std::cos(2.0 * kPi * (x / 4.1 - y / 5.1) + 1.4));
									double val = base + tex;
									// Localized moving non-rigid crease near the left interior boundary of cell (5, 5) ([40..48) x [40..48)).
									if (x >= 40 && x < 45)
									{
										const double dx = static_cast<double>(x) - 42.0;
										const double centerY = 42.5 + foldShiftCodedY;
										const double dy = static_cast<double>(y) - centerY;
										const double horizTaper = std::exp(-0.5 * (dx / 1.6) * (dx / 1.6));
										val -= sign * 150.0 * horizTaper * std::exp(-0.5 * (dy / 0.60) * (dy / 0.60));
										val += sign *  55.0 * horizTaper * std::exp(-0.5 * ((dy + 1.2) / 0.60) * ((dy + 1.2) / 0.60));
									}
									p.at(x, y) = val;
								}
							}
							return p;
						};

						const TdV2Plane refMasterCb = renderMicroChroma420(0.0, true);
						const TdV2Plane refMasterCr = renderMicroChroma420(0.0, false);
						const TdV2Plane curMasterCb = renderMicroChroma420(1.5, true);
						const TdV2Plane curMasterCr = renderMicroChroma420(1.5, false);

						const TdV2Plane curRecCb = AddGaussianNoise(curMasterCb, 7.0, 1801u, cActive);
						const TdV2Plane curRecCr = AddGaussianNoise(curMasterCr, 7.0, 1802u, cActive);
						const TdV2Plane prevPeakCb = AddGaussianNoise(refMasterCb, 1.2, 1803u, cActive);
						const TdV2Plane prevPeakCr = AddGaussianNoise(refMasterCr, 1.2, 1804u, cActive);
						const TdV2Plane nextPeakCb = AddGaussianNoise(refMasterCb, 1.2, 1805u, cActive);
						const TdV2Plane nextPeakCr = AddGaussianNoise(refMasterCr, 1.2, 1806u, cActive);

						const ActiveRect microCellRect420{ 40, 40, 48, 48 };
						const ActiveRect bgRect420{ 8, 8, 32, 32 };
						verifyMicroMotion(
							DeliveredChromaFormat::Yuv420, ChromaSubsampling::Yuv420,
							curMasterCb, curMasterCr, curRecCb, curRecCr,
							prevPeakCb, prevPeakCr, nextPeakCb, nextPeakCr,
							microCellRect420, bgRect420, 8, Preset::RealTime, "420");
					}
				}

				// ---- Sub-case C (Item #4c / H13): Pass-1 faceMask exact zero-delta + unmasked negative control ----
				// Mask an interior 2x2 luma-cell face region (cells (3..4, 3..4) -> 4 cells -> coded 4:2:0 [24..40)^2)
				// on BOTH Identified 4:2:0 (transformIdentified8x8=true, ResidualMode::Cell) AND Unidentified coarse-only (Branch A).
				{
					const int w = 128, h = 128;
					const int cellPx = 16;
					const int cw = w / cellPx, ch = h / cellPx; // 8x8 luma cells
					const int cW = w / 2, cH = h / 2;           // 64x64 coded 4:2:0 (8x8 coded px per cell)
					const ActiveRect active = ActiveRect::Full(w, h);
					const ActiveRect cActive = ActiveRect::Full(cW, cH);
					const SitingSpec siting = SitingSpec::Left();
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					// Smooth base chroma field on 64x64 coded 4:2:0.
					TdV2Plane masterCb(cW, cH, 512.0), masterCr(cW, cH, 512.0);
					for (int cy = 0; cy < cH; ++cy)
					{
						for (int cx = 0; cx < cW; ++cx)
						{
							masterCb.at(cx, cy) = 512.0 + 24.0 * std::sin(2.0 * kPi * cx / 64.0) * std::cos(2.0 * kPi * cy / 64.0);
							masterCr.at(cx, cy) = 512.0 - 24.0 * std::cos(2.0 * kPi * cx / 64.0) * std::sin(2.0 * kPi * cy / 64.0);
						}
					}

					// Interior 2x2 luma-cell face mask: cells (3,3), (4,3), (3,4), (4,4) -> 4 cells -> coded [24..40) x [24..40).
					std::vector<uint8_t> faceMask(static_cast<size_t>(cw) * ch, 0u);
					for (int cy = 3; cy < 5; ++cy)
						for (int cx = 3; cx < 5; ++cx)
							faceMask[static_cast<size_t>(cy) * cw + cx] = 1u;

					const ActiveRect bgRect{ 0, 0, 16, 16 }; // Stationary unmasked background far from face cells.

					const TdV2Plane prevPeakCb = AddGaussianNoise(masterCb, 0.8, 1851u, cActive);
					const TdV2Plane prevPeakCr = AddGaussianNoise(masterCr, 0.8, 1852u, cActive);
					const TdV2Plane nextPeakCb = AddGaussianNoise(masterCb, 0.8, 1853u, cActive);
					const TdV2Plane nextPeakCr = AddGaussianNoise(masterCr, 0.8, 1854u, cActive);

					// Identified (Branch B, 8x8 DCT CELL): Gaussian coding noise (sigma = 7.0);
					// Unidentified (Branch A, coarse G(16)): coarse low-frequency chroma banding (+9 Cb / -9 Cr + mild noise).
					const TdV2Plane curIdCb = AddGaussianNoise(masterCb, 7.0, 1855u, cActive);
					const TdV2Plane curIdCr = AddGaussianNoise(masterCr, 7.0, 1856u, cActive);

					TdV2Plane curUnidCb = AddGaussianNoise(masterCb, 0.5, 1857u, cActive);
					TdV2Plane curUnidCr = AddGaussianNoise(masterCr, 0.5, 1858u, cActive);
					for (size_t i = 0; i < curUnidCb.v.size(); ++i)
					{
						curUnidCb.v[i] += 9.0;
						curUnidCr.v[i] -= 9.0;
					}

					struct FaceGridCase
					{
						ChromaGridStatus status;
						bool transform8x8;
						const TdV2Plane* curCb;
						const TdV2Plane* curCr;
						const wchar_t* label;
					};
					const FaceGridCase cases[2] = {
						{ ChromaGridStatus::Identified,   true,  &curIdCb,   &curIdCr,   L"Identified-420-8x8DCT-Cell" },
						{ ChromaGridStatus::Unidentified, false, &curUnidCb, &curUnidCr, L"Unidentified-420-CoarseOnly" }
					};

					for (const FaceGridCase& fc : cases)
					{
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						cfg.residualMode = ResidualMode::Cell;
						cfg.cellProjIterations = 2;

						ChromaProcessInput baseIn;
						baseIn.seq = 190;
						baseIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
						baseIn.codedSubsampling = ChromaSubsampling::Yuv420;
						baseIn.gridStatus = fc.status;
						baseIn.siting = siting;
						baseIn.lumaW = w;
						baseIn.lumaH = h;
						baseIn.activeRect = active;
						baseIn.receivedCb = *fc.curCb;
						baseIn.receivedCr = *fc.curCr;
						baseIn.hasCleanSplit = true;
						baseIn.cleanCb = *fc.curCb;
						baseIn.cleanCr = *fc.curCr;
						baseIn.prevPeak = { &prevPeakCb, &prevPeakCr, &stillMf, false };
						baseIn.nextPeak = { &nextPeakCb, &nextPeakCr, &stillMf, false };
						baseIn.hasStepPrior = true;
						baseIn.estimatedStepDelta = 24.0;
						baseIn.deadZonePhi = kDeadZonePhiInter;
						baseIn.zeroCoeffFraction = 0.35;
						baseIn.transformIdentified8x8 = fc.transform8x8;

						// Mandatory Same-Input Negative Control (Rule 3): faceMask == nullptr MUST modify face pixels (> 0.5 code)!
						TdV2ChromaEngine engUnmasked(cfg);
						StandInRegistry regUnmasked;
						const ChromaProcessResult resUnmasked = engUnmasked.ProcessFrame(baseIn, &regUnmasked);
						Assert::AreEqual(static_cast<size_t>(0), resUnmasked.faceMaskedCells,
							(std::wstring(fc.label) + L": Negative control unmasked run must report faceMaskedCells == 0").c_str());
						Assert::IsFalse(regUnmasked.IsActive(StandInKind::FaceNonRigidMask),
							(std::wstring(fc.label) + L": Negative control unmasked run must NOT engage FaceNonRigidMask").c_str());

						double maxUnmaskedCb = 0.0, maxUnmaskedCr = 0.0;
						for (int y = 24; y < 40; ++y)
						{
							for (int x = 24; x < 40; ++x)
							{
								maxUnmaskedCb = (std::max)(maxUnmaskedCb, std::fabs(resUnmasked.outputCb.at(x, y) - fc.curCb->at(x, y)));
								maxUnmaskedCr = (std::max)(maxUnmaskedCr, std::fabs(resUnmasked.outputCr.at(x, y) - fc.curCr->at(x, y)));
							}
						}
						Assert::IsTrue(maxUnmaskedCb > 0.5 && maxUnmaskedCr > 0.5,
							(std::wstring(fc.label) + L": Negative control unmasked face cells MUST receive non-zero chroma delta (> 0.5 code)").c_str());

						// Protected Pass-1 faceMask run: exact bit-for-bit 0.0 delta on EVERY pixel in masked cells [24..40)^2!
						ChromaProcessInput maskedIn = baseIn;
						maskedIn.faceMask = &faceMask;
						TdV2ChromaEngine engMasked(cfg);
						StandInRegistry regMasked;
						const ChromaProcessResult resMasked = engMasked.ProcessFrame(maskedIn, &regMasked);
						AssertPlaneAllFinite(resMasked.outputCb, L"Chroma face-masked outputCb must be finite");
						AssertPlaneAllFinite(resMasked.outputCr, L"Chroma face-masked outputCr must be finite");

						Assert::AreEqual(static_cast<size_t>(4), resMasked.faceMaskedCells,
							(std::wstring(fc.label) + L": faceMaskedCells MUST equal exact 4 masked luma cells").c_str());
						Assert::IsTrue(regMasked.IsActive(StandInKind::FaceNonRigidMask),
							(std::wstring(fc.label) + L": FaceNonRigidMask stand-in MUST be visibly active").c_str());

						for (int y = 24; y < 40; ++y)
						{
							for (int x = 24; x < 40; ++x)
							{
								Assert::AreEqual(0.0, resMasked.codedDeltaCb.at(x, y), 0.0,
									(std::wstring(fc.label) + L": masked face codedDeltaCb MUST be exact 0.0").c_str());
								Assert::AreEqual(0.0, resMasked.codedDeltaCr.at(x, y), 0.0,
									(std::wstring(fc.label) + L": masked face codedDeltaCr MUST be exact 0.0").c_str());
								Assert::AreEqual(fc.curCb->at(x, y), resMasked.outputCb.at(x, y), 0.0,
									(std::wstring(fc.label) + L": masked face outputCb MUST be bit-exact to received").c_str());
								Assert::AreEqual(fc.curCr->at(x, y), resMasked.outputCr.at(x, y), 0.0,
									(std::wstring(fc.label) + L": masked face outputCr MUST be bit-exact to received").c_str());
							}
						}

						// Non-vacuity: stationary unmasked background MUST still repair >= 50% MSE!
						const double curBgRmseCb = MaskedRmse(masterCb, *fc.curCb, bgRect);
						const double curBgRmseCr = MaskedRmse(masterCr, *fc.curCr, bgRect);
						const double outBgRmseCb = MaskedRmse(masterCb, resMasked.outputCb, bgRect);
						const double outBgRmseCr = MaskedRmse(masterCr, resMasked.outputCr, bgRect);
						const double curBgMseCb = curBgRmseCb * curBgRmseCb;
						const double curBgMseCr = curBgRmseCr * curBgRmseCr;
						const double outBgMseCb = outBgRmseCb * outBgRmseCb;
						const double outBgMseCr = outBgRmseCr * outBgRmseCr;
						Assert::IsTrue(outBgMseCb < 0.50 * curBgMseCb && outBgMseCr < 0.50 * curBgMseCr,
							(std::wstring(fc.label) + L": stationary unmasked chroma background MUST achieve >= 50% MSE reduction").c_str());
					}
				}
			}

			// =========================================================================
			// 14. Opus Repeat Audit LOW-4: Blind Chroma NLF Must Divide Only by
			//     Contributing References (meanRef > refQuantFloor), Not K / activeK
			// =========================================================================
			TEST_METHOD(OpusLow4BlindChromaDividesByContributingRefsAndUsesBandWeightedWarpGain)
			{
				const int w = 64;
				const int h = 64;
				const int cellPx = 16;
				const int cw = w / cellPx; // 4
				const int ch = h / cellPx; // 4
				const int cW = w / 2;      // 32
				const int cH = h / 2;      // 32
				const size_t cN = static_cast<size_t>(cW) * static_cast<size_t>(cH);
				const ActiveRect active = ActiveRect::Full(w, h);
				const SitingSpec siting = SitingSpec::Left();
				const double grainSigma = 6.0;

				auto renderTexturedChroma = [&](double oxLuma, bool isCb) -> TdV2Plane
				{
					TdV2Plane p(cW, cH, 512.0);
					for (int cy = 0; cy < cH; ++cy)
					{
						const double ly = siting.CodedToLumaY(static_cast<double>(cy), 2);
						for (int cx = 0; cx < cW; ++cx)
						{
							const double lx = siting.CodedToLumaX(static_cast<double>(cx), 2) + oxLuma;
							const double t1 = (isCb ? 48.0 : -42.0) * std::sin(2.0 * kPi * lx / 24.0) * std::cos(2.0 * kPi * ly / 28.0);
							const double t2 = (isCb ? 22.0 : 26.0) * std::cos(2.0 * kPi * (lx + 0.5 * ly) / 14.0);
							p.at(cx, cy) = 512.0 + t1 + t2;
						}
					}
					return p;
				};

				TdV2Plane curRecCb = renderTexturedChroma(0.0, true);
				TdV2Plane curRecCr = renderTexturedChroma(0.0, false);
				TdV2Plane prevRecCb = renderTexturedChroma(-1.0, true);
				TdV2Plane prevRecCr = renderTexturedChroma(-1.0, false);
				TdV2Plane nextRealCb = renderTexturedChroma(+1.0, true);
				TdV2Plane nextRealCr = renderTexturedChroma(+1.0, false);

				std::mt19937 rng(42401u);
				std::normal_distribution<double> gd(0.0, grainSigma);
				for (size_t i = 0; i < cN; ++i)
				{
					curRecCb.v[i] += gd(rng);
					curRecCr.v[i] += gd(rng);
					prevRecCb.v[i] += gd(rng);
					prevRecCr.v[i] += gd(rng);
					nextRealCb.v[i] += gd(rng);
					nextRealCr.v[i] += gd(rng);
				}

				const TdV2Plane flatZero(cW, cH, 0.0);
				const CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +1.0, 0.0, +1.0, 0.0, RegionMotionClass::Camera);
				const CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -1.0, 0.0, -1.0, 0.0, RegionMotionClass::Camera);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				const Config cfg = MakePreset(Preset::Reference, Mode::On);

				struct BranchCase
				{
					ChromaGridStatus status;
					const wchar_t* label;
				};
				const BranchCase branches[2] = {
					{ ChromaGridStatus::Identified,   L"Branch B (Identified)" },
					{ ChromaGridStatus::Unidentified, L"Branch A (Unidentified)" }
				};

				for (const BranchCase& bc : branches)
				{
					auto makeBaseIn = [&](uint64_t seq) -> ChromaProcessInput
					{
						ChromaProcessInput in;
						in.seq = seq;
						in.deliveredFormat = DeliveredChromaFormat::Yuv420;
						in.codedSubsampling = ChromaSubsampling::Yuv420;
						in.gridStatus = bc.status;
						in.siting = siting;
						in.lumaW = w;
						in.lumaH = h;
						in.activeRect = active;
						in.receivedCb = curRecCb;
						in.receivedCr = curRecCr;
						in.hasCleanSplit = false; // Unsplit blind path; retainedGrainVarCb/Cr empty!
						in.prevPeak = { &prevRecCb, &prevRecCr, &mfPrev, false };
						in.hasStepPrior = true;
						in.estimatedStepDelta = 14.0;
						return in;
					};

					// Run 1 (oneRef): 1 normal textured+grainy reference (prevPeak, meanRef > refQuantFloor).
					ChromaProcessInput inOne = makeBaseIn(101);
					TdV2ChromaEngine engOne(cfg);
					StandInRegistry regOne;
					const ChromaProcessResult resOne = engOne.ProcessFrame(inOne, &regOne);
					AssertPlaneAllFinite(resOne.outputCb, L"LOW-4 oneRef outputCb must be finite");
					AssertPlaneAllFinite(resOne.outputCr, L"LOW-4 oneRef outputCr must be finite");
					Assert::IsTrue(resOne.estimatedBlindNlfCb > 0.25 * grainSigma * grainSigma,
						(std::wstring(bc.label) + L": oneRef estimatedBlindNlfCb must be > 0.25*sigma^2").c_str());
					Assert::IsTrue(resOne.estimatedBlindNlfCr > 0.25 * grainSigma * grainSigma,
						(std::wstring(bc.label) + L": oneRef estimatedBlindNlfCr must be > 0.25*sigma^2").c_str());

					// Run 2 (twoRefWithCrushedBlack): exact same prevPeak PLUS a 2nd reference whose chroma is flat 0.0
					// (meanRef == 0.0 <= refQuantFloor -> ONLY 1 of 2 references contributes!).
					ChromaProcessInput inTwo = makeBaseIn(102);
					inTwo.nextPeak = { &flatZero, &flatZero, &stillMf, false };
					TdV2ChromaEngine engTwo(cfg);
					StandInRegistry regTwo;
					const ChromaProcessResult resTwo = engTwo.ProcessFrame(inTwo, &regTwo);
					AssertPlaneAllFinite(resTwo.outputCb, L"LOW-4 twoRefWithCrushedBlack outputCb must be finite");
					AssertPlaneAllFinite(resTwo.outputCr, L"LOW-4 twoRefWithCrushedBlack outputCr must be finite");

					Assert::IsTrue(resTwo.estimatedBlindNlfCb >= 0.85 * resOne.estimatedBlindNlfCb,
						(std::wstring(bc.label) + L": Opus LOW-4 crushed-black 2nd ref MUST NOT dilute estimatedBlindNlfCb by dividing by K/activeK=2").c_str());
					Assert::IsTrue(resTwo.estimatedBlindNlfCr >= 0.85 * resOne.estimatedBlindNlfCr,
						(std::wstring(bc.label) + L": Opus LOW-4 crushed-black 2nd ref MUST NOT dilute estimatedBlindNlfCr by dividing by K/activeK=2").c_str());
					Assert::IsTrue(resTwo.estimatedBlindNlfCb <= 1.15 * resOne.estimatedBlindNlfCb &&
					               resTwo.estimatedBlindNlfCr <= 1.15 * resOne.estimatedBlindNlfCr,
						(std::wstring(bc.label) + L": crushed-black 2nd ref must not inflate estimatedBlindNlf beyond 1.15x oneRef").c_str());

					// Anti-cheat control (2 genuinely contributing refs): average over validRefCnt==2 stays near 1.0x oneRef
					// (catches any bogus fix that drops the denominator instead of dividing by validRefCnt!).
					ChromaProcessInput inTwoReal = makeBaseIn(103);
					inTwoReal.nextPeak = { &nextRealCb, &nextRealCr, &mfNext, false };
					TdV2ChromaEngine engTwoReal(cfg);
					StandInRegistry regTwoReal;
					const ChromaProcessResult resTwoReal = engTwoReal.ProcessFrame(inTwoReal, &regTwoReal);
					Assert::IsTrue(resTwoReal.estimatedBlindNlfCb >= 0.65 * resOne.estimatedBlindNlfCb &&
					               resTwoReal.estimatedBlindNlfCb <= 1.45 * resOne.estimatedBlindNlfCb,
						(std::wstring(bc.label) + L": 2 contributing refs must average cleanly by validRefCnt==2 (Cb)").c_str());
					Assert::IsTrue(resTwoReal.estimatedBlindNlfCr >= 0.65 * resOne.estimatedBlindNlfCr &&
					               resTwoReal.estimatedBlindNlfCr <= 1.45 * resOne.estimatedBlindNlfCr,
						(std::wstring(bc.label) + L": 2 contributing refs must average cleanly by validRefCnt==2 (Cr)").c_str());

					// Mandatory Same-Input Negative Control (Rule 3):
					// All-flat crushed-black frame + refs (0 contributing refs) must still report < 1.0 code^2!
					ChromaProcessInput inAllFlat = makeBaseIn(104);
					inAllFlat.receivedCb = flatZero;
					inAllFlat.receivedCr = flatZero;
					inAllFlat.prevPeak = { &flatZero, &flatZero, &stillMf, false };
					inAllFlat.nextPeak = { &flatZero, &flatZero, &stillMf, false };
					TdV2ChromaEngine engAllFlat(cfg);
					StandInRegistry regAllFlat;
					const ChromaProcessResult resAllFlat = engAllFlat.ProcessFrame(inAllFlat, &regAllFlat);
					Assert::IsTrue(resAllFlat.estimatedBlindNlfCb < 1.0 && resAllFlat.estimatedBlindNlfCr < 1.0,
						(std::wstring(bc.label) + L": Negative control all-flat zero-ref frame must remain < 1.0 code^2").c_str());
				}

				// ---- Direct Parseval / 2-D Spatial Convolution Verification of HighPassBicubicWarpVarianceGain ----
				{
					const std::vector<double> g1 = VpUnit::BandFromBase::Taps<double>(VpUnit::BandFromBase::kSigmaFine, 3.0, true, 0.0);
					Assert::IsFalse(g1.empty(), L"BandFromBase::Taps(kSigmaFine) must be non-empty");

					// Integer shifts MUST return exact 1.0 (zero bicubic attenuation on integer grid).
					Assert::AreEqual(1.0, HighPassBicubicWarpVarianceGain(0.0, 0.0, g1, g1), 1e-12,
						L"Opus LOW-4: HighPassBicubicWarpVarianceGain(0, 0) must equal exact 1.0");
					Assert::AreEqual(1.0, HighPassBicubicWarpVarianceGain(3.0, -2.0, g1, g1), 1e-12,
						L"Opus LOW-4: HighPassBicubicWarpVarianceGain(3, -2) must equal exact 1.0");

					// Independent brute-force 2-D spatial kernel convolution sum((H * W)^2) / sum(H^2)
					// where H(dx, dy) = delta(dx, dy) - g1(dx)*g1(dy) and W(ox, oy) = wx(ox)*wy(oy).
					const int gLen = static_cast<int>(g1.size());
					const int r = gLen / 2;
					const int dMin = -r;
					const int dMax = gLen - 1 - r;
					auto h2d = [&](int dx, int dy) -> double
					{
						if (dx < dMin || dx > dMax || dy < dMin || dy > dMax)
							return 0.0;
						const double gxy = g1[static_cast<size_t>(dx + r)] * g1[static_cast<size_t>(dy + r)];
						return ((dx == 0 && dy == 0) ? 1.0 : 0.0) - gxy;
					};
					double sumH2 = 0.0;
					for (int dy = dMin; dy <= dMax; ++dy)
						for (int dx = dMin; dx <= dMax; ++dx)
						{
							const double hv = h2d(dx, dy);
							sumH2 += hv * hv;
						}
					Assert::IsTrue(sumH2 > 0.1, L"High-pass kernel energy sum(H^2) must be strictly positive");

					auto bruteForce2dGain = [&](double vx, double vy) -> double
					{
						const double fx = vx - std::floor(vx);
						const double fy = vy - std::floor(vy);
						double wx[4] = {}, wy[4] = {};
						VpComposed::CubicWeights(fx, wx);
						VpComposed::CubicWeights(fy, wy);
						const int mMin = dMin - 1;
						const int mMax = dMax + 2;
						double sumHW2 = 0.0;
						for (int my = mMin; my <= mMax; ++my)
						{
							for (int mx = mMin; mx <= mMax; ++mx)
							{
								double acc = 0.0;
								for (int iy = 0; iy < 4; ++iy)
									for (int ix = 0; ix < 4; ++ix)
										acc += wx[ix] * wy[iy] * h2d(mx - (ix - 1), my - (iy - 1));
								sumHW2 += acc * acc;
							}
						}
						return sumHW2 / sumH2;
					};

					struct ShiftCase
					{
						double vx;
						double vy;
						double minGapVsWhite;
					};
					const ShiftCase shifts[2] = {
						{ 0.5, 0.0, 0.05 }, // 1-D half-pixel pan: white 164/256 ~= 0.6406 vs high-pass ~= 0.54 (gap ~= 0.10)
						{ 0.5, 0.5, 0.08 }  // 2-D half-pixel pan: stronger high-pass attenuation gap vs white-noise gain
					};
					for (const ShiftCase& sh : shifts)
					{
						const double hpGain = HighPassBicubicWarpVarianceGain(sh.vx, sh.vy, g1, g1);
						const double whiteGain = BicubicWarpVarianceGain(sh.vx, sh.vy);
						const double bruteGain = bruteForce2dGain(sh.vx, sh.vy);

						Assert::AreEqual(bruteGain, hpGain, 1e-10,
							L"Opus LOW-4: HighPassBicubicWarpVarianceGain MUST match independent 2-D spatial convolution sum((H*W)^2)/sum(H^2) to 1e-10");
						Assert::IsTrue(hpGain > 0.0 && hpGain < whiteGain,
							L"Opus LOW-4: HighPassBicubicWarpVarianceGain at sub-pixel shift MUST be strictly < white-noise BicubicWarpVarianceGain");
						// Negative control: white-noise BicubicWarpVarianceGain materially overstates high-pass warped grain gain
						// and genuinely underestimates true own grain variance sigma^2 = 36.0 in ComputeProjectionIr,
						// whereas band-weighted HighPassBicubicWarpVarianceGain recovers exact sigma^2 = 36.0!
						Assert::IsTrue((whiteGain - hpGain) > sh.minGapVsWhite,
							L"Negative control: white-noise BicubicWarpVarianceGain must materially overstate high-pass warp gain at half-pixel shift");

						const double texS = 2000.0;
						const double trueSigma2 = 36.0;
						const double alpha = 0.65;
						const double momE = texS + trueSigma2;
						const double momC = alpha * texS;
						const double momEprev = alpha * alpha * texS + hpGain * trueSigma2;
						const double irHp = ComputeProjectionIr(momE, momC, momEprev, nullptr, hpGain);
						const double irWhite = ComputeProjectionIr(momE, momC, momEprev, nullptr, whiteGain);
						Assert::AreEqual(trueSigma2, irHp, 1e-9,
							L"Opus LOW-4: ComputeProjectionIr with band-weighted HighPassBicubicWarpVarianceGain MUST recover exact sigma^2 = 36.0");
						Assert::IsTrue(irWhite < trueSigma2 - 2.0,
							L"Negative control: ComputeProjectionIr with white-noise BicubicWarpVarianceGain MUST underestimate true sigma^2 = 36.0 by > 2.0 code^2");
					}
				}
			}

			// =========================================================================
			// 15. Opus Repeat Audit LOW-5: Undersized faceMask Engages Visible Stand-In
			//     and Fails Closed Conservatively on Out-of-Range Trailing Cells
			// =========================================================================
			TEST_METHOD(OpusLow5UndersizedFaceMaskEngagesVisibleStandInAndFailsClosed)
			{
				const int w = 64;
				const int h = 64;
				const int cellPx = 16;
				const int cw = w / cellPx; // 4
				const int ch = h / cellPx; // 4 -> 16 cells
				const int cW = w / 2;      // 32
				const int cH = h / 2;      // 32
				const ActiveRect active = ActiveRect::Full(w, h);
				const SitingSpec siting = SitingSpec::Left();
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				TdV2Plane prevPeakCb(cW, cH, 512.0), prevPeakCr(cW, cH, 512.0);
				TdV2Plane nextPeakCb(cW, cH, 512.0), nextPeakCr(cW, cH, 512.0);
				TdV2Plane curCb(cW, cH, 512.0), curCr(cW, cH, 512.0);
				std::mt19937 refRng(5501u);
				std::normal_distribution<double> refJitter(0.0, 0.5);
				for (int cy = 0; cy < cH; ++cy)
				{
					for (int cx = 0; cx < cW; ++cx)
					{
						const double baseCb = 512.0 + 36.0 * std::sin(2.0 * kPi * (cx + 0.5) / 16.0);
						const double baseCr = 512.0 - 32.0 * std::cos(2.0 * kPi * (cy + 0.5) / 16.0);
						prevPeakCb.at(cx, cy) = baseCb + refJitter(refRng);
						prevPeakCr.at(cx, cy) = baseCr + refJitter(refRng);
						nextPeakCb.at(cx, cy) = baseCb + refJitter(refRng);
						nextPeakCr.at(cx, cy) = baseCr + refJitter(refRng);
						// Coherent +10.0 code low-frequency compression bias across all cells (including trailing cells 1..15).
						curCb.at(cx, cy) = baseCb + 10.0;
						curCr.at(cx, cy) = baseCr - 10.0;
					}
				}

				const std::vector<uint8_t> undersizedFaceMask{ 0u }; // Length 1 < 16: cells 1..15 missing!
				const Config cfg = MakePreset(Preset::Reference, Mode::On);

				struct GridCase
				{
					ChromaGridStatus status;
					bool transform8x8;
					const wchar_t* label;
				};
				const GridCase cases[2] = {
					{ ChromaGridStatus::Identified,   true,  L"Identified-420" },
					{ ChromaGridStatus::Unidentified, false, L"Unidentified-420" }
				};

				for (const GridCase& gc : cases)
				{
					ChromaProcessInput baseIn;
					baseIn.seq = 250;
					baseIn.deliveredFormat = DeliveredChromaFormat::Yuv420;
					baseIn.codedSubsampling = ChromaSubsampling::Yuv420;
					baseIn.gridStatus = gc.status;
					baseIn.siting = siting;
					baseIn.lumaW = w;
					baseIn.lumaH = h;
					baseIn.activeRect = active;
					baseIn.receivedCb = curCb;
					baseIn.receivedCr = curCr;
					baseIn.hasCleanSplit = true;
					baseIn.cleanCb = curCb;
					baseIn.cleanCr = curCr;
					baseIn.prevPeak = { &prevPeakCb, &prevPeakCr, &stillMf, false };
					baseIn.nextPeak = { &nextPeakCb, &nextPeakCr, &stillMf, false };
					baseIn.hasStepPrior = true;
					baseIn.estimatedStepDelta = 24.0;
					baseIn.deadZonePhi = kDeadZonePhiInter;
					baseIn.zeroCoeffFraction = 0.35;
					baseIn.transformIdentified8x8 = gc.transform8x8;

					// Mandatory Same-Input Negative Control (Rule 3): faceMask == nullptr MUST blend trailing cells 1..15!
					TdV2ChromaEngine engUnmasked(cfg);
					StandInRegistry regUnmasked;
					const ChromaProcessResult resUnmasked = engUnmasked.ProcessFrame(baseIn, &regUnmasked);
					Assert::IsFalse(regUnmasked.IsActive(StandInKind::FaceNonRigidMask),
						(std::wstring(gc.label) + L": Negative control unmasked run must not engage FaceNonRigidMask").c_str());
					Assert::IsFalse(regUnmasked.IsActive(StandInKind::FaceMaskSizeMismatch),
						(std::wstring(gc.label) + L": Negative control unmasked run must not engage FaceMaskSizeMismatch").c_str());

					double maxTrailingUnmaskedCb = 0.0, maxTrailingUnmaskedCr = 0.0;
					for (int cy = 0; cy < cH; ++cy)
					{
						for (int cx = 0; cx < cW; ++cx)
						{
							if (cx < 8 && cy < 8)
								continue; // Cell 0 (luma [0,16)^2)
							maxTrailingUnmaskedCb = (std::max)(maxTrailingUnmaskedCb, std::fabs(resUnmasked.outputCb.at(cx, cy) - curCb.at(cx, cy)));
							maxTrailingUnmaskedCr = (std::max)(maxTrailingUnmaskedCr, std::fabs(resUnmasked.outputCr.at(cx, cy) - curCr.at(cx, cy)));
						}
					}
					Assert::IsTrue(maxTrailingUnmaskedCb > 0.5 && maxTrailingUnmaskedCr > 0.5,
						(std::wstring(gc.label) + L": Negative control trailing cells 1..15 must receive real non-zero delta (> 0.5)").c_str());

					// Undersized non-empty faceMask (size 1 < 16): MUST engage visible stand-in AND conservatively mask trailing cells 1..15!
					ChromaProcessInput undersizedIn = baseIn;
					undersizedIn.faceMask = &undersizedFaceMask;
					TdV2ChromaEngine engUndersized(cfg);
					StandInRegistry regUndersized;
					const ChromaProcessResult resUndersized = engUndersized.ProcessFrame(undersizedIn, &regUndersized);
					AssertPlaneAllFinite(resUndersized.outputCb, L"Undersized faceMask outputCb must be finite");
					AssertPlaneAllFinite(resUndersized.outputCr, L"Undersized faceMask outputCr must be finite");

					Assert::IsTrue(regUndersized.IsActive(StandInKind::FaceNonRigidMask),
						(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST visibly engage StandInKind::FaceNonRigidMask").c_str());
					Assert::IsTrue(regUndersized.IsActive(StandInKind::FaceMaskSizeMismatch),
						(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST visibly engage StandInKind::FaceMaskSizeMismatch").c_str());
					Assert::AreEqual(cW, resUndersized.outputCb.w);
					Assert::AreEqual(cH, resUndersized.outputCb.h);
					Assert::AreEqual(cW, resUndersized.outputCr.w);
					Assert::AreEqual(cH, resUndersized.outputCr.h);

					for (int cy = 0; cy < cH; ++cy)
					{
						for (int cx = 0; cx < cW; ++cx)
						{
							if (cx < 8 && cy < 8)
								continue; // Trailing cells 1..15 only
							if (!resUndersized.codedDeltaCb.Empty())
							{
								Assert::AreEqual(0.0, resUndersized.codedDeltaCb.at(cx, cy), 0.0,
									(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST force trailing cells 1..15 codedDeltaCb == 0.0").c_str());
							}
							if (!resUndersized.codedDeltaCr.Empty())
							{
								Assert::AreEqual(0.0, resUndersized.codedDeltaCr.at(cx, cy), 0.0,
									(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST force trailing cells 1..15 codedDeltaCr == 0.0").c_str());
							}
							Assert::AreEqual(curCb.at(cx, cy), resUndersized.outputCb.at(cx, cy), 0.0,
								(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST leave trailing cells 1..15 outputCb == received").c_str());
							Assert::AreEqual(curCr.at(cx, cy), resUndersized.outputCr.at(cx, cy), 0.0,
								(std::wstring(gc.label) + L": Opus LOW-5 undersized faceMask MUST leave trailing cells 1..15 outputCr == received").c_str());
						}
					}
				}
			}
		};
	}
}
