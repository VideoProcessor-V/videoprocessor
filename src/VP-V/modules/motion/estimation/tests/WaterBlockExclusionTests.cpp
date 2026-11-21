// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/global_model/DynamicTexture.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>

#include <cmath>
#include <cstdint>
#include <cstring>
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
			// Bottom 60% of the frame = choppy chaotic water ripples; top 40% = true rigid camera pan (8, -3).
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

			Assert::IsTrue(last.valid && last.confidence >= 0.5, L"true camera pan recovered after water exclusion");
			Assert::IsTrue(last.signals.waterExcludedShare >= 0.45, L"water blocks excluded from inlier set");
			Assert::AreEqual(8.0, last.params.h[2], 0.25, L"pan X accurately recovered");
			Assert::AreEqual(-3.0, last.params.h[5], 0.25, L"pan Y accurately recovered");
			Assert::IsNotNull(last.blocks.get());
			int waterExcluded = 0, waterTotal = 0;
			for (int by = waterStartRow + 1; by < g.gh - 1; ++by)
				for (int bx = 1; bx < g.gw - 1; ++bx)
				{
					const size_t i = size_t(by) * g.gw + bx;
					++waterTotal;
					if (last.blocks->excluded[i] != 0)
						++waterExcluded;
				}
			Assert::IsTrue(waterExcluded * 100 >= waterTotal * 85, L"at least 85% of interior water blocks marked excluded");
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
			Assert::AreEqual(6.0, last.params.h[2], 0.3, L"camera pan X recovered instead of boat or water");
			Assert::AreEqual(1.0, last.params.h[5], 0.3, L"camera pan Y recovered");
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
	};
}
