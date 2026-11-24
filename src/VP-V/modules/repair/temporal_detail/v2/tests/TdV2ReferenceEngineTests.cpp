// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S3 Adversarial Tests (2026-11-23_TD-consolidated-plan.md s3.2 & s3.3):
// Independent, non-oracle, negative-control-backed CPU reference engine proofs:
//   1. Off and Shadow modes leave output bit-exact to received input
//   2. Honest Dual-Source vs Peaks-Only comparison on identical valley2 (carry uses independent nextPeak partner)
//   3. Camera pan retains full gain while fast moving object preserves sharpness (with naive unprotected negative control)
//   4. Half-pixel (0.5 px) sub-pixel camera pan preserves clean recovery (Grain MUST-2 / s3.4)
//   5. Honest 4-Layer Grain Preservation (realistic imperfect separation + raw hasCleanSplit==false + Layer-4 CarryClean
//      purity under guardViolationCells > 0 + naive temporal-average negative control; scored strictly vs masterClean!)
//   6. W1 Banded Sky never self-vetoed + Rack-Focus veto on L/M/H even under LowBandCellOnly (with negative control)
//      + Matched-focus bokeh M/H delta == 0 under LowBandCellOnly
//   7. Letterbox border pan integrity: bars bit-exact AND 1-18 px active edge zero 0.0-hole dark ringing (with negative control)
//   8. Hard cell-boundary isolation (Overlay, local focusChange, DynamicTexture): 0.000 bleed within 1-8 px (with negative control)
//   9. Transform-identified 8x8 DCT on non-multiple-of-8 active frame: STRICT/CELL + bounded remainder strips + genuine 2-iter POCS
//  10. Adversarial OOB / div-by-zero / non-finite / silent-fallback traps + resolution-change carry reset
//  11. Scene cut and non-finite receivedY/cleanY reset cleanly with zero ghosting
//  12. Pass-1 faceMask exact zero-delta + unmasked subtle micro-motion enablePerCornerHuber (with Huber-off controls)
//  13. H9 missing-motion guard, H14 explicit codecModelPb, carry resets, invalid A2, M2 mf.valid==0 POCS skip, M3 Parseval bound. CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2ReferenceEngine.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdV2ReferenceEngineNs
	{
		using namespace TdV2;

		constexpr double kPi = 3.14159265358979323846;

		static double SyntheticTexture(double x, double y)
		{
			const double base = 420.0 + 90.0 * std::sin(2.0 * kPi * x / 83.0 + 0.4) * std::cos(2.0 * kPi * y / 71.0);
			const double mid = 22.0 * std::sin(2.0 * kPi * (x / 11.0 + y / 17.0) + 0.7)
			                 + 16.0 * std::cos(2.0 * kPi * (x / 13.0 - y / 15.0) + 1.3);
			const double fine = 14.0 * std::sin(2.0 * kPi * (x / 4.7 + y / 7.3) + 0.2)
			                  + 11.0 * std::cos(2.0 * kPi * (x / 5.9 - y / 5.1) + 2.1);
			return base + mid + fine;
		}

		static TdV2Plane RenderMaster(int w, int h, double ox = 0.0, double oy = 0.0)
		{
			TdV2Plane p(w, h, 0.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					p.at(x, y) = SyntheticTexture(static_cast<double>(x) + ox, static_cast<double>(y) + oy);
			return p;
		}

		static TdV2Plane AddCodingError(const TdV2Plane& master, double sigma, uint32_t seed, double fineAtten = 0.0)
		{
			TdV2Plane out = master;
			if (fineAtten > 0.0)
			{
				const ThreeBandPlanes b = DecomposeActiveBands(master, ActiveRect::Full(master.w, master.h));
				for (size_t i = 0; i < out.v.size(); ++i)
					out.v[i] = master.v[i] - fineAtten * b.H.v[i];
			}
			std::mt19937 rng(seed);
			std::normal_distribution<double> nd(0.0, sigma);
			for (size_t i = 0; i < out.v.size(); ++i)
				out.v[i] += nd(rng);
			return out;
		}

		static double PlaneMse(const TdV2Plane& a, const TdV2Plane& b, const ActiveRect& r)
		{
			double sum = 0.0;
			size_t n = 0;
			for (int y = r.y0; y < r.y1; ++y)
			{
				for (int x = r.x0; x < r.x1; ++x)
				{
					const double d = a.at(x, y) - b.at(x, y);
					sum += d * d;
					++n;
				}
			}
			return n > 0 ? (sum / static_cast<double>(n)) : 0.0;
		}

		static double PlaneMeanSignedDiff(const TdV2Plane& out, const TdV2Plane& ref, const ActiveRect& r)
		{
			double sum = 0.0;
			size_t n = 0;
			for (int y = r.y0; y < r.y1; ++y)
			{
				for (int x = r.x0; x < r.x1; ++x)
				{
					sum += out.at(x, y) - ref.at(x, y);
					++n;
				}
			}
			return n > 0 ? (sum / static_cast<double>(n)) : 0.0;
		}

		static void AssertAllFinite(const TdV2Plane& p, const wchar_t* msg)
		{
			for (double v : p.v)
				Assert::IsTrue(std::isfinite(v), msg);
		}

		// Simple linear regression slope Cov(signal - ref, basis) / Var(basis) for direct carry-purity checks.
		static double RegressSlope(const TdV2Plane& signal, const TdV2Plane& ref, const std::vector<double>& basis)
		{
			const size_t n = basis.size();
			double meanY = 0.0, meanX = 0.0;
			for (size_t i = 0; i < n; ++i)
			{
				meanY += signal.v[i] - ref.v[i];
				meanX += basis[i];
			}
			const double invN = 1.0 / static_cast<double>(n);
			meanY *= invN;
			meanX *= invN;
			double cov = 0.0, varX = 0.0;
			for (size_t i = 0; i < n; ++i)
			{
				const double dy = (signal.v[i] - ref.v[i]) - meanY;
				const double dx = basis[i] - meanX;
				cov += dy * dx;
				varX += dx * dx;
			}
			return varX > 1e-12 ? (cov / varX) : 0.0;
		}

		TEST_CLASS(TdV2ReferenceEngineTests)
		{
		public:
			TEST_METHOD(OffAndShadowModesLeaveOutputBitExact)
			{
				const int w = 64, h = 64;
				const TdV2Plane master = RenderMaster(w, h);
				const TdV2Plane peakP = AddCodingError(master, 1.5, 11);
				const TdV2Plane peakN = AddCodingError(master, 1.5, 12);
				const TdV2Plane valley = AddCodingError(master, 6.0, 13, 0.35);

				const int cw = w / 16, ch = h / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				FrameProcessInput in;
				in.seq = 1;
				in.receivedY = valley;
				in.hasCleanSplit = true;
				in.cleanY = valley;
				in.prevPeak = { &peakP, &stillMf, false };
				in.nextPeak = { &peakN, &stillMf, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 12.0;

				// Mode::Off -> bit-exact passthrough, zero stand-ins engaged.
				Config offCfg;
				Assert::IsTrue(offCfg.mode == Mode::Off);
				TdV2ReferenceEngine offEng(offCfg);
				StandInRegistry offReg;
				const auto offRes = offEng.ProcessFrame(in, &offReg);
				for (size_t i = 0; i < valley.v.size(); ++i)
					Assert::AreEqual(valley.v[i], offRes.outputY.v[i], 0.0, L"Mode::Off must be bit-exact");
				Assert::IsFalse(offReg.AnyActive(), L"Mode::Off must perform zero work");

				// Mode::Shadow -> computes maps & stand-ins, yet output pixels remain bit-exact!
				Config shCfg;
				shCfg.mode = Mode::Shadow;
				TdV2ReferenceEngine shEng(shCfg);
				StandInRegistry shReg;
				const auto shRes = shEng.ProcessFrame(in, &shReg);
				for (size_t i = 0; i < valley.v.size(); ++i)
					Assert::AreEqual(valley.v[i], shRes.outputY.v[i], 0.0, L"Mode::Shadow must not alter output pixels");
				Assert::AreEqual(static_cast<size_t>(2), shRes.activeRefCount);
				Assert::IsTrue(shReg.AnyActive());
			}

			TEST_METHOD(DualSourceCarryPlusPeaksHonestlyBeatsPeaksOnlyOnValley2)
			{
				// Honest A/B comparison on the EXACT SAME valley2 frame:
				//   - Peaks-Only (enableCarryForward = false): fuses prevPeak + nextPeak (K = 2)
				//   - Dual-Source (enableCarryForward = true): fuses prevPeak + nextPeak + carry from valley1 (K = 3)
				// Using moderate peak noise (sigma = 2.8) and independent Ref-B valley1 (sigma = 2.0) so carry genuinely
				// brings independent low-noise signal that strictly improves valley2 over peaks-only!
				const int w = 96, h = 96;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const TdV2Plane master = RenderMaster(w, h);
				const TdV2Plane peakPrev = AddCodingError(master, 2.8, 101);
				const TdV2Plane peakNext = AddCodingError(master, 2.8, 102);
				const TdV2Plane valley1 = AddCodingError(master, 2.0, 103);
				const TdV2Plane valley2 = AddCodingError(master, 7.0, 104, 0.40);

				const int cw = w / 16, ch = h / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Run A: Peaks-Only directly on valley2.
				Config cfgPeaks = MakePreset(Preset::Reference, Mode::On);
				cfgPeaks.estimator = VarianceEstimator::A3_ConservativeMin;
				cfgPeaks.residualMode = ResidualMode::Cell;
				cfgPeaks.enableCarryForward = false;
				StandInRegistry regPeaks;
				TdV2ReferenceEngine engPeaks(cfgPeaks);

				FrameProcessInput f2Base;
				f2Base.seq = 2;
				f2Base.receivedY = valley2;
				f2Base.hasCleanSplit = true;
				f2Base.cleanY = valley2;
				f2Base.prevPeak = { &peakPrev, &stillMf, false };
				f2Base.nextPeak = { &peakNext, &stillMf, false };
				f2Base.hasStepPrior = true;
				f2Base.estimatedStepDelta = 14.0;
				const auto rPeaksOnly = engPeaks.ProcessFrame(f2Base, &regPeaks);

				// Run B: Dual-Source (Frame 1 seeds carry from valley1, Frame 2 runs on the EXACT SAME valley2!).
				Config cfgDual = cfgPeaks;
				cfgDual.enableCarryForward = true;
				StandInRegistry regDual;
				TdV2ReferenceEngine engDual(cfgDual);

				FrameProcessInput f1;
				f1.seq = 1;
				f1.receivedY = valley1;
				f1.hasCleanSplit = true;
				f1.cleanY = valley1;
				f1.prevPeak = { &peakPrev, &stillMf, false };
				f1.nextPeak = { &peakNext, &stillMf, false };
				f1.hasStepPrior = true;
				f1.estimatedStepDelta = 14.0;
				const auto r1 = engDual.ProcessFrame(f1, &regDual);
				Assert::IsTrue(engDual.HasCarry());

				FrameProcessInput f2Dual = f2Base;
				f2Dual.hasCarryMotion = true;
				f2Dual.carryMotion = stillMf;
				const auto rDual = engDual.ProcessFrame(f2Dual, &regDual);

				double dbBase2 = 0.0, dbPeaks = 0.0, dbDual = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, valley2, interior), 1023.0, dbBase2));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, rPeaksOnly.outputY, interior), 1023.0, dbPeaks));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, rDual.outputY, interior), 1023.0, dbDual));

				const double gainPeaksOnly = dbPeaks - dbBase2;
				const double gainDual = dbDual - dbBase2;
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Dual-source vs Peaks-only on identical valley2: peaksOnly=+%.3f dB (%zu refs), dual=+%.3f dB (%zu refs, carryUsed=%d)",
					gainPeaksOnly, rPeaksOnly.activeRefCount, gainDual, rDual.activeRefCount, rDual.carryUsed ? 1 : 0);
				Logger::WriteMessage(buf);

				Assert::IsFalse(rPeaksOnly.carryUsed);
				Assert::AreEqual(static_cast<size_t>(2), rPeaksOnly.activeRefCount);
				Assert::IsTrue(rDual.carryUsed && rDual.activeRefCount == 3,
					L"Dual-source engine must fuse prevPeak + nextPeak + carry");
				Assert::IsTrue(gainPeaksOnly > 2.5, L"Peaks-only valley frame must achieve strong positive PSNR gain");
				Assert::IsTrue(gainDual > gainPeaksOnly,
					L"Dual-source (carry + peaks) MUST strictly beat peaks-only on the exact same valley2 input");
				Assert::IsTrue(regDual.IsActive(StandInKind::TransformUnknownBound), L"Transform-unknown bound must be visible");

				// Verify U19 td.maps populated with valid cells & kRealDetail reflects spatial fine detail.
				int validBlocks = 0;
				double meanRealDetail = 0.0;
				for (const auto& b : rDual.tdMaps)
				{
					if (TdMapsView::Fetch(b, TdMapsView::kValidity) > 0.5f)
						++validBlocks;
					meanRealDetail += TdMapsView::Fetch(b, TdMapsView::kRealDetail);
				}
				Assert::IsTrue(validBlocks > 0, L"U19 td.maps must report valid repaired cells");
				Assert::IsTrue(meanRealDetail > 0.0, L"U19 kRealDetail must report spatial fine detail");

				// Verify carry uses independent nextPeak partner (k == 2 -> partner == 1, NOT prevPeak k == 0)
				// AND kTrailConfidence1 includes k == 2 carry weight!
				// Seed Frame 1 with ONLY prevPeak (strong shared error with prevPeak), then fuse Frame 2 with
				// prevPeak (copied!) + independent nextPeak + carry under A1_ThreeCorneredHat.
				{
					const TdV2Plane noisyPrev = AddCodingError(master, 6.0, 111);
					const TdV2Plane cleanNext = AddCodingError(master, 1.0, 112);
					const TdV2Plane curCopyOfPrev = AddCodingError(noisyPrev, 1.0, 113); // shares noisyPrev's big error!

					Config cfgPartner = MakePreset(Preset::Reference, Mode::On);
					cfgPartner.estimator = VarianceEstimator::A1_ThreeCorneredHat;
					TdV2ReferenceEngine engP(cfgPartner);
					StandInRegistry regP;

					// Seed carry with clean master on frame 1 (representing a clean I-anchor carried forward).
					FrameProcessInput s1;
					s1.seq = 10;
					s1.receivedY = master;
					s1.hasCleanSplit = true;
					s1.cleanY = master;
					engP.ProcessFrame(s1, &regP);

					// Frame 2: prevPeak shares error with cur (copied), nextPeak is independent clean, carry is clean.
					// Because carry's A1 partner is nextPeak (k=1, independent!), carry gets strong weight!
					FrameProcessInput s2;
					s2.seq = 11;
					s2.receivedY = curCopyOfPrev;
					s2.hasCleanSplit = true;
					s2.cleanY = curCopyOfPrev;
					s2.prevPeak = { &noisyPrev, &stillMf, true }; // exact copy -> weight 0
					s2.nextPeak = { &cleanNext, &stillMf, false };
					s2.hasCarryMotion = true;
					s2.carryMotion = stillMf;
					s2.hasStepPrior = true;
					s2.estimatedStepDelta = 14.0;
					const auto rs2 = engP.ProcessFrame(s2, &regP);
					Assert::IsTrue(rs2.carryUsed && rs2.activeRefCount == 3);
					double dbB = 0.0, dbO = 0.0;
					Assert::IsTrue(ComputePsnrDb(PlaneMse(master, curCopyOfPrev, interior), 1023.0, dbB));
					Assert::IsTrue(ComputePsnrDb(PlaneMse(master, rs2.outputY, interior), 1023.0, dbO));
					Assert::IsTrue(dbO - dbB > 4.0, L"Carry paired with independent nextPeak must recover strongly");

					// Verify kTrailConfidence1 sums carry (k == 2) even when BOTH prevPeak & nextPeak are exact copies!
					Config cfgCarryOnly = MakePreset(Preset::Reference, Mode::On);
					cfgCarryOnly.estimator = VarianceEstimator::A2_RoundingCells;
					TdV2ReferenceEngine engCO(cfgCarryOnly);
					engCO.ProcessFrame(s1, &regP);
					FrameProcessInput s3 = s2;
					s3.nextPeak.isExactCopyOfCurrent = true; // both peaks exact copy -> ONLY carry k=2 has weight!
					const auto rs3 = engCO.ProcessFrame(s3, &regP);
					double maxMidConf = 0.0;
					for (const auto& b : rs3.tdMaps)
						maxMidConf = (std::max)(maxMidConf, static_cast<double>(TdMapsView::Fetch(b, TdMapsView::kTrailConfidence1)));
					Assert::IsTrue(maxMidConf > 0.2,
						L"U19 kTrailConfidence1 MUST include k=2 carry weight when peaks are exact copies");
				}
			}

			TEST_METHOD(CameraPanRetainsFullGainWhileFastMovingObjectPreservesSharpness)
			{
				// 128x96 scene panning horizontally at camVx = +2.0 px/frame;
				// Right half contains a fast-moving foreground object (v_obj = 6.0 px/frame relative to camera)
				// whose texture in the references is displaced/mismatched -> blending it would smear it!
				const int w = 128, h = 96;
				const int cellPx = 16;
				const int cw = w / cellPx, ch = h / cellPx;

				const TdV2Plane masterCur = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane masterPrevBg = RenderMaster(w, h, -2.0, 0.0);
				const TdV2Plane masterNextBg = RenderMaster(w, h, +2.0, 0.0);

				TdV2Plane peakPrev = AddCodingError(masterPrevBg, 1.2, 201);
				TdV2Plane peakNext = AddCodingError(masterNextBg, 1.2, 202);
				TdV2Plane curReceived = AddCodingError(masterCur, 6.5, 203, 0.35);

				// Plant a sharp moving foreground object in x in [80, 112), y in [24, 72) in current frame,
				// shifted/unmatched in prev/next references so improper fusion would heavily blur it!
				TdV2Plane masterWithObj = masterCur;
				for (int y = 24; y < 72; ++y)
				{
					for (int x = 80; x < 112; ++x)
					{
						const double objVal = 520.0 + 45.0 * std::sin(2.0 * kPi * x / 5.0) * std::cos(2.0 * kPi * y / 5.0);
						masterWithObj.at(x, y) = objVal;
						curReceived.at(x, y) = objVal;
						peakPrev.at(x, y) = 420.0;
						peakNext.at(x, y) = 420.0;
					}
				}

				// Mandatory In-Test Negative Control (Rule 3):
				// Unprotected naive temporal blend on the EXACT SAME input data smears the moving object!
				TdV2Plane naiveOut(w, h, 0.0);
				for (size_t i = 0; i < curReceived.v.size(); ++i)
					naiveOut.v[i] = (curReceived.v[i] + peakPrev.v[i] + peakNext.v[i]) / 3.0;

				// Build motion fields: camera pan (+2, 0) on background, fast object residual (+6, 0) on object cells.
				CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +2.0, 0.0, +2.0, 0.0, RegionMotionClass::Camera);
				CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -2.0, 0.0, -2.0, 0.0, RegionMotionClass::Camera);
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 5; cx < 7; ++cx)
					{
						const size_t ci = static_cast<size_t>(cy) * cw + cx;
						mfPrev.vx[ci] = +8.0; // camera +2 + object +6
						mfPrev.cls[ci] = RegionMotionClass::Object;
						mfNext.vx[ci] = -8.0; // camera -2 + object -6
						mfNext.cls[ci] = RegionMotionClass::Object;
					}
				}

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 10;
				in.receivedY = curReceived;
				in.hasCleanSplit = true;
				in.cleanY = curReceived;
				in.prevPeak = { &peakPrev, &mfPrev, false };
				in.nextPeak = { &peakNext, &mfNext, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 14.0;
				const auto res = eng.ProcessFrame(in, &reg);

				// Evaluate via TD-S0 SpeedBinTable on interior (avoiding 4-px pan border).
				const int ew = w - 16, eh = h - 16;
				const size_t en = static_cast<size_t>(ew) * eh;
				std::vector<double> mVec(en), rVec(en), oVec(en), naiveVec(en), objSpd(en), camSpd(en, 2.0);
				std::vector<uint8_t> isCam(en, 1u);
				for (int y = 0; y < eh; ++y)
				{
					for (int x = 0; x < ew; ++x)
					{
						const int gx = x + 8, gy = y + 8;
						const size_t i = static_cast<size_t>(y) * ew + x;
						mVec[i] = masterWithObj.at(gx, gy);
						rVec[i] = curReceived.at(gx, gy);
						oVec[i] = res.outputY.at(gx, gy);
						naiveVec[i] = naiveOut.at(gx, gy);
						const bool inObj = (gx >= 80 && gx < 112 && gy >= 24 && gy < 72);
						if (inObj)
						{
							isCam[i] = 0u;
							objSpd[i] = 6.0;
						}
						else if (gx < 64)
						{
							isCam[i] = 1u;
							objSpd[i] = 0.0;
						}
						else
						{
							isCam[i] = 0u;
							objSpd[i] = 6.0;
						}
					}
				}

				const SpeedBinReport sbNaive = EvaluateSpeedBins(
					mVec.data(), rVec.data(), naiveVec.data(),
					objSpd.data(), camSpd.data(), isCam.data(), ew, eh, 1023.0);
				Assert::IsTrue(sbNaive.valid);
				Assert::IsTrue(sbNaive.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)].sharpnessRetention < 0.65,
					L"Negative control: unprotected temporal blending MUST smear the fast moving object");

				const SpeedBinReport sb = EvaluateSpeedBins(
					mVec.data(), rVec.data(), oVec.data(),
					objSpd.data(), camSpd.data(), isCam.data(), ew, eh, 1023.0);

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Pan vs Fast Object: panGain=+%.3f dB (ret=%.4f), fastObj ret=%.4f (naive ret=%.4f)",
					sb.cameraPan.deltaDb, sb.cameraPan.sharpnessRetention,
					sb.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)].sharpnessRetention,
					sbNaive.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)].sharpnessRetention);
				Logger::WriteMessage(buf);

				Assert::IsTrue(sb.valid);
				Assert::IsTrue(sb.cameraPan.valid);
				Assert::IsTrue(sb.cameraPan.deltaDb > 2.5, L"Camera pan MUST NOT be gated off and must gain PSNR");
				Assert::IsTrue(sb.cameraPan.sharpnessRetention > 0.90, L"Camera pan must retain high sharpness");
				const auto& fastRow = sb.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)];
				Assert::IsTrue(fastRow.valid);
				Assert::IsTrue(fastRow.sharpnessRetention > 0.98, L"Fast moving object must preserve sharpness without smearing");
			}

			TEST_METHOD(HalfPixelPanPreservesCleanRecoveryWithoutArtifacts)
			{
				// Plan s1.6, s3.2 TD-S3(4) & s3.4 (Grain MUST-2): 0.5-px sub-pixel pan invariant
				// WITH realistic 15%-leaked independent two-seed grain ACTUALLY fed into eng.ProcessFrame(in1/in2)
				// (cleanY, prevPeak, nextPeak) + retainedGrainVar, asserting genuine clean recovery (> 2.0 dB) AND
				// full authoritative grain preservation evaluated against the ACTUAL 0.5-px warped foreign grain
				// fields (betaOwn >= 0.98, |betaForeign| <= 0.02 vs BOTH warpedForeignPrev and warpedForeignNext,
				// RMS +/-2%, lag1Evaluated == true, lag1Rise <= +0.02), backed by BOTH:
				//   - Mandatory Negative Control 1: unprotected naive 0.5-px pan temporal blend (regressed vs warpedForeignPrev)
				//   - Mandatory Negative Control 2: same-input Layer-2-disabled engine (`retainedGrainVar` cleared)!
				const int w = 96, h = 96;
				const size_t n = static_cast<size_t>(w) * h;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const int cw = w / 16, ch = h / 16;

				const TdV2Plane masterF1 = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane masterPrev1 = RenderMaster(w, h, -0.5, 0.0);
				const TdV2Plane masterNext1 = RenderMaster(w, h, +0.5, 0.0);
				// Continuous 0.5-px/frame camera pan: Frame 2 advances by +0.5 px relative to Frame 1
				// so Frame 1 and Frame 2 have distinct sub-pixel phases (and carry warps by +0.5 px!).
				const TdV2Plane masterF2 = RenderMaster(w, h, +0.5, 0.0);
				const TdV2Plane masterPrev2 = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane masterNext2 = RenderMaster(w, h, +1.0, 0.0);

				// Construct realistic L+M band coding wear on valley1 and valley2 (~12.0 raw sigma, matching Test 5)
				// so clean recovery lives in L/M while 15% leaked film grain lives in H!
				auto addLowMidCodingWear = [&](const TdV2Plane& m, double rawSigma, uint32_t seed) -> TdV2Plane
				{
					const TdV2Plane whiteNoise = AddCodingError(TdV2Plane(w, h, 0.0), rawSigma, seed);
					const ThreeBandPlanes nb = DecomposeActiveBands(whiteNoise, ActiveRect::Full(w, h));
					TdV2Plane out = m;
					for (size_t i = 0; i < n; ++i)
						out.v[i] = m.v[i] + nb.L.v[i] + nb.M.v[i];
					return out;
				};

				const TdV2Plane peakPrev1Clean = AddCodingError(masterPrev1, 0.8, 301);
				const TdV2Plane peakNext1Clean = AddCodingError(masterNext1, 0.8, 302);
				const TdV2Plane valley1Clean = addLowMidCodingWear(masterF1, 12.0, 304);

				const TdV2Plane peakPrev2Clean = AddCodingError(masterPrev2, 0.8, 311);
				const TdV2Plane peakNext2Clean = AddCodingError(masterNext2, 0.8, 312);
				const TdV2Plane valley2Clean = addLowMidCodingWear(masterF2, 12.0, 303);

				// Plant independent film grain across Frame 1, Frame 2, and peak references, with realistic
				// 15% upstream separation leakage ACTUALLY present in cleanY, prevPeak, and nextPeak!
				std::mt19937 rng(305u);
				const double grainSigma = 14.0;
				const double leak = 0.15;
				std::normal_distribution<double> gDist(0.0, grainSigma);
				std::vector<double> prevOwnGrain(n), ownGrain(n);
				std::vector<double> foreignGrainPrev1(n), foreignGrainNext1(n);
				std::vector<double> foreignGrainPrev(n), foreignGrainNext(n);
				TdV2Plane valley1Recv(w, h), valley2Recv(w, h);
				TdV2Plane peakPrev1Recv(w, h), peakNext1Recv(w, h), peakPrev2Recv(w, h), peakNext2Recv(w, h);
				TdV2Plane valley1CleanImp(w, h), valley2CleanImp(w, h);
				TdV2Plane peakPrev1CleanImp(w, h), peakNext1CleanImp(w, h);
				TdV2Plane peakPrev2CleanImp(w, h), peakNext2CleanImp(w, h);
				for (size_t i = 0; i < n; ++i)
				{
					prevOwnGrain[i] = gDist(rng);
					ownGrain[i] = gDist(rng);
					foreignGrainPrev1[i] = gDist(rng);
					foreignGrainNext1[i] = gDist(rng);
					foreignGrainPrev[i] = gDist(rng);
					foreignGrainNext[i] = gDist(rng);

					valley1Recv.v[i] = valley1Clean.v[i] + prevOwnGrain[i];
					valley2Recv.v[i] = valley2Clean.v[i] + ownGrain[i];
					peakPrev1Recv.v[i] = peakPrev1Clean.v[i] + foreignGrainPrev1[i];
					peakNext1Recv.v[i] = peakNext1Clean.v[i] + foreignGrainNext1[i];
					peakPrev2Recv.v[i] = peakPrev2Clean.v[i] + foreignGrainPrev[i];
					peakNext2Recv.v[i] = peakNext2Clean.v[i] + foreignGrainNext[i];

					valley1CleanImp.v[i] = valley1Clean.v[i] + leak * prevOwnGrain[i];
					valley2CleanImp.v[i] = valley2Clean.v[i] + leak * ownGrain[i];
					peakPrev1CleanImp.v[i] = peakPrev1Clean.v[i] + leak * foreignGrainPrev1[i];
					peakNext1CleanImp.v[i] = peakNext1Clean.v[i] + leak * foreignGrainNext1[i];
					peakPrev2CleanImp.v[i] = peakPrev2Clean.v[i] + leak * foreignGrainPrev[i];
					peakNext2CleanImp.v[i] = peakNext2Clean.v[i] + leak * foreignGrainNext[i];
				}
				const double leakedVar = (leak * grainSigma) * (leak * grainSigma);
				const std::vector<double> retainedVar(static_cast<size_t>(cw) * ch, leakedVar);

				CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +0.5, 0.0, +0.5, 0.0, RegionMotionClass::Camera);
				CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -0.5, 0.0, -0.5, 0.0, RegionMotionClass::Camera);
				// Frame 2 (ox=+0.5) -> Frame 1 carry (ox=0.0): sample Frame 1 at x + 0.5.
				CellMotionField mfCarry = CellMotionField::Uniform(cw, ch, +0.5, 0.0, +0.5, 0.0, RegionMotionClass::Camera);

				// Warp foreignGrainPrev (+0.5, 0.0) and foreignGrainNext (-0.5, 0.0) via WarpCleanReference so
				// grain regression evaluates foreign grain transfer against the ACTUAL 0.5-px warped foreign grain!
				TdV2Plane foreignGrainPrevPlane(w, h, 0.0), foreignGrainNextPlane(w, h, 0.0), zeroPlane(w, h, 0.0);
				foreignGrainPrevPlane.v = foreignGrainPrev;
				foreignGrainNextPlane.v = foreignGrainNext;
				TdV2Plane warpedForeignPrev, warpedForeignNext;
				std::vector<uint8_t> wfpValid, wfnValid;
				WarpCleanReference(foreignGrainPrevPlane, zeroPlane, ActiveRect::Full(w, h), mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedForeignPrev, wfpValid);
				WarpCleanReference(foreignGrainNextPlane, zeroPlane, ActiveRect::Full(w, h), mfNext.vx, mfNext.vy, cw, ch, 16, warpedForeignNext, wfnValid);

				// Mandatory In-Test Negative Control 1 (Rule 3):
				// Unprotected temporal blend of the 0.5-px panned grainy frames destroys own grain & leaks warped foreign grain!
				TdV2Plane warpedPrev1Recv, warpedNext1Recv, warpedPrev2Recv, warpedNext2Recv;
				std::vector<uint8_t> vp1Valid, vn1Valid, vp2Valid, vn2Valid;
				WarpCleanReference(peakPrev1Recv, valley1Recv, ActiveRect::Full(w, h), mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedPrev1Recv, vp1Valid);
				WarpCleanReference(peakNext1Recv, valley1Recv, ActiveRect::Full(w, h), mfNext.vx, mfNext.vy, cw, ch, 16, warpedNext1Recv, vn1Valid);
				WarpCleanReference(peakPrev2Recv, valley2Recv, ActiveRect::Full(w, h), mfPrev.vx, mfPrev.vy, cw, ch, 16, warpedPrev2Recv, vp2Valid);
				WarpCleanReference(peakNext2Recv, valley2Recv, ActiveRect::Full(w, h), mfNext.vx, mfNext.vy, cw, ch, 16, warpedNext2Recv, vn2Valid);

				std::vector<double> naiveOut1(n), naiveOut2(n), naivePrevRes(n);
				for (size_t i = 0; i < n; ++i)
				{
					naiveOut1[i] = (valley1Recv.v[i] + warpedPrev1Recv.v[i] + warpedNext1Recv.v[i]) / 3.0;
					naiveOut2[i] = (valley2Recv.v[i] + warpedPrev2Recv.v[i] + warpedNext2Recv.v[i]) / 3.0;
					naivePrevRes[i] = naiveOut1[i] - masterF1.v[i];
				}
				const auto naiveRep = EvaluateGrainTransfer(
					masterF2.v.data(), ownGrain.data(), warpedForeignPrev.v.data(), naiveOut2.data(), n,
					naivePrevRes.data(), prevOwnGrain.data());
				Assert::IsTrue(naiveRep.valid && naiveRep.lag1Evaluated);
				Assert::IsFalse(naiveRep.passesAuthoritativeBar,
					L"Negative control: unprotected 0.5-px pan blend MUST fail authoritative grain bar");
				Assert::IsTrue(naiveRep.betaOwn < 0.75,
					L"Negative control: unprotected 0.5-px pan blend destroys own grain (betaOwn < 0.75)");
				Assert::IsTrue(naiveRep.betaForeign > 0.25,
					L"Negative control: unprotected 0.5-px pan blend leaks warped foreign grain (betaForeign > 0.25)");

				FrameProcessInput in1;
				in1.seq = 19;
				in1.receivedY = valley1Recv;
				in1.hasCleanSplit = true;
				in1.cleanY = valley1CleanImp;
				in1.retainedGrainVar = retainedVar;
				in1.prevPeak = { &peakPrev1CleanImp, &mfPrev, false };
				in1.nextPeak = { &peakNext1CleanImp, &mfNext, false };
				in1.hasStepPrior = true;
				in1.estimatedStepDelta = 12.0;

				FrameProcessInput in2;
				in2.seq = 20;
				in2.receivedY = valley2Recv;
				in2.hasCleanSplit = true;
				in2.cleanY = valley2CleanImp;
				in2.retainedGrainVar = retainedVar;
				in2.prevPeak = { &peakPrev2CleanImp, &mfPrev, false };
				in2.nextPeak = { &peakNext2CleanImp, &mfNext, false };
				in2.hasCarryMotion = true;
				in2.carryMotion = mfCarry;
				in2.hasStepPrior = true;
				in2.estimatedStepDelta = 12.0;

				// Mandatory In-Test Negative Control 2 (Rule 3):
				// Run the EXACT SAME 15%-leaked 0.5-px pan inputs with Layer-2 disabled (`retainedGrainVar` cleared)!
				{
					Config cfgNoL2 = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine engNoL2(cfgNoL2);
					StandInRegistry regNoL2;

					FrameProcessInput in1NoL2 = in1;
					in1NoL2.retainedGrainVar.clear();
					FrameProcessInput in2NoL2 = in2;
					in2NoL2.retainedGrainVar.clear();

					const auto res1NoL2 = engNoL2.ProcessFrame(in1NoL2, &regNoL2);
					const auto res2NoL2 = engNoL2.ProcessFrame(in2NoL2, &regNoL2);

					std::vector<double> prevResNoL2(n);
					for (size_t i = 0; i < n; ++i)
						prevResNoL2[i] = res1NoL2.outputY.v[i] - masterF1.v[i];

					const auto noL2Rep = EvaluateGrainTransfer(
						masterF2.v.data(), ownGrain.data(), warpedForeignPrev.v.data(), res2NoL2.outputY.v.data(), n,
						prevResNoL2.data(), prevOwnGrain.data());
					Assert::IsTrue(noL2Rep.valid && noL2Rep.lag1Evaluated);
					Assert::IsFalse(noL2Rep.passesAuthoritativeBar,
						L"Negative control: Layer-2-disabled engine on 15%-leaked 0.5-px pan inputs MUST fail authoritative grain bar");
					Assert::IsTrue(std::fabs(noL2Rep.betaForeign) > kGrainBarMaxBetaForeign || noL2Rep.betaOwn < kGrainBarMinBetaOwn,
						L"Negative control: Layer-2-disabled 0.5-px pan must exhibit real own-grain loss or warped foreign-grain leak");
				}

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				const auto res1 = eng.ProcessFrame(in1, &reg);
				const auto res2 = eng.ProcessFrame(in2, &reg);

				// Genuine clean recovery measured on true clean layer `valley2Clean + (res2.outputClean - valley2CleanImp)` vs `masterF2`!
				TdV2Plane repairedCleanTrue(w, h, 0.0);
				for (size_t i = 0; i < n; ++i)
					repairedCleanTrue.v[i] = valley2Clean.v[i] + (res2.outputClean.v[i] - valley2CleanImp.v[i]);

				double dbBase = 0.0, dbOut = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterF2, valley2Clean, interior), 1023.0, dbBase));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterF2, repairedCleanTrue, interior), 1023.0, dbOut));
				const double gain = dbOut - dbBase;

				std::vector<double> prevRes(n);
				for (size_t i = 0; i < n; ++i)
					prevRes[i] = res1.outputY.v[i] - masterF1.v[i];

				const auto gRep = EvaluateGrainTransfer(
					masterF2.v.data(), ownGrain.data(), warpedForeignPrev.v.data(), res2.outputY.v.data(), n,
					prevRes.data(), prevOwnGrain.data());
				const auto gRepNext = EvaluateGrainTransfer(
					masterF2.v.data(), ownGrain.data(), warpedForeignNext.v.data(), res2.outputY.v.data(), n,
					prevRes.data(), prevOwnGrain.data());

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"0.5-px sub-pixel pan (15%% leak): cleanGain=+%.3f dB | betaOwn=%.4f betaForP=%.4f betaForN=%.4f stdRatio=%.4f lag1Rise=%.4f",
					gain, gRep.betaOwn, gRep.betaForeign, gRepNext.betaForeign, gRep.stdRatio, gRep.lag1Rise);
				Logger::WriteMessage(buf);

				Assert::IsTrue(gain > 2.0, L"0.5-px sub-pixel pan must recover clean layer with strong positive dB gain");
				Assert::IsTrue(gRep.valid && gRep.lag1Evaluated && gRep.passesAuthoritativeBar,
					L"0.5-px sub-pixel pan with 15%-leaked grain MUST pass full authoritative grain bar (lag1Evaluated==true)");
				Assert::IsTrue(gRep.betaOwn >= kGrainBarMinBetaOwn);
				Assert::IsTrue(std::fabs(gRep.betaForeign) <= kGrainBarMaxBetaForeign);
				Assert::IsTrue(std::fabs(gRep.stdRatio - 1.0) <= kGrainBarMaxRmsError);
				Assert::IsTrue(gRep.lag1Rise <= kGrainBarMaxLag1Rise);
				Assert::IsTrue(gRepNext.valid && gRepNext.lag1Evaluated && std::fabs(gRepNext.betaForeign) <= kGrainBarMaxBetaForeign,
					L"0.5-px sub-pixel pan foreign grain leakage from warped nextPeak MUST stay <= 0.02");
			}

			TEST_METHOD(HonestFourLayerGrainPreservationAndLayer4CarryPurity)
			{
				// NON-TAUTOLOGICAL, NON-ORACLE 4-LAYER GRAIN PRESERVATION PROOF (Rule 3 + refs/111 s7.3):
				//   - Independent masterClean, independent ownGrain, independent foreignGrainPrev & foreignGrainNext
				//     ACTUALLY added to peakPrev and peakNext!
				//   - Mandatory Negative Control: naive temporal average on the EXACT SAME contaminated inputs
				//     MUST VIOLATE grain preservation (betaOwn < 0.75, betaForeign > 0.25).
				//   - Sub-case A: Realistic imperfect grain separation (residual ownGrain leaked in curClean AND
				//     residual foreignGrain leaked in peakPrevClean/peakNextClean), scored STRICTLY vs pre-generated masterClean!
				//   - Sub-case B: Raw noisy input path (hasCleanSplit == false) with visible P10g stand-in!
				//   - Sub-case C: Layer-4 TdGuardPort Carry-Purity test (guardViolationCells > 0) directly inspecting
				//     engine.CarryClean() AND verifying Frame t+1 stays clean of Frame t's grain!
				const int w = 96, h = 96;
				const size_t n = static_cast<size_t>(w) * h;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				const TdV2Plane masterClean = RenderMaster(w, h);
				const TdV2Plane peakPrevCleanTrue = AddCodingError(masterClean, 0.8, 601);
				const TdV2Plane peakNextCleanTrue = AddCodingError(masterClean, 0.8, 602);
				// Construct genuine L+M band compression wear on valley frames (blurred quantization/blocking noise
				// in L+M so compression error lives in L/M while leaked film grain lives in H!).
				auto addLowMidCodingWear = [&](const TdV2Plane& m, double rawSigma, uint32_t seed) -> TdV2Plane
				{
					const TdV2Plane whiteNoise = AddCodingError(TdV2Plane(w, h, 0.0), rawSigma, seed);
					const ThreeBandPlanes nb = DecomposeActiveBands(whiteNoise, ActiveRect::Full(w, h));
					TdV2Plane out = m;
					for (size_t i = 0; i < n; ++i)
						out.v[i] = m.v[i] + nb.L.v[i] + nb.M.v[i];
					return out;
				};
				const TdV2Plane prevCurCleanTrue = addLowMidCodingWear(masterClean, 12.0, 604);
				const TdV2Plane curCleanTrue = addLowMidCodingWear(masterClean, 12.0, 603);

				std::mt19937 rng(777);
				const double grainSigma = 14.0;
				std::normal_distribution<double> grainDist(0.0, grainSigma);
				std::vector<double> prevOwnGrain(n), ownGrain(n), foreignGrainPrev(n), foreignGrainNext(n);
				TdV2Plane prevReceived(w, h), curReceived(w, h), peakPrevReceived(w, h), peakNextReceived(w, h);
				for (size_t i = 0; i < n; ++i)
				{
					prevOwnGrain[i] = grainDist(rng);
					ownGrain[i] = grainDist(rng);
					foreignGrainPrev[i] = grainDist(rng);
					foreignGrainNext[i] = grainDist(rng);
					prevReceived.v[i] = prevCurCleanTrue.v[i] + prevOwnGrain[i];
					curReceived.v[i] = curCleanTrue.v[i] + ownGrain[i];
					peakPrevReceived.v[i] = peakPrevCleanTrue.v[i] + foreignGrainPrev[i];
					peakNextReceived.v[i] = peakNextCleanTrue.v[i] + foreignGrainNext[i];
				}

				// ---- Mandatory In-Test Negative Control 1 (Rule 3): Layers 1/2/4 disabled ----
				// Naive temporal average of the exact same contaminated frames (curReceived, peakPrevReceived, peakNextReceived).
				std::vector<double> naiveAvgPrev(n), naiveAvg(n), naivePrevRes(n);
				for (size_t i = 0; i < n; ++i)
				{
					naiveAvgPrev[i] = (prevReceived.v[i] + peakPrevReceived.v[i] + peakNextReceived.v[i]) / 3.0;
					naiveAvg[i] = (curReceived.v[i] + peakPrevReceived.v[i] + peakNextReceived.v[i]) / 3.0;
					naivePrevRes[i] = naiveAvgPrev[i] - masterClean.v[i];
				}
				const auto naiveRep = EvaluateGrainTransfer(
					masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), naiveAvg.data(), n,
					naivePrevRes.data(), prevOwnGrain.data());
				Assert::IsTrue(naiveRep.valid && naiveRep.lag1Evaluated);
				Assert::IsFalse(naiveRep.passesAuthoritativeBar,
					L"Negative control: naive temporal average MUST fail authoritative grain preservation bar");
				Assert::IsTrue(naiveRep.betaOwn < 0.75,
					L"Negative control: naive temporal average destroys own grain (betaOwn < 0.75)");
				Assert::IsTrue(naiveRep.betaForeign > 0.25,
					L"Negative control: naive temporal average leaks foreign grain (betaForeign > 0.25)");

				// ---- Sub-case A (H12): Realistic >= 10% Imperfect Grain Separation (leak = 15%!) ----
				// Upstream separator leaves 15% residual ownGrain inside curClean AND 15% residual foreignGrain inside peak cleans!
				// Any naive convex blend on band H leaks ~0.04+ foreign grain (> 0.02) and drops betaOwn < 0.94 (< 0.98);
				// ONLY genuine Layer-2 A1 grain subtraction (retainedGrainVar) suppresses band-H grain blending while repairing L/M!
				const double leakCur = 0.15;
				const double leakRef = 0.15;
				TdV2Plane prevCurCleanImp(w, h), curCleanImp(w, h), peakPrevCleanImp(w, h), peakNextCleanImp(w, h);
				for (size_t i = 0; i < n; ++i)
				{
					prevCurCleanImp.v[i] = prevCurCleanTrue.v[i] + leakCur * prevOwnGrain[i];
					curCleanImp.v[i] = curCleanTrue.v[i] + leakCur * ownGrain[i];
					peakPrevCleanImp.v[i] = peakPrevCleanTrue.v[i] + leakRef * foreignGrainPrev[i];
					peakNextCleanImp.v[i] = peakNextCleanTrue.v[i] + leakRef * foreignGrainNext[i];
				}
				const double leakedVar = (leakCur * grainSigma) * (leakCur * grainSigma);
				std::vector<double> retainedVar(static_cast<size_t>(cw) * ch, leakedVar);

				// ---- Mandatory In-Test Negative Control 2 (Rule 3 / H12) ----
				// Run the EXACT SAME clean inputs (`curCleanImp`, `peakPrevCleanImp`, `peakNextCleanImp`) through the
				// engine with Layer-2 grain subtraction disabled (`retainedGrainVar` empty) — MUST FAIL authoritative bar!
				{
					Config cfgNoL2 = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine engNoL2(cfgNoL2);
					StandInRegistry regNoL2;

					FrameProcessInput f1NoL2;
					f1NoL2.seq = 40;
					f1NoL2.receivedY = prevReceived;
					f1NoL2.hasCleanSplit = true;
					f1NoL2.cleanY = prevCurCleanImp;
					// Layer 2 disabled: retainedGrainVar intentionally omitted on the EXACT SAME 15%-leaked clean planes!
					f1NoL2.prevPeak = { &peakPrevCleanImp, &stillMf, false };
					f1NoL2.nextPeak = { &peakNextCleanImp, &stillMf, false };
					f1NoL2.hasStepPrior = true;
					f1NoL2.estimatedStepDelta = 12.0;
					const auto r1NoL2 = engNoL2.ProcessFrame(f1NoL2, &regNoL2);

					FrameProcessInput f2NoL2;
					f2NoL2.seq = 41;
					f2NoL2.receivedY = curReceived;
					f2NoL2.hasCleanSplit = true;
					f2NoL2.cleanY = curCleanImp;
					f2NoL2.prevPeak = { &peakPrevCleanImp, &stillMf, false };
					f2NoL2.nextPeak = { &peakNextCleanImp, &stillMf, false };
					f2NoL2.hasCarryMotion = true;
					f2NoL2.carryMotion = stillMf;
					f2NoL2.hasStepPrior = true;
					f2NoL2.estimatedStepDelta = 12.0;
					const auto r2NoL2 = engNoL2.ProcessFrame(f2NoL2, &regNoL2);

					std::vector<double> prevResNoL2(n);
					for (size_t i = 0; i < n; ++i)
						prevResNoL2[i] = r1NoL2.outputY.v[i] - masterClean.v[i];

					const auto noL2Rep = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), r2NoL2.outputY.v.data(), n,
						prevResNoL2.data(), prevOwnGrain.data());
					Assert::IsTrue(noL2Rep.valid && noL2Rep.lag1Evaluated);
					Assert::IsFalse(noL2Rep.passesAuthoritativeBar,
						L"H12 Negative control: disabling Layer-2 grain subtraction on 15%-leaked inputs MUST fail authoritative bar!");
					Assert::IsTrue(std::fabs(noL2Rep.betaForeign) > kGrainBarMaxBetaForeign || noL2Rep.betaOwn < kGrainBarMinBetaOwn,
						L"H12 Negative control: unprotected 15%-leaked inputs MUST violate betaForeign <= 0.02 or betaOwn >= 0.98");
				}

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput f1;
				f1.seq = 50;
				f1.receivedY = prevReceived;
				f1.hasCleanSplit = true;
				f1.cleanY = prevCurCleanImp;
				f1.retainedGrainVar = retainedVar;
				f1.prevPeak = { &peakPrevCleanImp, &stillMf, false };
				f1.nextPeak = { &peakNextCleanImp, &stillMf, false };
				f1.hasStepPrior = true;
				f1.estimatedStepDelta = 12.0;
				const auto r1 = eng.ProcessFrame(f1, &reg);

				FrameProcessInput f2;
				f2.seq = 51;
				f2.receivedY = curReceived;
				f2.hasCleanSplit = true;
				f2.cleanY = curCleanImp;
				f2.retainedGrainVar = retainedVar;
				f2.prevPeak = { &peakPrevCleanImp, &stillMf, false };
				f2.nextPeak = { &peakNextCleanImp, &stillMf, false };
				f2.hasCarryMotion = true;
				f2.carryMotion = stillMf;
				f2.hasStepPrior = true;
				f2.estimatedStepDelta = 12.0;
				const auto r2 = eng.ProcessFrame(f2, &reg);

				// Score STRICTLY against pre-generated independent masterClean (NEVER r2.outputClean)!
				std::vector<double> prevResMaster(n);
				for (size_t i = 0; i < n; ++i)
					prevResMaster[i] = r1.outputY.v[i] - masterClean.v[i];

				const auto gRepPrev = EvaluateGrainTransfer(
					masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), r2.outputY.v.data(), n,
					prevResMaster.data(), prevOwnGrain.data());
				const auto gRepNext = EvaluateGrainTransfer(
					masterClean.v.data(), ownGrain.data(), foreignGrainNext.data(), r2.outputY.v.data(), n,
					prevResMaster.data(), prevOwnGrain.data());

				double cleanDbBase = 0.0, cleanDbOut = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterClean, curCleanImp, interior), 1023.0, cleanDbBase));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterClean, r2.outputClean, interior), 1023.0, cleanDbOut));

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Honest Grain (15%% leak split vs masterClean): cleanGain=+%.3f dB | betaOwn=%.4f betaForP=%.4f betaForN=%.4f stdRatio=%.4f lag1Rise=%.4f",
					cleanDbOut - cleanDbBase, gRepPrev.betaOwn, gRepPrev.betaForeign, gRepNext.betaForeign,
					gRepPrev.stdRatio, gRepPrev.lag1Rise);
				Logger::WriteMessage(buf);

				Assert::IsTrue(cleanDbOut - cleanDbBase > 2.0, L"Clean layer must recover compression noise strongly vs masterClean");
				Assert::IsTrue(gRepPrev.valid && gRepPrev.lag1Evaluated && gRepPrev.passesAuthoritativeBar,
					L"15%-leaked split output scored strictly vs masterClean must pass full authoritative refs/111 s7.3 bar (lag1Evaluated==true)");
				Assert::IsTrue(gRepNext.valid && gRepNext.lag1Evaluated && std::fabs(gRepNext.betaForeign) <= kGrainBarMaxBetaForeign,
					L"Foreign grain leakage from nextPeak must stay <= 0.02");

				// ---- Sub-case B: Raw Noisy Input Path (hasCleanSplit == false) ----
				// Pure film grain on masterClean with NO upstream split: Layer 2 A1 subtraction + Layer 4 TdGuardPort
				// must protect own grain across the FULL authoritative bar (with lag1Evaluated==true) and engage P10gGrainSeparator visibly!
				{
					TdV2Plane rawPrevFrame(w, h), rawCur(w, h), rawPrev(w, h), rawNext(w, h);
					for (size_t i = 0; i < n; ++i)
					{
						rawPrevFrame.v[i] = masterClean.v[i] + prevOwnGrain[i];
						rawCur.v[i] = masterClean.v[i] + ownGrain[i];
						rawPrev.v[i] = masterClean.v[i] + foreignGrainPrev[i];
						rawNext.v[i] = masterClean.v[i] + foreignGrainNext[i];
					}
					std::vector<double> fullGrainVar(static_cast<size_t>(cw) * ch, grainSigma * grainSigma);

					Config cfgRaw = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine engRaw(cfgRaw);
					StandInRegistry regRaw;

					FrameProcessInput fRaw1;
					fRaw1.seq = 59;
					fRaw1.receivedY = rawPrevFrame;
					fRaw1.hasCleanSplit = false;
					fRaw1.retainedGrainVar = fullGrainVar;
					fRaw1.prevPeak = { &rawPrev, &stillMf, false };
					fRaw1.nextPeak = { &rawNext, &stillMf, false };
					fRaw1.hasStepPrior = true;
					fRaw1.estimatedStepDelta = 14.0;
					const auto rRaw1 = engRaw.ProcessFrame(fRaw1, &regRaw);

					FrameProcessInput fRaw2;
					fRaw2.seq = 60;
					fRaw2.receivedY = rawCur;
					fRaw2.hasCleanSplit = false;
					fRaw2.retainedGrainVar = fullGrainVar;
					fRaw2.prevPeak = { &rawPrev, &stillMf, false };
					fRaw2.nextPeak = { &rawNext, &stillMf, false };
					fRaw2.hasCarryMotion = true;
					fRaw2.carryMotion = stillMf;
					fRaw2.hasStepPrior = true;
					fRaw2.estimatedStepDelta = 14.0;
					const auto rRaw2 = engRaw.ProcessFrame(fRaw2, &regRaw);

					AssertAllFinite(rRaw2.outputY, L"Raw hasCleanSplit==false output must be finite");
					Assert::IsTrue(regRaw.IsActive(StandInKind::P10gGrainSeparator),
						L"Raw hasCleanSplit==false MUST engage P10gGrainSeparator stand-in visibly");
					std::vector<double> rawPrevRes(n);
					for (size_t i = 0; i < n; ++i)
						rawPrevRes[i] = rRaw1.outputY.v[i] - masterClean.v[i];
					const auto rawRep = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), rRaw2.outputY.v.data(), n,
						rawPrevRes.data(), prevOwnGrain.data());
					Assert::IsTrue(rawRep.valid && rawRep.lag1Evaluated && rawRep.passesAuthoritativeBar,
						L"Raw path with A1+Guard must pass full authoritative grain bar with lag1Evaluated==true");
				}

				// ---- Sub-case C: Layer-4 TdGuardPort Carry-Purity Test (guardViolationCells > 0) ----
				// On a smooth wall/sky background (zero fine scene texture so fine energy is pure film grain),
				// construct Frame 1 where 50% of ownGrain1 leaked into curClean and A2 blends it out against clean peaks,
				// genuinely triggering Layer-4 TdGuardPort (guardViolationCells > 0).
				// Verify directly on engGuard.CarryClean() that Frame 1's ownGrain1 add-back NEVER enters carryClean_,
				// AND Frame 2 (using carry!) stays clean of Frame 1's grain!
				{
					const TdV2Plane smoothBg(w, h, 400.0);
					const TdV2Plane smoothPeakP = AddCodingError(smoothBg, 0.8, 611);
					const TdV2Plane smoothPeakN = AddCodingError(smoothBg, 0.8, 612);
					const TdV2Plane smoothCur2Clean = AddCodingError(smoothBg, 5.5, 613);
					TdV2Plane smoothCur2Recv(w, h), leakCurClean1(w, h), leakRecv1(w, h);
					for (size_t i = 0; i < n; ++i)
					{
						leakCurClean1.v[i] = smoothBg.v[i] + 0.5 * prevOwnGrain[i];
						leakRecv1.v[i] = smoothBg.v[i] + prevOwnGrain[i];
						smoothCur2Recv.v[i] = smoothCur2Clean.v[i] + ownGrain[i];
					}
					const double leakVar1 = (0.5 * grainSigma) * (0.5 * grainSigma);
					std::vector<double> retVar1(static_cast<size_t>(cw) * ch, leakVar1);

					Config cfgG = MakePreset(Preset::Reference, Mode::On);
					cfgG.estimator = VarianceEstimator::A2_RoundingCells; // force blend so Layer-4 guard genuinely fires!
					TdV2ReferenceEngine engGuard(cfgG);
					StandInRegistry regGuard;

					FrameProcessInput g1;
					g1.seq = 70;
					g1.receivedY = leakRecv1;
					g1.hasCleanSplit = true;
					g1.cleanY = leakCurClean1;
					g1.retainedGrainVar = retVar1;
					g1.prevPeak = { &smoothBg, &stillMf, false };
					g1.nextPeak = { &smoothBg, &stillMf, false };
					g1.hasStepPrior = true;
					g1.estimatedStepDelta = 24.0;
					const auto rg1 = engGuard.ProcessFrame(g1, &regGuard);

					Assert::IsTrue(rg1.guardViolationCells > 0,
						L"Test fixture must genuinely trigger Layer-4 TdGuardPort (guardViolationCells > 0)");
					Assert::IsTrue(regGuard.IsActive(StandInKind::GrainGuardW4c),
						L"Layer-4 GrainGuardW4c stand-in must be visibly active");
					Assert::IsTrue(engGuard.HasCarry());

					// Direct inspection of CarryClean() vs outputY for Frame 1's ownGrain1!
					const double slopeOut1 = RegressSlope(rg1.outputY, smoothBg, prevOwnGrain);
					const double slopeCarry1 = RegressSlope(engGuard.CarryClean(), smoothBg, prevOwnGrain);
					Assert::IsTrue(slopeOut1 > 0.65,
						L"Layer-4 TdGuardPort add-back must restore Frame 1's ownGrain1 into outputY");
					Assert::IsTrue(slopeCarry1 < 0.15,
						L"Layer-4 add-back MUST NOT leak Frame 1's ownGrain1 into CarryClean()!");
					Assert::IsTrue(slopeCarry1 + 0.40 < slopeOut1,
						L"CarryClean() must remain cleanly separated from outputY's Layer-4 grain add-back");

					// Frame 2 fuses with CarryClean() + clean peaks; verify Frame 1's ownGrain1 does NOT leak into Frame 2!
					FrameProcessInput g2;
					g2.seq = 71;
					g2.receivedY = smoothCur2Recv;
					g2.hasCleanSplit = true;
					g2.cleanY = smoothCur2Clean;
					g2.prevPeak = { &smoothPeakP, &stillMf, false };
					g2.nextPeak = { &smoothPeakN, &stillMf, false };
					g2.hasCarryMotion = true;
					g2.carryMotion = stillMf;
					g2.hasStepPrior = true;
					g2.estimatedStepDelta = 14.0;
					const auto rg2 = engGuard.ProcessFrame(g2, &regGuard);

					Assert::IsTrue(rg2.carryUsed, L"Frame 2 must consume CarryClean() from Frame 1");
					const auto g2Rep = EvaluateGrainTransfer(
						smoothBg.v.data(), ownGrain.data(), prevOwnGrain.data(), rg2.outputY.v.data(), n);
					Assert::IsTrue(g2Rep.valid);
					Assert::IsTrue(g2Rep.betaOwn >= kGrainBarMinBetaOwn,
						L"Frame 2 must preserve its own grain >= 0.98");
					Assert::IsTrue(std::fabs(g2Rep.betaForeign) <= kGrainBarMaxBetaForeign,
						L"Frame 2 must remain clean of Frame 1's grain (<= 0.02) via pure CarryClean()");
				}
			}

			TEST_METHOD(W1BandedSkyNotSelfVetoedAndFocusGuardProtectsRackFocusAndBokeh)
			{
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect interior{ 16, 16, w - 16, h - 16 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// ---- Sub-case A: Smooth W1 Banded Sky MUST NOT be self-vetoed by Focus Guard ----
				{
					TdV2Plane masterSky(w, h), valleySky(w, h);
					std::vector<uint8_t> skyMask(static_cast<size_t>(w) * h, kMaskW1BandedSky | kMaskTrackable);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double val = 400.0 + 0.6 * x + 0.2 * y;
							masterSky.at(x, y) = val;
							// 8-code banding quantization steps on valley frame.
							valleySky.at(x, y) = 8.0 * std::floor(val / 8.0 + 0.5);
						}
					}
					const TdV2Plane peakPrevSky = AddCodingError(masterSky, 0.5, 801);
					const TdV2Plane peakNextSky = AddCodingError(masterSky, 0.5, 802);

					// Strictest BokehMode::Untouched — any false isDefocused classification would veto L/M/H to 0 dB!
					Config cfgSky = MakePreset(Preset::Reference, Mode::On);
					cfgSky.bokehMode = BokehMode::Untouched;
					TdV2ReferenceEngine engSky(cfgSky);
					StandInRegistry regSky;

					FrameProcessInput inSky;
					inSky.seq = 29;
					inSky.receivedY = valleySky;
					inSky.hasCleanSplit = true;
					inSky.cleanY = valleySky;
					inSky.prevPeak = { &peakPrevSky, &stillMf, false };
					inSky.nextPeak = { &peakNextSky, &stillMf, false };
					inSky.hasStepPrior = true;
					inSky.estimatedStepDelta = 8.0;
					const auto resSky = engSky.ProcessFrame(inSky, &regSky);

					Assert::AreEqual(static_cast<size_t>(0), resSky.focusVetoCells,
						L"Smooth W1 banded sky MUST NOT trigger false focus vetoes");
					const WorstAreaReport repSky = EvaluateWorstAreaMasks(
						masterSky.v.data(), valleySky.v.data(), resSky.outputY.v.data(), skyMask.data(), w, h, 16, 1023.0);
					Assert::IsTrue(repSky.valid);
					Assert::IsTrue(repSky.w1BandedSky.deltaDb > 3.0,
						L"Smooth W1 banded sky must achieve strong positive dB recovery (not self-vetoed!)");
					Assert::IsTrue(repSky.w1BandingStepReduction > 0.30,
						L"W1 banding step error must drop by >= 30%");
				}

				// ---- Sub-case B: Unmatched Rack-Focus MUST be gated to 0 on L, M, AND H even under LowBandCellOnly ----
				const TdV2Plane sharpRef = RenderMaster(w, h);
				const auto defocusTaps = VpUnit::BandFromBase::Taps<double>(4.0, 3.0, true, 0.0);
				TdV2Plane defocusedCur(w, h, 0.0);
				VpUnit::BandFromBase::BlurTaps(sharpRef.v, defocusedCur.v, w, h, defocusTaps, VpUnit::BandFromBase::EdgeMode::Replicate);

				const ThreeBandPlanes curB = DecomposeActiveBands(defocusedCur, ActiveRect::Full(w, h));
				const ThreeBandPlanes refB = DecomposeActiveBands(sharpRef, ActiveRect::Full(w, h));
				const double refHEnergy = PlaneMse(curB.H, refB.H, interior);
				const double refMEnergy = PlaneMse(curB.M, refB.M, interior);

				// Mandatory In-Test Negative Control (Rule 3):
				// Un-gated temporal blend on exact same (defocusedCur, sharpRef) severely violates Owner Focus Rule!
				TdV2Plane naiveRack(w, h, 0.0);
				for (size_t i = 0; i < defocusedCur.v.size(); ++i)
					naiveRack.v[i] = 0.5 * defocusedCur.v[i] + 0.5 * sharpRef.v[i];
				const ThreeBandPlanes naiveB = DecomposeActiveBands(naiveRack, ActiveRect::Full(w, h));
				const double naiveFalseH = PlaneMse(curB.H, naiveB.H, interior) / refHEnergy;
				const double naiveFalseM = PlaneMse(curB.M, naiveB.M, interior) / refMEnergy;
				Assert::IsTrue(naiveFalseH > 0.20 && naiveFalseM > 0.20,
					L"Negative control: un-gated temporal blending MUST smear/import sharpness across rack focus");

				// Adversarial check: enable BokehMode::LowBandCellOnly — focusChange MUST STILL hard-veto L, M, AND H!
				Config cfgRack = MakePreset(Preset::Reference, Mode::On);
				cfgRack.bokehMode = BokehMode::LowBandCellOnly;
				TdV2ReferenceEngine engRack(cfgRack);
				StandInRegistry regRack;

				FrameProcessInput inRack;
				inRack.seq = 30;
				inRack.receivedY = defocusedCur;
				inRack.hasCleanSplit = true;
				inRack.cleanY = defocusedCur;
				inRack.prevPeak = { &sharpRef, &stillMf, false };
				inRack.nextPeak = { &sharpRef, &stillMf, false };
				inRack.hasStepPrior = true;
				inRack.estimatedStepDelta = 12.0;

				const auto resRack = engRack.ProcessFrame(inRack, &regRack);
				Assert::IsTrue(resRack.focusVetoCells > 0, L"Focus guard must detect focal-plane shift across rack focus");
				Assert::IsTrue(regRack.IsActive(StandInKind::FocusMapK32), L"Focus stand-in must be visibly tagged");

				const ThreeBandPlanes outB = DecomposeActiveBands(resRack.outputY, ActiveRect::Full(w, h));
				const double falseRestoreH = refHEnergy > 1e-9 ? (PlaneMse(curB.H, outB.H, interior) / refHEnergy) : 0.0;
				const double falseRestoreM = refMEnergy > 1e-9 ? (PlaneMse(curB.M, outB.M, interior) / refMEnergy) : 0.0;
				const double rackInteriorMse = PlaneMse(defocusedCur, resRack.outputY, interior);
				const double errLOut = PlaneMse(curB.L, outB.L, interior);
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Rack-focus under LowBandCellOnly: falseH=%.6f (naive=%.3f) falseM=%.6f interiorMse=%.9f errL=%.9f",
					falseRestoreH, naiveFalseH, falseRestoreM, rackInteriorMse, errLOut);
				Logger::WriteMessage(buf);
				Assert::IsTrue(falseRestoreH <= 0.001, L"Owner Focus Rule: H defocus false-restore must be <= 0.1%");
				Assert::IsTrue(falseRestoreM <= 0.001, L"Owner Focus Rule: M defocus false-restore must be <= 0.1%");
				Assert::AreEqual(0.0, rackInteriorMse, 1e-12,
					L"Owner Focus Rule: rack-focus (focusChange) MUST leave interior pixels bit-exact untouched (zero L/M/H delta)");
				Assert::AreEqual(0.0, errLOut, 1e-12,
					L"Owner Focus Rule: rack-focus (focusChange) MUST leave L-band strictly untouched even under LowBandCellOnly");
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						const unsigned mask = static_cast<unsigned>(
							TdMapsView::Fetch(resRack.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						Assert::AreEqual(0u, mask,
							L"Plan s1.4 L141/L143: rack-focus (focusChange) MUST hard-veto L, M, and H bands (all bits 0)");
					}
				}

				// ---- Sub-case C: Matched-Focus Bokeh (Q4: Untouched vs LowBandCellOnly, focusChange == false!) ----
				// Plant genuine L-band bokeh noise/banding on defocusedCur (preserving defocusedCur's exact M/D3 optical
				// bokeh signature cleanM2 > 1.0 & curFDebiased <= fDefocus, with zero Parseval contradiction on white noise).
				const TdV2Plane bokehWhite = AddCodingError(defocusedCur, 18.0, 403);
				const ThreeBandPlanes bokehWhiteB = DecomposeActiveBands(bokehWhite, ActiveRect::Full(w, h));
				TdV2Plane bokehCur = defocusedCur;
				for (size_t i = 0; i < bokehCur.v.size(); ++i)
					bokehCur.v[i] += (bokehWhiteB.L.v[i] - curB.L.v[i]);
				const TdV2Plane bokehPrev = AddCodingError(defocusedCur, 0.5, 401);
				const TdV2Plane bokehNext = AddCodingError(defocusedCur, 0.5, 402);

				const std::vector<uint8_t> bokehMap(static_cast<size_t>(cw) * ch, 1u);
				FrameProcessInput bkIn;
				bkIn.seq = 31;
				bkIn.receivedY = bokehCur;
				bkIn.hasCleanSplit = true;
				bkIn.cleanY = bokehCur;
				bkIn.prevPeak = { &bokehPrev, &stillMf, false };
				bkIn.nextPeak = { &bokehNext, &stillMf, false };
				bkIn.hasStepPrior = true;
				bkIn.estimatedStepDelta = 12.0;
				bkIn.explicitDefocusedCells = &bokehMap; // kFocusMap (id 32) matched-focus bokeh mask (focusChange == false!)

				Config cfgUntouched = MakePreset(Preset::Reference, Mode::On);
				cfgUntouched.bokehMode = BokehMode::Untouched;
				TdV2ReferenceEngine engU(cfgUntouched);
				const auto resU = engU.ProcessFrame(bkIn, &regRack);

				Config cfgLowOnly = MakePreset(Preset::Reference, Mode::On);
				cfgLowOnly.bokehMode = BokehMode::LowBandCellOnly;
				TdV2ReferenceEngine engL(cfgLowOnly);
				const auto resL = engL.ProcessFrame(bkIn, &regRack);

				const double mseBaseBokeh = PlaneMse(defocusedCur, bokehCur, interior);
				const double mseU = PlaneMse(defocusedCur, resU.outputY, interior);
				const double mseL = PlaneMse(defocusedCur, resL.outputY, interior);
				const ThreeBandPlanes curBokehB = DecomposeActiveBands(bokehCur, ActiveRect::Full(w, h));
				const ThreeBandPlanes outUB = DecomposeActiveBands(resU.outputY, ActiveRect::Full(w, h));
				const ThreeBandPlanes outLB = DecomposeActiveBands(resL.outputY, ActiveRect::Full(w, h));
				const double mseU_Lband = PlaneMse(curB.L, outUB.L, interior);
				const double mseL_Lband = PlaneMse(curB.L, outLB.L, interior);

				Assert::IsTrue(resU.focusVetoCells > 0 && resL.focusVetoCells > 0,
					L"Q4: Matched-focus bokeh gated via explicitDefocusedCells (kFocusMap id 32) must register focusVetoCells > 0");
				Assert::AreEqual(mseBaseBokeh, mseU, 1e-12,
					L"Q4: BokehMode::Untouched MUST leave matched-focus bokeh completely untouched (mseU == mseBaseBokeh)");
				Assert::IsTrue(mseL < mseU,
					L"Q4: BokehMode::LowBandCellOnly must strictly reduce bokeh error compared to BokehMode::Untouched");
				Assert::IsTrue(mseL_Lband < mseU_Lband * 0.50,
					L"Q4: BokehMode::LowBandCellOnly must reduce L-band bokeh MSE by >= 50% vs Untouched");
				// Analytical Gaussian passband overlap bound: (I - G(1.0)) * (w_L(x,y) * G(2.5)) has a small (< 1%)
				// smooth-filter passband tail even with w_M == w_H == 0 strictly; exact w_M == w_H == 0 is additionally
				// proved by kBandMask (maskL & 6u == 0) and the matched-focus Nyquist checkerboard test immediately below.
				Assert::IsTrue(PlaneMse(curBokehB.H, outLB.H, interior) <= 0.01 * PlaneMse(curBokehB.L, outLB.L, interior),
					L"Q4: BokehMode::LowBandCellOnly H-band projection must stay <= 1% of L-band repair (Gaussian passband tail only)");

				// Verify TdMapsView::kBandMask on matched-focus defocused interior cells cleanly distinguishes Q4 modes:
				//   - Untouched: maskU == 0 (all bands L, M, H strictly 0)
				//   - LowBandCellOnly: (maskL & 6u) == 0 (M and H strictly 0) AND (maskL & 1u) == 1u (L active!)
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						const unsigned maskL = static_cast<unsigned>(
							TdMapsView::Fetch(resL.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						const unsigned maskU = static_cast<unsigned>(
							TdMapsView::Fetch(resU.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						Assert::AreEqual(0u, maskU,
							L"Q4: Matched-focus bokeh under BokehMode::Untouched MUST veto all bands (maskU == 0)");
						Assert::AreEqual(0u, maskL & 6u,
							L"Q4: Matched-focus bokeh under BokehMode::LowBandCellOnly MUST keep M (bit 1) and H (bit 2) strictly 0");
						Assert::AreEqual(1u, maskL & 1u,
							L"Q4: Matched-focus bokeh under BokehMode::LowBandCellOnly MUST keep L-band (bit 0) active");
					}
				}

				// Direct M/H delta == 0 proof on genuine matched-focus bokeh (isDefocused == true, focusChange == false):
				// plant a pure Nyquist checkerboard (zero L/M/D3 projection!) WITHOUT explicitFocusChangeCells ->
				// under LowBandCellOnly, isDefocused alone MUST hold H-band delta == 0.0!
				TdV2Plane bokehChecker = defocusedCur;
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						bokehChecker.at(x, y) += ((x ^ y) & 1) ? 4.0 : -4.0;
				FrameProcessInput bkMH;
				bkMH.seq = 32;
				bkMH.receivedY = bokehChecker;
				bkMH.hasCleanSplit = true;
				bkMH.cleanY = bokehChecker;
				bkMH.prevPeak = { &defocusedCur, &stillMf, false };
				bkMH.nextPeak = { &defocusedCur, &stillMf, false };
				bkMH.hasStepPrior = true;
				bkMH.estimatedStepDelta = 12.0;
				bkMH.explicitDefocusedCells = &bokehMap; // matched-focus bokeh mask (explicitFocusChangeCells == nullptr!)
				TdV2ReferenceEngine engLMH(cfgLowOnly);
				const auto resLMH = engLMH.ProcessFrame(bkMH, &regRack);
				Assert::IsTrue(resLMH.focusVetoCells > 0,
					L"Matched-focus bokeh checkerboard gated via explicitDefocusedCells must engage bokeh guard");
				double maxInteriorMhDelta = 0.0;
				for (int y = interior.y0; y < interior.y1; ++y)
					for (int x = interior.x0; x < interior.x1; ++x)
						maxInteriorMhDelta = (std::max)(maxInteriorMhDelta,
							std::fabs(resLMH.outputY.at(x, y) - bokehChecker.at(x, y)));
				Assert::IsTrue(maxInteriorMhDelta < 1e-5,
					L"Under genuine matched-focus bokeh (LowBandCellOnly, focusChange == false), pure H-band difference MUST yield 0.0 delta!");
			}

			TEST_METHOD(CurvedSmoothSkyBandingRepairedUnderUntouchedWithRackFocusControl)
			{
				// H8 Adversarial Test: Curved smooth sky / vignette gradient A * cos(2*pi*x/96) (A = 100)
				// has non-zero D3 curvature energy (> structureFloor) and low focus ratio F ~= 0.03 < fDefocus (0.117).
				// Under default BokehMode::Untouched, TD MUST NOT self-veto L-band banding repair on the curved sky!
				// Simultaneously, a genuine rack-focus pull on the EXACT SAME curved-sky scene MUST still hard-veto M/H
				// (defocus false-restore <= 0.1%).
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect interior{ 16, 16, w - 16, h - 16 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				TdV2Plane masterSky(w, h), valleySky(w, h);
				std::vector<uint8_t> skyMask(static_cast<size_t>(w) * h, kMaskW1BandedSky | kMaskTrackable);
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const double val = 450.0 + 100.0 * std::cos(2.0 * kPi * x / 96.0)
						                         + 20.0 * std::cos(2.0 * kPi * y / 96.0);
						masterSky.at(x, y) = val;
						// Quantized 8-code sky banding on the valley frame.
						valleySky.at(x, y) = 8.0 * std::floor(val / 8.0 + 0.5);
					}
				}

				// Non-vacuity proof: verify curved sky genuinely has curvature energy > 5 code^2 and F < fDefocus
				// (confirming the pre-H8 focus guard WOULD have falsely self-vetoed this sky under BokehMode::Untouched!).
				const ThreeBandPlanes skyBands = DecomposeActiveBands(masterSky, ActiveRect::Full(w, h));
				double skyE2 = 0.0, skyE3 = 0.0;
				size_t skyCnt = 0;
				for (int y = interior.y0; y < interior.y1; ++y)
				{
					for (int x = interior.x0; x < interior.x1; ++x)
					{
						const double mv = skyBands.M.at(x, y);
						const double d3 = skyBands.D3.at(x, y);
						skyE2 += mv * mv;
						skyE3 += d3 * d3;
						++skyCnt;
					}
				}
				const double skyBandEnergy = (skyE2 + skyE3) / static_cast<double>(skyCnt);
				const double skyFocusRatio = skyE2 / (skyE2 + skyE3 + 1e-12);
				Assert::IsTrue(skyBandEnergy > 5.0,
					L"Non-vacuity: curved cosine sky must have genuine D3 curvature energy above structureFloor");
				Assert::IsTrue(skyFocusRatio < AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase),
					L"Non-vacuity: curved cosine sky must have F < fDefocus (exposing pre-H8 self-veto hazard)");

				const TdV2Plane peakPrevSky = AddCodingError(masterSky, 0.5, 851);
				const TdV2Plane peakNextSky = AddCodingError(masterSky, 0.5, 852);

				// Run under default BokehMode::Untouched!
				Config cfgSky = MakePreset(Preset::Reference, Mode::On);
				Assert::IsTrue(cfgSky.bokehMode == BokehMode::Untouched);
				TdV2ReferenceEngine engSky(cfgSky);
				StandInRegistry regSky;

				FrameProcessInput inSky;
				inSky.seq = 1;
				inSky.receivedY = valleySky;
				inSky.hasCleanSplit = true;
				inSky.cleanY = valleySky;
				inSky.prevPeak = { &peakPrevSky, &stillMf, false };
				inSky.nextPeak = { &peakNextSky, &stillMf, false };
				inSky.hasStepPrior = true;
				inSky.estimatedStepDelta = 8.0;
				const auto resSky = engSky.ProcessFrame(inSky, &regSky);

				Assert::AreEqual(static_cast<size_t>(0), resSky.focusVetoCells,
					L"H8: curved smooth sky must NOT trigger false rack-focus vetoes");
				int lActiveInteriorCells = 0;
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						const unsigned mask = static_cast<unsigned>(
							TdMapsView::Fetch(resSky.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						if ((mask & 1u) != 0u)
							++lActiveInteriorCells;
					}
				}
				Assert::IsTrue(lActiveInteriorCells >= (cw - 2) * (ch - 2) / 2,
					L"H8: L-band MUST remain active (not vetoed) across curved smooth sky under BokehMode::Untouched");

				const WorstAreaReport repSky = EvaluateWorstAreaMasks(
					masterSky.v.data(), valleySky.v.data(), resSky.outputY.v.data(), skyMask.data(), w, h, 16, 1023.0);
				Assert::IsTrue(repSky.valid);
				Assert::IsTrue(repSky.w1BandedSky.deltaDb > 3.0,
					L"H8: curved smooth sky must achieve strong positive PSNR gain under default BokehMode::Untouched");
				Assert::IsTrue(repSky.w1BandingStepReduction >= 0.30,
					L"H8: curved smooth sky banding step error must drop >= 30% (plan s3.3 W1 bar)");

				// ---- Mandatory Genuine Rack-Focus Negative Control on the SAME Curved-Sky Scene ----
				// Add sharp foreground structure onto masterSky -> sharpRef, then pull focus optically (sigma = 4.0)
				// along motion -> defocusedCur. Genuine focal-plane shift MUST still hard-veto M/H (<= 0.1% false restore)!
				TdV2Plane sharpScene = masterSky;
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						sharpScene.at(x, y) += 28.0 * std::sin(2.0 * kPi * (x / 7.0 + y / 11.0))
						                     + 18.0 * std::cos(2.0 * kPi * (x / 5.0 - y / 6.0));
					}
				}
				const auto defocusTaps = VpUnit::BandFromBase::Taps<double>(4.0, 3.0, true, 0.0);
				TdV2Plane defocusedScene(w, h, 0.0);
				VpUnit::BandFromBase::BlurTaps(sharpScene.v, defocusedScene.v, w, h, defocusTaps, VpUnit::BandFromBase::EdgeMode::Replicate);

				const ThreeBandPlanes curRackB = DecomposeActiveBands(defocusedScene, ActiveRect::Full(w, h));
				const ThreeBandPlanes refRackB = DecomposeActiveBands(sharpScene, ActiveRect::Full(w, h));
				const double refHEnergy = PlaneMse(curRackB.H, refRackB.H, interior);
				const double refMEnergy = PlaneMse(curRackB.M, refRackB.M, interior);

				// Naive unprotected blend on the exact same rack-focus pair severely violates Owner Focus Rule!
				TdV2Plane naiveRack(w, h, 0.0);
				for (size_t i = 0; i < defocusedScene.v.size(); ++i)
					naiveRack.v[i] = 0.5 * defocusedScene.v[i] + 0.5 * sharpScene.v[i];
				const ThreeBandPlanes naiveRackB = DecomposeActiveBands(naiveRack, ActiveRect::Full(w, h));
				Assert::IsTrue(PlaneMse(curRackB.H, naiveRackB.H, interior) / refHEnergy > 0.20,
					L"Negative control: naive blend across rack-focus must severely import sharp H detail");
				Assert::IsTrue(PlaneMse(curRackB.M, naiveRackB.M, interior) / refMEnergy > 0.20,
					L"Negative control: naive blend across rack-focus must severely import sharp M detail");

				TdV2ReferenceEngine engRack(cfgSky);
				StandInRegistry regRack;
				FrameProcessInput inRack;
				inRack.seq = 2;
				inRack.receivedY = defocusedScene;
				inRack.hasCleanSplit = true;
				inRack.cleanY = defocusedScene;
				inRack.prevPeak = { &sharpScene, &stillMf, false };
				inRack.nextPeak = { &sharpScene, &stillMf, false };
				inRack.hasStepPrior = true;
				inRack.estimatedStepDelta = 12.0;
				const auto resRack = engRack.ProcessFrame(inRack, &regRack);

				Assert::IsTrue(resRack.focusVetoCells > 0,
					L"Genuine rack-focus pull on the curved-sky scene MUST still engage focus veto");
				const ThreeBandPlanes outRackB = DecomposeActiveBands(resRack.outputY, ActiveRect::Full(w, h));
				const double falseH = PlaneMse(curRackB.H, outRackB.H, interior) / refHEnergy;
				const double falseM = PlaneMse(curRackB.M, outRackB.M, interior) / refMEnergy;
				Assert::IsTrue(falseH <= 0.001, L"Owner Focus Rule: H defocus false-restore must remain <= 0.1%");
				Assert::IsTrue(falseM <= 0.001, L"Owner Focus Rule: M defocus false-restore must remain <= 0.1%");
			}

			TEST_METHOD(LetterboxBorderPanIntegrityBarsExactAndActiveEdgeZeroRinging)
			{
				const int w = 96, h = 96;
				const ActiveRect active{ 0, 16, w, h - 16 }; // 16-px top & bottom letterbox bars
				const int cw = w / 16, ch = h / 16;

				// Bright active picture (~500) adjacent to dark 64.0 letterbox bars under diagonal pan (+2, +2) / (-2, -2).
				TdV2Plane masterCur = RenderMaster(w, h, 0.0, 0.0);
				TdV2Plane masterPrev = RenderMaster(w, h, -2.0, -2.0);
				TdV2Plane masterNext = RenderMaster(w, h, +2.0, +2.0);
				for (size_t i = 0; i < masterCur.v.size(); ++i)
				{
					masterCur.v[i] += 100.0;
					masterPrev.v[i] += 100.0;
					masterNext.v[i] += 100.0;
				}

				TdV2Plane peakPrev = AddCodingError(masterPrev, 1.2, 501);
				TdV2Plane peakNext = AddCodingError(masterNext, 1.2, 502);
				TdV2Plane valley = AddCodingError(masterCur, 6.0, 503);

				for (int y = 0; y < h; ++y)
				{
					if (y >= active.y0 && y < active.y1) continue;
					for (int x = 0; x < w; ++x)
					{
						valley.at(x, y) = 64.0;
						peakPrev.at(x, y) = 64.0;
						peakNext.at(x, y) = 64.0;
					}
				}

				const ActiveRect topEdge18{ 8, 16, w - 8, 34 };       // 1-18 px directly below top letterbox bar
				const ActiveRect botEdge18{ 8, 62, w - 8, 80 };       // 1-18 px directly above bottom letterbox bar

				// Mandatory In-Test Negative Control (Rule 3):
				// Prove that leaving 0.0-holes at OOB warped landings causes dark ringing across the 1-18 px active strip!
				{
					TdV2Plane zeroHoleWarped = masterCur;
					for (int y = 16; y < 18; ++y)
						for (int x = 0; x < w; ++x)
							zeroHoleWarped.at(x, y) = 0.0; // OOB landing filled with 0.0 instead of edge-clamped sample
					const ThreeBandPlanes curB = DecomposeActiveBands(valley, active);
					const ThreeBandPlanes badB = DecomposeActiveBands(zeroHoleWarped, active);
					TdV2Plane naiveRingOut = valley;
					for (int y = active.y0; y < active.y1; ++y)
					{
						for (int x = active.x0; x < active.x1; ++x)
						{
							// Blend L-band with 0.0-hole-contaminated warped L-band (Gaussian radius reaches 18 px!).
							naiveRingOut.at(x, y) = valley.at(x, y) - 0.5 * (curB.L.at(x, y) - badB.L.at(x, y));
						}
					}
					const double naiveTopBias = PlaneMeanSignedDiff(naiveRingOut, masterCur, topEdge18);
					Assert::IsTrue(naiveTopBias < -10.0,
						L"Negative control: 0.0-hole OOB warp MUST induce severe dark ringing across 1-18 px active edge!");
				}

				CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +2.0, +2.0, +2.0, +2.0, RegionMotionClass::Camera);
				CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -2.0, -2.0, -2.0, -2.0, RegionMotionClass::Camera);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 40;
				in.receivedY = valley;
				in.hasCleanSplit = true;
				in.cleanY = valley;
				in.activeRect = active;
				in.prevPeak = { &peakPrev, &mfPrev, false };
				in.nextPeak = { &peakNext, &mfNext, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 14.0;

				const auto res = eng.ProcessFrame(in, &reg);

				// 1. Bars outside ActiveRect remain bit-exact 64.0.
				for (int y = 0; y < h; ++y)
				{
					if (y >= active.y0 && y < active.y1) continue;
					for (int x = 0; x < w; ++x)
					{
						Assert::AreEqual(64.0, res.outputY.at(x, y), 0.0,
							L"Letterbox bars outside ActiveRect MUST remain bit-exact under diagonal pan");
					}
				}

				// 2. Active pixels 1-18 px from BOTH top and bottom bars suffer ZERO dark 0.0-hole ringing and improve vs valley!
				const double topBias = PlaneMeanSignedDiff(res.outputY, masterCur, topEdge18);
				const double botBias = PlaneMeanSignedDiff(res.outputY, masterCur, botEdge18);
				const double topMseVal = PlaneMse(masterCur, valley, topEdge18);
				const double topMseOut = PlaneMse(masterCur, res.outputY, topEdge18);
				const double botMseVal = PlaneMse(masterCur, valley, botEdge18);
				const double botMseOut = PlaneMse(masterCur, res.outputY, botEdge18);

				Assert::IsTrue(std::fabs(topBias) < 2.0, L"Top 1-18 px active border must have zero dark ringing bias");
				Assert::IsTrue(std::fabs(botBias) < 2.0, L"Bottom 1-18 px active border must have zero dark ringing bias");
				Assert::IsTrue(topMseOut < topMseVal, L"Top 1-18 px active border must strictly improve MSE against clean ground truth");
				Assert::IsTrue(botMseOut < botMseVal, L"Bottom 1-18 px active border must strictly improve MSE against clean ground truth");

				// 3. 1-2 px active-border bilinear fallback must be visibly counted.
				Assert::IsTrue(res.borderBilinearPixels > 0, L"Panning active border must exercise border bilinear fallback");
				Assert::IsTrue(reg.IsActive(StandInKind::BorderBilinearFallback),
					L"Border bilinear warp fallback must be visibly registered");
			}

			TEST_METHOD(HardCellBoundaryIsolationOverlayDynamicTextureAndLocalFocusChange)
			{
				// Mandatory In-Test Negative Control (Rule 3):
				// Prove that unprotected SpatialBilinearWeight across a (0.8 | 0.0) cell boundary leaks > 0.35 weight
				// right inside the vetoed cell (within 1-8 px of the shared border)!
				{
					const double grid[2] = { 0.8, 0.0 }; // cell 0 = active (0.8), cell 1 = vetoed (0.0), cellPx = 16
					const double wAtBorder1Px = SpatialBilinearWeight(grid, 2, 1, 16.0, 7.5, 16);
					const double wAtBorder4Px = SpatialBilinearWeight(grid, 2, 1, 19.0, 7.5, 16);
					Assert::IsTrue(wAtBorder1Px > 0.35 && wAtBorder4Px > 0.15,
						L"Negative control: unprotected bilinear interpolation MUST bleed weight into adjacent vetoed cell!");
				}

				const int w = 96, h = 96;
				const int cellPx = 16;
				const int cw = w / cellPx, ch = h / cellPx; // 6x6 cells

				const TdV2Plane master = RenderMaster(w, h);
				TdV2Plane peakPrev = AddCodingError(master, 0.8, 901);
				TdV2Plane peakNext = AddCodingError(master, 0.8, 902);
				TdV2Plane cur = AddCodingError(master, 8.0, 903, 0.30);

				// Cell (2, 2): Hard-vetoed Overlay (+12-code subtitle contrast vs peak refs on master, avoiding 560-code G(2.5) neighbor occlusion).
				for (int y = 32; y < 48; ++y)
					for (int x = 32; x < 48; ++x)
						cur.at(x, y) = master.at(x, y) + 12.0;

				// Cell (4, 2): Local explicit focusChange — defocused cur (450 + 12 L-band offset) vs sharp refs carrying
				// BOTH period-8 M-band (20*sin(2*pi*x/8)) AND Nyquist H-band (+/-30) contrast directly adjacent to normal
				// high-weight cells (any L, M, or H bleed across the cell boundary immediately alters per-pixel output!).
				for (int y = 32; y < 48; ++y)
					for (int x = 64; x < 80; ++x)
					{
						const double midPat = 20.0 * std::sin(2.0 * kPi * static_cast<double>(x) / 8.0);
						const double highPat = (((x ^ y) & 1) ? 30.0 : -30.0);
						cur.at(x, y) = 450.0 + 12.0;
						peakPrev.at(x, y) = 450.0 + midPat + highPat;
						peakNext.at(x, y) = 450.0 + midPat + highPat;
					}

				// Cell (2, 4): DynamicTexture (water/foliage) — pure Nyquist checkerboard in cur vs flat in refs
				// (zero L-band diff, huge M/H diff -> any M/H bleed immediately alters the checkerboard!).
				for (int y = 64; y < 80; ++y)
					for (int x = 32; x < 48; ++x)
					{
						cur.at(x, y) = 450.0 + (((x ^ y) & 1) ? 30.0 : -30.0);
						peakPrev.at(x, y) = 450.0;
						peakNext.at(x, y) = 450.0;
					}

				CellMotionField mf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				mf.cls[static_cast<size_t>(2) * cw + 2] = RegionMotionClass::Overlay;
				mf.cls[static_cast<size_t>(4) * cw + 2] = RegionMotionClass::DynamicTexture;

				std::vector<uint8_t> explicitFocus(static_cast<size_t>(cw) * ch, 0u);
				explicitFocus[static_cast<size_t>(2) * cw + 4] = 1u; // Cell (4, 2)

				// Enable BokehMode::LowBandCellOnly too so focusChange isolation is tested at maximum severity!
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.bokehMode = BokehMode::LowBandCellOnly;
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 80;
				in.receivedY = cur;
				in.hasCleanSplit = true;
				in.cleanY = cur;
				in.prevPeak = { &peakPrev, &mf, false };
				in.nextPeak = { &peakNext, &mf, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 16.0;
				in.explicitFocusChangeCells = &explicitFocus;
				const auto res = eng.ProcessFrame(in, &reg);

				// First prove directly adjacent neighbor cells ARE genuinely high-weight active cells!
				const int adjCoords[][2] = { {1,2}, {3,2}, {2,1}, {2,3}, {5,2}, {4,1}, {4,3} };
				for (const auto& c : adjCoords)
				{
					const auto& blk = res.tdMaps[static_cast<size_t>(c[1]) * cw + c[0]];
					Assert::AreEqual(1.0f, TdMapsView::Fetch(blk, TdMapsView::kValidity), 1e-6f,
						L"Neighbor cells surrounding vetoed cells must be active high-weight cells");
					Assert::IsTrue(TdMapsView::Fetch(blk, TdMapsView::kTrailConfidence0) > 0.30f,
						L"Neighbor cells must have strong blend weight to genuinely stress the shared border");
				}

				// Assert 0.000 bleed inside EVERY pixel of Overlay cell (2, 2), including 0-8 px from border!
				Assert::AreEqual(0.0f, TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(2) * cw + 2], TdMapsView::kValidity), 0.0f);
				for (int y = 32; y < 48; ++y)
					for (int x = 32; x < 48; ++x)
						Assert::AreEqual(cur.at(x, y), res.outputY.at(x, y), 0.0,
							L"Overlay (subtitle) cell MUST have 0.000 weight bleed even 1 px from active neighbor border!");

				// Assert local focusChange cell (4, 2) hard-vetoes L, M, and H (all band bits 0, kValidity == 0)
				// with EXACT 0.0 per-pixel bleed on EVERY pixel in [64, 80) x [32, 48) (zero diluted averaging!).
				const unsigned focusMask = static_cast<unsigned>(
					TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(2) * cw + 4], TdMapsView::kBandMask));
				Assert::AreEqual(0u, focusMask,
					L"Local focusChange cell MUST hard-veto L, M, and H (all band bits strictly 0)");
				Assert::AreEqual(0.0f, TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(2) * cw + 4], TdMapsView::kValidity), 0.0f,
					L"Local focusChange cell MUST report kValidity == 0.0f");
				for (int y = 32; y < 48; ++y)
					for (int x = 64; x < 80; ++x)
						Assert::AreEqual(cur.at(x, y), res.outputY.at(x, y), 0.0,
							L"Local focusChange cell MUST have exact 0.000 L/M/H bleed on EVERY pixel even 1 px from active border!");

				// Assert DynamicTexture cell (2, 4) keeps M/H bits strictly 0 and zero M/H checkerboard bleed!
				const unsigned dynMask = static_cast<unsigned>(
					TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(4) * cw + 2], TdMapsView::kBandMask));
				Assert::AreEqual(0u, dynMask & 6u, L"DynamicTexture cell must keep M and H bands strictly 0");
				// Full-16x16 cell (2,4) Nyquist checkerboard amplitude check (ALL 256 px including borders; orthogonal to lawful L-band shift):
				double nyquistSum = 0.0;
				for (int y = 64; y < 80; ++y)
					for (int x = 32; x < 48; ++x)
						nyquistSum += (((x ^ y) & 1) ? 1.0 : -1.0) * res.outputY.at(x, y);
				const double ampOut = nyquistSum / 256.0;
				Assert::AreEqual(30.0, ampOut, 0.02,
					L"DynamicTexture cell (all 256 px incl. borders) must preserve Nyquist checkerboard amplitude with zero M/H bleed");
			}

			TEST_METHOD(TransformIdentified8x8OnNonMultipleOf8ActiveFrameAndMultiIterPocs)
			{
				// Non-8-aligned active rect: {2, 2, 70, 70} inside 72x72 =>
				//   - Letterbox/pillarbox bars: [0, 2) and [70, 72) (must remain bit-exact)
				//   - Leading partial strips: [2, 8) (bounded by TransformUnknownBound)
				//   - Codec-frame-grid-aligned interior 8x8 DCT blocks: [8, 64) x [8, 64)
				//   - Trailing partial strips: [64, 70) (bounded by TransformUnknownBound)
				const int w = 72, h = 72;
				const ActiveRect active{ 2, 2, 70, 70 }; // bxStart = 8, byStart = 8, bxEnd = 64, byEnd = 64
				const int cw = (w + 15) / 16, ch = (h + 15) / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				const double stepDelta = 12.0;
				const double phi = kDeadZonePhiInter; // 1/6 -> theta = 10.0
				const double theta = (1.0 - phi) * stepDelta;

				// ---- Part A & B: Real codec-grid-aligned 8x8 DCT STRICT vs CELL + bounded leading/trailing strips + bit-exact bars ----
				TdV2Plane pred(w, h, 350.0);
				TdV2Plane cur(w, h, 350.0);
				TdV2Plane nextPeak(w, h, 350.0);

				double sentDct[64] = {}, targetDct[64] = {}, sentSpat[64] = {}, targetSpat[64] = {};
				sentDct[0] = 24.0;   // k = 2 (sent DC), cell I_2 = [22.0, 34.0]
				sentDct[1] = 0.0;    // k = 0 (zeroed AC), dead-zone [-10.0, +10.0]
				targetDct[0] = 30.0; // inside I_2
				targetDct[1] = 28.0; // outside dead-zone -> must clip to +theta = 10.0!
				InverseDct8x8(sentDct, sentSpat);
				InverseDct8x8(targetDct, targetSpat);

				for (int by = 8; by < 64; by += 8)
				{
					for (int bx = 8; bx < 64; bx += 8)
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

				// Plant distinctive bar sentinels outside ActiveRect ([0, 2) and [70, 72)) and a huge +200.0 difference
				// in BOTH leading ([2, 8)) AND trailing ([64, 70)) non-8-aligned remainder strips!
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						if (!active.Contains(x, y))
						{
							const double barVal = (x < active.x0 || y < active.y0) ? 16.0 : 988.0;
							cur.at(x, y) = barVal;
							pred.at(x, y) = barVal;
							nextPeak.at(x, y) = barVal;
						}
						else if (x < 8 || x >= 64 || y < 8 || y >= 64)
						{
							// Keep cur continuous with interior (353) while planting a huge +200.0 diff vs pred/nextPeak (153)
							// so BOTH leading [2,8) and trailing [64,70) strips test TransformUnknownBound without creating a fake
							// 18-px G(6.0) D3 cliff tail into interior focus cells.
							cur.at(x, y) = 353.0;
							pred.at(x, y) = 153.0;
							nextPeak.at(x, y) = 153.0;
						}
					}
				}

				auto run8x8 = [&](ResidualMode rm, StandInRegistry& reg)
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.estimator = VarianceEstimator::A2_RoundingCells;
					cfg.residualMode = rm;
					cfg.momentPoolRadiusCells = 0; // 1x1 cell pooling so +200 remainder-strip spikes stay isolated to strip cells
					cfg.cellProjIterations = 1;
					TdV2ReferenceEngine eng(cfg);

					FrameProcessInput in;
					in.seq = 90;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.activeRect = active;
					in.prevPeak = { &pred, &stillMf, true }; // P_B prediction; weight driven by independent nextPeak
					in.nextPeak = { &nextPeak, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = stepDelta;
					in.deadZonePhi = phi;
					in.zeroCoeffFraction = 0.0;
					in.transformIdentified8x8 = true;
					return eng.ProcessFrame(in, &reg);
				};

				StandInRegistry regStrict, regCell;
				const auto resStrict = run8x8(ResidualMode::Strict, regStrict);
				const auto resCell = run8x8(ResidualMode::Cell, regCell);

				// Check interior codec-aligned 8x8 block (bx = 24, by = 24) in DCT domain of coded residual r = output - pred.
				double rStrictSpat[64] = {}, rCellSpat[64] = {}, rStrictDct[64] = {}, rCellDct[64] = {};
				for (int j = 0; j < 8; ++j)
				{
					for (int i = 0; i < 8; ++i)
					{
						rStrictSpat[j * 8 + i] = resStrict.outputClean.at(24 + i, 24 + j) - pred.at(24 + i, 24 + j);
						rCellSpat[j * 8 + i] = resCell.outputClean.at(24 + i, 24 + j) - pred.at(24 + i, 24 + j);
					}
				}
				ForwardDct8x8(rStrictSpat, rStrictDct);
				ForwardDct8x8(rCellSpat, rCellDct);

				Assert::AreEqual(24.0, rStrictDct[0], 1e-6, L"STRICT 8x8 DCT must keep sent DC residual exact");
				Assert::IsTrue(std::fabs(rStrictDct[1]) <= theta + 1e-6, L"STRICT 8x8 DCT must clip zeroed AC to [-theta,+theta]");
				Assert::IsTrue(rCellDct[0] >= 22.0 - 1e-6 && rCellDct[0] <= 34.0 + 1e-6,
					L"CELL 8x8 DCT must keep sent DC inside quantization cell I_2=[22,34]");
				Assert::IsTrue(std::fabs(rCellDct[0] - 24.0) > 1e-3,
					L"CELL 8x8 DCT must actually utilize rounding-cell room unlike STRICT");
				Assert::IsTrue(std::fabs(rCellDct[1]) <= theta + 1e-6, L"CELL 8x8 DCT must clip zeroed AC to [-theta,+theta]");

				// Verify BOTH leading ([2, 8)) AND trailing ([64, 70)) remainder strips are STRICTLY BOUNDED (NOT unbounded ~200!)
				// and engaged TransformUnknownBound visibly, while bars ([0, 2) and [70, 72)) stay bit-exact!
				const RoundingCellStats st = ComputeRoundingCellStats(stepDelta, phi, 0.0);
				// Exact Parseval passband supremum ||H_b||_inf^2 (M3): L=1.0, M=midPassbandPeak^2, H=1.0.
				const double s1Sq = VpUnit::BandFromBase::kSigmaFine * VpUnit::BandFromBase::kSigmaFine;
				const double s2Sq = VpUnit::BandFromBase::kSigmaBase * VpUnit::BandFromBase::kSigmaBase;
				const double wStarSq = (2.0 * std::log(VpUnit::BandFromBase::kSigmaBase / VpUnit::BandFromBase::kSigmaFine)) / (s2Sq - s1Sq);
				const double midPassbandPeak = std::exp(-0.5 * s1Sq * wStarSq) - std::exp(-0.5 * s2Sq * wStarSq);
				const double maxPixelBound = kPixelBoundSigma * (
					std::sqrt(st.totalVar * 1.0) +
					std::sqrt(st.totalVar * (midPassbandPeak * midPassbandPeak)) +
					std::sqrt(st.totalVar * 1.0));
				Assert::IsTrue(regStrict.IsActive(StandInKind::TransformUnknownBound),
					L"STRICT leading/trailing partial strips outside codec 8x8 grid must engage TransformUnknownBound visibly");
				Assert::IsTrue(regCell.IsActive(StandInKind::TransformUnknownBound),
					L"CELL leading/trailing partial strips outside codec 8x8 grid must engage TransformUnknownBound visibly");
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						if (!active.Contains(x, y))
						{
							Assert::AreEqual(cur.at(x, y), resStrict.outputY.at(x, y), 0.0,
								L"Bars outside ActiveRect ([0,2) and [70,72)) must remain bit-exact in STRICT mode");
							Assert::AreEqual(cur.at(x, y), resCell.outputY.at(x, y), 0.0,
								L"Bars outside ActiveRect ([0,2) and [70,72)) must remain bit-exact in CELL mode");
						}
						else if (x < 8 || x >= 64 || y < 8 || y >= 64)
						{
							const double absDeltaStrict = std::fabs(resStrict.outputY.at(x, y) - cur.at(x, y));
							const double absDeltaCell = std::fabs(resCell.outputY.at(x, y) - cur.at(x, y));
							Assert::IsTrue(absDeltaStrict <= maxPixelBound + 1e-6,
								L"Leading/trailing non-8-aligned remainder strip MUST be bounded in STRICT mode!");
							Assert::IsTrue(absDeltaCell <= maxPixelBound + 1e-6,
								L"Leading/trailing non-8-aligned remainder strip MUST be bounded in CELL mode!");
						}
					}
				}

				// ---- Part C: Genuine Multi-Iteration POCS (2 iterations vs 1 iteration) ----
				// Near-black 8x8 blocks: pred = 0.5, sentRes has DC = 20.0 (k=1 -> cur = 3.0 everywhere),
				// nextPeak has strong zero-mean square-wave AC pushing right-half pixels below 0.0.
				// Iter 1 Set 2 clamps negative pixels up to 0.0 (raising DC > 20.0); Iter 2 Set 1 pulls DC back toward 20.0!
				{
					const int pw = 32, ph = 32;
					const CellMotionField mfP = CellMotionField::Uniform(2, 2, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					TdV2Plane pPred(pw, ph, 0.5);
					TdV2Plane pCur(pw, ph, 3.0); // sentRes = +2.5 everywhere -> DC = 8 * 2.5 = 20.0 (k = 1 for stepDelta=20)
					TdV2Plane pNext(pw, ph, 3.0);
					for (int y = 0; y < ph; ++y)
						for (int x = 0; x < pw; ++x)
							pNext.at(x, y) = 3.0 + (((x % 8) < 4) ? 18.0 : -18.0); // zero-mean AC across each 8x8 block

					auto runPocs = [&](int iters)
					{
						Config cfg = MakePreset(Preset::Reference, Mode::On);
						cfg.estimator = VarianceEstimator::A2_RoundingCells;
						cfg.residualMode = ResidualMode::Strict;
						cfg.cellProjIterations = iters;
						TdV2ReferenceEngine eng(cfg);
						StandInRegistry reg;

						FrameProcessInput in;
						in.seq = 91;
						in.receivedY = pCur;
						in.hasCleanSplit = true;
						in.cleanY = pCur;
						in.prevPeak = { &pPred, &mfP, true }; // P_B prediction; weight driven by nextPeak
						in.nextPeak = { &pNext, &mfP, false };
						in.hasStepPrior = true;
						in.estimatedStepDelta = 20.0;
						in.deadZonePhi = kDeadZonePhiInter;
						in.zeroCoeffFraction = 0.0;
						in.transformIdentified8x8 = true;
						return eng.ProcessFrame(in, &reg);
					};

					const auto rIter1 = runPocs(1);
					const auto rIter2 = runPocs(2);

					double maxDiff12 = 0.0;
					for (size_t i = 0; i < rIter1.outputClean.v.size(); ++i)
					{
						Assert::IsTrue(rIter2.outputClean.v[i] >= -1e-9 && rIter2.outputClean.v[i] <= 1023.0 + 1e-9,
							L"POCS output must stay inside valid code range [0, PeakCodeForBits]");
						maxDiff12 = (std::max)(maxDiff12, std::fabs(rIter2.outputClean.v[i] - rIter1.outputClean.v[i]));
					}
					Assert::IsTrue(maxDiff12 > 1e-4,
						L"Multi-iteration POCS (iters=2) must genuinely differ from iters=1 when DCT cell & [0,peak] clamp interact!");

					// Measure DC residual distance to sent DC=20.0 on interior block (8,8): iter 2 must pull DC closer!
					double sp1[64] = {}, sp2[64] = {}, d1[64] = {}, d2[64] = {};
					for (int j = 0; j < 8; ++j)
						for (int i = 0; i < 8; ++i)
						{
							sp1[j * 8 + i] = rIter1.outputClean.at(8 + i, 8 + j) - pPred.at(8 + i, 8 + j);
							sp2[j * 8 + i] = rIter2.outputClean.at(8 + i, 8 + j) - pPred.at(8 + i, 8 + j);
						}
					ForwardDct8x8(sp1, d1);
					ForwardDct8x8(sp2, d2);
					Assert::IsTrue(std::fabs(d2[0] - 20.0) < std::fabs(d1[0] - 20.0),
						L"2nd POCS iteration must strictly tighten DCT-cell consistency after Set-2 pixel clamping");
				}
			}

			TEST_METHOD(AdversarialOobNonFiniteSilentFallbackTrapsAndResolutionChange)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;

				const TdV2Plane master = RenderMaster(w, h);
				const TdV2Plane ref1 = AddCodingError(master, 1.5, 1001);
				const TdV2Plane ref2 = AddCodingError(master, 1.5, 1002);
				const TdV2Plane cur = AddCodingError(master, 6.0, 1003);
				const CellMotionField goodMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// 1. Malformed / short CellMotionField vectors & dimension mismatch -> ZERO crash, ref rejected visibly!
				{
					CellMotionField shortMf;
					shortMf.cellsW = cw;
					shortMf.cellsH = ch;
					shortMf.vx.assign(2, 0.0); // truncated vector!
					shortMf.vy.assign(2, 0.0);
					shortMf.camVx.assign(2, 0.0);
					shortMf.camVy.assign(2, 0.0);
					shortMf.cls.assign(2, RegionMotionClass::Still);
					shortMf.valid.assign(2, 1u);

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 200;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &ref1, &shortMf, false };
					in.nextPeak = { &ref2, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Malformed motion field must not corrupt output");
					Assert::AreEqual(static_cast<size_t>(1), res.activeRefCount, L"Malformed motion ref must be rejected");
					Assert::IsTrue(res.motionStandInRefs >= 1);
					Assert::IsTrue(reg.IsActive(StandInKind::MotionFieldStandIn));
				}

				// 2. Null CellMotionField -> H9: NEVER whole-frame Still blend! Must engage MissingMotionNoWholeBlend visibly!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 201;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &ref1, nullptr, false };
					in.nextPeak = { &ref2, nullptr, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Null motion field must produce finite output");
					Assert::AreEqual(static_cast<size_t>(2), res.missingMotionSkippedRefs,
						L"Null motion refs must be visibly counted in missingMotionSkippedRefs");
					Assert::IsTrue(reg.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"Null motion field must visibly engage StandInKind::MissingMotionNoWholeBlend");
				}

				// 3. cfg_.grainCellPx <= 0 (and cellPx <= 0) -> ZERO divide-by-zero crash!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.grainCellPx = 0;
					cfg.cellPx = 0;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 202;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.retainedGrainVar.assign(static_cast<size_t>(cw) * ch, 4.0);
					in.prevPeak = { &ref1, &goodMf, false };
					in.nextPeak = { &ref2, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res0 = eng.ProcessFrame(in, &reg);
					AssertAllFinite(res0.outputY, L"grainCellPx==0 must not divide by zero");

					cfg.grainCellPx = -16;
					eng.SetConfig(cfg);
					const auto resNeg = eng.ProcessFrame(in, &reg);
					AssertAllFinite(resNeg.outputY, L"Negative grainCellPx must not divide by zero");
				}

				// 4. Non-finite pixels in prevPeak.cleanPlane and nextPeak.cleanPlane -> ZERO NaN output + visible counter!
				{
					TdV2Plane nanPrev = ref1;
					TdV2Plane infNext = ref2;
					nanPrev.at(20, 20) = qnan;
					infNext.at(40, 40) = inf;

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 203;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &nanPrev, &goodMf, false };
					in.nextPeak = { &infNext, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Non-finite reference pixels must NEVER leak NaN/Inf into outputY");
					AssertAllFinite(res.outputClean, L"Non-finite reference pixels must NEVER leak NaN/Inf into outputClean");
					Assert::IsTrue(res.nonFiniteCells > 0);
					Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));
				}

				// 5. Non-finite retainedGrainVar -> ZERO NaN output + visible counter!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 204;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.retainedGrainVar.assign(static_cast<size_t>(cw) * ch, 1.0);
					in.retainedGrainVar[3] = qnan;
					in.retainedGrainVar[5] = -inf;
					in.prevPeak = { &ref1, &goodMf, false };
					in.nextPeak = { &ref2, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Non-finite retainedGrainVar must not corrupt output");
					Assert::IsTrue(res.nonFiniteCells >= 2);
					Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));
				}

				// 6. Non-finite motion vectors -> ZERO NaN output + visible counter + affected cell gated closed!
				{
					CellMotionField badVecMf = goodMf;
					badVecMf.vx[static_cast<size_t>(1) * cw + 1] = qnan;
					badVecMf.camVy[static_cast<size_t>(2) * cw + 2] = inf;

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 205;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &ref1, &badVecMf, false };
					in.nextPeak = { &ref2, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Non-finite motion vectors must not corrupt output");
					Assert::IsTrue(res.nonFiniteCells >= 2);
					Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));
				}

				// 7. Single-ref K == 1 with A1_ThreeCorneredHat selected -> visible SingleRefA1ToA2Fallback!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.estimator = VarianceEstimator::A1_ThreeCorneredHat;
					cfg.enableCarryForward = false;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 206;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &ref1, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Single-ref A1 fallback must produce finite output");
					Assert::AreEqual(static_cast<size_t>(1), res.activeRefCount);
					Assert::IsTrue(res.singleRefA2Cells > 0);
					Assert::IsTrue(reg.IsActive(StandInKind::SingleRefA1ToA2Fallback));
				}

				// 8. Tiny-scale PSD covariance cell (C_00 = 2.5e-9, C_11 = 0, reg >= 1e-9 >> kSolvePivotFloor = 1e-12):
				// Because empirical pixel differences d_k = y_k - x always form a symmetric positive semi-definite
				// Gram matrix C = D^T D / N >= 0, adding Tikhonov regularization reg*I (reg >= kSolveNumericalFloor = 1e-9,
				// three orders of magnitude above kSolvePivotFloor = 1e-12) guarantees strictly positive-definite (SPD)
				// C + reg*I for ALL valid finite pixel covariances — so real pixel inputs solve cleanly and finitely
				// without false singularity! Direct rank-deficient / indefinite SingularSolveClosure verification is
				// enforced in TdV2BlendMathTests::NaNInfAndSingularCovarianceCloseAllWeightsToZeroVisibly.
				{
					TdV2Plane flatCur(w, h, 400.0);
					TdV2Plane flatRefEps(w, h, 400.0 + 5e-5);
					TdV2Plane flatRefZero(w, h, 400.0);

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.enableCarryForward = false;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 207;
					in.receivedY = flatCur;
					in.hasCleanSplit = true;
					in.cleanY = flatCur;
					in.prevPeak = { &flatRefEps, &goodMf, false };
					in.nextPeak = { &flatRefZero, &goodMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Tiny-scale PSD covariance cell must produce finite output");
					Assert::AreEqual(static_cast<size_t>(0), res.singularCells,
						L"Well-conditioned regularized PSD covariance must NOT falsely report singularity");
					Assert::IsFalse(reg.IsActive(StandInKind::SingularSolveClosure),
						L"Well-conditioned regularized PSD covariance must NOT falsely engage SingularSolveClosure");
					for (size_t i = 0; i < flatCur.v.size(); ++i)
						Assert::IsTrue(std::fabs(res.outputY.v[i] - flatCur.v[i]) <= 5e-5 + 1e-12,
							L"Convex solve on tiny-scale PSD refs must stay strictly inside convex hull of inputs");
				}

				// 9 & 10. hasStepPrior == false AND enableLongAnchor == true -> BOTH stand-ins visibly engaged!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.enableLongAnchor = true;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 208;
					in.receivedY = cur;
					in.hasCleanSplit = true;
					in.cleanY = cur;
					in.prevPeak = { &ref1, &goodMf, false };
					in.nextPeak = { &ref2, &goodMf, false };
					in.hasStepPrior = false;
					const auto res = eng.ProcessFrame(in, &reg);

					AssertAllFinite(res.outputY, L"Default step prior + long anchor stand-in must produce finite output");
					Assert::IsTrue(reg.IsActive(StandInKind::StepPriorDefault),
						L"hasStepPrior == false must engage StepPriorDefault visibly");
					Assert::IsTrue(reg.IsActive(StandInKind::LongPairDrift),
						L"enableLongAnchor == true must engage LongPairDrift visibly");
				}

				// 11. Resolution Change Carry Reset (64x64 -> 96x80 without isCutOrSpanInvalid flag)!
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput fSmall;
					fSmall.seq = 210;
					fSmall.receivedY = cur;
					fSmall.hasCleanSplit = true;
					fSmall.cleanY = cur;
					eng.ProcessFrame(fSmall, &reg);
					Assert::IsTrue(eng.HasCarry());
					Assert::AreEqual(64, eng.CarryClean().w);
					Assert::AreEqual(64, eng.CarryClean().h);

					const TdV2Plane bigPlane = RenderMaster(96, 80);
					FrameProcessInput fBig;
					fBig.seq = 211;
					fBig.receivedY = bigPlane;
					fBig.hasCleanSplit = true;
					fBig.cleanY = bigPlane;
					fBig.isCutOrSpanInvalid = false;
					const auto resBig = eng.ProcessFrame(fBig, &reg);

					AssertAllFinite(resBig.outputY, L"Resolution change frame must produce finite output");
					Assert::IsTrue(resBig.carryReset, L"Resolution change MUST set res.carryReset == true");
					Assert::IsFalse(resBig.carryUsed, L"Stale different-resolution carry MUST NOT be used");
					Assert::AreEqual(96, eng.CarryClean().w);
					Assert::AreEqual(80, eng.CarryClean().h);
				}
			}

			TEST_METHOD(SceneCutAndNonFiniteInputsResetCleanlyWithZeroGhosting)
			{
				const int w = 64, h = 64;
				const TdV2Plane sceneA = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane sceneB(w, h, 800.0); // Completely different bright scene after cut!

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				// Populate recurrent carry with scene A.
				FrameProcessInput fA;
				fA.seq = 1;
				fA.receivedY = sceneA;
				fA.hasCleanSplit = true;
				fA.cleanY = sceneA;
				eng.ProcessFrame(fA, &reg);
				Assert::IsTrue(eng.HasCarry());

				// Mandatory In-Test Negative Control (Rule 3):
				// Unprotected cross-cut blending on the EXACT SAME (sceneA, sceneB) inputs ghosts scene A into scene B!
				{
					TdV2Plane naiveCrossCut(w, h, 0.0);
					for (size_t i = 0; i < sceneB.v.size(); ++i)
						naiveCrossCut.v[i] = 0.5 * (sceneA.v[i] + sceneB.v[i]);
					const double naiveGhostRmse = std::sqrt(PlaneMse(sceneB, naiveCrossCut, ActiveRect::Full(w, h)));
					Assert::IsTrue(naiveGhostRmse > 150.0,
						L"Negative control: unprotected cross-cut blend must severely ghost sceneA into sceneB");

					// Also verify running the engine WITHOUT isCutOrSpanInvalid on the exact same inputs DOES blend carry/ref!
					const CellMotionField stillMf = CellMotionField::Uniform(w / 16, h / 16, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					Config noCutCfg = MakePreset(Preset::Reference, Mode::On);
					noCutCfg.estimator = VarianceEstimator::A2_RoundingCells;
					TdV2ReferenceEngine noCutEng(noCutCfg);
					StandInRegistry noCutReg;
					noCutEng.ProcessFrame(fA, &noCutReg);

					FrameProcessInput fNoCut;
					fNoCut.seq = 2;
					fNoCut.receivedY = sceneB;
					fNoCut.hasCleanSplit = true;
					fNoCut.cleanY = sceneB;
					fNoCut.prevPeak = { &sceneA, &stillMf, false };
					fNoCut.hasCarryMotion = true;
					fNoCut.carryMotion = stillMf;
					fNoCut.hasStepPrior = true;
					fNoCut.estimatedStepDelta = 24.0;
					fNoCut.isCutOrSpanInvalid = false;
					const auto noCutRes = noCutEng.ProcessFrame(fNoCut, &noCutReg);
					const double engNoCutGhost = std::sqrt(PlaneMse(sceneB, noCutRes.outputY, ActiveRect::Full(w, h)));
					Assert::IsTrue(noCutRes.carryUsed && noCutRes.activeRefCount >= 1 && engNoCutGhost > 0.1,
						L"Negative control: omitting isCutOrSpanInvalid on exact same sceneA->sceneB leaks carry/ref ghosting");
				}

				// Cut to scene B -> MUST reset carry and NEVER ghost scene A into scene B!
				FrameProcessInput fCut;
				fCut.seq = 2;
				fCut.receivedY = sceneB;
				fCut.hasCleanSplit = true;
				fCut.cleanY = sceneB;
				fCut.prevPeak.cleanPlane = &sceneA; // stale pre-cut ref must be ignored when isCutOrSpanInvalid == true
				fCut.isCutOrSpanInvalid = true;
				const auto cutRes = eng.ProcessFrame(fCut, &reg);

				Assert::IsTrue(cutRes.carryReset);
				Assert::IsFalse(cutRes.carryUsed);
				Assert::AreEqual(static_cast<size_t>(0), cutRes.activeRefCount);
				for (size_t i = 0; i < sceneB.v.size(); ++i)
					Assert::AreEqual(800.0, cutRes.outputY.v[i], 0.0, L"Scene cut must have zero ghosting from previous scene");

				// Non-finite receivedY closes cleanly without crashing or poisoning carry!
				TdV2Plane nanPlane = sceneB;
				nanPlane.at(10, 10) = std::numeric_limits<double>::quiet_NaN();
				FrameProcessInput fNan;
				fNan.seq = 3;
				fNan.receivedY = nanPlane;
				fNan.hasCleanSplit = true;
				fNan.cleanY = nanPlane;
				const auto nanRes = eng.ProcessFrame(fNan, &reg);
				Assert::IsTrue(nanRes.carryReset);
				Assert::IsTrue(nanRes.nonFiniteCells > 0);
				Assert::IsFalse(eng.HasCarry(), L"Non-finite frame must reset carry cleanly");
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));

				// Non-finite cleanY (with finite receivedY) also resets carry and closes visibly!
				eng.ProcessFrame(fA, &reg);
				Assert::IsTrue(eng.HasCarry());
				StandInRegistry regCleanNan;
				FrameProcessInput fCleanNan;
				fCleanNan.seq = 4;
				fCleanNan.receivedY = sceneB;
				fCleanNan.hasCleanSplit = true;
				fCleanNan.cleanY = nanPlane;
				const auto cleanNanRes = eng.ProcessFrame(fCleanNan, &regCleanNan);
				Assert::IsTrue(cleanNanRes.carryReset);
				Assert::IsTrue(cleanNanRes.nonFiniteCells > 0);
				Assert::IsFalse(eng.HasCarry(), L"Non-finite cleanY must reset carry cleanly");
				Assert::IsTrue(regCleanNan.IsActive(StandInKind::NonFiniteClosure));
			}

			// =========================================================================
			// 12a. H13a — Pass-1 Masked Face Test (Plan Q2 / s1.3 / s3.3):
			//      Moving faces supplied via `in.faceMask` MUST have exact bit-for-bit
			//      zero delta (`moving face delta hash == 0`), engage FaceNonRigidMask,
			//      and report `faceMaskedCells > 0`, while unmasked still background
			//      is repaired and an unmasked negative control proves the face WOULD
			//      be modified without `faceMask`.
			// =========================================================================
			TEST_METHOD(Pass1MaskedFaceZeroDeltaWithUnmaskedNegativeControl)
			{
				const int w = 64, h = 64;
				const int cellPx = 16;
				const int cw = w / cellPx, ch = h / cellPx; // 4x4 cells

				auto renderPortrait = [&](double eyelidShiftY) -> TdV2Plane
				{
					TdV2Plane p(w, h, 0.0);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double dx = static_cast<double>(x) - 32.0;
							const double dy = static_cast<double>(y) - 32.0;
							const double shading = 470.0 + 42.0 * std::cos(2.0 * kPi * dx / 96.0) * std::cos(2.0 * kPi * dy / 88.0);
							const double skinTex = 9.0 * std::sin(2.0 * kPi * (x / 7.3 + y / 9.1) + 0.3)
							                     + 7.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 6.7) + 1.4);
							double val = shading + skinTex;
							if (x >= 34 && x < 46)
							{
								const double cx = static_cast<double>(x) - 39.5;
								const double arch = 0.04 * cx * cx;
								const double centerY = 39.0 + eyelidShiftY + arch;
								const double distCrease = static_cast<double>(y) - centerY;
								const double distHighlight = static_cast<double>(y) - (centerY - 1.6);
								const double horizTaper = std::exp(-0.5 * (cx / 4.2) * (cx / 4.2));
								val -= 175.0 * horizTaper * std::exp(-0.5 * (distCrease / 0.75) * (distCrease / 0.75));
								val +=  65.0 * horizTaper * std::exp(-0.5 * (distHighlight / 0.75) * (distHighlight / 0.75));
							}
							p.at(x, y) = val;
						}
					}
					return p;
				};

				const TdV2Plane refMaster = renderPortrait(0.0);
				const TdV2Plane curMaster = renderPortrait(2.0);
				const TdV2Plane curReceived = AddCodingError(curMaster, 7.0, 1201u, 0.15);
				const TdV2Plane prevPeak = AddCodingError(refMaster, 1.2, 1202u, 0.0);
				const TdV2Plane nextPeak = AddCodingError(refMaster, 1.2, 1203u, 0.0);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Mask the 2x2 face block covering cells (2,2), (3,2), (2,3), (3,3) -> [32..64) x [32..64).
				std::vector<uint8_t> faceMask(static_cast<size_t>(cw) * ch, 0u);
				size_t expectedMaskedCells = 0;
				for (int cy = 2; cy < 4; ++cy)
				{
					for (int cx = 2; cx < 4; ++cx)
					{
						faceMask[static_cast<size_t>(cy) * cw + cx] = 1u;
						++expectedMaskedCells;
					}
				}
				const ActiveRect bgRect{ 2, 2, 30, 30 };

				struct FaceModeCase
				{
					bool transform8x8;
					ResidualMode resMode;
					const wchar_t* label;
				};
				const FaceModeCase modeCases[3] = {
					{ false, ResidualMode::Cell,   L"TransformUnknown-CELL" },
					{ true,  ResidualMode::Strict, L"8x8DCT-STRICT" },
					{ true,  ResidualMode::Cell,   L"8x8DCT-CELL" }
				};

				const double curBgMse = PlaneMse(curReceived, curMaster, bgRect);

				for (const FaceModeCase& mc : modeCases)
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.residualMode = mc.resMode;
					cfg.cellProjIterations = 2;

					FrameProcessInput baseIn;
					baseIn.seq = 120;
					baseIn.receivedY = curReceived;
					baseIn.hasCleanSplit = true;
					baseIn.cleanY = curReceived;
					baseIn.activeRect = ActiveRect::Full(w, h);
					baseIn.prevPeak = { &prevPeak, &stillMf, false };
					baseIn.nextPeak = { &nextPeak, &stillMf, false };
					baseIn.hasStepPrior = true;
					// Step delta = 24 (theta = 20 ~= 2.8*sigma for sigma=7 background noise) so ~95% of residual
					// DCT coefficients sit inside the dead-zone [-theta, +theta] where 8x8DCT-STRICT lawfully repairs >= 50% MSE.
					baseIn.estimatedStepDelta = 24.0;
					baseIn.deadZonePhi = kDeadZonePhiInter;
					baseIn.zeroCoeffFraction = 0.35;
					baseIn.transformIdentified8x8 = mc.transform8x8;

					// Mandatory In-Test Negative Control (Rule 3):
					// Run EXACT SAME inputs with faceMask == nullptr -> prove face pixels WOULD be modified!
					TdV2ReferenceEngine engUnmasked(cfg);
					StandInRegistry regUnmasked;
					const FrameProcessResult resUnmasked = engUnmasked.ProcessFrame(baseIn, &regUnmasked);
					Assert::AreEqual(static_cast<size_t>(0), resUnmasked.faceMaskedCells);
					Assert::IsFalse(regUnmasked.IsActive(StandInKind::FaceNonRigidMask));
					double maxUnmaskedFaceDelta = 0.0;
					for (int y = 32; y < 64; ++y)
						for (int x = 32; x < 64; ++x)
							maxUnmaskedFaceDelta = (std::max)(maxUnmaskedFaceDelta,
								std::fabs(resUnmasked.outputY.at(x, y) - curReceived.at(x, y)));
					Assert::IsTrue(maxUnmaskedFaceDelta > 1.0,
						(std::wstring(mc.label) + L": Negative control — unmasked face MUST be modified by engine").c_str());

					// Protected Pass-1 Masked Face run: supply in.faceMask!
					FrameProcessInput maskedIn = baseIn;
					maskedIn.faceMask = &faceMask;
					TdV2ReferenceEngine engMasked(cfg);
					StandInRegistry regMasked;
					const FrameProcessResult resMasked = engMasked.ProcessFrame(maskedIn, &regMasked);
					AssertAllFinite(resMasked.outputY, L"Masked face outputY must be finite");

					Assert::IsTrue(regMasked.IsActive(StandInKind::FaceNonRigidMask),
						(std::wstring(mc.label) + L": FaceNonRigidMask stand-in must be visibly active").c_str());
					Assert::AreEqual(expectedMaskedCells, resMasked.faceMaskedCells,
						(std::wstring(mc.label) + L": faceMaskedCells must match exact count of masked face cells").c_str());

					// Exact bit-for-bit zero delta on EVERY pixel of EVERY masked face cell (delta hash == 0)!
					for (int y = 32; y < 64; ++y)
					{
						for (int x = 32; x < 64; ++x)
						{
							Assert::AreEqual(curReceived.at(x, y), resMasked.outputY.at(x, y), 0.0,
								(std::wstring(mc.label) + L": Pass-1 masked face pixel MUST have exact bit-for-bit zero delta").c_str());
						}
					}

					// Non-vacuity: unmasked still background MUST still genuinely repair coding noise!
					const double tdBgMse = PlaneMse(resMasked.outputY, curMaster, bgRect);
					Assert::IsTrue(tdBgMse < curBgMse * 0.50,
						(std::wstring(mc.label) + L": Unmasked still background must still achieve >= 50% MSE reduction").c_str());
				}
			}

			// =========================================================================
			// 12b. H13b — Unmasked Subtle Non-Face Organic Micro-Motion Test:
			//      On unmasked subtle non-face micro-motion near a Still cell edge,
			//      `cfg.enablePerCornerHuber = true` enforces strict no-harm
			//      (`tdMicroRmse <= curMicroRmse`, `<= 1%` of blocks worsened `> 10%`
			//      per plan s3.3, ZERO arbitrary constants), while running the EXACT
			//      SAME engine with `cfg.enablePerCornerHuber = false` as the negative
			//      control proves disabling per-corner Huber degrades micro-motion worse!
			// =========================================================================
			TEST_METHOD(UnmaskedSubtleMicroMotionNoHarmWithHuberOffNegativeControl)
			{
				const int w = 160, h = 160; // 10 x 10 = 100 canonical 16x16 blocks (exact 1% granularity for plan s3.3!)
				const int cellPx = 16;
				const int cw = w / cellPx, ch = h / cellPx;

				// Organic textured surface with a localized subtle non-rigid crease/fold near the left/top
				// interior boundary of Still cell (5, 5) ([80..96) x [80..96)) shifting by +2.0 px vertically.
				auto renderOrganicScene = [&](double foldShiftY) -> TdV2Plane
				{
					TdV2Plane p(w, h, 0.0);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double base = 460.0 + 38.0 * std::cos(2.0 * kPi * x / 128.0) * std::cos(2.0 * kPi * y / 112.0);
							const double tex = 9.0 * std::sin(2.0 * kPi * (x / 7.3 + y / 9.1) + 0.3)
							                 + 7.0 * std::cos(2.0 * kPi * (x / 5.1 - y / 6.7) + 1.4);
							double val = base + tex;
							// Subtle non-face fold localized near the left boundary of cell (5, 5): x in [80, 90), y in [82, 92).
							if (x >= 80 && x < 90)
							{
								const double cx = static_cast<double>(x) - 84.5;
								const double centerY = 85.0 + foldShiftY;
								const double dist = static_cast<double>(y) - centerY;
								const double horizTaper = std::exp(-0.5 * (cx / 3.2) * (cx / 3.2));
								val -= 140.0 * horizTaper * std::exp(-0.5 * (dist / 0.75) * (dist / 0.75));
								val +=  50.0 * horizTaper * std::exp(-0.5 * ((dist + 1.6) / 0.75) * ((dist + 1.6) / 0.75));
							}
							p.at(x, y) = val;
						}
					}
					return p;
				};

				const TdV2Plane refMaster = renderOrganicScene(0.0);
				const TdV2Plane curMaster = renderOrganicScene(2.0);
				const TdV2Plane curReceived = AddCodingError(curMaster, 7.0, 1251u, 0.0);
				const TdV2Plane prevPeak = AddCodingError(refMaster, 1.2, 1252u, 0.0);
				const TdV2Plane nextPeak = AddCodingError(refMaster, 1.2, 1253u, 0.0);
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				// Evaluate micro-motion over cell (5, 5) [80..96)^2 and surrounding stationary background [16..64)^2.
				const ActiveRect microCellRect{ 80, 80, 96, 96 };
				const ActiveRect bgRect{ 16, 16, 64, 64 };
				const double curMicroRmse = std::sqrt(PlaneMse(curReceived, curMaster, microCellRect));
				const double curBgMse = PlaneMse(curReceived, curMaster, bgRect);

				auto runPreset = [&](Preset preset, bool enablePerCornerHuber) -> FrameProcessResult
				{
					Config cfg = MakePreset(preset, Mode::On);
					cfg.enablePerCornerHuber = enablePerCornerHuber;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 125;
					in.receivedY = curReceived;
					in.hasCleanSplit = true;
					in.cleanY = curReceived;
					in.prevPeak = { &prevPeak, &stillMf, false };
					in.nextPeak = { &nextPeak, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 14.0;
					in.deadZonePhi = kDeadZonePhiInter;
					in.zeroCoeffFraction = 0.20;
					return eng.ProcessFrame(in, &reg);
				};

				auto countBlocksWorsenedOver10Pct = [&](const TdV2Plane& out) -> size_t
				{
					size_t bad = 0;
					for (int cy = 0; cy < ch; ++cy)
					{
						for (int cx = 0; cx < cw; ++cx)
						{
							const ActiveRect br{ cx * cellPx, cy * cellPx, (cx + 1) * cellPx, (cy + 1) * cellPx };
							const double baseR = std::sqrt(PlaneMse(curReceived, curMaster, br));
							const double outR = std::sqrt(PlaneMse(out, curMaster, br));
							if (outR > 1.10 * baseR)
								++bad;
						}
					}
					return bad;
				};

				// 1. Preset::RealTime (poolR = 0): per-corner Huber ON vs exact same engine with per-corner Huber OFF!
				const FrameProcessResult resRtOn = runPreset(Preset::RealTime, /*enablePerCornerHuber=*/true);
				const FrameProcessResult resRtOff = runPreset(Preset::RealTime, /*enablePerCornerHuber=*/false);
				AssertAllFinite(resRtOn.outputY, L"RealTime Huber-ON output must be finite");
				AssertAllFinite(resRtOff.outputY, L"RealTime Huber-OFF output must be finite");

				const double rtOnMicroRmse = std::sqrt(PlaneMse(resRtOn.outputY, curMaster, microCellRect));
				const double rtOffMicroRmse = std::sqrt(PlaneMse(resRtOff.outputY, curMaster, microCellRect));
				const double rtOnBgMse = PlaneMse(resRtOn.outputY, curMaster, bgRect);
				const size_t rtOnBadBlocks = countBlocksWorsenedOver10Pct(resRtOn.outputY);
				const double rtOnBadFrac = static_cast<double>(rtOnBadBlocks) / static_cast<double>(cw * ch);

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Unmasked micro-motion (RT poolR=0): curRmse=%.3f | huberON=%.3f (badBlocks=%zu/100) | huberOFF=%.3f",
					curMicroRmse, rtOnMicroRmse, rtOnBadBlocks, rtOffMicroRmse);
				Logger::WriteMessage(buf);

				// Mandatory In-Test Negative Control (Rule 3): Exact same engine with enablePerCornerHuber = false!
				Assert::IsTrue(rtOffMicroRmse > rtOnMicroRmse,
					L"Negative control: disabling enablePerCornerHuber MUST worsen micro-motion RMSE on exact same inputs");
				Assert::IsTrue(rtOffMicroRmse > curMicroRmse,
					L"Negative control: disabling enablePerCornerHuber MUST violate strict no-harm on cell-boundary micro-motion");

				// Protected engine assertions (strict no-harm + plan s3.3 <= 1% blocks worsened > 10%, ZERO arbitrary magic constants):
				Assert::IsTrue(rtOnMicroRmse <= curMicroRmse,
					L"enablePerCornerHuber=true MUST satisfy strict no-harm (tdMicroRmse <= curMicroRmse) on unmasked micro-motion");
				Assert::IsTrue(rtOnBadFrac <= 0.01,
					L"Plan s3.3 safety bar: <= 1% of blocks may worsen error by > 10%");
				Assert::IsTrue(rtOnBgMse < curBgMse * 0.50,
					L"Non-vacuity: stationary surrounding cells must still achieve >= 50% MSE reduction");

				// 2. Preset::Reference (poolR = 1): verify strict no-harm + <= 1% worsened blocks AND Huber-off negative control!
				const FrameProcessResult resRefOn = runPreset(Preset::Reference, /*enablePerCornerHuber=*/true);
				const FrameProcessResult resRefOff = runPreset(Preset::Reference, /*enablePerCornerHuber=*/false);
				AssertAllFinite(resRefOn.outputY, L"Reference Huber-ON output must be finite");
				AssertAllFinite(resRefOff.outputY, L"Reference Huber-OFF output must be finite");
				const double refOnMicroRmse = std::sqrt(PlaneMse(resRefOn.outputY, curMaster, microCellRect));
				const double refOffMicroRmse = std::sqrt(PlaneMse(resRefOff.outputY, curMaster, microCellRect));
				const double refOnBgMse = PlaneMse(resRefOn.outputY, curMaster, bgRect);
				const size_t refOnBadBlocks = countBlocksWorsenedOver10Pct(resRefOn.outputY);
				const double refOnBadFrac = static_cast<double>(refOnBadBlocks) / static_cast<double>(cw * ch);
				Assert::IsTrue(refOffMicroRmse > refOnMicroRmse,
					L"Reference negative control: disabling enablePerCornerHuber MUST worsen micro-motion RMSE on exact same inputs");
				Assert::IsTrue(refOffMicroRmse > curMicroRmse,
					L"Reference negative control: disabling enablePerCornerHuber MUST violate strict no-harm on cell-boundary micro-motion");
				Assert::IsTrue(refOnMicroRmse <= curMicroRmse,
					L"Reference preset with enablePerCornerHuber=true MUST satisfy strict no-harm on unmasked micro-motion");
				Assert::IsTrue(refOnBadFrac <= 0.01,
					L"Reference preset: <= 1% of blocks may worsen error by > 10% (plan s3.3)");
				Assert::IsTrue(refOnBgMse < curBgMse * 0.50,
					L"Reference preset: stationary surrounding cells must achieve >= 50% MSE reduction");
			}

			// =========================================================================
			// 13. H9 — Missing Motion Field Never Whole-Blends Moving Content
			// =========================================================================
			TEST_METHOD(MissingMotionFieldNeverWholeBlendsMovingContentWithNegativeControl)
			{
				// Plan s1.2 L108 (L4): no valid vectors -> exact-copy static blocks only, or OFF (NEVER whole-frame "Still" blend!).
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect edgeRect{ 24, 16, 72, 80 };

				auto renderMovingBar = [&](double shiftX) -> TdV2Plane
				{
					TdV2Plane p(w, h, 300.0);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double lx = static_cast<double>(x) - shiftX;
							// High-contrast moving vertical bar (width 20 px, +300 codes) + texture!
							const double bar = (lx >= 38.0 && lx < 58.0) ? 300.0 : 0.0;
							const double tex = 25.0 * std::sin(2.0 * kPi * lx / 9.0) * std::cos(2.0 * kPi * y / 11.0);
							p.at(x, y) = 320.0 + bar + tex;
						}
					}
					return p;
				};

				const TdV2Plane prevFrame = renderMovingBar(-6.0);
				const TdV2Plane curFrame = renderMovingBar(0.0);
				const TdV2Plane nextFrame = renderMovingBar(+6.0);

				// ---- Mandatory In-Test Negative Control (Rule 3): Naive whole-frame Still blend ----
				// Simulates the pre-H9 buggy behavior that treated null motion as CellMotionField::Uniform(..., Still)!
				{
					const CellMotionField naiveStillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
					Config naiveCfg = MakePreset(Preset::Reference, Mode::On);
					naiveCfg.estimator = VarianceEstimator::A2_RoundingCells;
					TdV2ReferenceEngine naiveEng(naiveCfg);
					StandInRegistry naiveReg;

					FrameProcessInput naiveIn;
					naiveIn.seq = 1;
					naiveIn.receivedY = curFrame;
					naiveIn.hasCleanSplit = true;
					naiveIn.cleanY = curFrame;
					naiveIn.prevPeak = { &prevFrame, &naiveStillMf, false };
					naiveIn.nextPeak = { &nextFrame, &naiveStillMf, false };
					naiveIn.hasStepPrior = true;
					naiveIn.estimatedStepDelta = 24.0;
					const auto naiveRes = naiveEng.ProcessFrame(naiveIn, &naiveReg);

					TdV2Plane directAvg(w, h, 0.0);
					for (size_t i = 0; i < curFrame.v.size(); ++i)
						directAvg.v[i] = (curFrame.v[i] + prevFrame.v[i] + nextFrame.v[i]) / 3.0;
					const double directGhostRmse = std::sqrt(PlaneMse(curFrame, directAvg, edgeRect));
					const double stillEngGhostRmse = std::sqrt(PlaneMse(curFrame, naiveRes.outputY, edgeRect));
					Assert::IsTrue(directGhostRmse > 40.0,
						L"Negative control: naive temporal average on moving bar must ghost severely");
					Assert::IsTrue(stillEngGhostRmse > 2.0,
						L"Negative control: forcing Still motion field on moving bar must ghost moving edges");
				}

				// ---- Sub-case A: ref.motion == nullptr on both prevPeak and nextPeak ----
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.enableCarryForward = false;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 10;
					in.receivedY = curFrame;
					in.hasCleanSplit = true;
					in.cleanY = curFrame;
					in.prevPeak = { &prevFrame, nullptr, false };
					in.nextPeak = { &nextFrame, nullptr, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 24.0;

					const auto res = eng.ProcessFrame(in, &reg);
					AssertAllFinite(res.outputY, L"Null-motion output must be finite");
					Assert::IsTrue(reg.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"H9: null motion field MUST visibly engage StandInKind::MissingMotionNoWholeBlend");
					Assert::AreEqual(static_cast<size_t>(2), res.missingMotionSkippedRefs,
						L"H9: both null-motion peak references must be counted in missingMotionSkippedRefs");
					for (size_t i = 0; i < curFrame.v.size(); ++i)
						Assert::AreEqual(curFrame.v[i], res.outputY.v[i], 0.0,
							L"H9: moving content with null motion MUST have exact zero ghosting (no whole-frame Still blend)");
				}

				// ---- Sub-case B: Active carry WITHOUT carryMotion (hasCarryMotion == false) ----
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.enableCarryForward = true;
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					// Seed carry with prevFrame (shifted -6 px).
					FrameProcessInput f1;
					f1.seq = 20;
					f1.receivedY = prevFrame;
					f1.hasCleanSplit = true;
					f1.cleanY = prevFrame;
					eng.ProcessFrame(f1, &reg);
					Assert::IsTrue(eng.HasCarry());

					// Frame 2: curFrame with hasCarryMotion == false and no peaks -> carry MUST NOT whole-blend as Still!
					StandInRegistry reg2;
					FrameProcessInput f2;
					f2.seq = 21;
					f2.receivedY = curFrame;
					f2.hasCleanSplit = true;
					f2.cleanY = curFrame;
					f2.hasCarryMotion = false;
					f2.hasStepPrior = true;
					f2.estimatedStepDelta = 24.0;
					const auto res2 = eng.ProcessFrame(f2, &reg2);

					Assert::IsTrue(reg2.IsActive(StandInKind::MissingMotionNoWholeBlend),
						L"H9: carry without carryMotion MUST engage StandInKind::MissingMotionNoWholeBlend");
					Assert::IsTrue(res2.missingMotionSkippedRefs >= 1,
						L"H9: skipped carry without carryMotion must increment missingMotionSkippedRefs");
					Assert::IsFalse(res2.carryUsed,
						L"H9: carry without carryMotion must NOT be whole-blended as Still");
					for (size_t i = 0; i < curFrame.v.size(); ++i)
						Assert::AreEqual(curFrame.v[i], res2.outputY.v[i], 0.0,
							L"H9: carry without carryMotion MUST produce exact zero ghosting on moving content");
				}
			}

			// =========================================================================
			// 14. H14 — Identified-Transform STRICT/CELL Explicit codecModelPb vs Visible Stand-In
			// =========================================================================
			TEST_METHOD(TransformIdentifiedCodecModelPbExplicitVsStandIn)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const double stepDelta = 12.0;
				const double phi = kDeadZonePhiInter;

				// Construct explicit CodecModel prediction P_B = 350.0, DISTINCT from prevPeak (352.5 -> +20.0 DCT DC offset)!
				// Under explicitPb: cur - explicitPb has SENT DC = +24.0 (> theta = 10.0, k = 2) -> STRICT restores exact 24.0!
				// Under prevPeak stand-in: cur - prevPeak has ZEROED DC = +4.0 (< theta = 10.0, k = 0) -> STRICT moves DC toward blend!
				TdV2Plane explicitPb(w, h, 350.0);
				TdV2Plane prevPeakDiffFromPb(w, h, 352.5); // +2.5 spatial => +20.0 DCT DC offset (pushes cur-prev into dead-zone k=0!)
				TdV2Plane cur(w, h, 350.0);
				TdV2Plane nextPeak(w, h, 350.0);

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
								cur.at(bx + i, by + j) = explicitPb.at(bx + i, by + j) + sentSpat[j * 8 + i];
								nextPeak.at(bx + i, by + j) = explicitPb.at(bx + i, by + j) + targetSpat[j * 8 + i];
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

					FrameProcessInput baseIn;
					baseIn.seq = 140;
					baseIn.receivedY = cur;
					baseIn.hasCleanSplit = true;
					baseIn.cleanY = cur;
					baseIn.prevPeak = { &prevPeakDiffFromPb, &stillMf, true };
					baseIn.nextPeak = { &nextPeak, &stillMf, false };
					baseIn.hasStepPrior = true;
					baseIn.estimatedStepDelta = stepDelta;
					baseIn.deadZonePhi = phi;
					baseIn.zeroCoeffFraction = 0.0;
					baseIn.transformIdentified8x8 = true;

					// Case A: codecModelPb == nullptr -> MUST visibly engage CodecModelPbStandIn & increment counter!
					TdV2ReferenceEngine engStandIn(cfg);
					StandInRegistry regStandIn;
					const auto resStandIn = engStandIn.ProcessFrame(baseIn, &regStandIn);
					Assert::IsTrue(regStandIn.IsActive(StandInKind::CodecModelPbStandIn),
						L"H14: identified 8x8 DCT without explicit codecModelPb MUST visibly engage CodecModelPbStandIn");
					Assert::IsTrue(resStandIn.codecModelPbStandInCells > 0,
						L"H14: identified 8x8 DCT without explicit codecModelPb MUST increment codecModelPbStandInCells");

					// Case B: explicit codecModelPb supplied -> MUST use it WITHOUT engaging CodecModelPbStandIn!
					FrameProcessInput expIn = baseIn;
					expIn.codecModelPb = &explicitPb;
					TdV2ReferenceEngine engExplicit(cfg);
					StandInRegistry regExplicit;
					const auto resExplicit = engExplicit.ProcessFrame(expIn, &regExplicit);
					Assert::IsFalse(regExplicit.IsActive(StandInKind::CodecModelPbStandIn),
						L"H14: supplying explicit codecModelPb MUST NOT engage CodecModelPbStandIn");
					Assert::AreEqual(static_cast<size_t>(0), resExplicit.codecModelPbStandInCells,
						L"H14: supplying explicit codecModelPb MUST keep codecModelPbStandInCells == 0");

					// Verify explicit codecModelPb is genuinely used (STRICT keeps exact DC=24 relative to explicitPb,
					// and output differs from stand-in path where refs[0] was shifted by +15!).
					if (rm == ResidualMode::Strict)
					{
						double spExp[64] = {}, dctExp[64] = {};
						for (int j = 0; j < 8; ++j)
							for (int i = 0; i < 8; ++i)
								spExp[j * 8 + i] = resExplicit.outputClean.at(16 + i, 16 + j) - explicitPb.at(16 + i, 16 + j);
						ForwardDct8x8(spExp, dctExp);
						Assert::AreEqual(24.0, dctExp[0], 1e-6,
							L"Explicit codecModelPb in STRICT mode must preserve sent DC=24 relative to explicitPb");
						Assert::IsTrue(PlaneMse(resExplicit.outputClean, resStandIn.outputClean, ActiveRect::Full(w, h)) > 1e-4,
							L"Explicit codecModelPb must genuinely alter projection vs distinct refs[0] stand-in");
					}
				}
			}

			// =========================================================================
			// 15. MEDIUM Closures: Off->On / Seek / Non-Consecutive seq Carry Reset + Invalid a2Stats
			// =========================================================================
			TEST_METHOD(CarryResetsOnOffTransitionNonConsecutiveSeqAndInvalidA2Stats)
			{
				const int w = 64, h = 64;
				const int cw = w / 16, ch = h / 16;
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);
				const TdV2Plane sceneA = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane sceneB(w, h, 780.0);
				const TdV2Plane refA = AddCodingError(sceneA, 1.0, 1501u);

				// ---- Part A: Consecutive seq uses carry (positive control), whereas Off->On and seek/gap reset carry! ----
				{
					// Positive control: consecutive seq 10 -> 11 DOES use carry.
					Config onCfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine engConsec(onCfg);
					StandInRegistry reg;

					FrameProcessInput f10;
					f10.seq = 10;
					f10.receivedY = sceneA;
					f10.hasCleanSplit = true;
					f10.cleanY = sceneA;
					engConsec.ProcessFrame(f10, &reg);
					Assert::IsTrue(engConsec.HasCarry());

					FrameProcessInput f11 = f10;
					f11.seq = 11;
					f11.hasCarryMotion = true;
					f11.carryMotion = stillMf;
					const auto r11 = engConsec.ProcessFrame(f11, &reg);
					Assert::IsTrue(r11.carryUsed && !r11.carryReset,
						L"Positive control: consecutive seq 10->11 must reuse carry cleanly");

					// Test 1: Mode::On (seq 10) -> Mode::Off (seq 11) -> Mode::On (seq 12) MUST reset stale carry!
					TdV2ReferenceEngine engOffOn(onCfg);
					engOffOn.ProcessFrame(f10, &reg);
					Assert::IsTrue(engOffOn.HasCarry());

					Config offCfg = onCfg;
					offCfg.mode = Mode::Off;
					engOffOn.SetConfig(offCfg);
					FrameProcessInput fOff = f10;
					fOff.seq = 11;
					engOffOn.ProcessFrame(fOff, &reg);

					engOffOn.SetConfig(onCfg);
					FrameProcessInput fResume;
					fResume.seq = 12;
					fResume.receivedY = sceneB;
					fResume.hasCleanSplit = true;
					fResume.cleanY = sceneB;
					fResume.hasCarryMotion = true;
					fResume.carryMotion = stillMf;
					const auto rResume = engOffOn.ProcessFrame(fResume, &reg);
					Assert::IsFalse(rResume.carryUsed,
						L"Stale pre-Off carry MUST NOT be used after Mode::Off -> Mode::On transition");
					for (size_t i = 0; i < sceneB.v.size(); ++i)
						Assert::AreEqual(780.0, rResume.outputY.v[i], 0.0,
							L"Mode::Off -> Mode::On must produce zero ghosting from stale carry");

					// Test 2: Forward frame drop / seek (seq 10 -> seq 15, isCutOrSpanInvalid == false) MUST reset carry!
					TdV2ReferenceEngine engSeek(onCfg);
					engSeek.ProcessFrame(f10, &reg);
					Assert::IsTrue(engSeek.HasCarry());

					FrameProcessInput fSeekFwd;
					fSeekFwd.seq = 15; // non-consecutive jump (dropped 4 frames / seek)
					fSeekFwd.receivedY = sceneB;
					fSeekFwd.hasCleanSplit = true;
					fSeekFwd.cleanY = sceneB;
					fSeekFwd.hasCarryMotion = true;
					fSeekFwd.carryMotion = stillMf;
					fSeekFwd.isCutOrSpanInvalid = false;
					const auto rSeekFwd = engSeek.ProcessFrame(fSeekFwd, &reg);
					Assert::IsTrue(rSeekFwd.carryReset,
						L"Non-consecutive forward seq jump (10 -> 15) MUST set carryReset == true");
					Assert::IsFalse(rSeekFwd.carryUsed,
						L"Non-consecutive forward seq jump MUST NOT reuse stale carry");
					for (size_t i = 0; i < sceneB.v.size(); ++i)
						Assert::AreEqual(780.0, rSeekFwd.outputY.v[i], 0.0,
							L"Forward seek must have zero ghosting from stale carry");

					// Test 3: Backward seek (seq 15 -> seq 5, isCutOrSpanInvalid == false) MUST reset carry!
					FrameProcessInput fSeekBack;
					fSeekBack.seq = 5;
					fSeekBack.receivedY = sceneA;
					fSeekBack.hasCleanSplit = true;
					fSeekBack.cleanY = sceneA;
					fSeekBack.hasCarryMotion = true;
					fSeekBack.carryMotion = stillMf;
					fSeekBack.isCutOrSpanInvalid = false;
					const auto rSeekBack = engSeek.ProcessFrame(fSeekBack, &reg);
					Assert::IsTrue(rSeekBack.carryReset && !rSeekBack.carryUsed,
						L"Backward seek (15 -> 5) MUST reset carry and never reuse future carry");
				}

				// ---- Part B: Invalid a2Stats (deadZonePhi out of [0, 1) or NaN) -> A2StatsInvalid + maps invalid! ----
				const double badPhis[] = { -0.2, 1.0, 1.5, std::numeric_limits<double>::quiet_NaN() };
				for (double badPhi : badPhis)
				{
					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput in;
					in.seq = 30;
					in.receivedY = sceneA;
					in.hasCleanSplit = true;
					in.cleanY = sceneA;
					in.prevPeak = { &refA, &stillMf, false };
					in.nextPeak = { &refA, &stillMf, false };
					in.hasStepPrior = true;
					in.estimatedStepDelta = 12.0;
					in.deadZonePhi = badPhi;

					const auto res = eng.ProcessFrame(in, &reg);
					AssertAllFinite(res.outputY, L"Invalid a2Stats must keep output finite");
					Assert::IsTrue(reg.IsActive(StandInKind::A2StatsInvalid),
						L"Invalid deadZonePhi MUST visibly engage StandInKind::A2StatsInvalid");
					Assert::IsTrue(res.a2InvalidCells > 0,
						L"Invalid a2Stats MUST increment res.a2InvalidCells");
					for (size_t i = 0; i < sceneA.v.size(); ++i)
						Assert::AreEqual(sceneA.v[i], res.outputY.v[i], 0.0,
							L"Invalid a2Stats must close delta safely to zero (passthrough)");
					for (const auto& blk : res.tdMaps)
						Assert::AreEqual(0.0f, TdMapsView::Fetch(blk, TdMapsView::kValidity), 0.0f,
							L"Invalid a2Stats MUST mark tdMaps cells invalid (kValidity == 0.0), never silently valid!");
				}
			}

			// =========================================================================
			// 16. M2 & M3 Adversarial Tests: Luma mf.valid==0 POCS Skip + Parseval Transform-Unknown Bound
			// =========================================================================
			TEST_METHOD(LumaInvalidMotionPocsSkipAndTransformUnknownParsevalBound)
			{
				// ---- Part A (M2): mf.valid == 0 prevents bogus warp AND skips 8x8 DCT POCS against invalid refs[0] ----
				{
					const int w = 64, h = 64;
					const int cw = w / 16, ch = h / 16;
					const ActiveRect holeRect{ 16, 16, 32, 32 }; // exact cell (1, 1) = four 8x8 DCT blocks
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					const TdV2Plane master = RenderMaster(w, h);
					const TdV2Plane cur = AddCodingError(master, 4.0, 1601u);
					const TdV2Plane nextClean = AddCodingError(master, 0.5, 1602u);
					TdV2Plane corruptPrev = AddCodingError(master, 0.5, 1603u);
					// Corrupt BOTH [16, 32)^2 and the (vx=8, vy=8)-warped fetch region [24, 40)^2 by -40 codes so any
					// failure to respect mf.valid == 0 (in warp OR in STRICT POCS) immediately corrupts [16, 32)^2!
					for (int y = 16; y < 40; ++y)
						for (int x = 16; x < 40; ++x)
							corruptPrev.at(x, y) -= 40.0;

					const size_t holeCell = static_cast<size_t>(1) * cw + 1;
					CellMotionField occMf = stillMf;
					occMf.valid[holeCell] = 0u;
					occMf.vx[holeCell] = 8.0;
					occMf.vy[holeCell] = 8.0;

					CellMotionField bogusValidMf = occMf;
					bogusValidMf.valid[holeCell] = 1u; // Negative control: same bogus (8, 8) vector if valid==0 were ignored!

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					cfg.residualMode = ResidualMode::Strict;
					cfg.enableCarryForward = false;

					FrameProcessInput inOcc;
					inOcc.seq = 40;
					inOcc.receivedY = cur;
					inOcc.hasCleanSplit = true;
					inOcc.cleanY = cur;
					inOcc.prevPeak = { &corruptPrev, &occMf, false };   // refs[0]: valid==0 in cell (1, 1)
					inOcc.nextPeak = { &nextClean, &stillMf, false };   // refs[1]: valid==1 everywhere
					inOcc.hasStepPrior = true;
					inOcc.estimatedStepDelta = 12.0;
					inOcc.transformIdentified8x8 = true;

					TdV2ReferenceEngine engOcc(cfg);
					StandInRegistry regOcc;
					const auto resOcc = engOcc.ProcessFrame(inOcc, &regOcc);
					AssertAllFinite(resOcc.outputY, L"M2 occluded-prevPeak STRICT output must be finite");

					// Valid-prevPeak control: all 16 cells valid -> POCS runs on all 16 cells.
					FrameProcessInput inValid = inOcc;
					inValid.prevPeak = { &nextClean, &stillMf, false };
					TdV2ReferenceEngine engValid(cfg);
					StandInRegistry regValid;
					const auto resValid = engValid.ProcessFrame(inValid, &regValid);

					// Mandatory In-Test Negative Control (Rule 3): exact same bogus (8, 8) vector & corruptPrev with valid==1!
					FrameProcessInput inBogus = inOcc;
					inBogus.prevPeak = { &corruptPrev, &bogusValidMf, false };
					TdV2ReferenceEngine engBogus(cfg);
					StandInRegistry regBogus;
					const auto resBogus = engBogus.ProcessFrame(inBogus, &regBogus);

					const double baseHoleMse = PlaneMse(master, cur, holeRect);
					const double occHoleMse = PlaneMse(master, resOcc.outputY, holeRect);
					const double bogusHoleMse = PlaneMse(master, resBogus.outputY, holeRect);

					Assert::IsTrue(resOcc.codecModelPbStandInCells > 0,
						L"M2: valid cells outside [16,32)^2 must still run 8x8 DCT POCS cleanly");
					Assert::IsTrue(resOcc.codecModelPbStandInCells < resValid.codecModelPbStandInCells,
						L"M2: 8x8 DCT POCS must skip occluded cell (1,1) where refs[0].validPx == 0");
					Assert::IsTrue(occHoleMse < baseHoleMse * 0.65,
						L"M2: cell (1,1) must cleanly improve toward master via valid nextPeak with zero garbage warp from prevPeak");
					Assert::IsTrue(bogusHoleMse > occHoleMse * 2.0,
						L"M2 Negative control: ignoring mf.valid==0 on bogus (8,8) prevPeak must severely corrupt [16,32)^2");
				}

				// ---- Part B (M3): Transform-unknown pixel bound uses Parseval a2BandPeakSq (NOT white-noise over-clip!) ----
				{
					const int w = 128, h = 128;
					const int cw = w / 16, ch = h / 16;
					const ActiveRect interior{ 16, 16, w - 16, h - 16 };
					const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

					TdV2Plane masterSky(w, h, 0.0), valleySky(w, h, 0.0);
					std::vector<uint8_t> skyMask(static_cast<size_t>(w) * h, kMaskW1BandedSky);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double val = 400.0 + 0.6 * x + 0.2 * y;
							masterSky.at(x, y) = val;
							valleySky.at(x, y) = 8.0 * std::floor(val / 8.0 + 0.5);
						}
					}
					const TdV2Plane peakPrevSky = AddCodingError(masterSky, 0.5, 1611u);
					const TdV2Plane peakNextSky = AddCodingError(masterSky, 0.5, 1612u);

					Config cfg = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine eng(cfg);
					StandInRegistry reg;

					FrameProcessInput inSky;
					inSky.seq = 41;
					inSky.receivedY = valleySky;
					inSky.hasCleanSplit = true;
					inSky.cleanY = valleySky;
					inSky.prevPeak = { &peakPrevSky, &stillMf, false };
					inSky.nextPeak = { &peakNextSky, &stillMf, false };
					inSky.hasStepPrior = true;
					inSky.estimatedStepDelta = 8.0;
					inSky.transformIdentified8x8 = false; // Transform-unknown pixel-domain bound!
					const auto resSky = eng.ProcessFrame(inSky, &reg);

					Assert::IsTrue(reg.IsActive(StandInKind::TransformUnknownBound),
						L"M3: transformIdentified8x8 == false must visibly engage TransformUnknownBound");

					double maxAbsDelta = 0.0;
					for (int y = interior.y0; y < interior.y1; ++y)
						for (int x = interior.x0; x < interior.x1; ++x)
							maxAbsDelta = (std::max)(maxAbsDelta, std::fabs(resSky.outputY.at(x, y) - valleySky.at(x, y)));

					// Mandatory In-Test Negative Control (Rule 3): old white-noise over-clip 3*sqrt((8^2/12)*gains.low) ~= 0.78!
					const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
					const double oldWhiteNoiseBound = kPixelBoundSigma * std::sqrt((8.0 * 8.0 / 12.0) * gains.low);
					TdV2Plane oldOverClipped = valleySky;
					for (size_t i = 0; i < oldOverClipped.v.size(); ++i)
					{
						const double d = resSky.outputY.v[i] - valleySky.v[i];
						oldOverClipped.v[i] = valleySky.v[i] + (std::max)(-oldWhiteNoiseBound, (std::min)(oldWhiteNoiseBound, d));
					}
					const double engineMse = PlaneMse(masterSky, resSky.outputY, interior);
					const double overClippedMse = PlaneMse(masterSky, oldOverClipped, interior);

					const WorstAreaReport repSky = EvaluateWorstAreaMasks(
						masterSky.v.data(), valleySky.v.data(), resSky.outputY.v.data(), skyMask.data(), w, h, 16, 1023.0);
					Assert::IsTrue(repSky.valid);
					Assert::IsTrue(maxAbsDelta > 1.5,
						L"M3: Parseval TransformUnknownBound must recover coherent L-band sky steps well beyond old 0.78 white-noise over-clip");
					Assert::IsTrue(repSky.w1BandedSky.deltaDb > 3.0 && repSky.w1BandingStepReduction > 0.30,
						L"M3: W1 8-code sky banding must recover strongly under Parseval TransformUnknownBound");
					Assert::IsTrue(overClippedMse > engineMse * 2.0,
						L"M3 Negative control: old white-noise 0.78 over-clip must degrade sky MSE > 2x vs Parseval bound");
				}
			}

			// =========================================================================
			// 17. Round-3 HIGH-1 Regression: Asymmetric Focus Debiasing Must Not Veto W3 Softened B-Frames
			// =========================================================================
			TEST_METHOD(W3CompressionSoftenedEdgeNotFalselyVetoedByAsymmetricFocusDebiasing)
			{
				// Construct exact BandFromBase eigenfunction waves (2 cycles/cell M-wave lambda=8 + 1 cycle/cell D3-wave lambda=16,
				// even-symmetric at active borders) with moderate contrast (refM2 ~= 1.00, refD3 ~= 1.73, refE ~= 2.74 >> 1.83,
				// refF ~= 0.367) and a mildly W3 compression-softened B-frame (20% M-band attenuation: curM2 < refM2, curF ~= 0.271,
				// true focus drop refF - curF ~= 0.096 << fShiftDelta = 0.1908) at estimatedStepDelta = 14.0.
				// Under the old buggy asymmetric predicate (refF - curFDebiased > fShiftDelta), subtracting stepNoiseVar*gains.mid
				// ONLY from cur (and NOT ref) pulled cleanM2 to ~0 (curFDebiased ~= 0 < fSharp) and refF - curFDebiased ~= 0.367 > 0.1908,
				// falsely all-band vetoing sharp edges and W3 softened B-frames!
				const int w = 96, h = 96;
				const int cw = w / 16, ch = h / 16;
				const ActiveRect full = ActiveRect::Full(w, h);
				const ActiveRect interior{ 16, 16, w - 16, h - 16 };
				const CellMotionField stillMf = CellMotionField::Uniform(cw, ch, 0.0, 0.0, 0.0, 0.0, RegionMotionClass::Still);

				TdV2Plane master(w, h, 0.0), curSoft(w, h, 0.0);
				for (int y = 0; y < h; ++y)
				{
					const double d3Wave = 4.2 * std::cos(2.0 * kPi * (static_cast<double>(y) + 0.5) / 16.0);
					for (int x = 0; x < w; ++x)
					{
						const double mWave = 3.2 * std::cos(2.0 * kPi * (static_cast<double>(x) + 0.5) / 8.0);
						master.at(x, y) = 450.0 + mWave + d3Wave;
						curSoft.at(x, y) = 450.0 + 0.80 * mWave + d3Wave; // mild W3 M-band compression softening (still sharp!)
					}
				}
				const TdV2Plane peakPrev = AddCodingError(master, 0.10, 1701u);
				const TdV2Plane peakNext = AddCodingError(master, 0.10, 1702u);

				const double stepDelta = 14.0;
				const RoundingCellStats a2 = ComputeRoundingCellStats(stepDelta, kDeadZonePhiInter, 0.0);
				const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
				const double structureFloor = 2.0 * (gains.mid + gains.d3) * a2.totalVar;
				const double fSharp = AnalyticalEdgeFocusRatio(0.0);
				const double fShiftDelta = fSharp - AnalyticalEdgeFocusRatio(VpUnit::BandFromBase::kSigmaBase);

				// Mandatory In-Test Negative Control (Rule 3): prove the old buggy asymmetric predicate WOULD falsely
				// veto EVERY interior cell on the EXACT SAME (curSoft, peakPrev) planes at stepDelta = 14.0!
				const ThreeBandPlanes curB = DecomposeActiveBands(curSoft, full);
				const ThreeBandPlanes refB = DecomposeActiveBands(peakPrev, full);
				size_t oldBuggyFalseVetoes = 0;
				size_t interiorCells = 0;
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						double cM2 = 0.0, cD3 = 0.0, rM2 = 0.0, rD3 = 0.0;
						for (int y = cy * 16; y < (cy + 1) * 16; ++y)
						{
							for (int x = cx * 16; x < (cx + 1) * 16; ++x)
							{
								const double cm = curB.M.at(x, y), cd = curB.D3.at(x, y);
								const double rm = refB.M.at(x, y), rd = refB.D3.at(x, y);
								cM2 += cm * cm; cD3 += cd * cd;
								rM2 += rm * rm; rD3 += rd * rd;
							}
						}
						cM2 /= 256.0; cD3 /= 256.0; rM2 /= 256.0; rD3 /= 256.0;
						const double refE = rM2 + rD3;
						const double refF = rM2 / (refE + 1e-12);
						const double curF = cM2 / (cM2 + cD3 + 1e-12);
						const double cleanM2 = (std::max)(0.0, cM2 - a2.totalVar * gains.mid);
						const double cleanM3 = (std::max)(0.0, cD3 - a2.totalVar * gains.d3);
						const double curFDebiased = cleanM2 / (cleanM2 + cleanM3 + 1e-12);
						Assert::IsTrue(refE > structureFloor && cM2 < rM2 && (refF - curF) < fShiftDelta * 0.60,
							L"Fixture check: mild W3 softening must stay well below true focus-shift threshold");
						if (refE > structureFloor && cM2 < rM2 && curFDebiased < fSharp && (refF - curFDebiased) > fShiftDelta)
							++oldBuggyFalseVetoes;
						++interiorCells;
					}
				}
				Assert::AreEqual(interiorCells, oldBuggyFalseVetoes,
					L"Negative control: old asymmetric (refF - curFDebiased) > fShiftDelta predicate MUST falsely veto all interior cells");

				// Protected engine under fixed symmetric focus guard: ZERO false focus vetoes + strong M-band recovery!
				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 55;
				in.receivedY = curSoft;
				in.hasCleanSplit = true;
				in.cleanY = curSoft;
				in.prevPeak = { &peakPrev, &stillMf, false };
				in.nextPeak = { &peakNext, &stillMf, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = stepDelta;
				in.deadZonePhi = kDeadZonePhiInter;
				in.zeroCoeffFraction = 0.0;
				const auto res = eng.ProcessFrame(in, &reg);

				AssertAllFinite(res.outputY, L"W3 softened B-frame output must be finite");
				Assert::AreEqual(static_cast<size_t>(0), res.focusVetoCells,
					L"Round-3 HIGH-1: mild W3 compression-softened sharp edge at stepDelta=14 MUST NOT trigger false focus vetoes");
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						const unsigned mask = static_cast<unsigned>(
							TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						Assert::AreNotEqual(0u, mask & 2u,
							L"W3 compression-softened cell must keep M-band (bit 1) active for detail recovery");
					}
				}

				const ThreeBandPlanes masterB = DecomposeActiveBands(master, full);
				const ThreeBandPlanes outB = DecomposeActiveBands(res.outputY, full);
				const double baseMseM = PlaneMse(masterB.M, curB.M, interior);
				const double outMseM = PlaneMse(masterB.M, outB.M, interior);
				Assert::IsTrue(outMseM < baseMseM * 0.35,
					L"W3 compression-softened sharp edge must achieve strong M-band detail recovery (>= 65% MSE drop)");
			}
		};
	}
}
