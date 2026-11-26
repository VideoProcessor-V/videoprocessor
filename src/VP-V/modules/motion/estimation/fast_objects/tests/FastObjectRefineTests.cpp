// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Milestone 1 Adversarial Unit Suite — Stage B.5 & Stage C Consumer Overlay:
// Masked pyramid ZNCC sub-pel EPE proof (with unmasked block negative control), differential 1D
// motion-blur streak PSF refinement, transparent kCentroidVector fallback on tiny/textureless discs,
// FastObjectOverlay curved/impulse interpolation & swept disocclusion & TD protection mask, and
// end-to-end Soccer/Hockey/Tennis benchmark vs Unprotected Naive 16x16 Block Matcher baseline.
#include <VP-V/modules/motion/estimation/fast_objects/tests/FastObjectSynthScenes.h>

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(FastObjectRefineTests)
	{
	public:
		TEST_METHOD(MaskedZnccAchievesSubPixelEpeOverPanningTexturedBackground)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Render a 12 px textured soccer ball moving by a non-integer image displacement (+24.35, -8.85) px
			// while the crisp-lined 1080p grass pitch pans by (-6.25, +0.40) px!
			SceneRecipe rec;
			rec.srcW = 1920;
			rec.srcH = 1080;
			rec.numFrames = 4;
			rec.fps = 60.0;
			rec.shutterFraction = 0.08;
			rec.surface = SurfaceKind::SoccerGrass;
			rec.camPanVxPxFrame = 6.25;
			rec.camPanVyPxFrame = -0.40;
			rec.grainSigma = 0.002;
			rec.seed = 7701ull;

			BallSpec b;
			b.radiusPx = 6.0;
			b.x0 = 620.40;
			b.y0 = 526.20;
			b.vx0 = 30.60 * 60.0;
			b.vy0 = -9.25 * 60.0;
			b.baseY = 0.90f;
			b.textured = true;
			rec.balls.push_back(b);

			const SynthSequence seq = RenderSequence(rec);
			const SynthFrame& fA = seq.frames[1];
			const SynthFrame& fB = seq.frames[2];
			const GroundTruthBall& gt = fA.gtBalls[0];

			// 1. Mandatory In-Test Negative Control: unmasked 16x16 SAD block matcher on the exact same frame pair
			// suffers >= 15 px EPE because the 24x24 support window is dominated by panning grass + pitch lines!
			double naiveCentroidEpe = 0.0;
			const double naivePxEpe = MeasureNaive16x16BlockMatcherBallEpe(fA, fB, 48, &naiveCentroidEpe);
			Assert::IsTrue(naivePxEpe >= 15.0 && naiveCentroidEpe >= 15.0,
				L"Unprotected 16x16 block matcher negative control must lock onto panning background (>= 15 px EPE)");

			// 2. Build compact 16x16 circular support mask around the ball at frame A and verify Stage B.5 masked pyramid ZNCC
			// refines from a perturbed (+2.2, -1.8 px) seed to < 0.30 px sub-pel EPE!
			RefineInput rin;
			rin.lumaA = fA.fullY.data();
			rin.lumaB = fB.fullY.data();
			rin.width = fA.srcW;
			rin.height = fA.srcH;
			rin.planeScale = 1.0f;
			rin.camAToB = fB.camPrevToCur;
			rin.camUsable = true;
			rin.picture = fA.picture;
			rin.cx = gt.cx;
			rin.cy = gt.cy;
			rin.predVx = gt.vx + 2.2f; // Deliberately perturbed initial trajectory seed (> 2.8 px error)
			rin.predVy = gt.vy - 1.8f;
			rin.radiusPx = gt.radiusPx;
			rin.searchRadiusPx = 10.0f;
			rin.maskBox.left = static_cast<int>(std::floor(gt.cx - gt.radiusPx));
			rin.maskBox.top = static_cast<int>(std::floor(gt.cy - gt.radiusPx));
			rin.maskBox.right = static_cast<int>(std::ceil(gt.cx + gt.radiusPx));
			rin.maskBox.bottom = static_cast<int>(std::ceil(gt.cy + gt.radiusPx));
			MaskClear(rin.maskBits);
			for (int my = 0; my < kMaskDim; ++my)
			{
				for (int mx = 0; mx < kMaskDim; ++mx)
				{
					const double ux = (mx + 0.5) / static_cast<double>(kMaskDim) * 2.0 - 1.0;
					const double uy = (my + 0.5) / static_cast<double>(kMaskDim) * 2.0 - 1.0;
					if (ux * ux + uy * uy <= 1.0)
						MaskSetBit(rin.maskBits, mx, my);
				}
			}
			rin.hasMaskBits = true;

			RefineWorkspace ws;
			RefineOutput refOut;
			RefineFastObjectVector(rin, ws, refOut);

			Assert::IsFalse(refOut.usedCentroidFallback, L"Textured 12 px ball must refine via masked ZNCC without fallback");
			Assert::IsTrue(refOut.usedBackgroundSub, L"Disjoint fast mover must subtract camera-warped background");
			Assert::IsTrue(refOut.bestZncc > 0.85f, L"Masked ZNCC correlation on true ball shift must exceed 0.85");

			const double refinedEpe = std::hypot(refOut.vx - gt.vx, refOut.vy - gt.vy);
			Assert::IsTrue(refinedEpe < 0.30, L"Masked pyramid ZNCC + parabolic sub-pel fit must achieve < 0.30 px EPE");
		}

		TEST_METHOD(DifferentialMotionBlurRefinementMatchesAcceleratingStreak)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Render an accelerating streak: frame A has blur length L_A = 8 px, frame B has blur length L_B = 26 px (Delta L = +18 px)!
			const int w = 320, h = 200;
			std::vector<float> lumaA(static_cast<size_t>(w) * h, 0.20f);
			std::vector<float> lumaB(static_cast<size_t>(w) * h, 0.20f);

			auto stampBlurredBall = [&](std::vector<float>& img, double cx, double cy, double r, double streakL)
			{
				for (int s = 0; s < kSubframes; ++s)
				{
					const double u = (s + 0.5) / static_cast<double>(kSubframes) - 0.5;
					const double sx = cx + u * streakL;
					const double sy = cy;
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const double dx = (x + 0.5) - sx;
							const double dy = (y + 0.5) - sy;
							const double d = std::hypot(dx, dy);
							const float a = static_cast<float>(std::fmin(1.0, std::fmax(0.0, r + 0.5 - d)));
							if (a > 0.0f)
							{
								const float pat = static_cast<float>(0.88 + 0.10 * std::cos(dx * 0.9) * std::cos(dy * 0.9));
								img[static_cast<size_t>(y) * w + x] += (pat - 0.20f) * a / static_cast<float>(kSubframes);
							}
						}
					}
				}
			};

			const double trueVx = 42.30, trueVy = 3.20;
			stampBlurredBall(lumaA, 90.0, 100.0, 5.0, 8.0);
			stampBlurredBall(lumaB, 90.0 + trueVx, 100.0 + trueVy, 5.0, 26.0);

			RefineInput rin;
			rin.lumaA = lumaA.data();
			rin.lumaB = lumaB.data();
			rin.width = w;
			rin.height = h;
			rin.planeScale = 1.0f;
			rin.camAToB = IdentityHomography();
			rin.camUsable = true;
			rin.cx = 90.0f;
			rin.cy = 100.0f;
			rin.predVx = 41.0f;
			rin.predVy = 4.0f;
			rin.radiusPx = 5.0f;
			rin.streakLenPx = 12.0f; // > radiusPx so streak blur path is eligible
			rin.streakAngleRad = 0.0f;
			rin.blurAxisX = 1.0f;
			rin.blurAxisY = 0.0f;
			rin.searchRadiusPx = 8.0f;
			rin.maskBox.left = 78;
			rin.maskBox.top = 92;
			rin.maskBox.right = 102;
			rin.maskBox.bottom = 108;
			rin.hasMaskBits = false;

			RefineWorkspace ws;

			// Negative control: without differential blur synthesis (deltaBlurLenPx = 0), mismatched streak lengths degrade ZNCC!
			rin.deltaBlurLenPx = 0.0f;
			RefineOutput noBlurOut;
			RefineFastObjectVector(rin, ws, noBlurOut);
			Assert::IsFalse(noBlurOut.usedDifferentialBlur);

			// With physical differential blur synthesis (deltaBlurLenPx = +18.0 px): higher ZNCC and accurate sub-pel match!
			rin.deltaBlurLenPx = 18.0f;
			RefineOutput diffBlurOut;
			RefineFastObjectVector(rin, ws, diffBlurOut);
			Assert::IsTrue(diffBlurOut.usedDifferentialBlur);
			Assert::IsTrue(diffBlurOut.bestZncc > noBlurOut.bestZncc + 0.04f,
				L"1D differential motion-blur PSF synthesis must improve masked ZNCC correlation over mismatched-blur baseline");
			Assert::IsTrue(std::hypot(diffBlurOut.vx - trueVx, diffBlurOut.vy - trueVy) < 0.50,
				L"Differential blur refinement must recover sub-half-pixel displacement on accelerating streak");
		}

		TEST_METHOD(TinyAndTexturelessDiscsFallbackTransparentlyToCentroidVector)
		{
			using namespace FastObjects;

			const int w = 160, h = 120;
			std::vector<float> planeA(static_cast<size_t>(w) * h, 0.5f);
			std::vector<float> planeB(static_cast<size_t>(w) * h, 0.5f);

			RefineWorkspace ws;
			RefineInput rin;
			rin.lumaA = planeA.data();
			rin.lumaB = planeB.data();
			rin.width = w;
			rin.height = h;
			rin.planeScale = 1.0f;
			rin.cx = 60.0f;
			rin.cy = 60.0f;
			rin.predVx = 19.25f;
			rin.predVy = -7.50f;

			// 1. Tiny ball (r_est = 2.2 px < 3.0 px): must fall back to trajectory centroid vector visibly!
			rin.radiusPx = 2.2f;
			RefineOutput tinyOut;
			RefineFastObjectVector(rin, ws, tinyOut);
			Assert::IsTrue(tinyOut.usedCentroidFallback);
			Assert::AreEqual(19.25f, tinyOut.vx, 0.0f);
			Assert::AreEqual(-7.50f, tinyOut.vy, 0.0f);

			// 2. Larger disc (r_est = 5.0 px) on completely flat/textureless image: zero ZNCC curvature -> transparent fallback!
			rin.radiusPx = 5.0f;
			RefineOutput flatOut;
			RefineFastObjectVector(rin, ws, flatOut);
			Assert::IsTrue(flatOut.usedCentroidFallback);
			Assert::AreEqual(19.25f, flatOut.vx, 0.0f);
			Assert::AreEqual(-7.50f, flatOut.vy, 0.0f);

			// 3. End-to-end tracker check on a tiny golf-ball-sized mover (r = 2.2 px): sets kCentroidVector and stays Valid!
			FastObjectSynth::SceneRecipe rec;
			rec.srcW = 960;
			rec.srcH = 540;
			rec.numFrames = 8;
			rec.fps = 60.0;
			rec.shutterFraction = 0.15;
			rec.surface = FastObjectSynth::SurfaceKind::SoccerGrass;
			rec.camPanVxPxFrame = 4.0;
			rec.grainSigma = 0.004;

			FastObjectSynth::BallSpec golf;
			golf.radiusPx = 2.1; // < 3.0 px -> triggers kCentroidVector fallback in Stage B.5
			golf.x0 = 220.0;
			golf.y0 = 240.0;
			golf.vx0 = 1320.0;
			golf.vy0 = -360.0;
			golf.baseY = 0.98f;
			golf.textured = false;
			rec.balls.push_back(golf);

			const FastObjectSynth::SynthSequence seq = FastObjectSynth::RenderSequence(rec);
			Config cfg;
			cfg.enabled = true;
			FastObjectTracker tracker;
			for (int fi = 1; fi + 1 < static_cast<int>(seq.frames.size()); ++fi)
				tracker.PushFrame(cfg, FastObjectSynth::BuildTrackerFrameInput(seq, fi, true), nullptr);

			FastObjectTrackSet ts;
			const auto rp = FastObjectSynth::BuildRefinePlanes(seq, 3);
			Assert::IsTrue(tracker.ResolveAt(cfg, 2, &rp, ts));
			Assert::AreEqual(1u, ts.diag.validTracks, L"Tiny golf ball with centroid fallback must remain Valid");
			Assert::AreEqual(1u, ts.diag.centroidFallbackTracks);
			Assert::IsTrue((ts.tracks[0].flags & TrackFlags::kCentroidVector) != 0u,
				L"Tiny ball must visibly set TrackFlags::kCentroidVector");
		}

		TEST_METHOD(OverlayCurvedAndImpulseInterpolationEliminatesChordErrorAndProtectsTd)
		{
			using namespace FastObjects;

			// 1. Parabolic Ballistic mid-frame curve accuracy vs linear chord negative control!
			FastObjectTrack bal;
			bal.id = 7;
			bal.type = TrajectoryType::Ballistic;
			bal.state = ValidityState::Valid;
			bal.cx = 200.0f;
			bal.cy = 300.0f;
			bal.vx = 40.0f;
			bal.vy = 0.0f; // Symmetric apex between t and t+1!
			bal.pairSeconds = 1.0f / 30.0f;
			bal.ax = 0.0f;
			bal.ay = 7200.0f; // ay * dt^2 = 8.0 px -> true mid-frame apex is at y = 300 - 0.125 * 8 = 299.0 px!
			bal.radiusPx = 6.0f;
			bal.confidence = 0.90f;
			bal.disocclusionCapsule = { bal.cx, bal.cy, bal.cx + bal.vx, bal.cy + bal.vy, bal.radiusPx };

			float midX = 0.0f, midY = 0.0f;
			EvaluateTrackPosition(bal, 0.5f, midX, midY);
			// Negative control: naive linear chord puts midY at 300.0f (1.0 px sagitta error), while parabolic curve hits 299.0f!
			const float linearChordMidY = bal.cy + 0.5f * bal.vy;
			Assert::IsTrue(std::fabs(linearChordMidY - 299.0f) >= 0.99f, L"Linear chord baseline misses parabolic apex");
			Assert::AreEqual(220.0f, midX, 1e-4f);
			Assert::AreEqual(299.0f, midY, 1e-4f, L"EvaluateTrackPosition must hit exact parabolic apex at phase 0.5");

			// 2. Impulse two-leg court bounce interpolation: passes through bounce vertex (230, 325) at tau=0.5 with zero underground overshoot!
			FastObjectTrack imp = bal;
			imp.id = 9;
			imp.type = TrajectoryType::Impulse;
			imp.cx = 200.0f; imp.cy = 300.0f;
			imp.vx = 60.0f;  imp.vy = -10.0f; // Ends at (260, 290)
			imp.ax = 0.0f;   imp.ay = 0.0f;
			imp.hasImpulse = true;
			imp.impulseTau = 0.5f;
			imp.impulseX = 230.0f;
			imp.impulseY = 325.0f; // Court floor at y = 325!
			imp.flags = TrackFlags::kImpulsePair | TrackFlags::kBlurStretched;
			imp.streakLenPx = 28.0f;
			imp.streakAngleRad = 0.4f;
			imp.maskBox = { 194, 294, 206, 306 };
			imp.hasMaskBits = false;
			imp.disocclusionCapsule = { imp.cx, imp.cy, imp.cx + imp.vx, imp.cy + imp.vy, imp.radiusPx };

			float vxAtBounce = 0.0f, vyAtBounce = 0.0f;
			EvaluateTrackPosition(imp, 0.5f, vxAtBounce, vyAtBounce);
			// Negative control: linear chord at phase 0.5 is at y = 295 (30 px above court floor!), missing the bounce completely!
			Assert::IsTrue(std::fabs((imp.cy + 0.5f * imp.vy) - 325.0f) >= 29.0f);
			Assert::AreEqual(230.0f, vxAtBounce, 1e-4f);
			Assert::AreEqual(325.0f, vyAtBounce, 1e-4f, L"Impulse interpolation must hit exact court bounce vertex without overshoot");

			// Verify zero overshoot below y = 325 across all phases in [0, 1]!
			for (int p = 0; p <= 20; ++p)
			{
				float px = 0.0f, py = 0.0f;
				EvaluateTrackPosition(imp, static_cast<float>(p) * 0.05f, px, py);
				Assert::IsTrue(py <= 325.001f, L"Impulse curve must never overshoot below the court floor vertex");
			}

			// 3. SampleObjectWarp, swept two-leg disocclusion trail, TD protection mask, SearchHints ROIs, & MicroRoiDeblurHint!
			FastObjectTrackSet ts;
			ts.tracks[0] = imp;
			ts.count = 1;
			ts.mode = SchedulerMode::ActiveT1;

			// Query right at bounce vertex (230, 325) at phaseT = 0.5 -> hits Valid object!
			float toPrevX = 0.0f, toPrevY = 0.0f, toNextX = 0.0f, toNextY = 0.0f;
			bool disocc = false;
			uint32_t hitId = 0;
			Assert::IsTrue(SampleObjectWarp(ts, 230.0f, 325.0f, 0.5f, toPrevX, toPrevY, toNextX, toNextY, disocc, &hitId));
			Assert::AreEqual(9u, hitId);
			Assert::IsFalse(disocc);
			Assert::AreEqual(-30.0f, toPrevX, 1e-4f);
			Assert::AreEqual(-25.0f, toPrevY, 1e-4f);
			Assert::AreEqual(30.0f, toNextX, 1e-4f);
			Assert::AreEqual(-35.0f, toNextY, 1e-4f);

			// Query along incoming swept trail (215, 312.5) at phaseT = 0.5 (ball has already moved to 230, 325) -> disoccluded!
			Assert::IsFalse(SampleObjectWarp(ts, 215.0f, 312.5f, 0.5f, toPrevX, toPrevY, toNextX, toNextY, disocc));
			Assert::IsTrue(disocc, L"Swept trail behind moving ball must set outDisoccluded = true for clean background fill");

			// Query far outside ball & trail (100, 100) -> 100% untouched dense field!
			Assert::IsFalse(SampleObjectWarp(ts, 100.0f, 100.0f, 0.5f, toPrevX, toPrevY, toNextX, toNextY, disocc));
			Assert::IsFalse(disocc);

			// Temporal Detail protection (dilated r_est + 2 px around ball & two-leg swept trail)!
			Assert::IsTrue(IsProtectedForTemporalDetail(ts, 230.0f, 332.5f, 2.0f),
				L"TD protection mask must cover r_est (6) + dilate (2) = 8 px around bounce vertex");
			Assert::IsFalse(IsProtectedForTemporalDetail(ts, 230.0f, 335.0f, 2.0f));

			// BuildTinyMoverRois & MicroRoiDeblurHint!
			std::vector<CameraModel::SearchHints::Roi> rois;
			BuildTinyMoverRois(ts, 120, 68, 16.0, rois);
			Assert::AreEqual(static_cast<size_t>(2), rois.size(), L"BuildTinyMoverRois must emit launch and landing block ROIs");

			float ax = 0.0f, ay = 0.0f, slen = 0.0f;
			Assert::IsTrue(MicroRoiDeblurHint(ts, 200.0f, 300.0f, ax, ay, slen));
			Assert::AreEqual(28.0f, slen, 1e-4f);
			Assert::AreEqual(1.0f, std::hypot(ax, ay), 1e-4f);
		}

		TEST_METHOD(EndToEndSportsBenchmarkBeatsNaiveSixteenBySixteenBlockMatcher)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			struct BenchCase
			{
				const wchar_t* name;
				SynthSequence seq;
				double maxVelEpePx;
				double maxSmallFastPxEpe;
				double minNaiveBlockEpe;
			};

			std::vector<BenchCase> cases = {
				{ L"Soccer60p", MakeSoccerParabolicLobScene(60.0, 11), 0.45, 3.5, 15.0 },
				{ L"Soccer30p", MakeSoccerParabolicLobScene(30.0, 10), 0.65, 3.8, 15.0 },
				{ L"Hockey60p", MakeHockeySlapshotGlideScene(11),      0.55, 3.5, 15.0 },
				{ L"Tennis60p", MakeTennisServeBounceScene(11),        1.00, 4.0, 15.0 }
			};

			Config cfg;
			cfg.enabled = true;

			for (const BenchCase& bc : cases)
			{
				FastObjectTracker tracker;
				const int numTriplets = static_cast<int>(bc.seq.frames.size()) - 2;
				int nextPush = 0;

				double sumNaivePxEpe = 0.0;
				double sumTrackedPxEpe = 0.0;
				double sumVelEpe = 0.0;
				int evalFrames = 0;

				for (int t = 1; t < numTriplets - 1; ++t)
				{
					const int maxPush = std::min(numTriplets - 1, t + 4);
					while (nextPush <= maxPush)
					{
						tracker.PushFrame(cfg, BuildTrackerFrameInput(bc.seq, nextPush + 1, true), nullptr);
						++nextPush;
					}

					const int fi = t + 1;
					const TrackerRefinePlanes rp = BuildRefinePlanes(bc.seq, fi);
					FastObjectTrackSet ts;
					tracker.ResolveAt(cfg, static_cast<uint64_t>(t), &rp, ts);

					const SynthFrame& fA = bc.seq.frames[static_cast<size_t>(fi)];
					const SynthFrame& fB = bc.seq.frames[static_cast<size_t>(fi + 1)];
					const GroundTruthBall& gt = fA.gtBalls[0];

					sumNaivePxEpe += MeasureNaive16x16BlockMatcherBallEpe(fA, fB, 72);
					sumTrackedPxEpe += MeasureTrackedSmallFastPixelEpe(fA, fB, ts);

					Assert::IsTrue(ts.diag.validTracks >= 1u, bc.name);
					for (uint32_t k = 0; k < ts.count; ++k)
					{
						if (ts.tracks[k].state == ValidityState::Valid)
						{
							sumVelEpe += std::hypot(ts.tracks[k].vx - gt.vx, ts.tracks[k].vy - gt.vy);
							break;
						}
					}
					++evalFrames;
				}

				const double meanNaiveEpe = sumNaivePxEpe / std::max(1, evalFrames);
				const double meanTrackedPxEpe = sumTrackedPxEpe / std::max(1, evalFrames);
				const double meanVelEpe = sumVelEpe / std::max(1, evalFrames);

				Assert::IsTrue(meanNaiveEpe >= bc.minNaiveBlockEpe,
					L"Unprotected 16x16 block matcher negative control must suffer >= 15 px EPE on sports ball");
				Assert::IsTrue(meanVelEpe < bc.maxVelEpePx,
					L"FastObjectTracker object velocity EPE must meet < 1.0 px Milestone 1 gate");
				Assert::IsTrue(meanTrackedPxEpe < bc.maxSmallFastPxEpe,
					L"FastObjectTracker per-pixel small-fast EPE must meet < 4.0 px Milestone 1 gate");
			}
		}
	};
}
