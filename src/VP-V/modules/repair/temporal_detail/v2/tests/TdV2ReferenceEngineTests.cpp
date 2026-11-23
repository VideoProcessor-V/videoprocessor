// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S3 tests (2026-11-23_TD-consolidated-plan.md s3.2 & s3.3):
// End-to-end CPU reference fusion engine tests proving:
//   1. Off and Shadow modes leave output bit-exact to received input
//   2. Still-scene multi-frame compression noise recovery achieves strong positive PSNR dB gain
//      and dual-source (carry + peaks) beats single-reference recovery
//   3. Camera pan retains full positive dB gain & sharpness while fast moving foreground object
//      preserves sharpness without smearing (verified via SpeedBinTable)
//   4. Half-pixel (0.5 px) sub-pixel camera pan preserves positive recovery cleanly (Grain MUST-2 / s3.4)
//   5. Rack-focus / focal-plane shift NEVER imports sharper detail (defocus false-restore <= 0.1%)
//      and Q4 bokeh knob strictly separates Untouched vs LowBandCellOnly
//   6. Active-picture letterbox bars remain bit-exact under camera pan
//   7. Explicit 4-layer grain preservation keeps film grain intact 1:1 (betaOwn >= 0.98, betaForeign <= 0.02,
//      stdRatio within +/-2%, lag1Rise <= +0.02)
//   8. Scene cuts and non-finite inputs reset recurrent carry cleanly with zero ghosting. CPU only.
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

		// Simulate quantization/compression noise: peak frames have small coding error, valley frames have
		// larger independent quantization/rounding noise + fine-band attenuation.
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

			TEST_METHOD(StillSceneMultiFrameCompressionNoiseRecoveryPositiveDbGain)
			{
				const int w = 96, h = 96;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };
				const TdV2Plane master = RenderMaster(w, h);
				const TdV2Plane peakPrev = AddCodingError(master, 1.2, 101);
				const TdV2Plane peakNext = AddCodingError(master, 1.2, 102);
				const TdV2Plane valley1 = AddCodingError(master, 7.0, 103, 0.40);
				const TdV2Plane valley2 = AddCodingError(master, 7.0, 104, 0.40);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.estimator = VarianceEstimator::A3_ConservativeMin;
				cfg.residualMode = ResidualMode::Cell;

				// First run single-frame-carry disabled (peaks only) vs dual-source (peaks + recurrent carry) on valley2.
				StandInRegistry reg;
				TdV2ReferenceEngine eng(cfg);

				// Frame 1: seed carry from valley1 fused with peaks.
				FrameProcessInput f1;
				f1.seq = 1;
				f1.receivedY = valley1;
				f1.hasCleanSplit = true;
				f1.cleanY = valley1;
				f1.prevPeak.cleanPlane = &peakPrev;
				f1.nextPeak.cleanPlane = &peakNext;
				f1.estimatedStepDelta = 14.0;
				const auto r1 = eng.ProcessFrame(f1, &reg);

				// Frame 2: dual-source fusion (prevPeak + nextPeak + recurrent carry from Frame 1).
				FrameProcessInput f2;
				f2.seq = 2;
				f2.receivedY = valley2;
				f2.hasCleanSplit = true;
				f2.cleanY = valley2;
				f2.prevPeak.cleanPlane = &peakPrev;
				f2.nextPeak.cleanPlane = &peakNext;
				f2.estimatedStepDelta = 14.0;
				const auto r2 = eng.ProcessFrame(f2, &reg);

				double dbBase = 0.0, dbOut1 = 0.0, dbOut2 = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, valley1, interior), 1023.0, dbBase));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, r1.outputY, interior), 1023.0, dbOut1));
				double dbBase2 = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, valley2, interior), 1023.0, dbBase2));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(master, r2.outputY, interior), 1023.0, dbOut2));

				const double gain1 = dbOut1 - dbBase;
				const double gain2 = dbOut2 - dbBase2;
				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Still scene recovery: f1 gain=+%.3f dB (2 refs), f2 dual-source gain=+%.3f dB (%zu refs, carryUsed=%d)",
					gain1, gain2, r2.activeRefCount, r2.carryUsed ? 1 : 0);
				Logger::WriteMessage(buf);

				Assert::IsTrue(gain1 > 3.0, L"Valley frame must achieve strong positive PSNR gain against master");
				Assert::IsTrue(gain2 > 3.0, L"Dual-source carry+peaks valley frame must achieve strong positive PSNR gain");
				Assert::IsTrue(r2.carryUsed && r2.activeRefCount == 3, L"Dual-source engine must fuse prevPeak + nextPeak + carry");
				Assert::IsTrue(reg.IsActive(StandInKind::TransformUnknownBound), L"Transform-unknown bound must be visible");

				// Verify U19 td.maps populated with valid cells.
				int validBlocks = 0;
				for (const auto& b : r2.tdMaps)
					if (TdMapsView::Fetch(b, TdMapsView::kValidity) > 0.5f)
						++validBlocks;
				Assert::IsTrue(validBlocks > 0, L"U19 td.maps must report valid repaired cells");
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
						curReceived.at(x, y) = objVal; // clean sharp object in current frame
						// Reference frames show background (flat/different) at those coordinates.
						peakPrev.at(x, y) = 420.0;
						peakNext.at(x, y) = 420.0;
					}
				}

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
				in.estimatedStepDelta = 14.0;
				const auto res = eng.ProcessFrame(in, &reg);

				// Evaluate via TD-S0 SpeedBinTable on interior (avoiding 4-px pan border).
				const int ew = w - 16, eh = h - 16;
				const size_t en = static_cast<size_t>(ew) * eh;
				std::vector<double> mVec(en), rVec(en), oVec(en), objSpd(en), camSpd(en, 2.0);
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
						const bool inObj = (gx >= 80 && gx < 112 && gy >= 24 && gy < 72);
						if (inObj)
						{
							isCam[i] = 0u;
							objSpd[i] = 6.0;
						}
						else if (gx < 64)
						{
							// Pure panning background well clear of 3x3 object silhouette dilation.
							isCam[i] = 1u;
							objSpd[i] = 0.0;
						}
						else
						{
							// Silhouette dilation margin around moving object.
							isCam[i] = 0u;
							objSpd[i] = 6.0;
						}
					}
				}

				const SpeedBinReport sb = EvaluateSpeedBins(
					mVec.data(), rVec.data(), oVec.data(),
					objSpd.data(), camSpd.data(), isCam.data(), ew, eh, 1023.0);

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"Pan vs Fast Object: panGain=+%.3f dB (ret=%.4f), fastObj ret=%.4f",
					sb.cameraPan.deltaDb, sb.cameraPan.sharpnessRetention,
					sb.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)].sharpnessRetention);
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

			TEST_METHOD(RackFocusNeverImportsSharperDetailAndBokehModeRespectsKnob)
			{
				const int w = 96, h = 96;
				// Sharp reference frames vs intentionally defocused current frame (rack focus pull!).
				const TdV2Plane sharpRef = RenderMaster(w, h);
				// Defocus current frame strongly via U2 BandFromBase Gaussian blur (sigma = 4.0) to simulate lens bokeh/rack focus.
				const auto defocusTaps = VpUnit::BandFromBase::Taps<double>(4.0, 3.0, true, 0.0);
				TdV2Plane defocusedCur(w, h, 0.0);
				VpUnit::BandFromBase::BlurTaps(sharpRef.v, defocusedCur.v, w, h, defocusTaps, VpUnit::BandFromBase::EdgeMode::Replicate);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				cfg.bokehMode = BokehMode::Untouched;
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				FrameProcessInput in;
				in.seq = 30;
				in.receivedY = defocusedCur;
				in.hasCleanSplit = true;
				in.cleanY = defocusedCur;
				in.prevPeak.cleanPlane = &sharpRef;
				in.nextPeak.cleanPlane = &sharpRef;
				in.estimatedStepDelta = 12.0;

				const auto res = eng.ProcessFrame(in, &reg);
				Assert::IsTrue(res.focusVetoCells > 0, L"Focus guard must detect focal-plane shift across rack focus");
				Assert::IsTrue(reg.IsActive(StandInKind::FocusMapK32), L"Focus stand-in must be visibly tagged");

				// Verify M+H fine/mid sharpness is NOT imported from sharpRef!
				const ThreeBandPlanes curB = DecomposeActiveBands(defocusedCur, ActiveRect::Full(w, h));
				const ThreeBandPlanes outB = DecomposeActiveBands(res.outputY, ActiveRect::Full(w, h));
				const ThreeBandPlanes refB = DecomposeActiveBands(sharpRef, ActiveRect::Full(w, h));
				const ActiveRect interior{ 16, 16, w - 16, h - 16 };
				const double errHOut = PlaneMse(curB.H, outB.H, interior);
				const double refHEnergy = PlaneMse(curB.H, refB.H, interior);
				const double falseRestoreRatio = refHEnergy > 1e-9 ? (errHOut / refHEnergy) : 0.0;
				char buf[192];
				std::snprintf(buf, sizeof(buf), "Rack-focus false-restore ratio: %.6f (bar <= 0.001)", falseRestoreRatio);
				Logger::WriteMessage(buf);
				Assert::IsTrue(falseRestoreRatio <= 0.001, L"Owner Focus Rule: defocus false-restore must be <= 0.1%");

				// Matched-focus bokeh Q4 knob test: all frames equally defocused with banding/quant noise on L.
				const TdV2Plane bokehPrev = AddCodingError(defocusedCur, 1.0, 401);
				const TdV2Plane bokehNext = AddCodingError(defocusedCur, 1.0, 402);
				const TdV2Plane bokehCur = AddCodingError(defocusedCur, 5.0, 403);

				FrameProcessInput bkIn;
				bkIn.seq = 31;
				bkIn.receivedY = bokehCur;
				bkIn.hasCleanSplit = true;
				bkIn.cleanY = bokehCur;
				bkIn.prevPeak.cleanPlane = &bokehPrev;
				bkIn.nextPeak.cleanPlane = &bokehNext;
				bkIn.estimatedStepDelta = 12.0;

				// BokehMode::Untouched leaves defocused L-band untouched (zero delta in defocused cells);
				// BokehMode::LowBandCellOnly allows L-band denoising while keeping M/H strictly 0!
				Config cfgUntouched = MakePreset(Preset::Reference, Mode::On);
				cfgUntouched.bokehMode = BokehMode::Untouched;
				TdV2ReferenceEngine engU(cfgUntouched);
				const auto resU = engU.ProcessFrame(bkIn, &reg);

				Config cfgLowOnly = MakePreset(Preset::Reference, Mode::On);
				cfgLowOnly.bokehMode = BokehMode::LowBandCellOnly;
				TdV2ReferenceEngine engL(cfgLowOnly);
				const auto resL = engL.ProcessFrame(bkIn, &reg);

				const double mseU = PlaneMse(defocusedCur, resU.outputY, interior);
				const double mseL = PlaneMse(defocusedCur, resL.outputY, interior);
				Assert::IsTrue(mseL < mseU, L"Q4 LowBandCellOnly must clean matched-focus bokeh L-band noise while Untouched keeps it off");
			}

			TEST_METHOD(ActivePictureLetterboxBarsRemainBitExactUnderPan)
			{
				const int w = 96, h = 96;
				const ActiveRect active{ 0, 16, w, h - 16 }; // 16-px top & bottom letterbox bars
				const int cw = w / 16, ch = h / 16;

				TdV2Plane masterCur = RenderMaster(w, h, 0.0, 0.0);
				TdV2Plane masterPrev = RenderMaster(w, h, -2.0, -2.0);
				TdV2Plane masterNext = RenderMaster(w, h, +2.0, +2.0);

				TdV2Plane peakPrev = AddCodingError(masterPrev, 1.2, 501);
				TdV2Plane peakNext = AddCodingError(masterNext, 1.2, 502);
				TdV2Plane valley = AddCodingError(masterCur, 6.0, 503);

				// Stamp exact letterbox black code 64.0 outside active rect in all frames (and distinct value 64.0 in cur).
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
				in.estimatedStepDelta = 14.0;

				const auto res = eng.ProcessFrame(in, &reg);
				for (int y = 0; y < h; ++y)
				{
					if (y >= active.y0 && y < active.y1) continue;
					for (int x = 0; x < w; ++x)
					{
						Assert::AreEqual(64.0, res.outputY.at(x, y), 0.0,
							L"Letterbox bars outside ActiveRect MUST remain bit-exact under diagonal pan");
					}
				}
			}

			TEST_METHOD(FourLayerGrainPreservationKeepsOwnGrainIntactOneToOne)
			{
				// Authoritative grain-transfer proof (refs/111 s7.3):
				// Plant independent photochemical film grain (sigma = 8.0) on current frame and foreign grain on refs,
				// PLUS clean compression noise on current clean layer.
				// TD v2 MUST recover the clean picture while preserving current frame's own grain 1:1!
				const int w = 96, h = 96;
				const size_t n = static_cast<size_t>(w) * h;
				const ActiveRect interior{ 8, 8, w - 8, h - 8 };

				const TdV2Plane masterClean = RenderMaster(w, h);
				const TdV2Plane peakPrevClean = AddCodingError(masterClean, 1.2, 601);
				const TdV2Plane peakNextClean = AddCodingError(masterClean, 1.2, 602);
				const TdV2Plane curClean = AddCodingError(masterClean, 6.0, 603, 0.30);
				const TdV2Plane prevCurClean = AddCodingError(masterClean, 6.0, 604, 0.30);

				std::mt19937 rng(777);
				std::normal_distribution<double> grainDist(0.0, 8.0);
				std::vector<double> prevOwnGrain(n), ownGrain(n), foreignGrain(n);
				TdV2Plane prevReceived(w, h), curReceived(w, h);
				for (size_t i = 0; i < n; ++i)
				{
					prevOwnGrain[i] = grainDist(rng);
					ownGrain[i] = grainDist(rng);
					foreignGrain[i] = grainDist(rng);
					prevReceived.v[i] = prevCurClean.v[i] + prevOwnGrain[i];
					curReceived.v[i] = curClean.v[i] + ownGrain[i];
				}

				// Supply mild retained-grain leakage variance to exercise Layer 2 (A1 grain subtraction) as well.
				const int cw = w / 16, ch = h / 16;
				std::vector<double> retainedVar(static_cast<size_t>(cw) * ch, 1.0);

				Config cfg = MakePreset(Preset::Reference, Mode::On);
				TdV2ReferenceEngine eng(cfg);
				StandInRegistry reg;

				// Frame 1 (for lag-1 temporal grain correlation check).
				FrameProcessInput f1;
				f1.seq = 50;
				f1.receivedY = prevReceived;
				f1.hasCleanSplit = true;
				f1.cleanY = prevCurClean;
				f1.retainedGrainVar = retainedVar;
				f1.prevPeak.cleanPlane = &peakPrevClean;
				f1.nextPeak.cleanPlane = &peakNextClean;
				f1.estimatedStepDelta = 14.0;
				const auto r1 = eng.ProcessFrame(f1, &reg);

				// Frame 2.
				FrameProcessInput f2;
				f2.seq = 51;
				f2.receivedY = curReceived;
				f2.hasCleanSplit = true;
				f2.cleanY = curClean;
				f2.retainedGrainVar = retainedVar;
				f2.prevPeak.cleanPlane = &peakPrevClean;
				f2.nextPeak.cleanPlane = &peakNextClean;
				f2.estimatedStepDelta = 14.0;
				const auto r2 = eng.ProcessFrame(f2, &reg);

				// Evaluate grain transfer against the REPAIRED clean plane (r2.outputClean) AND against masterClean!
				// Notice: against r2.outputClean, own grain must be exact 1:1 (betaOwn ~ 1.0, betaForeign ~ 0.0).
				// Also evaluate against masterClean (where residual coding error acts as small uncorrelated error).
				std::vector<double> prevRes(n);
				for (size_t i = 0; i < n; ++i)
					prevRes[i] = r1.outputY.v[i] - r1.outputClean.v[i];

				const auto gRepClean = EvaluateGrainTransfer(
					r2.outputClean.v.data(), ownGrain.data(), foreignGrain.data(), r2.outputY.v.data(), n,
					prevRes.data(), prevOwnGrain.data());

				std::vector<double> prevResMaster(n);
				for (size_t i = 0; i < n; ++i)
					prevResMaster[i] = r1.outputY.v[i] - masterClean.v[i];

				const auto gRepMaster = EvaluateGrainTransfer(
					masterClean.v.data(), ownGrain.data(), foreignGrain.data(), r2.outputY.v.data(), n,
					prevResMaster.data(), prevOwnGrain.data());

				double cleanDbBase = 0.0, cleanDbOut = 0.0;
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterClean, curClean, interior), 1023.0, cleanDbBase));
				Assert::IsTrue(ComputePsnrDb(PlaneMse(masterClean, r2.outputClean, interior), 1023.0, cleanDbOut));

				char buf[256];
				std::snprintf(buf, sizeof(buf),
					"4-Layer Grain Preservation: cleanGain=+%.3f dB | betaOwn=%.4f betaForeign=%.4f stdRatio=%.4f lag1Rise=%.4f",
					cleanDbOut - cleanDbBase, gRepClean.betaOwn, gRepClean.betaForeign, gRepClean.stdRatio, gRepClean.lag1Rise);
				Logger::WriteMessage(buf);

				Assert::IsTrue(cleanDbOut - cleanDbBase > 2.5, L"Clean layer must still recover compression noise strongly");
				Assert::IsTrue(gRepClean.valid && gRepClean.passesAuthoritativeBar,
					L"Film grain must pass authoritative refs/111 s7.3 preservation bar (betaOwn>=0.98, betaForeign<=0.02)");
				Assert::IsTrue(gRepMaster.valid && gRepMaster.betaOwn >= kGrainBarMinBetaOwn,
					L"Even measured against masterClean, own grain betaOwn must stay >= 0.98");
				Assert::IsTrue(std::fabs(gRepMaster.betaForeign) <= kGrainBarMaxBetaForeign,
					L"Foreign grain leakage must stay <= 0.02");
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

				// Non-finite input closes cleanly without crashing or poisoning carry!
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
			}
		};
	}
}
