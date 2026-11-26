// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// Milestone 1 Adversarial Unit Suite — Stage B & Orchestrator:
// Exact BIC 4-model trajectory classification (LinearGlide CV default / drag, Ballistic, Impulse
// court bounce & goalkeeper 180-deg rebound & active player strike, Clutter rejection), 2.5D
// approaching perspective ball, iso-luma chroma-only end-to-end tracking, 3-frame player occlusion
// coasting + re-association, crossing-ball ambiguity under look-ahead, Milestone 0 negative controls,
// 64x36 clutter-hotspot auto-mask, session appearance prior, Dormant/Active scheduler, OverloadArbiter
// budget shedding, and default-OFF / OriginalPathway zero-work invariance.
#include <VP-V/modules/motion/estimation/fast_objects/tests/FastObjectSynthScenes.h>

#include <cmath>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Helper: run FastObjectTracker on a SynthSequence using standard sports look-ahead `aheadFrames`
		// (matching VP-V MotionAhead queue depth: push up to t + aheadFrames before ResolveAt(t)).
		std::vector<FastObjects::FastObjectTrackSet> RunTrackerWithLookAhead(
			const FastObjectSynth::SynthSequence& seq,
			const FastObjects::Config& cfg,
			int aheadFrames,
			bool passRefinePlanes,
			FastObjects::FastObjectTracker* externalTracker = nullptr)
		{
			FastObjects::FastObjectTracker localTracker;
			FastObjects::FastObjectTracker& tracker = externalTracker ? *externalTracker : localTracker;

			const int numTriplets = static_cast<int>(seq.frames.size()) - 2; // fi in [1 .. size-2]
			std::vector<FastObjects::FastObjectTrackSet> resolved(static_cast<size_t>(std::max(0, numTriplets)));
			if (numTriplets <= 0)
				return resolved;

			int nextPushIdx = 0;
			for (int t = 0; t < numTriplets; ++t)
			{
				const int targetMaxPush = std::min(numTriplets - 1, t + std::max(0, aheadFrames));
				while (nextPushIdx <= targetMaxPush)
				{
					const int synthFi = nextPushIdx + 1;
					const FastObjects::TrackerFrameInput tfi = FastObjectSynth::BuildTrackerFrameInput(seq, synthFi, true);
					tracker.PushFrame(cfg, tfi, nullptr);
					++nextPushIdx;
				}

				const int synthFi = t + 1;
				const FastObjects::TrackerRefinePlanes rp = FastObjectSynth::BuildRefinePlanes(seq, synthFi);
				tracker.ResolveAt(cfg, static_cast<uint64_t>(t), passRefinePlanes ? &rp : nullptr, resolved[static_cast<size_t>(t)]);
			}
			return resolved;
		}
	}

	TEST_CLASS(FastObjectTrajectoryTests)
	{
	public:
		TEST_METHOD(BicClassifierSelectsCvDefaultBallisticImpulseAndClutterAccurately)
		{
			using namespace FastObjects;

			Config cfg;
			cfg.enabled = true;
			CameraModel::Rect pic;
			pic.left = 0; pic.top = 0; pic.right = 1920; pic.bottom = 1080;
			constexpr double kDt = 1.0 / 60.0;

			int totalCases = 0;
			int correctCases = 0;

			auto makeBaseSample = [&](int idx, double x, double y, double r = 5.0) -> WindowSample
			{
				WindowSample s;
				s.tauSec = static_cast<double>(idx - 3) * kDt;
				s.dtStepSec = kDt;
				s.x = x;
				s.y = y;
				s.covXX = 0.25;
				s.covYY = 0.25;
				s.radiusPx = r;
				s.intSnr = 16.0;
				return s;
			};

			// 1. Owner CV-Default Ruling: pure linear glide AND tiny sub-threshold curvature must select LinearGlide (d=4), NOT Ballistic (d=6)!
			for (int sub = 0; sub < 6; ++sub)
			{
				WindowSample win[7];
				const double tinyAy = (sub < 3) ? 0.0 : (30.0 * sub); // Sub-pixel sagitta (< 0.04 px) -> CV must win by BIC!
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * kDt;
					win[i] = makeBaseSample(i, 500.0 + (1800.0 + 120.0 * sub) * t, 400.0 - 300.0 * t + 0.5 * tinyAy * t * t);
				}
				const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, 7, kDt, pic);
				++totalCases;
				if (cls.type == TrajectoryType::LinearGlide)
					++correctCases;
			}

			// 2. LinearGlide with strong tangential ice/air drag (d=5) -> still classified as LinearGlide!
			for (int sub = 0; sub < 4; ++sub)
			{
				WindowSample win[7];
				const double v0 = 2800.0 + 150.0 * sub;
				const double decel = 4800.0;
				const double ang = 0.2 * sub;
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * kDt;
					const double d = v0 * t - 0.5 * decel * t * t;
					win[i] = makeBaseSample(i, 600.0 + d * std::cos(ang), 350.0 + d * std::sin(ang));
				}
				const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, 7, kDt, pic);
				++totalCases;
				if (cls.type == TrajectoryType::LinearGlide)
					++correctCases;
			}

			// 3. Ballistic parabolic arcs (d=6, downward gravity ay > 0) across 60p & 30p cadences.
			for (int sub = 0; sub < 6; ++sub)
			{
				WindowSample win[7];
				const double dt = (sub < 3) ? (1.0 / 60.0) : (1.0 / 30.0);
				const double ay = 2600.0 + 400.0 * sub;
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * dt;
					win[i] = makeBaseSample(i, 500.0 + 1100.0 * t, 320.0 - 550.0 * t + 0.5 * ay * t * t);
					win[i].tauSec = t;
					win[i].dtStepSec = dt;
				}
				const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, 7, dt, pic);
				++totalCases;
				if (cls.type == TrajectoryType::Ballistic && std::fabs(cls.aySec2 - ay) < 50.0)
					++correctCases;
			}

			// 4. Impulse passive bounces, active player kicks (||v+|| > ||v-|| near P8 Object), AND 180-deg goalkeeper/post rebound!
			// 4a. Court bounces (3 angles)
			for (int sub = 0; sub < 3; ++sub)
			{
				WindowSample win[7];
				const double tauS = 0.4 * kDt;
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * kDt;
					const double dtRel = t - tauS;
					const double vx = (t <= tauS) ? 2400.0 : 2000.0;
					const double vy = (t <= tauS) ? (1800.0 + 200.0 * sub) : -(1400.0 + 150.0 * sub);
					win[i] = makeBaseSample(i, 600.0 + vx * dtRel, 500.0 + vy * dtRel);
				}
				const TrajectoryClassification cls = ClassifyTrajectoryWindow(cfg, win, 7, kDt, pic);
				++totalCases;
				if (cls.type == TrajectoryType::Impulse && cls.hasImpulse &&
					std::hypot(cls.impulseX - 600.0, cls.impulseY - 500.0) < 1.5)
					++correctCases;
			}

			// 4b. Active strike/kick (||v+|| = 3200 > ||v-|| = 1200): rejected when far from Object, accepted when nearLargeObject!
			{
				WindowSample winFar[7], winNear[7];
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * kDt;
					const double vx = (t <= 0.0) ? -1200.0 : 2600.0;
					const double vy = (t <= 0.0) ? 400.0 : -1400.0;
					winFar[i] = makeBaseSample(i, 700.0 + vx * t, 450.0 + vy * t);
					winNear[i] = winFar[i];
					if (i == 3 || i == 4)
						winNear[i].nearLargeObject = true;
				}
				const TrajectoryClassification clsFar = ClassifyTrajectoryWindow(cfg, winFar, 7, kDt, pic);
				Assert::AreEqual(static_cast<int>(TrajectoryType::Clutter), static_cast<int>(clsFar.type),
					L"Unexplained mid-air kinetic energy gain away from any player/object must be rejected");

				const TrajectoryClassification clsNear = ClassifyTrajectoryWindow(cfg, winNear, 7, kDt, pic);
				++totalCases;
				if (clsNear.type == TrajectoryType::Impulse && clsNear.hasImpulse)
					++correctCases;
			}

			// 4c. Symmetric 180-deg post/goalkeeper save rebound (spec B.3.3 Sidemen_Sketch_Save: p(0) == p(N-1) after 140 px flight + rebound!).
			{
				WindowSample winRebound[7];
				for (int i = 0; i < 7; ++i)
				{
					const double t = static_cast<double>(i - 3) * kDt; // Vertex at i=3 (t=0): travels +40 px/frame to x=620, rebounds -40 px/frame back to x=500!
					const double vx = (t <= 0.0) ? 2400.0 : -2400.0;
					winRebound[i] = makeBaseSample(i, 620.0 + vx * t, 400.0 + 180.0 * std::fabs(t));
				}
				const TrajectoryClassification clsReb = ClassifyTrajectoryWindow(cfg, winRebound, 7, kDt, pic);
				++totalCases;
				if (clsReb.type == TrajectoryType::Impulse)
					++correctCases;
				Assert::AreEqual(static_cast<int>(TrajectoryType::Impulse), static_cast<int>(clsReb.type),
					L"Symmetric 180-deg post/goalkeeper rebound travelling 120 px and returning must classify as Impulse, NOT stationary Clutter");
			}

			// Verify overall BIC classification accuracy >= 95% (Milestone 1 exit gate) AND unphysical upward gravity rejection!
			const double accuracy = static_cast<double>(correctCases) / static_cast<double>(totalCases);
			Assert::IsTrue(accuracy >= 0.95, L"Trajectory-type BIC classification accuracy must be >= 95%");

			WindowSample antiGrav[7];
			for (int i = 0; i < 7; ++i)
			{
				const double t = static_cast<double>(i - 3) * kDt;
				antiGrav[i] = makeBaseSample(i, 500.0 + 900.0 * t, 500.0 + 0.5 * (-4500.0) * t * t); // Strong upward acceleration!
			}
			const TrajectoryClassification clsAnti = ClassifyTrajectoryWindow(cfg, antiGrav, 7, kDt, pic);
			Assert::AreEqual(static_cast<int>(TrajectoryType::Clutter), static_cast<int>(clsAnti.type),
				L"Strong upward anti-gravity parabola (ay << -3*sigma) must be rejected as Clutter");

			// End-to-end rendered Tennis Serve + Court Bounce (60p, 180-deg motion blur): must detect Impulse & accurate bounce chord!
			const FastObjectSynth::SynthSequence tennisSeq = FastObjectSynth::MakeTennisServeBounceScene(11);
			const auto tennisOut = RunTrackerWithLookAhead(tennisSeq, cfg, 4, true);
			bool sawImpulse = false;
			bool sawBracketedBouncePair = false;
			for (size_t t = 0; t < tennisOut.size(); ++t)
			{
				const FastObjectTrackSet& ts = tennisOut[t];
				const FastObjectSynth::GroundTruthBall& gt = tennisSeq.frames[t + 1].gtBalls[0];
				for (uint32_t i = 0; i < ts.count; ++i)
				{
					const FastObjectTrack& trk = ts.tracks[i];
					if (trk.state != ValidityState::Valid)
						continue;
					if (trk.type == TrajectoryType::Impulse)
						sawImpulse = true;
					if (gt.pairBracketsImpulse)
					{
						sawBracketedBouncePair = true;
						Assert::AreEqual(static_cast<int>(TrajectoryType::Impulse), static_cast<int>(trk.type),
							L"Bounce-bracketing frame pair must classify as TrajectoryType::Impulse");
						Assert::IsTrue((trk.flags & TrackFlags::kImpulsePair) != 0u && trk.hasImpulse,
							L"Bounce-bracketing pair must set kImpulsePair and bounce vertex");
						const double chordErr = std::hypot(trk.vx - gt.vx, trk.vy - gt.vy);
						const double vertErr = std::hypot(trk.impulseX - gt.impulseImgX, trk.impulseY - gt.impulseImgY);
						Assert::IsTrue(chordErr < 2.5, L"Impulse pair displacement must match true bounce chord");
						Assert::IsTrue(vertErr < 5.0, L"Impulse vertex must match court bounce point");
					}
				}
			}
			Assert::IsTrue(sawImpulse && sawBracketedBouncePair, L"Rendered 60p tennis serve bounce must lock as Impulse across vertex");
		}

		TEST_METHOD(PerspectiveApproachingBallSelectsDepthModelWithoutFalseClutterRejection)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence seq = MakePerspectiveApproachingBallScene(10);
			Config cfg;
			cfg.enabled = true;
			cfg.enablePerspectiveDepth = true;

			const auto results = RunTrackerWithLookAhead(seq, cfg, 4, true);
			int validFrames = 0;
			bool usedPerspective = false;
			for (size_t t = 1; t + 1 < results.size(); ++t)
			{
				const FastObjectTrackSet& ts = results[t];
				const GroundTruthBall& gt = seq.frames[t + 1].gtBalls[0];
				for (uint32_t i = 0; i < ts.count; ++i)
				{
					const FastObjectTrack& trk = ts.tracks[i];
					if (trk.state == ValidityState::Valid)
					{
						++validFrames;
						if ((trk.flags & TrackFlags::kDepthPerspective) != 0u && trk.radiusRate > 0.0f)
							usedPerspective = true;
						const double epe = std::hypot(trk.vx - gt.vx, trk.vy - gt.vy);
						Assert::IsTrue(epe < 1.8, L"2.5D approaching ball velocity EPE must stay accurate");
					}
				}
			}
			Assert::IsTrue(validFrames >= 5, L"2.5D approaching ball must NOT be falsely rejected as clutter");
			Assert::IsTrue(usedPerspective, L"2.5D approaching ball must activate kDepthPerspective with positive dr/dt");
		}

		TEST_METHOD(ChromaOnlyIsoLumaBallTracksEndToEndUnderGrainAndFullResRefine)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Realistic iso-luma optic-yellow ball (|Delta Y| == 0, strong Cb/Cr) with grain (sigma_g = 0.005)
			// AND real caller full-res luma TrackerRefinePlanes supplied!
			const SynthSequence seq = MakeChromaOnlyIsoLumaBallScene(10, 0.005);

			// Mandatory Negative Control: luma-only tracker sees zero valid tracks on the exact same inputs!
			Config lumaCfg;
			lumaCfg.enabled = true;
			lumaCfg.enableChromaResidual = false;
			const auto lumaRes = RunTrackerWithLookAhead(seq, lumaCfg, 4, true);
			uint32_t lumaValidTotal = 0;
			for (const auto& ts : lumaRes)
				lumaValidTotal += ts.diag.validTracks;
			Assert::AreEqual(0u, lumaValidTotal, L"Luma-only negative control must produce 0 valid tracks on iso-luma ball");

			// Luma + Chroma tracker with full-res refine planes active: must track cleanly across the sequence!
			Config chromaCfg;
			chromaCfg.enabled = true;
			chromaCfg.enableChromaResidual = true;
			const auto chromaRes = RunTrackerWithLookAhead(seq, chromaCfg, 4, true);

			int validCount = 0;
			double sumVelEpe = 0.0;
			for (size_t t = 0; t + 1 < chromaRes.size(); ++t)
			{
				const FastObjectTrackSet& ts = chromaRes[t];
				const GroundTruthBall& gt = seq.frames[t + 1].gtBalls[0];
				for (uint32_t i = 0; i < ts.count; ++i)
				{
					if (ts.tracks[i].state == ValidityState::Valid)
					{
						++validCount;
						Assert::IsTrue((ts.tracks[i].flags & TrackFlags::kChromaAssisted) != 0u);
						sumVelEpe += std::hypot(ts.tracks[i].vx - gt.vx, ts.tracks[i].vy - gt.vy);
					}
				}
			}
			Assert::IsTrue(validCount >= 6, L"Luma+chroma tracker must sustain Valid tracking across iso-luma court under grain + full-res refine");
			Assert::IsTrue((sumVelEpe / std::max(1, validCount)) < 1.0, L"Iso-luma chroma ball velocity EPE must be < 1.0 px");
		}

		TEST_METHOD(BasketballPlayerOcclusionCoastsWithZeroVectorsAndReassociates)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			// Test BOTH when ball enters right before occlusion under standard 4-frame sports look-ahead (W_a = 4)
			// AND across the full basketball pass sequence!
			const SynthSequence fullSeq = MakeBasketballOcclusionScene();
			Config cfg;
			cfg.enabled = true;

			// Slice starting 1 frame later so the ball has 3 visible frames (frames 1,2,3 -> tracker 0,1,2) before
			// entering the 3-frame player occlusion (tracker 3,4,5) under standard W_a = 4 look-ahead!
			SynthSequence seq = fullSeq;
			seq.frames.erase(seq.frames.begin() + 1);
			for (size_t i = 0; i < seq.frames.size(); ++i)
			{
				seq.frames[i].frameIndex = static_cast<uint64_t>(i);
				seq.frames[i].timestampSec = static_cast<double>(i) / seq.fps;
				if (i > 0)
				{
					seq.frames[i].camPrevToCur = ComposeHomography(
						seq.frames[i].camWorldToImg,
						InvertHomography(seq.frames[i - 1].camWorldToImg));
				}
			}

			const auto results = RunTrackerWithLookAhead(seq, cfg, 4, true);

			uint32_t preOccId = 0;
			int preValid = 0;
			int occCoasting = 0;
			int postValidSameId = 0;

			for (size_t t = 0; t + 1 < results.size(); ++t)
			{
				const FastObjectTrackSet& ts = results[t];
				const SynthFrame& curFr = seq.frames[t + 1];
				const GroundTruthBall& gt = curFr.gtBalls[0];

				if (gt.occludedByPlayer)
				{
					Assert::AreEqual(0u, ts.diag.validTracks, L"Occluded ball behind player must emit ZERO Valid tracks");
					for (uint32_t i = 0; i < ts.count; ++i)
					{
						if (ts.tracks[i].state == ValidityState::Coasting)
							++occCoasting;
						// Spec C.3: Coasting track must emit ZERO vectors!
						float vx = 0.0f, vy = 0.0f;
						bool disocc = false;
						Assert::IsFalse(SampleObjectVector(ts, gt.cx, gt.cy, 0.5f, vx, vy, disocc),
							L"SampleObjectVector must emit ZERO vectors during occlusion coasting");
						Assert::IsFalse(IsProtectedForTemporalDetail(ts, gt.cx, gt.cy),
							L"TD protection mask must stay inactive during occlusion coasting");
					}
				}
				else if (t < 3)
				{
					for (uint32_t i = 0; i < ts.count; ++i)
					{
						if (ts.tracks[i].state == ValidityState::Valid)
						{
							++preValid;
							preOccId = ts.tracks[i].id;
						}
					}
				}
				else
				{
					for (uint32_t i = 0; i < ts.count; ++i)
					{
						if (ts.tracks[i].state == ValidityState::Valid && ts.tracks[i].id == preOccId)
							++postValidSameId;
					}
				}
			}

			Assert::IsTrue(preValid >= 2, L"Basketball must be tracked as Valid prior to player occlusion under W_a=4 look-ahead");
			Assert::AreEqual(3, occCoasting, L"Basketball must report ValidityState::Coasting across all 3 occluded frames");
			Assert::IsTrue(postValidSameId >= 3, L"Basketball must re-associate with persistent track ID after 3-frame player occlusion");
		}

		TEST_METHOD(CrossingBallsWithinOneRadiusFlagsUncertainWithZeroIdSwaps)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence seq = MakeCrossingBallsScene();
			Config cfg;
			cfg.enabled = true;

			// Run with standard 60p sports look-ahead W_a = 4!
			const auto results = RunTrackerWithLookAhead(seq, cfg, 4, true);

			// Identify persistent track IDs on pre-crossing frame t = 2 (synth frame 3: balls > 110 px apart).
			uint32_t idBall1 = 0, idBall2 = 0;
			{
				const FastObjectTrackSet& preTs = results[2];
				const auto& gts = seq.frames[3].gtBalls;
				for (uint32_t i = 0; i < preTs.count; ++i)
				{
					const FastObjectTrack& trk = preTs.tracks[i];
					if (trk.state != ValidityState::Valid)
						continue;
					if (std::hypot(trk.cx - gts[0].cx, trk.cy - gts[0].cy) < 4.0)
						idBall1 = trk.id;
					if (std::hypot(trk.cx - gts[1].cx, trk.cy - gts[1].cy) < 4.0)
						idBall2 = trk.id;
				}
			}
			Assert::IsTrue(idBall1 != 0u && idBall2 != 0u && idBall1 != idBall2,
				L"Both balls must have distinct Valid track IDs well before crossing (even when future crossing frame is in look-ahead ring)");

			// Crossing occurs at tracker frame t = 5 (synth frame 6, separation = 2.0 px <= 0.36 * radius):
			// MUST flag Uncertain / non-Valid (ZERO Valid vectors emitted on crossing frame!).
			{
				const FastObjectTrackSet& crossTs = results[5];
				const auto& gts = seq.frames[6].gtBalls;
				Assert::IsTrue(gts[0].crossingAmbiguous && gts[1].crossingAmbiguous);
				Assert::AreEqual(0u, crossTs.diag.validTracks,
					L"Crossing within 1 blob radius MUST flag Uncertain and emit ZERO Valid tracks even under W_a=4 look-ahead");
				Assert::IsTrue(crossTs.diag.uncertainTracks >= 1u,
					L"Crossing frame must explicitly report Uncertain track state");

				float vx = 0.0f, vy = 0.0f;
				bool disocc = false;
				Assert::IsFalse(SampleObjectVector(crossTs, gts[0].cx, gts[0].cy, 0.0f, vx, vy, disocc),
					L"Crossing ambiguity must emit ZERO object vectors to consumers");
			}

			// Post-crossing frame t = 8 (synth frame 9: balls separated by > 110 px): ZERO ID swaps and accurate vectors!
			{
				const FastObjectTrackSet& postTs = results[8];
				const auto& gts = seq.frames[9].gtBalls;
				uint32_t postId1 = 0, postId2 = 0;
				for (uint32_t i = 0; i < postTs.count; ++i)
				{
					const FastObjectTrack& trk = postTs.tracks[i];
					if (trk.state != ValidityState::Valid)
						continue;
					if (std::hypot(trk.cx - gts[0].cx, trk.cy - gts[0].cy) < 4.0)
					{
						postId1 = trk.id;
						Assert::IsTrue(std::hypot(trk.vx - gts[0].vx, trk.vy - gts[0].vy) < 1.5);
					}
					if (std::hypot(trk.cx - gts[1].cx, trk.cy - gts[1].cy) < 4.0)
					{
						postId2 = trk.id;
						Assert::IsTrue(std::hypot(trk.vx - gts[1].vx, trk.vy - gts[1].vy) < 1.5);
					}
				}
				Assert::AreEqual(idBall1, postId1, L"Ball 1 must retain its original ID after crossing (zero ID swap)");
				Assert::AreEqual(idBall2, postId2, L"Ball 2 must retain its original ID after crossing (zero ID swap)");
			}
		}

		TEST_METHOD(NegativeControlsProduceZeroFalseLocksAndHotspotAutoMaskSuppressesClutter)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			Config cfg;
			cfg.enabled = true;

			// 1. Crowd hands & falling confetti (30p, random walk + oscillatory hands) — both causal (W_a=0) and look-ahead (W_a=4)!
			const SynthSequence confettiSeq = MakeCrowdConfettiClutterScene(20);
			for (int ahead : { 0, 4 })
			{
				const auto res = RunTrackerWithLookAhead(confettiSeq, cfg, ahead, true);
				uint32_t falseLocks = 0;
				for (const auto& ts : res)
					falseLocks += ts.diag.validTracks;
				Assert::AreEqual(0u, falseLocks, L"Crowd hands & falling confetti negative control MUST produce 0 false locks");
			}

			// 2. Rippling water + specular sun glints (CE-WATER waterProb >= 0.5 + unmasked oscillatory glints)!
			const SynthSequence waterSeq = MakeRipplingWaterGlintsScene(20);
			{
				const auto res = RunTrackerWithLookAhead(waterSeq, cfg, 4, true);
				uint32_t falseLocks = 0;
				for (const auto& ts : res)
					falseLocks += ts.diag.validTracks;
				Assert::AreEqual(0u, falseLocks, L"Rippling water + specular sun glints MUST produce 0 false locks");
			}

			// 3. Scrolling scoreboard ticker (P8 Overlay) + flashing stationary LED ad boards (64x36 hotspot auto-mask)!
			const SynthSequence ledSeq = MakeScoreboardAndLedClutterScene(22);
			{
				FastObjectTracker tracker;
				const auto res = RunTrackerWithLookAhead(ledSeq, cfg, 2, true, &tracker);
				uint32_t falseLocks = 0;
				uint32_t totalHotspotSuppressed = 0;
				for (const auto& ts : res)
				{
					falseLocks += ts.diag.validTracks;
					totalHotspotSuppressed += ts.diag.hotspotSuppressedBlobs;
				}
				Assert::AreEqual(0u, falseLocks, L"Scrolling scoreboard ticker & flashing LED boards MUST produce 0 false locks");

				const CameraModel::Rect pic = ledSeq.frames[1].picture;
				const float heatAtLed = tracker.Trajectory().HotspotHeatAt(180.0f, 72.0f, pic);
				Assert::IsTrue(heatAtLed >= 3.0f && totalHotspotSuppressed > 0u,
					L"64x36 clutter-hotspot auto-mask must accumulate heat >= 3.0 and suppress repeated LED clutter births");

				// Verify scene cut / ResetScene immediately clears hotspot heat!
				tracker.ResetScene();
				Assert::AreEqual(0.0f, tracker.Trajectory().HotspotHeatAt(180.0f, 72.0f, pic), 0.0f,
					L"Scene reset must immediately clear 64x36 clutter-hotspot heat map");
			}

			// 4. Heavy panning film grain (sigma_g = 0.03) + crisp high-contrast panning pitch lines!
			const SynthSequence grainSeq = MakeHeavyGrainPanningLinesScene(20);
			{
				const auto res = RunTrackerWithLookAhead(grainSeq, cfg, 4, true);
				uint32_t falseLocks = 0;
				for (const auto& ts : res)
					falseLocks += ts.diag.validTracks;
				Assert::AreEqual(0u, falseLocks, L"Heavy panning film grain + crisp white lines MUST produce 0 false locks");
			}
		}

		TEST_METHOD(SessionPriorDormantSchedulerBudgetSheddingAndDefaultOffInvariance)
		{
			using namespace FastObjects;
			using namespace FastObjectSynth;

			const SynthSequence soccerSeq = MakeSoccerParabolicLobScene(60.0, 10);

			// 1. Default Config MUST be strictly OFF, and both enabled==false and originalPathway==true must do zero work!
			Config defaultCfg;
			Assert::IsFalse(defaultCfg.enabled, L"FastObjects::Config::enabled must default strictly to false");

			FastObjectTracker offTracker;
			FastObjectTrackSet offSet;
			const TrackerFrameInput tfi0 = BuildTrackerFrameInput(soccerSeq, 2, true);
			Assert::IsFalse(offTracker.ProcessFrame(defaultCfg, tfi0, nullptr, offSet));
			Assert::AreEqual(static_cast<int>(SchedulerMode::Off), static_cast<int>(offSet.mode));
			Assert::IsTrue((offSet.abstainReason & AbstainReason::Disabled) != 0u);
			Assert::AreEqual(std::string("fast_objects: off"), offSet.FormatStatus());

			Config origCfg;
			origCfg.enabled = true;
			origCfg.originalPathway = true;
			Assert::IsFalse(offTracker.ProcessFrame(origCfg, tfi0, nullptr, offSet));
			Assert::AreEqual(static_cast<int>(SchedulerMode::Off), static_cast<int>(offSet.mode));
			Assert::IsTrue((offSet.abstainReason & AbstainReason::OriginalPathway) != 0u);
			Assert::AreEqual(std::string("fast_objects: off (original pathway)"), offSet.FormatStatus());

			// 2. Dormant / Active scheduler + OverloadArbiter budget shedding!
			Config activeCfg;
			activeCfg.enabled = true;
			FastObjectTracker schedTracker;

			// Render 9 completely quiet grass-pan frames (no ball) followed by soccer ball frames.
			SceneRecipe quietRec;
			quietRec.srcW = 960;
			quietRec.srcH = 540;
			quietRec.numFrames = 11;
			quietRec.fps = 60.0;
			quietRec.surface = SurfaceKind::SoccerGrass;
			quietRec.camPanVxPxFrame = 6.0;
			quietRec.grainSigma = 0.003;
			const SynthSequence quietSeq = RenderSequence(quietRec);

			int skippedDormantCount = 0;
			for (int fi = 1; fi + 1 < static_cast<int>(quietSeq.frames.size()); ++fi)
			{
				FastObjectTrackSet qs;
				TrackerFrameInput qIn = BuildTrackerFrameInput(quietSeq, fi, true);
				Assert::IsTrue(schedTracker.ProcessFrame(activeCfg, qIn, nullptr, qs));
				if (qs.diag.scanSkippedDormant)
					++skippedDormantCount;
			}
			Assert::AreEqual(static_cast<int>(SchedulerMode::Dormant), static_cast<int>(schedTracker.CurrentMode()),
				L"Scheduler must enter Dormant after kDormantIdleFrames = 6 quiet frames");
			Assert::IsTrue(skippedDormantCount >= 1, L"Dormant scheduler must skip odd full-frame scans");

			// Wake immediately on active ball wake-up check (within the 2-frame Dormant scan cadence, spec C.4.1)!
			for (int step = 0; step < 2; ++step)
			{
				TrackerFrameInput wakeIn = BuildTrackerFrameInput(soccerSeq, 2 + step, true);
				wakeIn.frameIndex = static_cast<uint64_t>(20 + step);
				wakeIn.timestampSec = static_cast<double>(20 + step) / 60.0;
				FastObjectTrackSet wakeSet;
				schedTracker.ProcessFrame(activeCfg, wakeIn, nullptr, wakeSet);
				if (schedTracker.CurrentMode() == SchedulerMode::ActiveT1)
					break;
			}
			Assert::AreEqual(static_cast<int>(SchedulerMode::ActiveT1), static_cast<int>(schedTracker.CurrentMode()),
				L"Scheduler must snap immediately from Dormant to ActiveT1 on first active wake-up scan");
			Assert::AreEqual(0u, schedTracker.QuietFrames());

			FastObjectTrackSet wakeSet;
			TrackerFrameInput wakeIn = BuildTrackerFrameInput(soccerSeq, 4, true);
			// OverloadArbiter budget shedding step-down: ActiveT2 -> ActiveT1 -> Dormant -> ShedBudget!
			schedTracker.SetBudgetTier(SchedulerMode::ActiveT2);
			wakeIn.frameIndex = 22;
			wakeIn.timestampSec = 22.0 / 60.0;
			schedTracker.ProcessFrame(activeCfg, wakeIn, nullptr, wakeSet);
			Assert::AreEqual(static_cast<int>(SchedulerMode::ActiveT2), static_cast<int>(wakeSet.mode));

			schedTracker.SetBudgetTier(SchedulerMode::ShedBudget);
			wakeIn.frameIndex = 23;
			wakeIn.timestampSec = 23.0 / 60.0;
			Assert::IsFalse(schedTracker.ProcessFrame(activeCfg, wakeIn, nullptr, wakeSet));
			Assert::AreEqual(static_cast<int>(SchedulerMode::ShedBudget), static_cast<int>(wakeSet.mode));
			Assert::IsTrue((wakeSet.abstainReason & AbstainReason::BudgetShed) != 0u);
			Assert::AreEqual(std::string("fast_objects: off (budget)"), wakeSet.FormatStatus());

			// 3. SessionBallPrior persistence across scene cuts (ResetScene) + soft priority + ResetSession clear,
			// plus bit-exact determinism across two independent runs!
			FastObjectTracker priorTracker1, priorTracker2;
			const auto run1 = RunTrackerWithLookAhead(soccerSeq, activeCfg, 4, true, &priorTracker1);
			const auto run2 = RunTrackerWithLookAhead(soccerSeq, activeCfg, 4, true, &priorTracker2);
			Assert::AreEqual(run1.size(), run2.size());
			for (size_t i = 0; i < run1.size(); ++i)
			{
				Assert::AreEqual(run1[i].count, run2[i].count);
				for (uint32_t k = 0; k < run1[i].count; ++k)
				{
					Assert::AreEqual(run1[i].tracks[k].vx, run2[i].tracks[k].vx, 0.0f, L"Deterministic run parity vx");
					Assert::AreEqual(run1[i].tracks[k].vy, run2[i].tracks[k].vy, 0.0f, L"Deterministic run parity vy");
				}
			}

			Assert::IsTrue(priorTracker1.Trajectory().Prior().updates >= 3u);
			Assert::IsTrue(priorTracker1.Trajectory().Prior().polarityScore > 0.5);

			// Cut preserves session prior and sets kSessionPriorMatch on Play 2, YET still tracks a dark hockey puck!
			priorTracker1.ResetScene();
			Assert::IsTrue(priorTracker1.Trajectory().Prior().updates >= 3u, L"ResetScene (cut) must preserve SessionBallPrior");

			const auto play2 = RunTrackerWithLookAhead(soccerSeq, activeCfg, 4, true, &priorTracker1);
			bool sawPriorMatchFlag = false;
			for (const auto& ts : play2)
				for (uint32_t i = 0; i < ts.count; ++i)
					if ((ts.tracks[i].flags & TrackFlags::kSessionPriorMatch) != 0u)
						sawPriorMatchFlag = true;
			Assert::IsTrue(sawPriorMatchFlag, L"Play 2 after scene cut must prioritize matching ball via SessionBallPrior");

			// Verify SessionBallPrior is strictly soft (never blocks opposite-polarity dark hockey puck)!
			priorTracker1.ResetScene();
			const SynthSequence hockeySeq = MakeHockeySlapshotGlideScene(9);
			const auto puckPlay = RunTrackerWithLookAhead(hockeySeq, activeCfg, 4, true, &priorTracker1);
			uint32_t puckValid = 0;
			for (const auto& ts : puckPlay)
				puckValid += ts.diag.validTracks;
			Assert::IsTrue(puckValid >= 4u, L"SessionBallPrior must remain soft and never block an opposite-polarity puck");

			priorTracker1.ResetSession();
			Assert::AreEqual(0u, priorTracker1.Trajectory().Prior().updates, L"ResetSession must clear SessionBallPrior");
		}
	};
}
