// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Milestone 1 Adversarial Unit Suite — Stage A: 3-frame same-polarity residual ghost cancellation,
// physics-derived grain + gradient noise floor, 3-DOF chroma residual vs luma-only negative control,
// exact convolution-variance streak deconvolution & conserved flux, clutter-overflow abstention,
// and causal two-back (t-2, t-1, t) fallback.
#include <VP-V/modules/motion/estimation/fast_objects/tests/FastObjectSynthScenes.h>

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(FastObjectResidualTests)
	{
	public:
		TEST_METHOD(ThreeFrameResidualCancelsTwoFrameGhostAndOppositeSignFlicker)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence seq = MakeSoccerParabolicLobScene(60.0, 6);
			Config cfg;
			cfg.enabled = true;
			ResidualWorkspace ws;

			// Frame 2 has previous ball at frame 1 (p_{t-1}) and next ball at frame 3 (p_{t+1}), > 2*r apart.
			const ResidualInput in = BuildBidirectionalInput(seq, 2);
			const GroundTruthBall& gtCur = seq.frames[2].gtBalls[0];
			const GroundTruthBall& gtPrev = seq.frames[1].gtBalls[0];

			// Mandatory In-Test Negative Control: 2-frame single-pair residual creates >= 2 blobs (ball + ghost at p_{t-1}).
			const uint32_t twoFrameBlobs = CountTwoFrameSinglePairResidualBlobs(cfg, in);
			Assert::IsTrue(twoFrameBlobs >= 2u, L"2-frame residual negative control must exhibit ghost blob at prior ball position");

			// 3-frame same-polarity symmetric residual cancels both p_{t-1} and p_{t+1} ghosts cleanly!
			ResidualOutput out;
			Assert::IsTrue(DetectFastObjectBlobs(cfg, in, ws, out));
			Assert::AreEqual(1u, out.blobCount, L"3-frame residual must emit exactly 1 ghost-free blob");
			Assert::AreEqual(static_cast<int>(1), static_cast<int>(out.blobs[0].polarity));

			const double centErrPx = std::hypot(out.blobs[0].cx - gtCur.cx, out.blobs[0].cy - gtCur.cy);
			const double distToGhostPx = std::hypot(out.blobs[0].cx - gtPrev.cx, out.blobs[0].cy - gtPrev.cy);
			Assert::IsTrue(centErrPx < 1.5, L"3-frame residual centroid must lock onto current frame ball");
			Assert::IsTrue(distToGhostPx > 15.0, L"3-frame residual must completely suppress ghost at previous ball position");

			// Opposite-polarity strobe check: dark flicker in D_- paired with bright flicker in D_+ must cancel to zero.
			std::vector<float> strobePrev = seq.frames[2].halfY;
			std::vector<float> strobeCur = seq.frames[2].halfY;
			std::vector<float> strobeNext = seq.frames[2].halfY;
			const int hw = in.width;
			for (int y = 40; y < 48; ++y)
			{
				for (int x = 60; x < 68; ++x)
				{
					const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(hw) + static_cast<size_t>(x);
					// Force D_- = cur - prev > 0 while D_+ = cur - next < 0 at the exact same static patch:
					strobePrev[idx] = 0.05f;
					strobeCur[idx] = 0.45f;
					strobeNext[idx] = 0.90f;
				}
			}
			ResidualInput strobeIn = in;
			strobeIn.camPrevToCur = IdentityHomography();
			strobeIn.camCurToNext = IdentityHomography();
			strobeIn.lumaPrev = strobePrev.data();
			strobeIn.lumaCur = strobeCur.data();
			strobeIn.lumaNext = strobeNext.data();
			strobeIn.cbPrev = nullptr;
			strobeIn.crPrev = nullptr;

			ResidualOutput strobeOut;
			Config lumaCfg = cfg;
			lumaCfg.enableChromaResidual = false;
			Assert::IsTrue(DetectFastObjectBlobs(lumaCfg, strobeIn, ws, strobeOut));
			Assert::AreEqual(0u, strobeOut.blobCount, L"Opposite-sign D_- / D_+ strobe patch must cancel identically to zero");
		}

		TEST_METHOD(PhysicsNoiseAndGradientFloorSuppressesHeavyGrainAndPanningEdgesAndFade)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence seq = MakeHeavyGrainPanningLinesScene(8);
			Config cfg;
			cfg.enabled = true;
			ResidualWorkspace ws;

			// 1. Mandatory Unprotected Negative Control: zeroing grain floor & camera sigma_H causes massive false alarms
			// along crisp panning white pitch lines and sigma_g = 0.03 film grain!
			ResidualInput unprotectedIn = BuildBidirectionalInput(seq, 3, false);
			unprotectedIn.grainVarMap = nullptr;
			unprotectedIn.uniformGrainSigma = 0.0;
			unprotectedIn.uniformChromaGrainSigma = 0.0;
			unprotectedIn.camSigmaPx = 0.0;
			ResidualOutput unprotectedOut;
			const bool unprotectedRan = DetectFastObjectBlobs(cfg, unprotectedIn, ws, unprotectedOut);
			Assert::IsTrue(!unprotectedRan && unprotectedOut.clutterOverflow || unprotectedOut.activeCells > 20u,
				L"Unprotected baseline without grain+gradient noise floor must suffer false active cells on heavy grain + panning lines");

			// 2. Protected Stage A with P10g grainVarMap + sigma_H * |grad Y_t|: 0 false blobs!
			for (int fi = 1; fi + 1 < static_cast<int>(seq.frames.size()); ++fi)
			{
				const ResidualInput inP10g = BuildBidirectionalInput(seq, fi, true);
				ResidualOutput outP10g;
				Assert::IsTrue(DetectFastObjectBlobs(cfg, inP10g, ws, outP10g));
				Assert::AreEqual(0u, outP10g.blobCount, L"P10g + gradient floor must produce 0 false blobs on heavy panning grain");

				// 3. Protected Stage A with robust MAD noise fallback (grainVarMap == nullptr, uniformGrainSigma < 0): 0 false blobs!
				const ResidualInput inMad = BuildBidirectionalInput(seq, fi, false);
				ResidualOutput outMad;
				Assert::IsTrue(DetectFastObjectBlobs(cfg, inMad, ws, outMad));
				Assert::AreEqual(0u, outMad.blobCount, L"MAD noise estimator + gradient floor must produce 0 false blobs");
				Assert::IsTrue(outMad.noiseSigmaEst > 0.01f, L"MAD noise sigma estimate must reflect heavy film grain");
			}

			// 4. P4 Fade gain/offset normalization check vs unnormalized negative control.
			const SynthSequence cleanSeq = MakeSoccerParabolicLobScene(60.0, 5);
			std::vector<float> fadedPrev = cleanSeq.frames[1].halfY;
			std::vector<float> fadedNext = cleanSeq.frames[3].halfY;
			constexpr double kGainPrev = 0.72, kOffPrev = 0.04;
			constexpr double kGainNext = 1.25, kOffNext = -0.03;
			for (float& v : fadedPrev) v = static_cast<float>(v * kGainPrev + kOffPrev);
			for (float& v : fadedNext) v = static_cast<float>(v * kGainNext + kOffNext);

			ResidualInput fadeIn = BuildBidirectionalInput(cleanSeq, 2, true);
			fadeIn.lumaPrev = fadedPrev.data();
			fadeIn.lumaNext = fadedNext.data();
			Config fadeCfg = cfg;
			fadeCfg.enableChromaResidual = false;

			// Negative control: without fade normalization, global brightness shift floods active cells!
			ResidualOutput unnormOut;
			DetectFastObjectBlobs(fadeCfg, fadeIn, ws, unnormOut);
			Assert::IsTrue(unnormOut.clutterOverflow || unnormOut.blobCount != 1u,
				L"Unnormalized global fade must corrupt residual without P4 fade gain/offset compensation");

			// With exact P4 fade parameters: recovers the single soccer ball cleanly!
			fadeIn.fadePrev = true;
			fadeIn.fadeGainPrev = kGainPrev;
			fadeIn.fadeOffsetPrev = kOffPrev;
			fadeIn.fadeNext = true;
			fadeIn.fadeGainNext = kGainNext;
			fadeIn.fadeOffsetNext = kOffNext;
			ResidualOutput normOut;
			Assert::IsTrue(DetectFastObjectBlobs(fadeCfg, fadeIn, ws, normOut));
			Assert::AreEqual(1u, normOut.blobCount, L"P4 fade normalization must restore clean single-ball detection");
		}

		TEST_METHOD(ChromaResidualDetectsIsoLumaOpticYellowBallMissedByLumaOnly)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence seq = MakeChromaOnlyIsoLumaBallScene(7, 0.005);
			ResidualWorkspace ws;

			for (int fi = 1; fi + 1 < static_cast<int>(seq.frames.size()); ++fi)
			{
				const ResidualInput in = BuildBidirectionalInput(seq, fi, true);
				const GroundTruthBall& gt = seq.frames[static_cast<size_t>(fi)].gtBalls[0];

				// Mandatory Luma-Only Negative Control: |Delta Y| == 0 so luma-only is completely blind!
				Config lumaOnlyCfg;
				lumaOnlyCfg.enabled = true;
				lumaOnlyCfg.enableChromaResidual = false;
				ResidualOutput lumaOut;
				Assert::IsTrue(DetectFastObjectBlobs(lumaOnlyCfg, in, ws, lumaOut));
				Assert::AreEqual(0u, lumaOut.blobCount, L"Luma-only negative control must detect 0 blobs on iso-luma ball");

				// Luma + Half-Res Cb/Cr 3-DOF Chi^2(3) Residual: detects ball cleanly on every frame!
				Config chromaCfg;
				chromaCfg.enabled = true;
				chromaCfg.enableChromaResidual = true;
				ResidualOutput chromaOut;
				Assert::IsTrue(DetectFastObjectBlobs(chromaCfg, in, ws, chromaOut));
				Assert::IsTrue(chromaOut.chromaUsed);
				Assert::AreEqual(1u, chromaOut.blobCount, L"3-DOF luma+chroma residual must detect the iso-luma optic-yellow ball");

				const BlobCandidate& b = chromaOut.blobs[0];
				Assert::IsTrue(b.hasChroma);
				Assert::AreEqual(static_cast<int>(0), static_cast<int>(b.polarity), L"Iso-luma blob must report polarity == 0");
				Assert::IsTrue(b.cbContrast < -0.10f && b.crContrast > 0.08f, L"Chroma contrast must capture optic-yellow signature");
				Assert::IsTrue(std::hypot(b.cx - gt.cx, b.cy - gt.cy) < 1.5, L"Chroma-detected centroid must match GT within 1.5 px");
			}
		}

		TEST_METHOD(StreakMomentDeconvolutionRecoversRadiusLengthAndConservedFlux)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Render a 10 px diameter ball (r = 5.0 px) across 4 speeds (24, 48, 72, 90 px/frame) with 180-deg shutter
			// -> physical streak lengths L = 12, 24, 36, 45 px (note 45 px >> d_max = 0.03 * 540 = 16.2 px!).
			const double speedsPxFrame[4] = { 24.0, 48.0, 72.0, 90.0 };
			double fluxes[4] = {};
			double radii[4] = {};
			double lengths[4] = {};

			Config cfg;
			cfg.enabled = true;
			cfg.enableChromaResidual = false;
			ResidualWorkspace ws;

			for (int si = 0; si < 4; ++si)
			{
				SceneRecipe rec;
				rec.srcW = 960;
				rec.srcH = 540; // d_max = 16.2 px: proves 45 px streak is NOT falsely rejected as a large object!
				rec.numFrames = 5;
				rec.fps = 60.0;
				rec.shutterFraction = 0.5;
				rec.surface = SurfaceKind::TennisCourt;
				rec.camPanVxPxFrame = 0.0;
				rec.grainSigma = 0.002;
				rec.seed = 4400ull + static_cast<uint64_t>(si);

				BallSpec b;
				b.radiusPx = 5.0; // 10 px diameter
				b.x0 = 180.0;
				b.y0 = 220.0;
				const double angleTrue = 0.32; // ~18.3 deg diagonal
				b.vx0 = speedsPxFrame[si] * 60.0 * std::cos(angleTrue);
				b.vy0 = speedsPxFrame[si] * 60.0 * std::sin(angleTrue);
				b.baseY = 0.92f;
				b.textured = false;
				rec.balls.push_back(b);

				const SynthSequence seq = RenderSequence(rec);
				const ResidualInput in = BuildBidirectionalInput(seq, 2, true);
				ResidualOutput out;
				Assert::IsTrue(DetectFastObjectBlobs(cfg, in, ws, out));
				Assert::AreEqual(1u, out.blobCount, L"Motion-blurred streak must be detected without minor-axis false rejection");

				const BlobCandidate& det = out.blobs[0];
				radii[si] = det.radiusPx;
				lengths[si] = det.streakLenPx;
				fluxes[si] = det.flux;

				const double expectedL = speedsPxFrame[si] * 0.5;
				Assert::IsTrue(std::fabs(det.radiusPx - 5.0) < 1.6, L"Deconvolved minor-axis radius r_est must remain ~5 px across blur speeds");
				Assert::IsTrue(std::fabs(det.streakLenPx - expectedL) < 5.5, L"Deconvolved streak length L_est must track v * tau_s");

				const double axisDot = std::fabs(std::cos(det.streakAngleRad) * std::cos(angleTrue) +
					std::sin(det.streakAngleRad) * std::sin(angleTrue));
				if (expectedL > 15.0)
					Assert::IsTrue(axisDot > 0.97, L"Principal streak orientation must align with velocity vector within ~14 deg");
			}

			// Verify L_est increases monotonically with speed and integrated contrast flux Phi stays conserved within 25% across 3.75x blur dilution!
			Assert::IsTrue(lengths[0] < lengths[1] && lengths[1] < lengths[2] && lengths[2] < lengths[3],
				L"Deconvolved streak length must grow monotonically with ball speed");
			for (int si = 1; si < 4; ++si)
			{
				const double fluxRatio = fluxes[si] / fluxes[0];
				Assert::IsTrue(fluxRatio > 0.72 && fluxRatio < 1.28,
					L"Integrated signed contrast flux Phi must stay approximately blur-invariant across 3.75x streak dilution");
			}

			// Also verify 4x4 cell Neyman-Pearson matched filter catches a heavily diluted sub-4-sigma/px streak!
			const int hw = 240, hh = 270; // Half-1080p height so d_max = 0.03 * 540 = 16.2 px (> 8 px streak width)
			std::vector<float> zeroPlane(static_cast<size_t>(hw) * hh, 0.25f);
			std::vector<float> streakPlane = zeroPlane;
			// Noise std = 0.01 -> sigma_diff = sqrt(2) * 0.01 = 0.01414.
			// Place a 4x8 half-res streak with residual 0.030 -> per-pixel z_Y = 2.12 (< kSeedSigma = 4.0!),
			// while 4x4 cell matched-filter z_cell = 16 * 2.12 / 4 = 8.48 >= 4.0!
			for (int y = 36; y < 40; ++y)
				for (int x = 40; x < 48; ++x)
					streakPlane[static_cast<size_t>(y) * hw + x] = 0.280f;

			ResidualInput mfIn;
			mfIn.lumaPrev = zeroPlane.data();
			mfIn.lumaCur = streakPlane.data();
			mfIn.lumaNext = zeroPlane.data();
			mfIn.width = hw;
			mfIn.height = hh;
			mfIn.sourceScale = 2.0f;
			mfIn.camPrevToCur = IdentityHomography();
			mfIn.camCurToNext = IdentityHomography();
			mfIn.camPrevUsable = true;
			mfIn.camNextUsable = true;
			mfIn.uniformGrainSigma = 0.01;
			cfg.quantizationStepOverride = 1e-6;

			ResidualOutput mfOut;
			Assert::IsTrue(DetectFastObjectBlobs(cfg, mfIn, ws, mfOut));
			Assert::AreEqual(1u, mfOut.blobCount, L"Neyman-Pearson 4x4 cell matched filter must detect sub-4-sigma/px diluted streak");
			Assert::IsTrue(mfOut.blobs[0].peakSnr < 3.0f && mfOut.blobs[0].intSnr >= 4.0f);
		}

		TEST_METHOD(ClutterOverflowAbstainsVisiblyWithoutSilentTruncation)
		{
			using namespace FastObjects;

			const int hw = 160, hh = 120; // 40 x 30 = 1200 cells (> kMaxActiveCells = 256)
			std::vector<float> bg(static_cast<size_t>(hw) * hh, 0.20f);
			std::vector<float> flash = bg;
			// Activate 300 distinct 4x4 cells (> 256 cap).
			int triggered = 0;
			for (int cy = 1; cy < 29 && triggered < 300; ++cy)
			{
				for (int cx = 1; cx < 39 && triggered < 300; ++cx)
				{
					const int px = cx * kCellSizeHalfPx + 1;
					const int py = cy * kCellSizeHalfPx + 1;
					flash[static_cast<size_t>(py) * hw + px] = 0.90f;
					++triggered;
				}
			}

			Config cfg;
			cfg.enabled = true;
			cfg.enableChromaResidual = false;

			TrackerFrameInput tfi;
			tfi.frameIndex = 0;
			tfi.timestampSec = 0.0;
			tfi.dtFromPrevSec = 1.0 / 60.0;
			tfi.residual.lumaPrev = bg.data();
			tfi.residual.lumaCur = flash.data();
			tfi.residual.lumaNext = bg.data();
			tfi.residual.width = hw;
			tfi.residual.height = hh;
			tfi.residual.sourceScale = 2.0f;
			tfi.residual.camPrevToCur = IdentityHomography();
			tfi.residual.camCurToNext = IdentityHomography();
			tfi.residual.camPrevUsable = true;
			tfi.residual.camNextUsable = true;
			tfi.residual.uniformGrainSigma = 0.005;

			FastObjectTracker tracker;
			FastObjectTrackSet trackSet;
			const bool ok = tracker.ProcessFrame(cfg, tfi, nullptr, trackSet);
			Assert::IsFalse(ok, L"Clutter overflow exceeding kMaxActiveCells must return false");
			Assert::AreEqual(0u, trackSet.count, L"Clutter overflow must emit zero tracks (never silently truncate)");
			Assert::IsTrue(trackSet.diag.clutterOverflow);
			Assert::IsTrue(trackSet.diag.activeCells > kMaxActiveCells);
			Assert::IsTrue((trackSet.abstainReason & AbstainReason::ClutterOverflow) != 0u);
			Assert::AreEqual(1u, tracker.TotalClutterOverflows());
			Assert::IsTrue(trackSet.FormatStatus().find("clutter overflow") != std::string::npos);
		}

		TEST_METHOD(CausalTwoBackFallbackComposesTwoStepHomographyAndFlagsMode)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Render scene with BOTH camera pan AND subtle zoom so (H_{t-1->t} o H_{t-2->t-1})^{-1} is non-trivial!
			SceneRecipe rec;
			rec.srcW = 960;
			rec.srcH = 540;
			rec.numFrames = 6;
			rec.fps = 60.0;
			rec.shutterFraction = 0.25;
			rec.surface = SurfaceKind::SoccerGrass;
			rec.camPanVxPxFrame = 6.0;
			rec.camPanVyPxFrame = -1.5;
			rec.camZoomRatePerFrame = 0.004;
			rec.grainSigma = 0.004;
			rec.seed = 6606ull;

			BallSpec b;
			b.radiusPx = 6.0;
			b.x0 = 200.0;
			b.y0 = 260.0;
			b.vx0 = 1800.0;
			b.vy0 = -300.0;
			b.baseY = 0.92f;
			rec.balls.push_back(b);

			const SynthSequence seq = RenderSequence(rec);
			const ResidualInput causalIn = BuildCausalTwoBackInput(seq, 3);
			const GroundTruthBall& gtCur = seq.frames[3].gtBalls[0];
			const GroundTruthBall& gtPrev1 = seq.frames[2].gtBalls[0];
			const GroundTruthBall& gtPrev2 = seq.frames[1].gtBalls[0];

			Config cfg;
			cfg.enabled = true;
			ResidualWorkspace ws;
			ResidualOutput out;
			Assert::IsTrue(DetectFastObjectBlobs(cfg, causalIn, ws, out));
			Assert::IsTrue(out.causalMode);
			Assert::IsTrue((out.abstainReason & AbstainReason::CausalNoLookAhead) != 0u);
			Assert::AreEqual(1u, out.blobCount, L"Causal two-back triplet (t-2, t-1, t) must cancel both t-1 and t-2 ghosts");

			Assert::IsTrue(std::hypot(out.blobs[0].cx - gtCur.cx, out.blobs[0].cy - gtCur.cy) < 1.6,
				L"Causal two-back blob centroid must lock onto frame t ball");
			Assert::IsTrue(std::hypot(out.blobs[0].cx - gtPrev1.cx, out.blobs[0].cy - gtPrev1.cy) > 15.0);
			Assert::IsTrue(std::hypot(out.blobs[0].cx - gtPrev2.cx, out.blobs[0].cy - gtPrev2.cy) > 30.0);
		}
	};
}
