// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S0 tests (2026-11-23_TD-consolidated-plan.md s3.2):
// Verifies default-OFF config & Original pathway override, Reference vs RealTime preset shortcut ledger,
// non-silent StandInRegistry & TdGuardPort, W1-W4 worst-area masks (never hiding W2 regression in global mean),
// camera-relative speed-bin sharpness (camera pan separate from moving objects), authoritative grain-transfer
// detection, and non-finite fail-closed behavior. CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/v2/TdV2Config.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Metrics.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdV2ConfigMetricsNs
	{
		using namespace TdV2;

		TEST_CLASS(TdV2ConfigMetricsTests)
		{
		public:
			TEST_METHOD(DefaultConfigIsOffAndOriginalPathwayForcesOff)
			{
				Config cfg;
				Assert::IsTrue(cfg.mode == Mode::Off, L"TD v2 must default to Mode::Off");
				Assert::IsFalse(cfg.IsEnabled(), L"Default config must not be enabled");
				Assert::IsFalse(cfg.ModifiesPixels(), L"Default config must not modify pixels");
				Assert::IsTrue(cfg.residualMode == ResidualMode::Cell, L"Default residual mode must be Cell (Q1)");
				Assert::IsTrue(cfg.estimator == VarianceEstimator::A3_ConservativeMin, L"Default estimator must be A3");
				Assert::IsTrue(cfg.bokehMode == BokehMode::Untouched, L"Default bokeh mode must be Untouched (Q4)");

				// Dynamic P10-D6 block-grid mapping: 16-px TD cell to 32-px 4K grain cell.
				Assert::AreEqual(0, MapCellToGrainGrid(0, 16, 32));
				Assert::AreEqual(0, MapCellToGrainGrid(1, 16, 32));
				Assert::AreEqual(1, MapCellToGrainGrid(2, 16, 32));
				Assert::AreEqual(3, MapCellToGrainGrid(3, 16, 16));

				// DepthScale: 10-bit = 1.0, 12-bit = 4.0, 8-bit = 0.25.
				Assert::AreEqual(1.0, DepthScale(10), 1e-12);
				Assert::AreEqual(4.0, DepthScale(12), 1e-12);
				Assert::AreEqual(1023.0, PeakCodeForBits(10), 1e-12);
				Assert::AreEqual(4095.0, PeakCodeForBits(12), 1e-12);

				// Shadow mode enables processing without modifying pixels; Original pathway forces Off.
				cfg.mode = Mode::Shadow;
				Assert::IsTrue(cfg.IsEnabled());
				Assert::IsFalse(cfg.ModifiesPixels());
				cfg.mode = Mode::On;
				Assert::IsTrue(cfg.ModifiesPixels());
				ApplyOriginalPathwayOverride(cfg);
				Assert::IsTrue(cfg.mode == Mode::Off, L"Original pathway override must force Mode::Off");
			}

			TEST_METHOD(ReferenceVsRealTimePresetDiffMatchesShortcutLedger)
			{
				size_t count = 0;
				const ShortcutEntry* ledger = ShortcutLedger(count);
				Assert::IsTrue(ledger != nullptr && count >= 4, L"Shortcut ledger must enumerate SC-01..SC-04");
				for (size_t i = 0; i < count; ++i)
				{
					Assert::IsFalse(ledger[i].activeInReference, L"Reference preset must not activate RT shortcuts");
					Assert::IsTrue(ledger[i].activeInRealTime, L"RealTime preset must name every active shortcut");
				}

				const Config ref = MakePreset(Preset::Reference);
				const Config rt = MakePreset(Preset::RealTime);
				Assert::IsTrue(ref.mode == Mode::Off && rt.mode == Mode::Off, L"Presets keep default Mode::Off");
				Assert::AreEqual(1, ref.momentPoolRadiusCells);
				Assert::AreEqual(0, rt.momentPoolRadiusCells);
				Assert::IsTrue(ref.enableLongAnchor);
				Assert::IsFalse(rt.enableLongAnchor);
				Assert::AreEqual(2, ref.cellProjIterations);
				Assert::AreEqual(1, rt.cellProjIterations);
			}

			TEST_METHOD(StandInRegistryIsNeverSilentAndTracksEveryHit)
			{
				StandInRegistry reg;
				Assert::IsFalse(reg.AnyActive());
				Assert::AreEqual(static_cast<size_t>(0), reg.ActiveCount());

				reg.Engage(StandInKind::P15FramePattern, "CPU tile P16");
				reg.Engage(StandInKind::P16BlockGrid);
				reg.Engage(StandInKind::P16BlockGrid);
				reg.Engage(StandInKind::FocusMapK32);

				Assert::IsTrue(reg.AnyActive());
				Assert::AreEqual(static_cast<size_t>(3), reg.ActiveCount());
				Assert::AreEqual(static_cast<uint64_t>(2), reg.HitCount(StandInKind::P16BlockGrid));
				Assert::AreEqual(static_cast<uint64_t>(1), reg.HitCount(StandInKind::P15FramePattern));

				const std::string summary = reg.FormatStatusSummary();
				Assert::IsTrue(summary.find("P16 grid: TD stand-in") != std::string::npos, L"P16 stand-in must be visible");
				Assert::IsTrue(summary.find("focus_map: TD E2/E3 stand-in") != std::string::npos, L"Focus stand-in must be visible");
				Assert::IsTrue(summary.find("CPU tile P16") != std::string::npos, L"Detail token must be preserved");

				// Verify TdGuardPort triggers visibly when output fine variance drops > 2% in RMS.
				const auto okDec = TdGuardPort::Evaluate(100.0, 99.0, &reg);
				Assert::IsFalse(okDec.violated);
				Assert::AreEqual(0.0, okDec.addBackFraction, 1e-12);
				Assert::IsTrue(reg.IsActive(StandInKind::GrainGuardW4c));

				const auto badDec = TdGuardPort::Evaluate(100.0, 64.0, &reg);
				Assert::IsTrue(badDec.violated, L"20% RMS drop must trigger grain guard violation");
				// std drops from 10 to 8; target 9.8 -> addBack = (9.8 - 8.0) / (10.0 - 8.0) = 0.9.
				Assert::AreEqual(0.9, badDec.addBackFraction, 1e-9);
			}

			TEST_METHOD(WorstAreaMasksIsolateW1ToW4AndExposeHiddenW2Regression)
			{
				const int w = 64, h = 64;
				const size_t n = static_cast<size_t>(w) * h;
				std::vector<double> master(n, 200.0);
				std::vector<double> received(n, 200.0);
				std::vector<double> output(n, 200.0);
				std::vector<uint8_t> masks(n, kMaskTrackable);

				// Quadrant layout (32x32 each):
				//   TL: W1 banded sky (output improves step error & MSE)
				//   TR: W2 dark still (output REGRESSES by > 1 dB while global improves!)
				//   BL: W3 between keyframes (output improves strongly)
				//   BR: W4 chroma edge (output improves)
				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const size_t i = static_cast<size_t>(y) * w + x;
						if (y < 32 && x < 32)
						{
							masks[i] |= kMaskW1BandedSky;
							master[i] = 400.0 + 0.5 * x;
							// Banded quantization step every 8 px in received, smoothed closer to master in output.
							received[i] = 400.0 + 4.0 * std::floor((0.5 * x) / 4.0);
							output[i] = master[i] + 0.25 * (received[i] - master[i]);
						}
						else if (y < 32 && x >= 32)
						{
							masks[i] |= kMaskW2DarkStill;
							master[i] = 80.0;
							received[i] = 80.0 + ((x + y) % 2 == 0 ? 2.0 : -2.0);
							// Hidden regression in W2: output doubles the error (~ -6 dB in W2).
							output[i] = 80.0 + ((x + y) % 2 == 0 ? 4.0 : -4.0);
						}
						else if (y >= 32 && x < 32)
						{
							masks[i] |= kMaskW3BetweenKeyframes;
							master[i] = 300.0;
							received[i] = 300.0 + ((x % 2 == 0) ? 10.0 : -10.0);
							output[i] = 300.0 + ((x % 2 == 0) ? 2.0 : -2.0);
						}
						else
						{
							masks[i] |= kMaskW4ChromaEdges;
							master[i] = 500.0;
							received[i] = 500.0 + ((y % 2 == 0) ? 8.0 : -8.0);
							output[i] = 500.0 + ((y % 2 == 0) ? 3.0 : -3.0);
						}
					}
				}

				const WorstAreaReport rep = EvaluateWorstAreaMasks(master.data(), received.data(), output.data(), masks.data(), w, h, 16, 1023.0);
				Assert::IsTrue(rep.valid);
				Assert::IsTrue(rep.global.deltaDb > 2.0, L"Global mean PSNR improves strongly overall");
				Assert::IsTrue(rep.w1BandedSky.deltaDb > 6.0, L"W1 sky improves");
				Assert::IsTrue(rep.w1BandingStepReduction > 0.5, L"W1 banding step error reduced by > 50%");
				Assert::IsTrue(rep.w2DarkStill.deltaDb < -5.0, L"W2 mask MUST expose the hidden dark-still regression");
				Assert::IsTrue(rep.w3BetweenKeyframes.deltaDb > 10.0, L"W3 improves");
				Assert::IsTrue(rep.w4ChromaEdges.deltaDb > 6.0, L"W4 improves");
				Assert::IsTrue(rep.trackableCellCount == 16);
				Assert::IsTrue(rep.worst5PctCellCount >= 1);
			}

			TEST_METHOD(SpeedBinTableSeparatesCameraPanFromFastObjects)
			{
				const int w = 64, h = 32;
				const size_t n = static_cast<size_t>(w) * h;
				std::vector<double> master(n), received(n), output(n);
				std::vector<double> objSpeed(n, 0.0), camSpeed(n, 4.0);
				std::vector<uint8_t> isCamBg(n, 1);

				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const size_t i = static_cast<size_t>(y) * w + x;
						const double tex = 20.0 * std::sin(0.8 * x) * std::cos(0.7 * y);
						master[i] = 500.0 + tex;
						received[i] = master[i] + ((x + y) % 2 == 0 ? 4.0 : -4.0);
						if (x < 32)
						{
							// Camera pan background: v_cam = 4.0 px, v_obj = 0.0 px; full sharpness & gain.
							isCamBg[i] = 1;
							objSpeed[i] = 0.0;
							output[i] = master[i] + ((x + y) % 2 == 0 ? 1.0 : -1.0);
						}
						else
						{
							// Fast moving foreground object: v_obj = 5.0 px; intentionally smeared in output.
							isCamBg[i] = 0;
							objSpeed[i] = 5.0;
							output[i] = 500.0 + 0.4 * tex; // 60% sharpness loss
						}
					}
				}

				const SpeedBinReport rep = EvaluateSpeedBins(
					master.data(), received.data(), output.data(),
					objSpeed.data(), camSpeed.data(), isCamBg.data(), w, h, 1023.0);
				Assert::IsTrue(rep.valid);
				Assert::IsTrue(rep.cameraPan.valid);
				Assert::IsTrue(rep.cameraPan.sharpnessRetention > 0.95, L"Camera pan retains full sharpness");
				Assert::IsTrue(rep.cameraPan.deltaDb > 6.0, L"Camera pan shows positive dB gain");

				const auto& fastBin = rep.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)];
				Assert::IsTrue(fastBin.valid);
				Assert::IsTrue(fastBin.sharpnessRetention < 0.55, L"Fast object bin must catch the smearing");
				Assert::IsTrue(rep.worstObjectBin == ObjectSpeedBinId::Fast);
				Assert::IsTrue(rep.worstObjectRetention < 0.55);
			}

			TEST_METHOD(SyntheticGrainTransferDetectsForeignAveraging)
			{
				const size_t n = 16384;
				std::mt19937 rng(42);
				std::normal_distribution<double> dist(0.0, 5.0);

				std::vector<double> clean(n, 400.0);
				std::vector<double> ownG(n), foreignG(n), prevG(n);
				std::vector<double> intactOut(n), blendedOut(n), prevResIntact(n), prevResBlended(n);
				for (size_t i = 0; i < n; ++i)
				{
					ownG[i] = dist(rng);
					foreignG[i] = dist(rng);
					prevG[i] = dist(rng);
					// Case 1: own grain preserved 1:1 (intact).
					intactOut[i] = clean[i] + ownG[i];
					prevResIntact[i] = prevG[i];
					// Case 2: 10% blend with foreign grain (90% own + 10% foreign) — MUST FAIL authoritative bar!
					blendedOut[i] = clean[i] + 0.90 * ownG[i] + 0.10 * foreignG[i];
					prevResBlended[i] = 0.90 * prevG[i] + 0.10 * ownG[i];
				}

				const auto intactRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n,
					prevResIntact.data(), prevG.data());
				Assert::IsTrue(intactRep.valid);
				Assert::IsTrue(intactRep.passesAuthoritativeBar, L"Intact 1:1 grain must pass authoritative bar");
				Assert::AreEqual(1.0, intactRep.betaOwn, 1e-3);
				Assert::AreEqual(0.0, intactRep.betaForeign, 1e-3);
				Assert::AreEqual(1.0, intactRep.stdRatio, 1e-3);

				const auto blendedRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), blendedOut.data(), n,
					prevResBlended.data(), prevG.data());
				Assert::IsTrue(blendedRep.valid);
				Assert::IsFalse(blendedRep.passesAuthoritativeBar, L"10% foreign grain averaging MUST fail authoritative bar");
				Assert::AreEqual(0.90, blendedRep.betaOwn, 2e-2);
				Assert::AreEqual(0.10, blendedRep.betaForeign, 2e-2);
			}

			TEST_METHOD(NonFiniteInputsAndMetricsFailClosedWithVisibleCount)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();

				double db = 123.0;
				Assert::IsFalse(ComputePsnrDb(qnan, 1023.0, db));
				Assert::IsFalse(ComputePsnrDb(inf, 1023.0, db));
				Assert::IsFalse(ComputePsnrDb(-1.0, 1023.0, db));

				std::vector<double> m(16, 100.0), r(16, 101.0), o(16, 100.0);
				std::vector<uint8_t> mask(16, kMaskTrackable);
				o[5] = qnan;
				r[9] = inf;
				const auto rep = EvaluateWorstAreaMasks(m.data(), r.data(), o.data(), mask.data(), 4, 4);
				Assert::IsFalse(rep.valid, L"Non-finite pixel must fail closed");
				Assert::AreEqual(static_cast<size_t>(2), rep.nonFiniteCount);

				StandInRegistry reg;
				const auto gDec = TdGuardPort::Evaluate(qnan, 10.0, &reg);
				Assert::IsTrue(gDec.violated);
				Assert::AreEqual(1.0, gDec.addBackFraction, 1e-12);
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure), L"Non-finite guard input must register visibly");
			}
		};
	}
}
