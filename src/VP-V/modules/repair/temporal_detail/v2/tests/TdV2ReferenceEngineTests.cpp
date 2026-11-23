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
//  11. Scene cut and non-finite receivedY/cleanY reset cleanly with zero ghosting. CPU only.
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

				FrameProcessInput in;
				in.seq = 1;
				in.receivedY = valley;
				in.hasCleanSplit = true;
				in.cleanY = valley;
				in.prevPeak.cleanPlane = &peakP;
				in.nextPeak.cleanPlane = &peakN;
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
				// Plan s1.6 & s3.4 (Grain MUST-2): 0.5-px sub-pixel pan invariant.
				const int w = 96, h = 96;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const int cw = w / 16, ch = h / 16;

				const TdV2Plane masterCur = RenderMaster(w, h, 0.0, 0.0);
				const TdV2Plane masterPrev = RenderMaster(w, h, -0.5, 0.0);
				const TdV2Plane masterNext = RenderMaster(w, h, +0.5, 0.0);

				const TdV2Plane peakPrev = AddCodingError(masterPrev, 1.2, 301);
				const TdV2Plane peakNext = AddCodingError(masterNext, 1.2, 302);
				const TdV2Plane valley = AddCodingError(masterCur, 6.5, 303, 0.30);

				CellMotionField mfPrev = CellMotionField::Uniform(cw, ch, +0.5, 0.0, +0.5, 0.0, RegionMotionClass::Camera);
				CellMotionField mfNext = CellMotionField::Uniform(cw, ch, -0.5, 0.0, -0.5, 0.0, RegionMotionClass::Camera);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 20;
				in.receivedY = valley;
				in.hasCleanSplit = true;
				in.cleanY = valley;
				in.prevPeak = { &peakPrev, &mfPrev, false };
				in.nextPeak = { &peakNext, &mfNext, false };
				in.hasStepPrior = true;
				in.estimatedStepDelta = 14.0;
				const auto res = eng.ProcessFrame(in, &reg);

				double dbBase = 0.0, dbOut = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterCur, valley, interior), 1023.0, dbBase));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterCur, res.outputY, interior), 1023.0, dbOut));
				const double gain = dbOut - dbBase;
				char buf[192];
				std::snprintf(buf, sizeof(buf), "0.5-px sub-pixel pan recovery: gain=+%.3f dB", gain);
				Logger::WriteMessage(buf);
				Assert::IsTrue(gain > 2.0, L"0.5-px sub-pixel pan must recover cleanly with strong positive dB gain");
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
				const TdV2Plane peakPrevCleanTrue = AddCodingError(masterClean, 1.0, 601);
				const TdV2Plane peakNextCleanTrue = AddCodingError(masterClean, 1.0, 602);
				const TdV2Plane prevCurCleanTrue = AddCodingError(masterClean, 5.5, 604, 0.30);
				const TdV2Plane curCleanTrue = AddCodingError(masterClean, 5.5, 603, 0.30);

				std::mt19937 rng(777);
				const double grainSigma = 10.0;
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

				// ---- Mandatory In-Test Negative Control (Rule 3) ----
				// Naive temporal average of the exact same contaminated frames (curReceived, peakPrevReceived, peakNextReceived).
				std::vector<double> naiveAvg(n);
				for (size_t i = 0; i < n; ++i)
					naiveAvg[i] = (curReceived.v[i] + peakPrevReceived.v[i] + peakNextReceived.v[i]) / 3.0;
				const auto naiveRep = EvaluateGrainTransfer(
					masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), naiveAvg.data(), n);
				Assert::IsTrue(naiveRep.valid);
				Assert::IsFalse(naiveRep.passesAuthoritativeBar,
					L"Negative control: naive temporal average MUST fail authoritative grain preservation bar");
				Assert::IsTrue(naiveRep.betaOwn < 0.75,
					L"Negative control: naive temporal average destroys own grain (betaOwn < 0.75)");
				Assert::IsTrue(naiveRep.betaForeign > 0.25,
					L"Negative control: naive temporal average leaks foreign grain (betaForeign > 0.25)");

				// ---- Sub-case A: Realistic Imperfect Separation (hasCleanSplit == true) ----
				// Upstream separator leaves 1.5% residual ownGrain inside curClean AND 1.5% residual foreignGrain inside peak cleans
				// (physically feasible for betaOwn >= 0.98 even at full temporal blend W -> 1.0).
				const double leakCur = 0.015;
				const double leakRef = 0.015;
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
				f1.estimatedStepDelta = 14.0;
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
				f2.estimatedStepDelta = 14.0;
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
					"Honest Grain (imperfect split vs masterClean): cleanGain=+%.3f dB | betaOwn=%.4f betaForP=%.4f betaForN=%.4f stdRatio=%.4f lag1Rise=%.4f",
					cleanDbOut - cleanDbBase, gRepPrev.betaOwn, gRepPrev.betaForeign, gRepNext.betaForeign,
					gRepPrev.stdRatio, gRepPrev.lag1Rise);
				Logger::WriteMessage(buf);

				Assert::IsTrue(cleanDbOut - cleanDbBase > 2.0, L"Clean layer must recover compression noise strongly vs masterClean");
				Assert::IsTrue(gRepPrev.valid && gRepPrev.passesAuthoritativeBar,
					L"Imperfect-split output scored strictly vs masterClean must pass authoritative refs/111 s7.3 bar");
				Assert::IsTrue(gRepNext.valid && std::fabs(gRepNext.betaForeign) <= kGrainBarMaxBetaForeign,
					L"Foreign grain leakage from nextPeak must stay <= 0.02");

				// ---- Sub-case B: Raw Noisy Input Path (hasCleanSplit == false) ----
				// Pure film grain on masterClean with NO upstream split: Layer 2 A1 subtraction + Layer 4 TdGuardPort
				// must protect own grain and engage P10gGrainSeparator stand-in visibly!
				{
					TdV2Plane rawCur(w, h), rawPrev(w, h), rawNext(w, h);
					for (size_t i = 0; i < n; ++i)
					{
						rawCur.v[i] = masterClean.v[i] + ownGrain[i];
						rawPrev.v[i] = masterClean.v[i] + foreignGrainPrev[i];
						rawNext.v[i] = masterClean.v[i] + foreignGrainNext[i];
					}
					std::vector<double> fullGrainVar(static_cast<size_t>(cw) * ch, grainSigma * grainSigma);

					Config cfgRaw = MakePreset(Preset::Reference, Mode::On);
					TdV2ReferenceEngine engRaw(cfgRaw);
					StandInRegistry regRaw;

					FrameProcessInput fRaw;
					fRaw.seq = 60;
					fRaw.receivedY = rawCur;
					fRaw.hasCleanSplit = false;
					fRaw.retainedGrainVar = fullGrainVar;
					fRaw.prevPeak = { &rawPrev, &stillMf, false };
					fRaw.nextPeak = { &rawNext, &stillMf, false };
					fRaw.hasStepPrior = true;
					fRaw.estimatedStepDelta = 14.0;
					const auto rRaw = engRaw.ProcessFrame(fRaw, &regRaw);

					AssertAllFinite(rRaw.outputY, L"Raw hasCleanSplit==false output must be finite");
					Assert::IsTrue(regRaw.IsActive(StandInKind::P10gGrainSeparator),
						L"Raw hasCleanSplit==false MUST engage P10gGrainSeparator stand-in visibly");
					const auto rawRep = EvaluateGrainTransfer(
						masterClean.v.data(), ownGrain.data(), foreignGrainPrev.data(), rRaw.outputY.v.data(), n);
					Assert::IsTrue(rawRep.valid);
					Assert::IsTrue(rawRep.betaOwn >= 0.95,
						L"Raw path with A1+Guard must retain >= 95% of own grain (vs naive 33%)");
					Assert::IsTrue(std::fabs(rawRep.betaForeign) <= 0.03,
						L"Raw path with A1+Guard must reject foreign grain <= 0.03 (vs naive 0.33)");
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
				const double errLOut = PlaneMse(curB.L, outB.L, interior);
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Rack-focus under LowBandCellOnly: falseH=%.6f (naive=%.3f) falseM=%.6f errL=%.9f",
					falseRestoreH, naiveFalseH, falseRestoreM, errLOut);
				Logger::WriteMessage(buf);
				Assert::IsTrue(falseRestoreH <= 0.001, L"Owner Focus Rule: H defocus false-restore must be <= 0.1%");
				Assert::IsTrue(falseRestoreM <= 0.001, L"Owner Focus Rule: M defocus false-restore must be <= 0.1%");
				Assert::AreEqual(0.0, errLOut, 1e-12,
					L"Unmatched rack-focus (focusChange) MUST gate L-band to 0 even under BokehMode::LowBandCellOnly!");

				// ---- Sub-case C: Matched-Focus Bokeh (LowBandCellOnly cleans L ONLY; M/H delta == 0!) ----
				const TdV2Plane bokehPrev = AddCodingError(defocusedCur, 1.0, 401);
				const TdV2Plane bokehNext = AddCodingError(defocusedCur, 1.0, 402);
				const TdV2Plane bokehCur = AddCodingError(defocusedCur, 5.0, 403);

				FrameProcessInput bkIn;
				bkIn.seq = 31;
				bkIn.receivedY = bokehCur;
				bkIn.hasCleanSplit = true;
				bkIn.cleanY = bokehCur;
				bkIn.prevPeak = { &bokehPrev, &stillMf, false };
				bkIn.nextPeak = { &bokehNext, &stillMf, false };
				bkIn.hasStepPrior = true;
				bkIn.estimatedStepDelta = 12.0;

				Config cfgUntouched = MakePreset(Preset::Reference, Mode::On);
				cfgUntouched.bokehMode = BokehMode::Untouched;
				TdV2ReferenceEngine engU(cfgUntouched);
				const auto resU = engU.ProcessFrame(bkIn, &regRack);

				Config cfgLowOnly = MakePreset(Preset::Reference, Mode::On);
				cfgLowOnly.bokehMode = BokehMode::LowBandCellOnly;
				TdV2ReferenceEngine engL(cfgLowOnly);
				const auto resL = engL.ProcessFrame(bkIn, &regRack);

				const double mseU = PlaneMse(defocusedCur, resU.outputY, interior);
				const double mseL = PlaneMse(defocusedCur, resL.outputY, interior);
				Assert::IsTrue(mseL < mseU, L"Q4 LowBandCellOnly must clean matched-focus bokeh L-band noise while Untouched keeps it off");

				// Verify TdMapsView::kBandMask on matched-focus defocused interior cells has ZERO M/H bits (only bit 0 = L)!
				for (int cy = 1; cy < ch - 1; ++cy)
				{
					for (int cx = 1; cx < cw - 1; ++cx)
					{
						const unsigned mask = static_cast<unsigned>(
							TdMapsView::Fetch(resL.tdMaps[static_cast<size_t>(cy) * cw + cx], TdMapsView::kBandMask));
						Assert::AreEqual(0u, mask & 6u,
							L"Matched-focus bokeh under LowBandCellOnly MUST keep M (bit 1) and H (bit 2) strictly 0");
					}
				}

				// Direct M/H delta == 0 proof: plant a pure Nyquist checkerboard (zero L-band projection under G(2.5)!)
				// on matched-focus bokehCur vs clean defocusedCur refs -> LowBandCellOnly must leave it untouched!
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
				TdV2ReferenceEngine engLMH(cfgLowOnly);
				const auto resLMH = engLMH.ProcessFrame(bkMH, &regRack);
				double maxInteriorMhDelta = 0.0;
				for (int y = interior.y0; y < interior.y1; ++y)
					for (int x = interior.x0; x < interior.x1; ++x)
						maxInteriorMhDelta = (std::max)(maxInteriorMhDelta,
							std::fabs(resLMH.outputY.at(x, y) - bokehChecker.at(x, y)));
				Assert::IsTrue(maxInteriorMhDelta < 1e-5,
					L"Under LowBandCellOnly matched bokeh, pure H-band difference MUST yield 0.0 delta!");
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

				// Cell (4, 2): Local explicit focusChange (+12-code contrast vs peak refs) directly adjacent to normal high-weight cells!
				for (int y = 32; y < 48; ++y)
					for (int x = 64; x < 80; ++x)
						cur.at(x, y) = master.at(x, y) + 12.0;

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

				// Assert 0.000 bleed inside EVERY pixel of local focusChange cell (4, 2), including 0-8 px from border!
				Assert::AreEqual(0.0f, TdMapsView::Fetch(res.tdMaps[static_cast<size_t>(2) * cw + 4], TdMapsView::kValidity), 0.0f);
				for (int y = 32; y < 48; ++y)
					for (int x = 64; x < 80; ++x)
						Assert::AreEqual(cur.at(x, y), res.outputY.at(x, y), 0.0,
							L"Local focusChange cell MUST have 0.000 weight bleed even 1 px from active neighbor border!");

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
				const BandWhiteNoiseGains gains = ComputeBandWhiteNoiseGains();
				const double maxPixelBound = kPixelBoundSigma * (
					std::sqrt(st.totalVar * gains.low) +
					std::sqrt(st.totalVar * gains.mid) +
					std::sqrt(st.totalVar * gains.fine));
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

				// 2. Null CellMotionField -> visible MotionFieldStandIn!
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

					AssertAllFinite(res.outputY, L"Null motion field stand-in must produce finite output");
					Assert::AreEqual(static_cast<size_t>(2), res.motionStandInRefs);
					Assert::IsTrue(reg.IsActive(StandInKind::MotionFieldStandIn));
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

				// 8. Singular covariance cell -> ZERO crash, finite output, singularCells > 0, SingularSolveClosure visible!
				// prevPeak = cur + 5e-5 (C_00 = 2.5e-9 > 1e-9), nextPeak = cur (C_11 = 0, a_11 = 0 + reg = 1e-9 <= kSolveNumericalFloor).
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

					AssertAllFinite(res.outputY, L"Singular covariance cell must produce finite output");
					Assert::IsTrue(res.singularCells > 0, L"Singular covariance cell must increment singularCells counter");
					Assert::IsTrue(reg.IsActive(StandInKind::SingularSolveClosure),
						L"Singular covariance closure must engage SingularSolveClosure visibly");
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
		};
	}
}
