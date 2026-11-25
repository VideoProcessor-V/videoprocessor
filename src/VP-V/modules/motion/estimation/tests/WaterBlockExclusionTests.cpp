// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/global_model/DynamicTexture.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelSearchHints.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraSmoother.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraUsable.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		struct SyntheticGrid
		{
			int gw = 32, gh = 18;
			double blockSize = 16.0;
			CameraModel::Rect pic{ 0, 0, 32 * 16, 18 * 16 };
			std::vector<float> fx, fy, ft;

			SyntheticGrid(int w = 32, int h = 18)
				: gw(w), gh(h), pic{ 0, 0, w * 16, h * 16 }
				, fx(size_t(w) * h, 0.0f), fy(size_t(w) * h, 0.0f), ft(size_t(w) * h, 1.0f) {}

			CameraModel::PairInput Input(uint64_t seq) const
			{
				CameraModel::PairInput in;
				in.gridW = gw;
				in.gridH = gh;
				in.blockSize = blockSize;
				in.picture = pic;
				in.fwdX = fx.data();
				in.fwdY = fy.data();
				in.fwdTrust = ft.data();
				in.seq = seq;
				in.pairFrom = seq > 0 ? seq - 1 : 0;
				in.pairTo = seq;
				in.pairSeconds = 1.0 / 24.0;
				in.precisionPx = 0.25;
				return in;
			}
		};

		// Deterministic pseudo-random ripple vector (mean ~0, large spatial & temporal incoherence like choppy water).
		void WaterRipple(uint64_t seq, int bx, int by, float& vx, float& vy)
		{
			uint64_t h = (seq * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(bx) * 0xBF58476D1CE4E5B9ull) ^
				(static_cast<uint64_t>(by) * 0x94D049BB133111EBull);
			h ^= (h >> 30); h *= 0xBF58476D1CE4E5B9ull; h ^= (h >> 27); h *= 0x94D049BB133111EBull; h ^= (h >> 31);
			const double a = (static_cast<double>(h & 0xFFFFu) / 65535.0) * 6.283185307179586;
			const double mag = 3.5 + (static_cast<double>((h >> 16) & 0xFFFFu) / 65535.0) * 8.5;
			vx = static_cast<float>(mag * std::cos(a));
			vy = static_cast<float>(mag * std::sin(a));
		}

		std::shared_ptr<CameraModel::CameraMotion> MakePanMotion(uint64_t seq, double tx, double ty,
			double pairSeconds = 1.0 / 24.0)
		{
			auto m = std::make_shared<CameraModel::CameraMotion>();
			m->seq = seq;
			m->pairFrom = seq > 0 ? seq - 1 : 0;
			m->pairTo = seq;
			m->pairSeconds = pairSeconds;
			m->resyncGeneration = 1;
			m->rebaseGeneration = 1;
			m->valid = true;
			m->reason = CameraModel::Reason::Ok;
			m->confidence = 0.92;
			m->picture = { 0, 0, 512, 320 };
			m->params.type = CameraModel::ModelType::Translation;
			m->params.h[0] = 1.0; m->params.h[1] = 0.0; m->params.h[2] = tx;
			m->params.h[3] = 0.0; m->params.h[4] = 1.0; m->params.h[5] = ty;
			m->params.h[6] = 0.0; m->params.h[7] = 0.0; m->params.h[8] = 1.0;
			m->rates.tx = tx / pairSeconds;
			m->rates.ty = ty / pairSeconds;
			return m;
		}
	}

	TEST_CLASS(WaterBlockExclusionTests)
	{
	public:
		TEST_METHOD(DefaultOffIsByteIdenticalToSingleLayerFitter)
		{
			CameraModel::Config def;
			Assert::IsFalse(def.waterBlockExclude, L"CE-WATER-C1 defaults to OFF");
			Assert::IsFalse(def.twoLayer, L"twoLayer stays default OFF");
			Assert::IsFalse(def.dynTexture, L"dynTexture stays default OFF");
			Assert::IsFalse(def.staticCamera, L"staticCamera stays default OFF");

			CameraModel::Fitter a(def), b(def);
			SyntheticGrid g;
			for (uint64_t s = 1; s <= 4; ++s)
			{
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const size_t i = size_t(by) * g.gw + bx;
						if (by >= g.gh / 2)
							WaterRipple(s, bx, by, g.fx[i], g.fy[i]);
						else
						{
							g.fx[i] = 7.0f;
							g.fy[i] = -2.0f;
						}
					}
				const auto ra = a.Fit(g.Input(s));
				const auto rb = b.Fit(g.Input(s));
				Assert::AreEqual(0.0, ra.signals.waterExcludedShare, 1e-12);
				Assert::AreEqual(ra.confidence, rb.confidence, 1e-12);
				Assert::AreEqual(0, std::memcmp(ra.params.h, rb.params.h, sizeof(ra.params.h)));
			}
		}

		TEST_METHOD(CleanPanAndZoomExcludeZeroBlocksAndKeepFullAccuracy)
		{
			CameraModel::Config cfgOff, cfgOn;
			cfgOn.waterBlockExclude = true;
			CameraModel::Fitter fOff(cfgOff), fOn(cfgOn);

			// 1. Clean pan (10, -4).
			SyntheticGrid pan;
			for (size_t i = 0; i < pan.fx.size(); ++i) { pan.fx[i] = 10.0f; pan.fy[i] = -4.0f; }
			for (uint64_t s = 1; s <= 4; ++s)
			{
				const auto rOff = fOff.Fit(pan.Input(s));
				const auto rOn = fOn.Fit(pan.Input(s));
				Assert::AreEqual(0.0, rOn.signals.waterExcludedShare, 1e-12, L"clean pan excludes zero blocks");
				Assert::IsTrue(rOn.valid && rOn.confidence >= 0.8);
				Assert::AreEqual(rOff.params.h[2], rOn.params.h[2], 1e-6);
				Assert::AreEqual(rOff.params.h[5], rOn.params.h[5], 1e-6);
			}

			// 2. Clean 1.04x zoom + pan (4, 2).
			fOff.Reset();
			fOn.Reset();
			SyntheticGrid zoom;
			for (int by = 0; by < zoom.gh; ++by)
				for (int bx = 0; bx < zoom.gw; ++bx)
				{
					double cx = 0.0, cy = 0.0;
					CameraModel::BlockCentre(bx, by, zoom.blockSize, zoom.pic, &cx, &cy);
					const size_t i = size_t(by) * zoom.gw + bx;
					zoom.fx[i] = static_cast<float>(0.04 * cx + 4.0);
					zoom.fy[i] = static_cast<float>(0.04 * cy + 2.0);
				}
			for (uint64_t s = 1; s <= 4; ++s)
			{
				const auto rOn = fOn.Fit(zoom.Input(s));
				Assert::AreEqual(0.0, rOn.signals.waterExcludedShare, 1e-12, L"clean zoom excludes zero blocks");
				Assert::IsTrue(rOn.valid && rOn.confidence >= 0.8);
				Assert::AreEqual(1.04, rOn.params.h[0], 5e-3);
				Assert::AreEqual(4.0, rOn.params.h[2], 0.2);
			}
		}

		TEST_METHOD(SyntheticWaterAndPanExcludesWaterBlocksAndRecoversTrueCameraPan)
		{
			// (a) Bottom 60% of the frame = choppy chaotic water ripples; top 40% = true rigid camera pan (8, -3).
			// Verifies HIGH-1 allows a 2-quadrant horizontal shoreline with borderSupport >= borderVetoRatio and
			// preWaterInlierShare >= inlierShareLo, while keeping water strictly OUT of BlockData::excluded,
			// Label::Overlay, and BlockHint::Excluded (CE-WATER-C1 contract).
			{
				CameraModel::Config cfg;
				cfg.waterBlockExclude = true;
				CameraModel::Fitter fitter(cfg);

				SyntheticGrid g(32, 20);
				const int waterStartRow = 8;   // 12 of 20 rows (60%) are water; 8 rows (40%) are rigid pan
				CameraModel::CameraMotion last;
				for (uint64_t s = 1; s <= 4; ++s)
				{
					for (int by = 0; by < g.gh; ++by)
						for (int bx = 0; bx < g.gw; ++bx)
						{
							const size_t i = size_t(by) * g.gw + bx;
							if (by >= waterStartRow)
								WaterRipple(s, bx, by, g.fx[i], g.fy[i]);
							else
							{
								g.fx[i] = 8.0f;
								g.fy[i] = -3.0f;
							}
						}
					last = fitter.Fit(g.Input(s));
				}

				Assert::IsTrue(CameraLink::Usable(last), L"true 2-quadrant shoreline pan is usable after water exclusion");
				Assert::IsTrue(last.signals.waterExcludedShare >= 0.45, L"water blocks excluded from fitter sample set");
				Assert::AreEqual(8.0, last.params.h[2], 0.25, L"pan X accurately recovered");
				Assert::AreEqual(-3.0, last.params.h[5], 0.25, L"pan Y accurately recovered");
				Assert::IsNotNull(last.blocks.get());

				const CameraModel::HintConfig hintCfg;
				const CameraModel::SearchHints hints = CameraModel::BuildHints(
					hintCfg, &last, g.fx.data(), g.fy.data(), nullptr, nullptr, false, false, false, 1.0, 0.0, 0.25);

				int waterMarkedExcluded = 0, waterLabeledOverlay = 0, waterHintExcluded = 0;
				for (int by = waterStartRow; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const size_t i = size_t(by) * g.gw + bx;
						if (last.blocks->excluded[i] != 0)
							++waterMarkedExcluded;
						if (last.blocks->label[i] == static_cast<uint8_t>(CameraModel::Label::Overlay))
							++waterLabeledOverlay;
						if (hints.hint[i] == static_cast<uint8_t>(CameraModel::BlockHint::Excluded))
							++waterHintExcluded;
					}
				Assert::AreEqual(0, waterMarkedExcluded, L"water blocks must NEVER be stamped into BlockData::excluded");
				Assert::AreEqual(0, waterLabeledOverlay, L"water blocks must NEVER be stamped Label::Overlay");
				Assert::AreEqual(0, waterHintExcluded, L"BuildHints must NEVER mark water blocks BlockHint::Excluded");
			}

			// (b) Rule 3 Same-Input Negative Control ("Prove the Hazard Is Real"):
			// Bottom 60% = coherent-drift water (-24, 0) with modest spatio-temporal checkerboard ripples (+-0.58 px,
			// within 0.05 * 24 = 1.20 px RANSAC agreement gate so unprotected RANSAC locks onto the 60% drifting water,
			// while discrete curvature 2 * 0.58 = 1.16 px > 3 * sigma_C and temporal diff 1.16 px > 3 * sqrt(2) * prec
			// cleanly exclude it); top 40% = true rigid camera pan (8, -3).
			{
				CameraModel::Config cfgOff, cfgOn;
				cfgOff.waterBlockExclude = false;
				cfgOn.waterBlockExclude = true;
				CameraModel::Fitter fOff(cfgOff), fOn(cfgOn);

				SyntheticGrid g(32, 20);
				const int waterStartRow = 8;
				CameraModel::CameraMotion lastOff, lastOn;
				for (uint64_t s = 1; s <= 4; ++s)
				{
					for (int by = 0; by < g.gh; ++by)
						for (int bx = 0; bx < g.gw; ++bx)
						{
							const size_t i = size_t(by) * g.gw + bx;
							if (by >= waterStartRow)
							{
								const float ripple = (((bx + by + static_cast<int>(s)) & 1) != 0) ? 0.58f : -0.58f;
								g.fx[i] = -24.0f + ripple;
								g.fy[i] = 0.0f;
							}
							else
							{
								g.fx[i] = 8.0f;
								g.fy[i] = -3.0f;
							}
						}
					const CameraModel::PairInput in = g.Input(s);
					lastOff = fOff.Fit(in);
					lastOn = fOn.Fit(in);
				}

				// Negative control: unprotected fitter fails on the exact same input (locks onto drifting water).
				double offDx = 0.0, offDy = 0.0;
				CameraModel::Displacement(lastOff.params, 0.0, 0.0, &offDx, &offDy);
				const double offErrPx = std::hypot(offDx - 8.0, offDy - (-3.0));
				Assert::IsFalse(CameraLink::Usable(lastOff), L"unprotected fitter fails to recover usable camera pan");
				Assert::IsTrue(offErrPx > 0.5, L"unprotected fitter drifts / locks onto coherent-drift water (> 0.5 px error)");

				// Protected fitter: excludes water cleanly and recovers exact camera pan on the exact same input.
				Assert::IsTrue(CameraLink::Usable(lastOn), L"protected fitter recovers usable camera pan");
				Assert::IsTrue(lastOn.signals.waterExcludedShare > 0.35, L"coherent-drift rippling water excluded");
				Assert::AreEqual(8.0, lastOn.params.h[2], 0.25, L"protected pan X recovered");
				Assert::AreEqual(-3.0, lastOn.params.h[5], 0.25, L"protected pan Y recovered");
				Assert::IsNotNull(lastOn.blocks.get());

				const CameraModel::HintConfig hintCfg;
				const CameraModel::SearchHints hints = CameraModel::BuildHints(
					hintCfg, &lastOn, g.fx.data(), g.fy.data(), nullptr, nullptr, false, false, false, 1.0, 0.0, 0.25);
				for (int by = waterStartRow; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const size_t i = size_t(by) * g.gw + bx;
						Assert::AreEqual<uint8_t>(0u, lastOn.blocks->excluded[i], L"drifting water not stamped excluded");
						Assert::IsTrue(lastOn.blocks->label[i] != static_cast<uint8_t>(CameraModel::Label::Overlay),
							L"drifting water not stamped Label::Overlay");
						Assert::IsTrue(hints.hint[i] != static_cast<uint8_t>(CameraModel::BlockHint::Excluded),
							L"drifting water not marked BlockHint::Excluded");
					}
			}
		}

		TEST_METHOD(FastPanWithModestRipplesExcludesWaterViaCurvatureInvariance)
		{
			// Fast pan (12.0, 0.0) over top 40% shoreline + bottom 60% water carrying modest 2.5 px spatial ripples
			// on top of the fast pan (|v| ~ 12.1 .. 14.6 px).
			// Model-free discrete curvature (nabla^2 v) cancels the 12 px/frame pan identically and excludes water,
			// whereas the pre-fix 0.25 * max(|v|, |nv|) ~ 3.64 px threshold exceeds the 2.5 px ripple difference
			// (pan blind spot).
			CameraModel::Config cfgOff, cfgOn;
			cfgOff.waterBlockExclude = false;
			cfgOn.waterBlockExclude = true;
			CameraModel::Fitter fOff(cfgOff), fOn(cfgOn);

			SyntheticGrid g(32, 20);
			const int waterStartRow = 8;   // 12 rows (60%) water, 8 rows (40%) rigid fast-pan shoreline
			CameraModel::CameraMotion lastOff, lastOn;
			for (uint64_t s = 1; s <= 4; ++s)
			{
				const float tempRippleY = ((s & 1u) != 0u) ? 1.5f : -1.5f;
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const size_t i = size_t(by) * g.gw + bx;
						if (by >= waterStartRow)
						{
							const float spatialRippleX = (((bx + by) & 1) != 0) ? 2.5f : 0.0f;
							g.fx[i] = 12.0f + spatialRippleX;
							g.fy[i] = tempRippleY;
						}
						else
						{
							g.fx[i] = 12.0f;
							g.fy[i] = 0.0f;
						}
					}
				const CameraModel::PairInput in = g.Input(s);
				lastOff = fOff.Fit(in);
				lastOn = fOn.Fit(in);
			}

			// Rule 3 Same-Input Negative Control 1 (pre-fix pan blind-spot hazard):
			// On the exact same frame-4 vector field, verify every interior water neighbour pair has
			// |v - nv| == 2.5 px < 0.25 * max(|v|, |nv|) (~3.64 px), so the pre-fix relative-magnitude
			// heuristic flags ZERO interior water neighbour pairs as spatially incoherent.
			int checkedPairs = 0, preFixFlaggedPairs = 0;
			for (int by = waterStartRow + 1; by < g.gh - 1; ++by)
				for (int bx = 1; bx < g.gw - 1; ++bx)
				{
					const size_t b = size_t(by) * g.gw + bx;
					const double vx = g.fx[b], vy = g.fy[b];
					const double magB = std::hypot(vx, vy);
					const size_t nbrs[2] = { b + 1, b + static_cast<size_t>(g.gw) };
					for (size_t nb : nbrs)
					{
						const double nvx = g.fx[nb], nvy = g.fy[nb];
						const double diff = std::hypot(vx - nvx, vy - nvy);
						const double preFixThr = 0.25 * std::max(magB, std::hypot(nvx, nvy));
						Assert::IsTrue(preFixThr > diff, L"pre-fix 0.25 * max(|v|, |nv|) threshold exceeds 2.5 px ripple diff");
						if (diff > preFixThr)
							++preFixFlaggedPairs;
						++checkedPairs;
					}
				}
			Assert::IsTrue(checkedPairs > 200, L"checked substantial interior water neighbour pairs");
			Assert::AreEqual(0, preFixFlaggedPairs, L"pre-fix relative-magnitude heuristic has 100% blind spot under fast pan");

			// Rule 3 Same-Input Negative Control 2: unprotected fitter (waterBlockExclude = false) fails usability.
			Assert::IsFalse(CameraLink::Usable(lastOff), L"unprotected fitter cannot recover usable fast pan over modest-ripple water");

			// Protected fitter: model-free curvature excludes water and recovers exact fast pan (12.0, 0.0).
			Assert::IsTrue(CameraLink::Usable(lastOn), L"curvature-based water exclusion recovers usable fast pan");
			Assert::IsTrue(lastOn.signals.waterExcludedShare > 0.35, L"water excluded despite 12 px/frame fast pan");
			Assert::AreEqual(12.0, lastOn.params.h[2], 0.25, L"fast pan X recovered cleanly");
			Assert::AreEqual(0.0, lastOn.params.h[5], 0.25, L"fast pan Y recovered cleanly");
			Assert::IsNotNull(lastOn.blocks.get());
			for (int by = waterStartRow; by < g.gh; ++by)
				for (int bx = 0; bx < g.gw; ++bx)
				{
					const size_t i = size_t(by) * g.gw + bx;
					Assert::AreEqual<uint8_t>(0u, lastOn.blocks->excluded[i], L"fast-pan water not stamped excluded");
					Assert::IsTrue(lastOn.blocks->label[i] != static_cast<uint8_t>(CameraModel::Label::Overlay),
						L"fast-pan water not stamped Label::Overlay");
				}
		}

		TEST_METHOD(BoatOnWaterWithForegroundExcludesWaterWithoutFalseWaterLock)
		{
			// 55% chaotic water (lower rows), 35% rigid sky/shore panning (6, 1), 10% rigid boat moving (-10, 0).
			CameraModel::Config cfg;
			cfg.waterBlockExclude = true;
			CameraModel::Fitter fitter(cfg);

			SyntheticGrid g(32, 20);
			CameraModel::CameraMotion last;
			for (uint64_t s = 1; s <= 4; ++s)
			{
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
					{
						const size_t i = size_t(by) * g.gw + bx;
						const bool boat = (by >= 7 && by < 11 && bx >= 10 && bx < 22);
						if (boat)
						{
							g.fx[i] = -10.0f;
							g.fy[i] = 0.0f;
						}
						else if (by >= 7)
							WaterRipple(s, bx, by, g.fx[i], g.fy[i]);
						else
						{
							g.fx[i] = 6.0f;
							g.fy[i] = 1.0f;
						}
					}
				last = fitter.Fit(g.Input(s));
			}

			Assert::IsTrue(last.signals.waterExcludedShare >= 0.35, L"water region excluded");
			Assert::IsTrue(last.valid && last.confidence >= 0.5, L"sky/shore pan survives water exclusion");
			Assert::IsTrue(CameraLink::Usable(last), L"shoreline camera model is usable");
			Assert::AreEqual(6.0, last.params.h[2], 0.3, L"camera pan X recovered instead of boat or water");
			Assert::AreEqual(1.0, last.params.h[5], 0.3, L"camera pan Y recovered");
			Assert::IsNotNull(last.blocks.get());

			const CameraModel::HintConfig hintCfg;
			const CameraModel::SearchHints hints = CameraModel::BuildHints(
				hintCfg, &last, g.fx.data(), g.fy.data(), nullptr, nullptr, false, false, false, 1.0, 0.0, 0.25);
			for (int by = 7; by < g.gh; ++by)
				for (int bx = 0; bx < g.gw; ++bx)
				{
					const bool boat = (by >= 7 && by < 11 && bx >= 10 && bx < 22);
					if (boat)
						continue;
					const size_t i = size_t(by) * g.gw + bx;
					Assert::AreEqual<uint8_t>(0u, last.blocks->excluded[i], L"boat-scene water not stamped excluded");
					Assert::IsTrue(last.blocks->label[i] != static_cast<uint8_t>(CameraModel::Label::Overlay),
						L"boat-scene water not stamped Label::Overlay");
					Assert::IsTrue(hints.hint[i] != static_cast<uint8_t>(CameraModel::BlockHint::Excluded),
						L"boat-scene water not marked BlockHint::Excluded");
				}
		}

		TEST_METHOD(ChoppyWaterWithSmallMovingBoatTriggersRemainingRigidSupportGuard)
		{
			// 85% choppy water + 15% moving boat hull (16x6 = 96 of 640 blocks = 0.15 < inlierShareLo = 0.25),
			// centred so it spans all 4 quadrants (proving a quadrant-count-only guard would miss it).
			// Remaining-rigid-support guard must DEMOTE to Reason::Ambiguous | kVerdictDynamicTexture (!Usable).
			CameraModel::Config cfgOn;
			cfgOn.waterBlockExclude = true;
			CameraModel::Fitter fProtected(cfgOn);

			CameraModel::Config cfgNoGuard;
			cfgNoGuard.waterBlockExclude = false;
			CameraModel::Fitter fUnguarded(cfgNoGuard);

			SyntheticGrid gFull(32, 20);
			SyntheticGrid gBoatOnly(32, 20);
			CameraModel::CameraMotion lastProtected, lastBoatOnly;
			for (uint64_t s = 1; s <= 4; ++s)
			{
				for (int by = 0; by < gFull.gh; ++by)
					for (int bx = 0; bx < gFull.gw; ++bx)
					{
						const size_t i = size_t(by) * gFull.gw + bx;
						const bool boat = (by >= 7 && by < 13 && bx >= 8 && bx < 24);   // 96 / 640 = 15%
						if (boat)
						{
							gFull.fx[i] = -6.0f;
							gFull.fy[i] = 2.0f;
							gFull.ft[i] = 1.0f;

							gBoatOnly.fx[i] = -6.0f;
							gBoatOnly.fy[i] = 2.0f;
							gBoatOnly.ft[i] = 1.0f;
						}
						else
						{
							WaterRipple(s, bx, by, gFull.fx[i], gFull.fy[i]);
							gFull.ft[i] = 1.0f;

							// Same boat blocks alone (simulating water stripped WITHOUT the remaining-rigid-support guard).
							gBoatOnly.fx[i] = 0.0f;
							gBoatOnly.fy[i] = 0.0f;
							gBoatOnly.ft[i] = 0.0f;
						}
					}
				lastProtected = fProtected.Fit(gFull.Input(s));
				lastBoatOnly = fUnguarded.Fit(gBoatOnly.Input(s));
			}

			// Rule 3 Same-Input Negative Control: without the remaining-rigid-support guard, feeding the exact
			// same 15% moving boat hull blocks to RANSAC produces a confident false lock onto boat velocity!
			Assert::IsTrue(CameraLink::Usable(lastBoatOnly), L"without remaining-rigid-support guard, RANSAC locks onto 15% boat");
			Assert::IsTrue(lastBoatOnly.reason == CameraModel::Reason::Ok, L"unguarded boat-only fit reports Reason::Ok");
			Assert::AreEqual(-6.0, lastBoatOnly.params.h[2], 0.25, L"unguarded fit locks onto boat vx");
			Assert::AreEqual(2.0, lastBoatOnly.params.h[5], 0.25, L"unguarded fit locks onto boat vy");

			// Protected path: water is excluded, and the remaining-rigid-support guard demotes the 15% boat cleanly.
			Assert::IsTrue(lastProtected.signals.waterExcludedShare > 0.70, L"majority water excluded");
			Assert::IsFalse(CameraLink::Usable(lastProtected), L"85% water + 15% moving boat must never be usable");
			Assert::IsTrue(lastProtected.reason == CameraModel::Reason::Ambiguous, L"demoted to Reason::Ambiguous");
			Assert::IsTrue((lastProtected.verdictFlags & CameraModel::kVerdictDynamicTexture) != 0,
				L"verdictFlags carries kVerdictDynamicTexture");
			Assert::IsTrue(lastProtected.confidence < 0.5, L"demoted confidence stays strictly below 0.5");
		}

		TEST_METHOD(PureWaterSceneExcludesIncoherentBlocksAndNeverReportsConfidentCamera)
		{
			CameraModel::Config cfg;
			cfg.waterBlockExclude = true;
			CameraModel::Fitter fitter(cfg);

			SyntheticGrid g(32, 18);
			for (uint64_t s = 1; s <= 4; ++s)
			{
				for (int by = 0; by < g.gh; ++by)
					for (int bx = 0; bx < g.gw; ++bx)
						WaterRipple(s, bx, by, g.fx[size_t(by) * g.gw + bx], g.fy[size_t(by) * g.gw + bx]);
				const auto r = fitter.Fit(g.Input(s));
				const bool usable = r.valid && r.confidence >= 0.5;
				Assert::IsFalse(usable, L"100% chaotic water must never produce a confident camera model");
				if (s >= 2)
					Assert::IsTrue(r.signals.waterExcludedShare >= 0.80, L"majority of pure-water blocks excluded");
			}
		}

		TEST_METHOD(C4SignalsAndShearDipoleOverfitGuard)
		{
			// (a) Verify rawShare stays unsaturated (< 1.0) at 0.60 true inlier fraction while inlierShare saturates at 1.0,
			// and verify dipoleTB / dipoleLR are populated on CameraMotion::Signals.
			{
				CameraModel::Fitter fitter;
				SyntheticGrid gTB(32, 20);
				for (int by = 0; by < gTB.gh; ++by)
					for (int bx = 0; bx < gTB.gw; ++bx)
					{
						const size_t i = size_t(by) * gTB.gw + bx;
						if (by < 12)   // 12 / 20 = exact 60% rigid camera pan
						{
							gTB.fx[i] = 6.0f;
							gTB.fy[i] = 0.0f;
						}
						else           // 40% scattered rightward bottom dipole (no single competing layer)
						{
							const int m = (bx + by) % 3;
							gTB.fx[i] = 9.0f;
							gTB.fy[i] = (m == 0) ? 4.0f : (m == 1 ? -4.0f : 8.0f);
						}
					}
				const CameraModel::CameraMotion rTB = fitter.Fit(gTB.Input(1));
				Assert::IsTrue(rTB.valid, L"60% pan fit is valid");
				Assert::AreEqual(0.60, rTB.signals.rawShare, 0.02, L"rawShare reports exact unsaturated 0.60 inlier fraction");
				Assert::IsTrue(rTB.signals.rawShare < 1.0, L"rawShare is strictly unsaturated (< 1.0) at 60% inliers");
				Assert::AreEqual(1.0, rTB.signals.inlierShare, 1e-9, L"ramped inlierShare saturates at 1.0 above inlierShareHi (0.55)");
				Assert::IsTrue(rTB.signals.dipoleTB > 0.75, L"top-bottom signed residual dipole populated (> absThr)");
				Assert::IsTrue(rTB.signals.dipoleLR < 0.15, L"left-right dipole stays near zero on symmetric rows");
				Assert::AreEqual(0.0, rTB.signals.shearEnergy, 1e-12, L"Translation model reports zero shearEnergy");
			}

			// Verify dipoleLR is populated symmetrically on a left-vs-right 60%/40% dipole field.
			{
				CameraModel::Fitter fitter;
				SyntheticGrid gLR(20, 20);
				for (int by = 0; by < gLR.gh; ++by)
					for (int bx = 0; bx < gLR.gw; ++bx)
					{
						const size_t i = size_t(by) * gLR.gw + bx;
						if (bx < 12)   // 12 / 20 = exact 60% rigid pan on left
						{
							gLR.fx[i] = 6.0f;
							gLR.fy[i] = 0.0f;
						}
						else           // 40% scattered downward dipole on right
						{
							const int m = (bx + by) % 3;
							gLR.fx[i] = (m == 0) ? 10.0f : (m == 1 ? 2.0f : 14.0f);
							gLR.fy[i] = 3.0f;
						}
					}
				const CameraModel::CameraMotion rLR = fitter.Fit(gLR.Input(1));
				Assert::IsTrue(rLR.valid, L"left-right 60% pan fit is valid");
				Assert::AreEqual(0.60, rLR.signals.rawShare, 0.02, L"rawShare unsaturated 0.60 on left-right split");
				Assert::AreEqual(1.0, rLR.signals.inlierShare, 1e-9, L"inlierShare saturated 1.0 on left-right split");
				Assert::IsTrue(rLR.signals.dipoleLR > 0.75, L"left-right signed residual dipole populated (> absThr)");
				Assert::IsTrue(rLR.signals.dipoleTB < 0.15, L"top-bottom dipole stays near zero on symmetric columns");
			}

			// (b) C4 shear-dipole overfit guard + Rule 3 same-input zero-dipole negative control:
			// Top 50% (rawShare = 0.50 < inlierShareHi = 0.55) has a horizontal shear (dx = 2 + 0.015*(y + 80), dy = 0)
			// whose Affine fit has shearEnergy ~ 1.82 px > absThr (0.75 px).
			//  * With a bottom-half water dipole (dipoleTB > 0.75 px), C4 steps back to Similarity/Translation (shearEnergy == 0).
			//  * Without the dipole (zero-mean symmetric bottom outliers, same top-50% shear field), Affine wins and
			//    shearEnergy > 1.0 is populated on CameraMotion::Signals.
			{
				SyntheticGrid gDipole(32, 20);
				SyntheticGrid gNoDipole(32, 20);
				for (int by = 0; by < gDipole.gh; ++by)
					for (int bx = 0; bx < gDipole.gw; ++bx)
					{
						const size_t i = size_t(by) * gDipole.gw + bx;
						double cx = 0.0, cy = 0.0;
						CameraModel::BlockCentre(bx, by, gDipole.blockSize, gDipole.pic, &cx, &cy);
						if (by < 10)   // exact 50% top half (y < 0)
						{
							const float sx = static_cast<float>(2.0 + 0.015 * (cy + 80.0));
							gDipole.fx[i] = sx;
							gDipole.fy[i] = 0.0f;
							gNoDipole.fx[i] = sx;
							gNoDipole.fy[i] = 0.0f;
						}
						else           // exact 50% bottom half (y >= 0)
						{
							const int m3 = (bx + by) % 3;
							gDipole.fx[i] = 10.0f;
							gDipole.fy[i] = (m3 == 0) ? -5.0f : (m3 == 1 ? 5.0f : 10.0f);

							const float ox = ((bx & 1) != 0) ? 12.0f : -12.0f;
							const float oy = ((by & 1) != 0) ? 12.0f : -12.0f;
							gNoDipole.fx[i] = 2.0f + ox;
							gNoDipole.fy[i] = oy;
						}
					}

				CameraModel::Fitter fDipole, fNoDipole;
				const CameraModel::CameraMotion rDipole = fDipole.Fit(gDipole.Input(1));
				const CameraModel::CameraMotion rNoDipole = fNoDipole.Fit(gNoDipole.Input(1));

				// Negative control (same top-50% shear without bottom dipole): Affine wins and populates shearEnergy > 1.0.
				Assert::IsTrue(rNoDipole.valid, L"no-dipole shear fit is valid");
				Assert::IsTrue(rNoDipole.params.type == CameraModel::ModelType::Affine,
					L"without bottom dipole, Affine legitimately wins the model ladder");
				Assert::IsTrue(rNoDipole.signals.shearEnergy > 1.0,
					L"Affine shearEnergy is populated (> 1.0 px) on CameraMotion::Signals");
				Assert::IsTrue(rNoDipole.signals.dipoleTB < 0.5, L"zero-mean bottom outliers produce low dipoleTB");
				Assert::IsTrue(rNoDipole.signals.rawShare <= 0.52, L"rawShare stays below inlierShareHi (0.55)");

				// Protected case (same top-50% shear WITH bottom dipole): C4 steps back cleanly from Affine/Homography.
				Assert::IsTrue(rDipole.valid, L"dipole shear fit is valid");
				Assert::IsTrue(rDipole.params.type == CameraModel::ModelType::Translation
					|| rDipole.params.type == CameraModel::ModelType::Similarity,
					L"C4 shear-dipole guard steps back to Translation/Similarity on water-below-horizon dipole");
				Assert::AreEqual(0.0, rDipole.signals.shearEnergy, 1e-12,
					L"stepped-back Similarity/Translation has zero shearEnergy");
				Assert::IsTrue(rDipole.signals.dipoleTB > 0.75, L"dipoleTB exceeds absThr on half-frame dipole");
				Assert::IsTrue(rDipole.signals.rawShare < 0.55, L"rawShare is below inlierShareHi");
			}
		}

		TEST_METHOD(CameraSmootherLooInnovationFlagsSingleFrameSpike)
		{
			// 5 frames: frames 1, 2, 4, 5 are a smooth pan (4.0, 0.0); frame 3 has a +6.0 px single-frame water spike (10.0, 0.0).
			CameraLink::CameraSmoother smoother(2);
			CameraModel::FixedLagSmoother naiveSmoother(2);
			for (uint64_t seq = 1; seq <= 5; ++seq)
			{
				const double tx = (seq == 3) ? 10.0 : 4.0;
				auto rec = MakePanMotion(seq, tx, 0.0);
				smoother.Push(rec);
				naiveSmoother.Push(*rec);
			}

			CameraLink::SmoothQueryResult q3;
			Assert::IsTrue(smoother.Query(3, q3), L"Query(3) succeeds");
			Assert::IsTrue(q3.haveLoo, L"Query(3) computes LOO innovation from usable neighbours 2 and 4");
			Assert::IsTrue(q3.looSpikeFlagged, L"Query(3) flags the +6.0 px single-frame spike");
			Assert::IsTrue(q3.looInnovationPx > 3.0, L"Query(3) reports looInnovationPx > 3.0 px");
			Assert::AreEqual(6.0, q3.looInnovationPx, 1e-6, L"LOO innovation equals exact 6.0 px spike magnitude");
			Assert::IsTrue(smoother.Counters().looSpikes.load() >= 1u, L"looSpikes counter incremented visibly");
			Assert::IsTrue(q3.smoothed && q3.record != nullptr, L"Query(3) returns smoothed substituted record");
			const double smoothErr = std::fabs(q3.record->smoothedParams.h[2] - 4.0);
			const double rawErr = std::fabs(10.0 - 4.0);
			Assert::IsTrue(smoothErr < rawErr, L"smoothed output is strictly closer to (4.0, 0.0) than raw (10.0, 0.0) spike");
			Assert::AreEqual(4.0, q3.record->smoothedParams.h[2], 1e-6, L"LOO substitution eliminates the single-frame spike");
			Assert::AreEqual(0.0, q3.record->smoothedParams.h[5], 1e-6, L"pan Y remains 0.0");

			// Rule 3 Same-Input Negative Control 1 (unprotected naive triangular smoother on the exact same 5 frames):
			// Without LOO spike substitution, centre-frame self-attenuation leaves a +2.0 px residual spike (6.0 vs 4.0).
			CameraModel::Params naiveSp;
			Assert::IsTrue(naiveSmoother.Smoothed(3, naiveSp), L"naive FixedLagSmoother produces smoothed frame 3");
			Assert::IsTrue(std::fabs(naiveSp.h[2] - 4.0) > 1.5,
				L"naive triangular smoothing without LOO leaves > 1.5 px spike residual (exact 2.0 px)");

			// Rule 3 Negative Control 2: clean smooth pan (4.0, 0.0) on frames 1..5 never false-flags frame 2 or frame 3.
			CameraLink::CameraSmoother cleanSmoother(2);
			for (uint64_t seq = 1; seq <= 5; ++seq)
				cleanSmoother.Push(MakePanMotion(seq, 4.0, 0.0));
			CameraLink::SmoothQueryResult qClean2, qClean3;
			Assert::IsTrue(cleanSmoother.Query(2, qClean2), L"clean Query(2) succeeds");
			Assert::IsTrue(qClean2.haveLoo, L"clean frame 2 has symmetric usable neighbours 1 and 3");
			Assert::IsFalse(qClean2.looSpikeFlagged, L"clean frame 2 has looSpikeFlagged == false");
			Assert::AreEqual(0.0, qClean2.looInnovationPx, 1e-6, L"clean frame 2 has zero LOO innovation");
			Assert::IsTrue(cleanSmoother.Query(3, qClean3), L"clean Query(3) succeeds");
			Assert::IsTrue(qClean3.haveLoo && !qClean3.looSpikeFlagged, L"clean frame 3 has looSpikeFlagged == false");
			Assert::AreEqual<uint64_t>(0u, cleanSmoother.Counters().looSpikes.load(), L"clean pan records zero LOO spikes");

			// Rule 3 Negative Control 3: in a 6-frame stream with a single spike at frame 4, clean frame 2
			// (whose neighbours 1 and 3 are clean) has looSpikeFlagged == false while spiked frame 4 is flagged.
			CameraLink::CameraSmoother streamSmoother(2);
			for (uint64_t seq = 1; seq <= 6; ++seq)
				streamSmoother.Push(MakePanMotion(seq, (seq == 4) ? 10.0 : 4.0, 0.0));
			CameraLink::SmoothQueryResult qStream2, qStream4;
			Assert::IsTrue(streamSmoother.Query(2, qStream2), L"stream Query(2) succeeds");
			Assert::IsTrue(qStream2.haveLoo && !qStream2.looSpikeFlagged,
				L"in-stream clean frame 2 has looSpikeFlagged == false");
			Assert::IsTrue(streamSmoother.Query(4, qStream4), L"stream Query(4) succeeds");
			Assert::IsTrue(qStream4.haveLoo && qStream4.looSpikeFlagged && qStream4.looInnovationPx > 3.0,
				L"in-stream spiked frame 4 is flagged");
			Assert::AreEqual(4.0, qStream4.record->smoothedParams.h[2], 1e-6,
				L"in-stream spiked frame 4 is cleanly substituted to (4.0, 0.0)");
		}

		TEST_METHOD(CameraSmootherLooSpikesCountsOnlyWhenSubstitutedNotOnShortWindow)
		{
			// Lead = 2: push frames 1, 2, 3 (spike +6.0 px), 4 (so immediate neighbours 2 and 4 exist for LOO
			// detection, while frame 5 = seq + 2 has NOT arrived yet -> causalWindowShort == true, smoothed == false).
			CameraLink::CameraSmoother smoother(2);
			CameraLink::CameraSmoother lead1Smoother(1);
			Assert::AreEqual(2, smoother.EffectiveLead(), L"effective lead is 2");
			Assert::AreEqual(1, lead1Smoother.EffectiveLead(), L"control smoother effective lead is 1");

			for (uint64_t seq = 1; seq <= 4; ++seq)
			{
				const double tx = (seq == 3) ? 10.0 : 4.0;
				auto rec = MakePanMotion(seq, tx, 0.0);
				smoother.Push(rec);
				lead1Smoother.Push(rec);
			}

			// 1. Query seq = 3 BEFORE frame 5 arrives: LOO detects the +6.0 px spike, but because the lead = 2
			// window is still short (causalWindowShort == true, smoothed == false), substitution has NOT occurred
			// and Counters().looSpikes MUST remain 0 (not incremented prematurely).
			CameraLink::SmoothQueryResult qShort;
			Assert::IsTrue(smoother.Query(3, qShort), L"short-window Query(3) succeeds with causal fallback");
			Assert::IsTrue(qShort.haveLoo, L"short-window Query(3) computes LOO from neighbours 2 and 4");
			Assert::IsTrue(qShort.looSpikeFlagged, L"short-window Query(3) flags the +6.0 px spike");
			Assert::AreEqual(6.0, qShort.looInnovationPx, 1e-6, L"short-window Query(3) reports exact 6.0 px LOO innovation");
			Assert::IsFalse(qShort.smoothed, L"short-window Query(3) is not smoothed yet");
			Assert::IsTrue(qShort.causalWindowShort, L"short-window Query(3) reports causalWindowShort == true");
			Assert::IsNotNull(qShort.record.get(), L"short-window Query(3) returns causal record");
			Assert::IsFalse(qShort.record->smoothed, L"short-window causal record is not marked smoothed");
			Assert::AreEqual(10.0, CameraLink::ActiveParams(*qShort.record).h[2], 1e-6,
				L"short-window causal record preserves raw un-substituted spike");
			Assert::AreEqual<uint64_t>(0u, smoother.Counters().looSpikes.load(),
				L"looSpikes counter must stay 0 while causalWindowShort prevents substitution");
			Assert::AreEqual<uint64_t>(1u, smoother.Counters().causalWindowShort.load(),
				L"causalWindowShort counter incremented visibly");
			Assert::AreEqual<uint64_t>(0u, smoother.Counters().smoothed.load(),
				L"smoothed counter remains 0 while window is short");

			// Same-input control (exact same 4 frames 1..4 with lead = 1, where window [2, 3, 4] IS complete):
			// proves the exact same input immediately triggers substitution and increments looSpikes to 1 when not short.
			CameraLink::SmoothQueryResult qLead1;
			Assert::IsTrue(lead1Smoother.Query(3, qLead1), L"lead=1 Query(3) succeeds on frames 1..4");
			Assert::IsTrue(qLead1.smoothed && !qLead1.causalWindowShort,
				L"lead=1 window [2, 3, 4] is complete on frames 1..4");
			Assert::IsTrue(qLead1.haveLoo && qLead1.looSpikeFlagged,
				L"lead=1 flags the exact same +6.0 px spike");
			Assert::IsNotNull(qLead1.record.get(), L"lead=1 returns substituted record");
			Assert::AreEqual(4.0, qLead1.record->smoothedParams.h[2], 1e-6,
				L"lead=1 substitutes the spike to 4.0 px immediately");
			Assert::AreEqual<uint64_t>(1u, lead1Smoother.Counters().looSpikes.load(),
				L"lead=1 increments looSpikes strictly once upon substitution");

			// 2. Push frame 5 to complete the lead = 2 window and re-query seq = 3:
			// substitution now occurs cleanly and Counters().looSpikes increments strictly once (1, NOT 2).
			smoother.Push(MakePanMotion(5, 4.0, 0.0));
			CameraLink::SmoothQueryResult qFull;
			Assert::IsTrue(smoother.Query(3, qFull), L"full-window Query(3) succeeds after frame 5 arrives");
			Assert::IsTrue(qFull.smoothed, L"full-window Query(3) is smoothed");
			Assert::IsFalse(qFull.causalWindowShort, L"full-window Query(3) clears causalWindowShort");
			Assert::IsTrue(qFull.haveLoo, L"full-window Query(3) has LOO");
			Assert::IsTrue(qFull.looSpikeFlagged, L"full-window Query(3) flags the spike");
			Assert::AreEqual(6.0, qFull.looInnovationPx, 1e-6, L"full-window Query(3) reports 6.0 px LOO innovation");
			Assert::IsNotNull(qFull.record.get(), L"full-window Query(3) returns substituted record");
			Assert::IsTrue(qFull.record->smoothed, L"full-window record is marked smoothed");
			Assert::AreEqual(4.0, qFull.record->smoothedParams.h[2], 1e-6,
				L"full-window Query(3) substitutes spike to 4.0 px");
			Assert::AreEqual(0.0, qFull.record->smoothedParams.h[5], 1e-6,
				L"full-window Query(3) keeps pan Y at 0.0 px");
			Assert::AreEqual(4.0, CameraLink::ActiveParams(*qFull.record).h[2], 1e-6,
				L"ActiveParams reflects substituted 4.0 px motion");
			Assert::AreEqual<uint64_t>(1u, smoother.Counters().looSpikes.load(),
				L"looSpikes counted strictly once upon substitution (NOT double-counted as 2)");
			Assert::AreEqual<uint64_t>(1u, smoother.Counters().smoothed.load(),
				L"smoothed counter incremented once on full-window query");
			Assert::AreEqual<uint64_t>(1u, smoother.Counters().causalWindowShort.load(),
				L"causalWindowShort counter remains 1 from the earlier short-window query");
		}
	};
}
