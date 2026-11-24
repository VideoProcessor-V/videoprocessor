// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S0 tests (2026-11-23_TD-consolidated-plan.md s3.2):
// Adversarial verification of:
//   - Default-OFF config, Original pathway override, DepthScale / PeakCodeForBits / MapCellToGrainGrid bounds
//   - Reference vs RealTime preset shortcut ledger (SC-01..SC-04)
//   - Exhaustive non-silent StandInRegistry (every StandInKind token unique, formatted, hit-counted) & TdGuardPort
//   - W1-W4 worst-area masks (proving global mean hides W2 regression & worst-5% worsened fraction)
//   - Camera-relative speed-bin sharpness with exact derived physical bin edges (0.25, 1.0, 3.0, 8.0 px)
//   - Authoritative synthetic grain-transfer detection (foreign averaging, RMS drift, lag-1 rise)
//   - Non-finite fail-closed behavior across all evaluators. CPU only.
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
#include <set>
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
				Assert::IsTrue(cfg.enablePerCornerHuber, L"Default config must enable per-corner Huber taper");

				// Dynamic P10-D6 block-grid mapping: 16-px TD cell to 32-px 4K grain cell + invalid-arg guards.
				Assert::AreEqual(0, MapCellToGrainGrid(0, 16, 32));
				Assert::AreEqual(0, MapCellToGrainGrid(1, 16, 32));
				Assert::AreEqual(1, MapCellToGrainGrid(2, 16, 32));
				Assert::AreEqual(3, MapCellToGrainGrid(3, 16, 16));
				Assert::AreEqual(0, MapCellToGrainGrid(-1, 16, 32));
				Assert::AreEqual(0, MapCellToGrainGrid(2, 0, 32));
				Assert::AreEqual(0, MapCellToGrainGrid(2, 16, 0));
				Assert::AreEqual(0, MapCellToGrainGrid(2, 16, -8));

				// DepthScale: 8-bit = 0.25, 10-bit = 1.0, 12-bit = 4.0; invalid PeakCodeForBits fails closed to 0.
				Assert::AreEqual(0.25, DepthScale(8), 1e-12);
				Assert::AreEqual(1.0, DepthScale(10), 1e-12);
				Assert::AreEqual(4.0, DepthScale(12), 1e-12);
				Assert::AreEqual(1023.0, PeakCodeForBits(10), 1e-12);
				Assert::AreEqual(4095.0, PeakCodeForBits(12), 1e-12);
				Assert::AreEqual(0.0, PeakCodeForBits(0), 1e-12);
				Assert::AreEqual(0.0, PeakCodeForBits(-10), 1e-12);
				Assert::AreEqual(0.0, PeakCodeForBits(25), 1e-12);

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

			TEST_METHOD(StandInRegistryIsNeverSilentAndTracksEveryHitAndToken)
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

				// Verify ALL StandInKind enum tokens (including runtime fallbacks StepPriorDefault,
				// SingleRefA1ToA2Fallback, MotionFieldStandIn, SingularSolveClosure, BorderBilinearFallback)
				// have unique non-empty catalog keys, non-empty status text, and visible summaries.
				const size_t totalKinds = static_cast<size_t>(StandInKind::Count);
				Assert::IsTrue(totalKinds >= 21, L"All D1-D12 and runtime fallback stand-in tokens must be enumerated");
				std::set<std::string> seenKeys;
				StandInRegistry allReg;
				for (size_t i = 0; i < totalKinds; ++i)
				{
					const auto kind = static_cast<StandInKind>(i);
					const StandInMeta& meta = MetaOf(kind);
					Assert::IsTrue(meta.kind == kind, L"StandInMeta table order must match StandInKind enum");
					Assert::IsTrue(meta.catalogKey != nullptr && meta.catalogKey[0] != '\0');
					Assert::IsTrue(meta.statusText != nullptr && meta.statusText[0] != '\0');
					Assert::IsTrue(meta.swapTarget != nullptr && meta.swapTarget[0] != '\0');
					Assert::IsTrue(seenKeys.insert(meta.catalogKey).second, L"Every StandInKind catalogKey must be unique");
					allReg.Engage(kind);
					Assert::IsTrue(allReg.IsActive(kind));
					Assert::AreEqual(static_cast<uint64_t>(1), allReg.HitCount(kind));
				}
				Assert::AreEqual(totalKinds, allReg.ActiveCount());
				const std::string allSummary = allReg.FormatStatusSummary();
				Assert::IsTrue(allSummary.find("step prior:") != std::string::npos);
				Assert::IsTrue(allSummary.find("A1 single-ref fallback") != std::string::npos);
				Assert::IsTrue(allSummary.find("motion field:") != std::string::npos);
				Assert::IsTrue(allSummary.find("covariance solve singular") != std::string::npos);
				Assert::IsTrue(allSummary.find("border bilinear warp fallback") != std::string::npos);

				// Verify new Pass-2 audit stand-in tokens exist, are active, and have valid metadata.
				const StandInKind newKinds[] = {
					StandInKind::CodecModelPbStandIn,
					StandInKind::ChromaCanvasGapFilled,
					StandInKind::A2StatsInvalid,
					StandInKind::GrainPriorOutOfRange,
					StandInKind::GpuInputOutputAlias,
					StandInKind::FaceNonRigidMask,
					StandInKind::MissingMotionNoWholeBlend,
					StandInKind::FaceMaskSizeMismatch
				};
				for (StandInKind nk : newKinds)
				{
					Assert::IsTrue(allReg.IsActive(nk), L"Every Pass-2 StandInKind token must be registered in StandInRegistry");
					const StandInMeta& nm = MetaOf(nk);
					Assert::IsTrue(nm.kind == nk);
					Assert::IsTrue(allSummary.find(nm.statusText) != std::string::npos);
				}

				allReg.Clear(StandInKind::StepPriorDefault);
				Assert::IsFalse(allReg.IsActive(StandInKind::StepPriorDefault));
				Assert::AreEqual(static_cast<uint64_t>(1), allReg.HitCount(StandInKind::StepPriorDefault));
				allReg.Reset();
				Assert::IsFalse(allReg.AnyActive());
				Assert::AreEqual(static_cast<uint64_t>(0), allReg.HitCount(StandInKind::StepPriorDefault));

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
				Assert::IsTrue(rep.worst5PctMeanDeltaDb > 10.0, L"Worst 5% cells (W3) improved strongly");

				// Verify worst-5% worsened-over-0.1dB detector fires when the highest-error cell regresses!
				std::vector<double> worstRegOut = output;
				for (int y = 48; y < 64; ++y)
				{
					for (int x = 0; x < 16; ++x)
					{
						const size_t i = static_cast<size_t>(y) * w + x;
						received[i] = master[i] + 20.0; // guaranteed top-5% received error cell
						worstRegOut[i] = master[i] + 25.0; // worsened by ~-1.94 dB
					}
				}
				const WorstAreaReport repWorse = EvaluateWorstAreaMasks(
					master.data(), received.data(), worstRegOut.data(), masks.data(), w, h, 16, 1023.0);
				Assert::IsTrue(repWorse.valid);
				Assert::IsTrue(repWorse.worst5PctMeanDeltaDb < -1.0, L"Worst-5% mean must expose regression in worst cell");
				Assert::IsTrue(repWorse.worst5PctWorsenedOver01DbFrac > 0.5, L"Worsened-over-0.1dB fraction must fire");
			}

			TEST_METHOD(SpeedBinTableSeparatesCameraPanFromFastObjectsAndUsesDerivedEdges)
			{
				// Verify exact Rule-1 derived speed-bin edges:
				//   Still:    [0.0,  0.25)               (quarter-pel P3r ceiling)
				//   Slow:     [0.25, kSigmaFine = 1.0)
				//   Medium:   [1.0,  BandRadius(1.0) = 3.0)
				//   Fast:     [3.0,  0.5 * kCanonicalBlockPx = 8.0)
				//   VeryFast: [8.0,  inf)
				Assert::IsTrue(ClassifyObjectSpeed(0.0)   == ObjectSpeedBinId::Still);
				Assert::IsTrue(ClassifyObjectSpeed(0.249) == ObjectSpeedBinId::Still);
				Assert::IsTrue(ClassifyObjectSpeed(0.25)  == ObjectSpeedBinId::Slow);
				Assert::IsTrue(ClassifyObjectSpeed(0.999) == ObjectSpeedBinId::Slow);
				Assert::IsTrue(ClassifyObjectSpeed(1.0)   == ObjectSpeedBinId::Medium);
				Assert::IsTrue(ClassifyObjectSpeed(2.999) == ObjectSpeedBinId::Medium);
				Assert::IsTrue(ClassifyObjectSpeed(3.0)   == ObjectSpeedBinId::Fast);
				Assert::IsTrue(ClassifyObjectSpeed(7.999) == ObjectSpeedBinId::Fast);
				Assert::IsTrue(ClassifyObjectSpeed(8.0)   == ObjectSpeedBinId::VeryFast);
				Assert::IsTrue(ClassifyObjectSpeed(20.0)  == ObjectSpeedBinId::VeryFast);

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
							// Fast moving foreground object: v_obj = 5.0 px (inside [3.0, 8.0) Fast bin); intentionally smeared.
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
				Assert::IsFalse(rep.objectBins[static_cast<size_t>(ObjectSpeedBinId::Still)].valid,
					L"Camera pan pixels must NOT leak into Still object bin");

				const auto& fastBin = rep.objectBins[static_cast<size_t>(ObjectSpeedBinId::Fast)];
				Assert::IsTrue(fastBin.valid);
				Assert::IsTrue(fastBin.sharpnessRetention < 0.55, L"Fast object bin must catch the smearing");
				Assert::IsTrue(rep.worstObjectBin == ObjectSpeedBinId::Fast);
				Assert::IsTrue(rep.worstObjectRetention < 0.55);
			}

			TEST_METHOD(SyntheticGrainTransferDetectsForeignAveragingAndTemporalFreeze)
			{
				const size_t n = 16384;
				std::mt19937 rng(42);
				std::normal_distribution<double> dist(0.0, 5.0);

				std::vector<double> clean(n, 400.0);
				std::vector<double> ownG(n), foreignG(n), prevG(n);
				std::vector<double> intactOut(n), blendedOut(n), prevResIntact(n), prevResBlended(n), prevResFrozen(n);
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
					// Case 3: frozen lag-1 correlation (prev residual leaks 20% of current grain) — MUST FAIL lag1 bar!
					prevResFrozen[i] = 0.80 * prevG[i] + 0.20 * ownG[i];
				}

				const auto intactRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n,
					prevResIntact.data(), prevG.data());
				Assert::IsTrue(intactRep.valid);
				Assert::IsTrue(intactRep.lag1Evaluated, L"Lag-1 must be marked evaluated when prev arrays are supplied");
				Assert::IsTrue(intactRep.passesAuthoritativeBar, L"Intact 1:1 grain must pass authoritative bar");
				Assert::AreEqual(1.0, intactRep.betaOwn, 1e-3);
				Assert::AreEqual(0.0, intactRep.betaForeign, 1e-3);
				Assert::AreEqual(1.0, intactRep.stdRatio, 1e-3);

				const auto blendedRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), blendedOut.data(), n,
					prevResBlended.data(), prevG.data());
				Assert::IsTrue(blendedRep.valid);
				Assert::IsTrue(blendedRep.lag1Evaluated);
				Assert::IsFalse(blendedRep.passesAuthoritativeBar, L"10% foreign grain averaging MUST fail authoritative bar");
				Assert::AreEqual(0.90, blendedRep.betaOwn, 2e-2);
				Assert::AreEqual(0.10, blendedRep.betaForeign, 2e-2);

				const auto frozenRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n,
					prevResFrozen.data(), prevG.data());
				Assert::IsTrue(frozenRep.valid);
				Assert::IsTrue(frozenRep.lag1Evaluated);
				Assert::IsTrue(frozenRep.lag1Rise > kGrainBarMaxLag1Rise, L"Temporal grain correlation rise must be detected");
				Assert::IsFalse(frozenRep.passesAuthoritativeBar, L"Frozen lag-1 grain correlation MUST fail authoritative bar");
			}

			TEST_METHOD(AuthoritativeGrainBarRequiresLag1Evaluation)
			{
				// H11 adversarial test: EvaluateGrainTransfer MUST NOT pass passesAuthoritativeBar without
				// actually evaluating lag-1 temporal correlation (lag1Evaluated == true).
				const size_t n = 16384;
				std::mt19937 rng(20261123u);
				std::normal_distribution<double> dist(0.0, 5.0);

				std::vector<double> clean(n, 400.0);
				std::vector<double> ownG(n), foreignG(n), prevG(n);
				std::vector<double> intactOut(n), prevResIntact(n), prevResFrozen(n);
				for (size_t i = 0; i < n; ++i)
				{
					ownG[i] = dist(rng);
					foreignG[i] = dist(rng);
					prevG[i] = dist(rng);
					intactOut[i] = clean[i] + ownG[i];
					prevResIntact[i] = prevG[i];
					// Same-input temporal grain freeze hazard: previous frame leaks 20% of current frame's grain.
					prevResFrozen[i] = 0.80 * prevG[i] + 0.20 * ownG[i];
				}

				// 1. Both prev pointers null -> valid regression metrics, yet lag1Evaluated == false & bar MUST FAIL!
				const auto noPrevRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n, nullptr, nullptr);
				Assert::IsTrue(noPrevRep.valid);
				Assert::AreEqual(1.0, noPrevRep.betaOwn, 1e-3);
				Assert::AreEqual(0.0, noPrevRep.betaForeign, 1e-3);
				Assert::AreEqual(1.0, noPrevRep.stdRatio, 1e-3);
				Assert::IsFalse(noPrevRep.lag1Evaluated,
					L"Omitting prevOutputResidual/prevOwnGrain MUST set lag1Evaluated == false");
				Assert::IsFalse(noPrevRep.passesAuthoritativeBar,
					L"Authoritative grain bar MUST NOT pass when lag-1 was never evaluated");

				// 2. Half-supplied prev pointers (only one non-null) -> lag1Evaluated == false & bar MUST FAIL!
				const auto onlyPrevRes = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n, prevResIntact.data(), nullptr);
				Assert::IsTrue(onlyPrevRes.valid);
				Assert::IsFalse(onlyPrevRes.lag1Evaluated);
				Assert::IsFalse(onlyPrevRes.passesAuthoritativeBar);

				const auto onlyPrevGrain = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n, nullptr, prevG.data());
				Assert::IsTrue(onlyPrevGrain.valid);
				Assert::IsFalse(onlyPrevGrain.lag1Evaluated);
				Assert::IsFalse(onlyPrevGrain.passesAuthoritativeBar);

				// 3. Negative control on exact same current frame with frozen lag-1 prev residual:
				//    Without prev arrays this frame falsely looked perfect (betaOwn=1, betaForeign=0, stdRatio=1),
				//    proving why requiring lag1Evaluated == true is essential to catch temporal grain freeze!
				const auto frozenLagRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n,
					prevResFrozen.data(), prevG.data());
				Assert::IsTrue(frozenLagRep.valid);
				Assert::IsTrue(frozenLagRep.lag1Evaluated);
				Assert::IsTrue(frozenLagRep.lag1Rise > kGrainBarMaxLag1Rise,
					L"Negative control: frozen lag-1 residual MUST exceed +0.02 lag-1 rise bar");
				Assert::IsFalse(frozenLagRep.passesAuthoritativeBar,
					L"Negative control: frozen lag-1 residual MUST fail authoritative bar");

				// 4. Both prev arrays supplied with independent grain -> lag1Evaluated == true & bar PASSES!
				const auto fullRep = EvaluateGrainTransfer(
					clean.data(), ownG.data(), foreignG.data(), intactOut.data(), n,
					prevResIntact.data(), prevG.data());
				Assert::IsTrue(fullRep.valid);
				Assert::IsTrue(fullRep.lag1Evaluated);
				Assert::IsTrue(fullRep.lag1Rise <= kGrainBarMaxLag1Rise);
				Assert::IsTrue(fullRep.passesAuthoritativeBar,
					L"Authoritative bar must pass when betaOwn/betaForeign/RMS AND lag-1 (lag1Evaluated==true) all pass");
			}

			TEST_METHOD(NonFiniteInputsAndMetricsFailClosedWithVisibleCount)
			{
				const double qnan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();

				double db = 123.0;
				Assert::IsFalse(ComputePsnrDb(qnan, 1023.0, db));
				Assert::IsFalse(ComputePsnrDb(inf, 1023.0, db));
				Assert::IsFalse(ComputePsnrDb(-1.0, 1023.0, db));
				Assert::IsFalse(ComputePsnrDb(1.0, 0.0, db));

				std::vector<double> m(16, 100.0), r(16, 101.0), o(16, 100.0);
				std::vector<uint8_t> mask(16, kMaskTrackable);
				o[5] = qnan;
				r[9] = inf;
				const auto rep = EvaluateWorstAreaMasks(m.data(), r.data(), o.data(), mask.data(), 4, 4);
				Assert::IsFalse(rep.valid, L"Non-finite pixel must fail closed");
				Assert::AreEqual(static_cast<size_t>(2), rep.nonFiniteCount);

				// EvaluateSpeedBins must fail closed on non-finite or negative speeds.
				std::vector<double> vo(16, 1.0), vc(16, 1.0);
				std::vector<uint8_t> isCam(16, 0u);
				o[5] = 100.0;
				r[9] = 101.0;
				vo[0] = -0.5;
				vc[1] = qnan;
				const auto sbBad = EvaluateSpeedBins(m.data(), r.data(), o.data(), vo.data(), vc.data(), isCam.data(), 4, 4);
				Assert::IsFalse(sbBad.valid);
				Assert::IsTrue(sbBad.nonFiniteCount >= 2);

				// EvaluateGrainTransfer must fail closed on non-finite input.
				std::vector<double> g0(16, 1.0), gf(16, -1.0);
				g0[3] = inf;
				const auto gtBad = EvaluateGrainTransfer(m.data(), g0.data(), gf.data(), o.data(), 16);
				Assert::IsFalse(gtBad.valid);
				Assert::AreEqual(static_cast<size_t>(1), gtBad.nonFiniteCount);

				StandInRegistry reg;
				const auto gDec = TdGuardPort::Evaluate(qnan, 10.0, &reg);
				Assert::IsTrue(gDec.violated);
				Assert::AreEqual(1.0, gDec.addBackFraction, 1e-12);
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure), L"Non-finite guard input must register visibly");
			}
		};
	}
}
