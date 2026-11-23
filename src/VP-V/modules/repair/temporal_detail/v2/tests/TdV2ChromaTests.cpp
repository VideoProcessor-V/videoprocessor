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
			}

			// =========================================================================
			// 5. Unidentified Coded Grid Runs Visible Coarse-Band-Only Mode (M/H == 0)
			// =========================================================================
			TEST_METHOD(UnidentifiedCodedGridRunsVisibleCoarseBandOnly)
			{
				// 160x160 with interior [56, 104) (48x48 >= 1 full 40-px banding cycle) so even SpatialBilinearWeight's
				// neighbor cells ([48, 112)) and their 3x3 pooling neighborhoods ([32, 128)) stay strictly beyond
				// the 3*sigma = 24 px G(8.0) border-replication radius!
				const int w = 160, h = 160;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect active = ActiveRect::Full(w, h);
				const ActiveRect interior{ 56, 56, 104, 104 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Construct signal with BOTH:
				//   (1) Low-frequency chroma banding (smooth ramp quantized to 14-code bands in current frame, clean in peaks)
				//   (2) High-frequency chroma detail (period-3 px sinusoid, strictly above sigma = 8 px coarse band,
				//       present ONLY in current frame and absent in peaks so any full-band blend would destroy it!)
				TdV2Plane masterLowCb(w, h, 512.0), masterLowCr(w, h, 512.0);
				TdV2Plane hfDetailCb(w, h, 0.0), hfDetailCr(w, h, 0.0);
				TdV2Plane curNoHfCb(w, h, 512.0), curNoHfCr(w, h, 512.0);
				TdV2Plane curCb(w, h, 512.0), curCr(w, h, 512.0);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						// Genuine low-frequency chroma banding: smooth sky-like gradient quantized to 16-code steps
						// (~40 px wide bands that genuinely live in the sigma = 8 px coarse band, baseCoarseErr > 1.5 codes).
						const double lowCb = 460.0 + 0.40 * x + 0.10 * y;
						const double lowCr = 560.0 - 0.35 * x + 0.10 * y;
						masterLowCb.at(x, y) = lowCb;
						masterLowCr.at(x, y) = lowCr;

						// Coarse 16-code banding error on low-frequency ramp.
						const double bandCb = 16.0 * std::floor(lowCb / 16.0 + 0.5);
						const double bandCr = 16.0 * std::floor(lowCr / 16.0 + 0.5);

						// High-frequency period-3 px chroma detail (attenuated by e^-140 in sigma=8 coarse band!).
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
				Assert::IsTrue(hfDeltaDiffCb <= 1.25 * idealHfLeakCb && hfDeltaDiffCb < 1e-5,
					L"Direct HF-detail invariance: planted period-3 px HF Cb must stay at exact truncated G(8) floor (< 1e-5)");
				Assert::IsTrue(hfDeltaDiffCr <= 1.25 * idealHfLeakCr && hfDeltaDiffCr < 1e-5,
					L"Direct HF-detail invariance: planted period-3 px HF Cr must stay at exact truncated G(8) floor (< 1e-5)");
				Assert::IsTrue(deltaFineRmsCb <= 1.15 * idealFineOverlapCb && deltaMidRmsCb <= 1.15 * idealMidOverlapCb,
					L"Cb coarse-only delta M/H must stay within 1.15x exact linear G(8) filter overlap");
				Assert::IsTrue(deltaFineRmsCr <= 1.15 * idealFineOverlapCr && deltaMidRmsCr <= 1.15 * idealMidOverlapCr,
					L"Cr coarse-only delta M/H must stay within 1.15x exact linear G(8) filter overlap");
				Assert::IsTrue(hfPreservErrCb <= 1.15 * idealFineOverlapCb,
					L"Unidentified grid coarse-only mode must leave high-frequency chroma untouched within exact G(8) overlap");
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

				const double grainSigma = 12.0;
				const double leakFrac = 0.015; // realistic imperfect separation (non-oracle!)
				const double leakedVar = (leakFrac * grainSigma) * (leakFrac * grainSigma);
				const std::vector<double> retainedVar(static_cast<size_t>(cw) * ch, leakedVar);

				// ---- Sub-case A: Delivered 4:2:0 (coded 4:2:0) ----
				{
					const TdV2Plane masterCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
					const TdV2Plane masterCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);

					const TdV2Plane curCleanTrueCb = AddGaussianNoise(masterCb, 4.0, 601, cActive);
					const TdV2Plane curCleanTrueCr = AddGaussianNoise(masterCr, 4.0, 602, cActive);
					const TdV2Plane prevCleanTrueCb = AddGaussianNoise(masterCb, 0.8, 603, cActive);
					const TdV2Plane prevCleanTrueCr = AddGaussianNoise(masterCr, 0.8, 604, cActive);
					const TdV2Plane nextCleanTrueCb = AddGaussianNoise(masterCb, 0.8, 605, cActive);
					const TdV2Plane nextCleanTrueCr = AddGaussianNoise(masterCr, 0.8, 606, cActive);

					std::mt19937 rngOwn(6101), rngForPrev(6102), rngForNext(6103);
					std::normal_distribution<double> gd(0.0, grainSigma);
					std::vector<double> ownCb(cN), ownCr(cN), forPrevCb(cN), forPrevCr(cN), forNextCb(cN), forNextCr(cN);
					TdV2Plane recCb(cW, cH), recCr(cW, cH), prevRecCb(cW, cH), prevRecCr(cW, cH), nextRecCb(cW, cH), nextRecCr(cW, cH);
					TdV2Plane curCleanImpCb(cW, cH), curCleanImpCr(cW, cH);
					TdV2Plane prevCleanImpCb(cW, cH), prevCleanImpCr(cW, cH), nextCleanImpCb(cW, cH), nextCleanImpCr(cW, cH);

					for (size_t i = 0; i < cN; ++i)
					{
						ownCb[i] = gd(rngOwn);
						ownCr[i] = gd(rngOwn);
						forPrevCb[i] = gd(rngForPrev);
						forPrevCr[i] = gd(rngForPrev);
						forNextCb[i] = gd(rngForNext);
						forNextCr[i] = gd(rngForNext);

						recCb.v[i] = curCleanTrueCb.v[i] + ownCb[i];
						recCr.v[i] = curCleanTrueCr.v[i] + ownCr[i];
						prevRecCb.v[i] = prevCleanTrueCb.v[i] + forPrevCb[i];
						prevRecCr.v[i] = prevCleanTrueCr.v[i] + forPrevCr[i];
						nextRecCb.v[i] = nextCleanTrueCb.v[i] + forNextCb[i];
						nextRecCr.v[i] = nextCleanTrueCr.v[i] + forNextCr[i];

						curCleanImpCb.v[i] = curCleanTrueCb.v[i] + leakFrac * ownCb[i];
						curCleanImpCr.v[i] = curCleanTrueCr.v[i] + leakFrac * ownCr[i];
						prevCleanImpCb.v[i] = prevCleanTrueCb.v[i] + leakFrac * forPrevCb[i];
						prevCleanImpCr.v[i] = prevCleanTrueCr.v[i] + leakFrac * forPrevCr[i];
						nextCleanImpCb.v[i] = nextCleanTrueCb.v[i] + leakFrac * forNextCb[i];
						nextCleanImpCr.v[i] = nextCleanTrueCr.v[i] + leakFrac * forNextCr[i];
					}

					// Mandatory In-Test Negative Control (Rule 3): naive un-separated chroma average on EXACT SAME input!
					std::vector<double> naiveAvgCb(cN), naiveAvgCr(cN);
					for (size_t i = 0; i < cN; ++i)
					{
						naiveAvgCb[i] = (recCb.v[i] + prevRecCb.v[i] + nextRecCb.v[i]) / 3.0;
						naiveAvgCr[i] = (recCr.v[i] + prevRecCr.v[i] + nextRecCr.v[i]) / 3.0;
					}
					const auto naiveRepCb = EvaluateGrainTransfer(masterCb.v.data(), ownCb.data(), forPrevCb.data(), naiveAvgCb.data(), cN);
					const auto naiveRepCr = EvaluateGrainTransfer(masterCr.v.data(), ownCr.data(), forPrevCr.data(), naiveAvgCr.data(), cN);
					Assert::IsTrue(naiveRepCb.valid && naiveRepCr.valid);
					Assert::IsFalse(naiveRepCb.passesAuthoritativeBar, L"Negative control: naive 4:2:0 average MUST fail grain bar");
					Assert::IsTrue(naiveRepCb.betaOwn < 0.90 && naiveRepCb.betaForeign > 0.10,
						L"Negative control: naive 4:2:0 average destroys own grain and leaks foreign grain");
					Assert::IsTrue(naiveRepCr.betaOwn < 0.90 && naiveRepCr.betaForeign > 0.10);

					// Run TdV2ChromaEngine on 4:2:0.
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in;
					in.seq = 50;
					in.deliveredFormat = DeliveredChromaFormat::Yuv420;
					in.codedSubsampling = ChromaSubsampling::Yuv420;
					in.gridStatus = ChromaGridStatus::Identified;
					in.siting = siting;
					in.lumaW = w;
					in.lumaH = h;
					in.activeRect = active;
					in.receivedCb = recCb;
					in.receivedCr = recCr;
					in.hasCleanSplit = true;
					in.cleanCb = curCleanImpCb;
					in.cleanCr = curCleanImpCr;
					in.retainedGrainVarCb = retainedVar;
					in.retainedGrainVarCr = retainedVar;
					in.prevPeak = { &prevCleanImpCb, &prevCleanImpCr, &stillMf, false };
					in.nextPeak = { &nextCleanImpCb, &nextCleanImpCr, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;

					const auto res = eng.ProcessFrame(in, &reg);

					// Prove non-trivial clean-layer repair actually occurred (not a vacuous zero-delta!).
					TdV2Plane repairedCleanCb(cW, cH);
					for (size_t i = 0; i < cN; ++i)
						repairedCleanCb.v[i] = curCleanImpCb.v[i] + res.codedDeltaCb.v[i];
					const double baseCleanErr = MaskedRmse(masterCb, curCleanImpCb, cActive);
					const double outCleanErr = MaskedRmse(masterCb, repairedCleanCb, cActive);
					Assert::IsTrue(outCleanErr < baseCleanErr * 0.70, L"4:2:0 chroma clean layer must genuinely improve");

					// Evaluate authoritative grain transfer strictly against independent masterCb/Cr!
					const auto repCb = EvaluateGrainTransfer(masterCb.v.data(), ownCb.data(), forPrevCb.data(), res.outputCb.v.data(), cN);
					const auto repCr = EvaluateGrainTransfer(masterCr.v.data(), ownCr.data(), forPrevCr.data(), res.outputCr.v.data(), cN);
					const auto repCbNext = EvaluateGrainTransfer(masterCb.v.data(), ownCb.data(), forNextCb.data(), res.outputCb.v.data(), cN);

					Assert::IsTrue(repCb.valid && repCb.passesAuthoritativeBar, L"4:2:0 Cb must pass authoritative grain bar");
					Assert::IsTrue(repCr.valid && repCr.passesAuthoritativeBar, L"4:2:0 Cr must pass authoritative grain bar");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(std::fabs(repCbNext.betaForeign) <= kGrainBarMaxBetaForeign);
				}

				// ---- Sub-case B: Delivered RGB 4:4:4 (coded 4:2:0) ----
				{
					const TdV2Plane masterCodedCb = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, true);
					const TdV2Plane masterCodedCr = SampleAnalyticChromaPlane(w, h, 2, 2, siting, 0.0, 0.0, 64.0, 64.0, 6.0, false);
					const TdV2Plane master444Cb = UpsampleChromaPlaneSited(masterCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane master444Cr = UpsampleChromaPlaneSited(masterCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane masterY(w, h, 480.0);

					const TdV2Plane curCleanCodedCb = AddGaussianNoise(masterCodedCb, 4.0, 621, cActive);
					const TdV2Plane curCleanCodedCr = AddGaussianNoise(masterCodedCr, 4.0, 622, cActive);
					const TdV2Plane prevCleanCodedCb = AddGaussianNoise(masterCodedCb, 0.8, 623, cActive);
					const TdV2Plane prevCleanCodedCr = AddGaussianNoise(masterCodedCr, 0.8, 624, cActive);
					const TdV2Plane nextCleanCodedCb = AddGaussianNoise(masterCodedCb, 0.8, 625, cActive);
					const TdV2Plane nextCleanCodedCr = AddGaussianNoise(masterCodedCr, 0.8, 626, cActive);

					const TdV2Plane curClean444Cb = UpsampleChromaPlaneSited(curCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane curClean444Cr = UpsampleChromaPlaneSited(curCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane prevClean444Cb = UpsampleChromaPlaneSited(prevCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane prevClean444Cr = UpsampleChromaPlaneSited(prevCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane nextClean444Cb = UpsampleChromaPlaneSited(nextCleanCodedCb, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);
					const TdV2Plane nextClean444Cr = UpsampleChromaPlaneSited(nextCleanCodedCr, w, h, ChromaSubsampling::Yuv420, DeliveredChromaFormat::Rgb444, siting, active);

					// Plant independent two-seed 4:4:4 chroma grain on the delivered RGB stream!
					std::mt19937 rngOwn(6301), rngForPrev(6302), rngForNext(6303);
					std::normal_distribution<double> gd(0.0, grainSigma);
					std::vector<double> ownCb(fN), ownCr(fN), forPrevCb(fN), forPrevCr(fN), forNextCb(fN), forNextCr(fN);
					TdV2Plane recCb444(w, h), recCr444(w, h), prevRecCb444(w, h), prevRecCr444(w, h), nextRecCb444(w, h), nextRecCr444(w, h);
					TdV2Plane curCleanImp444Cb(w, h), curCleanImp444Cr(w, h);
					TdV2Plane prevCleanImp444Cb(w, h), prevCleanImp444Cr(w, h), nextCleanImp444Cb(w, h), nextCleanImp444Cr(w, h);
					for (size_t i = 0; i < fN; ++i)
					{
						ownCb[i] = gd(rngOwn);
						ownCr[i] = gd(rngOwn);
						forPrevCb[i] = gd(rngForPrev);
						forPrevCr[i] = gd(rngForPrev);
						forNextCb[i] = gd(rngForNext);
						forNextCr[i] = gd(rngForNext);

						recCb444.v[i] = curClean444Cb.v[i] + ownCb[i];
						recCr444.v[i] = curClean444Cr.v[i] + ownCr[i];
						prevRecCb444.v[i] = prevClean444Cb.v[i] + forPrevCb[i];
						prevRecCr444.v[i] = prevClean444Cr.v[i] + forPrevCr[i];
						nextRecCb444.v[i] = nextClean444Cb.v[i] + forNextCb[i];
						nextRecCr444.v[i] = nextClean444Cr.v[i] + forNextCr[i];

						curCleanImp444Cb.v[i] = curClean444Cb.v[i] + leakFrac * ownCb[i];
						curCleanImp444Cr.v[i] = curClean444Cr.v[i] + leakFrac * ownCr[i];
						prevCleanImp444Cb.v[i] = prevClean444Cb.v[i] + leakFrac * forPrevCb[i];
						prevCleanImp444Cr.v[i] = prevClean444Cr.v[i] + leakFrac * forPrevCr[i];
						nextCleanImp444Cb.v[i] = nextClean444Cb.v[i] + leakFrac * forNextCb[i];
						nextCleanImp444Cr.v[i] = nextClean444Cr.v[i] + leakFrac * forNextCr[i];
					}

					TdV2Plane recR, recG, recB, prevR, prevG, prevB, nextR, nextG, nextB;
					YCbCrPlanesToRgb(mat, masterY, recCb444, recCr444, midCode, recR, recG, recB);
					YCbCrPlanesToRgb(mat, masterY, prevRecCb444, prevRecCr444, midCode, prevR, prevG, prevB);
					YCbCrPlanesToRgb(mat, masterY, nextRecCb444, nextRecCr444, midCode, nextR, nextG, nextB);

					// Mandatory In-Test Negative Control (Rule 3): naive un-separated RGB average on EXACT SAME input!
					TdV2Plane naiveR(w, h), naiveG(w, h), naiveB(w, h);
					for (size_t i = 0; i < fN; ++i)
					{
						naiveR.v[i] = (recR.v[i] + prevR.v[i] + nextR.v[i]) / 3.0;
						naiveG.v[i] = (recG.v[i] + prevG.v[i] + nextG.v[i]) / 3.0;
						naiveB.v[i] = (recB.v[i] + prevB.v[i] + nextB.v[i]) / 3.0;
					}
					TdV2Plane naiveY, naiveCb, naiveCr;
					RgbPlanesToYCbCr(mat, naiveR, naiveG, naiveB, midCode, naiveY, naiveCb, naiveCr);
					const auto naiveRepCb = EvaluateGrainTransfer(master444Cb.v.data(), ownCb.data(), forPrevCb.data(), naiveCb.v.data(), fN);
					const auto naiveRepCr = EvaluateGrainTransfer(master444Cr.v.data(), ownCr.data(), forPrevCr.data(), naiveCr.v.data(), fN);
					Assert::IsTrue(naiveRepCb.valid && naiveRepCr.valid);
					Assert::IsFalse(naiveRepCb.passesAuthoritativeBar, L"Negative control: naive RGB average MUST fail grain bar");
					Assert::IsTrue(naiveRepCb.betaOwn < 0.90 && naiveRepCb.betaForeign > 0.10);
					Assert::IsTrue(naiveRepCr.betaOwn < 0.90 && naiveRepCr.betaForeign > 0.10);

					// Run TdV2ChromaEngine on RGB 4:4:4 (coded 4:2:0).
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ChromaEngine eng(cfg);
					StandInRegistry reg;

					ChromaProcessInput in;
					in.seq = 51;
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
					in.cleanCb = curCleanImp444Cb;
					in.cleanCr = curCleanImp444Cr;
					in.retainedGrainVarCb = retainedVar;
					in.retainedGrainVarCr = retainedVar;
					in.prevPeak = { &prevCleanImp444Cb, &prevCleanImp444Cr, &stillMf, false };
					in.nextPeak = { &nextCleanImp444Cb, &nextCleanImp444Cr, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;

					const auto res = eng.ProcessFrame(in, &reg);
					TdV2Plane outY, outCb, outCr;
					RgbPlanesToYCbCr(mat, res.outputR, res.outputG, res.outputB, midCode, outY, outCb, outCr);

					// Verify non-trivial clean-layer repair on RGB delivered delta.
					TdV2Plane repCleanCb(w, h);
					for (size_t i = 0; i < fN; ++i)
						repCleanCb.v[i] = curClean444Cb.v[i] + res.deliveredDeltaCb.v[i];
					Assert::IsTrue(MaskedRmse(master444Cb, repCleanCb, active) < MaskedRmse(master444Cb, curClean444Cb, active) * 0.70,
						L"RGB chroma clean layer must genuinely improve");

					const auto repCb = EvaluateGrainTransfer(master444Cb.v.data(), ownCb.data(), forPrevCb.data(), outCb.v.data(), fN);
					const auto repCr = EvaluateGrainTransfer(master444Cr.v.data(), ownCr.data(), forPrevCr.data(), outCr.v.data(), fN);

					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"RGB Chroma Grain: betaOwn=%.4f/%.4f betaFor=%.4f/%.4f stdRatio=%.4f/%.4f | naive betaOwn=%.3f betaFor=%.3f",
						repCb.betaOwn, repCr.betaOwn, repCb.betaForeign, repCr.betaForeign,
						repCb.stdRatio, repCr.stdRatio, naiveRepCb.betaOwn, naiveRepCb.betaForeign);
					Logger::WriteMessage(buf);

					Assert::IsTrue(repCb.valid && repCb.passesAuthoritativeBar, L"RGB Cb must pass authoritative grain bar");
					Assert::IsTrue(repCr.valid && repCr.passesAuthoritativeBar, L"RGB Cr must pass authoritative grain bar");
					Assert::IsTrue(repCb.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCb.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCb.stdRatio - 1.0) <= kGrainBarMaxRmsError);
					Assert::IsTrue(repCr.betaOwn >= kGrainBarMinBetaOwn && std::fabs(repCr.betaForeign) <= kGrainBarMaxBetaForeign);
					Assert::IsTrue(std::fabs(repCr.stdRatio - 1.0) <= kGrainBarMaxRmsError);
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
				Assert::IsTrue(std::fabs(evalA.meanProjIr - trueGrainVar) < std::fabs(evalA.meanCrossIr - trueGrainVar) * 0.5,
					L"Projection form must cut 0.5-px pan texture bias decisively compared to raw cross form");

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
				std::vector<double> btbEnergies, btbIr, midEnergies, midIr, wtwEnergies, wtwIr;

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

					double blkBtbC[64], blkBtbP[64], blkMidC[64], blkMidP[64], blkWtwC[64], blkWtwP[64];
					for (int i = 0; i < 64; ++i)
					{
						blkBtbC[i] = 32.0 + gCur[i];  blkBtbP[i] = 32.0 + gPrev[i];
						blkMidC[i] = 512.0 + gCur[i]; blkMidP[i] = 512.0 + gPrev[i];
						blkWtwC[i] = 980.0 + gCur[i]; blkWtwP[i] = 980.0 + gPrev[i];
					}
					double eB = 0.0, irB = 0.0, eM = 0.0, irM = 0.0, eW = 0.0, irW = 0.0;
					measureBlock(blkBtbC, blkBtbP, eB, irB);
					measureBlock(blkMidC, blkMidP, eM, irM);
					measureBlock(blkWtwC, blkWtwP, eW, irW);

					btbEnergies.push_back(eB); btbIr.push_back(irB);
					midEnergies.push_back(eM); midIr.push_back(irM);
					wtwEnergies.push_back(eW); wtwIr.push_back(irW);

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

				// Prove strict code-value invariance: BTB (32) == Mid-Gray (512) == WTW (980)!
				StandInRegistry regBtb, regMid, regWtw;
				const auto rBtb = AccumulateDarkNlfExcludingZeroEnergy(btbEnergies, btbIr, regionPixels, lsbStep, qP, &regBtb);
				const auto rMid = AccumulateDarkNlfExcludingZeroEnergy(midEnergies, midIr, regionPixels, lsbStep, qP, &regMid);
				const auto rWtw = AccumulateDarkNlfExcludingZeroEnergy(wtwEnergies, wtwIr, regionPixels, lsbStep, qP, &regWtw);
				Assert::IsTrue(rBtb.valid && rMid.valid && rWtw.valid);
				Assert::AreEqual(static_cast<size_t>(15), rBtb.retainedBlocks);
				Assert::AreEqual(static_cast<size_t>(15), rWtw.retainedBlocks);
				Assert::IsFalse(regBtb.IsActive(StandInKind::ZeroEnergyDarkBlockExcluded),
					L"Valid grainy BTB blocks must NEVER be excluded");
				Assert::IsFalse(regWtw.IsActive(StandInKind::ZeroEnergyDarkBlockExcluded),
					L"Valid grainy WTW blocks must NEVER be excluded");
				Assert::AreEqual(rMid.nlfEstimate, rBtb.nlfEstimate, 1e-10,
					L"BTB grainy blocks must produce identical NLF to mid-gray blocks");
				Assert::AreEqual(rMid.nlfEstimate, rWtw.nlfEstimate, 1e-10,
					L"WTW grainy blocks must produce identical NLF to mid-gray blocks");
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
					const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
					const double maxStripBound = kPixelBoundSigma * (
						std::sqrt(st.totalVar * gains.low) +
						std::sqrt(st.totalVar * gains.mid) +
						std::sqrt(st.totalVar * gains.fine));
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
			}
		};
	}
}
