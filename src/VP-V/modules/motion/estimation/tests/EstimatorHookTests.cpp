// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>

namespace VideoProcessorTest
{
	TEST_CLASS(EstimatorHookTests)
	{
	public:


		// D5-1 (audit delta5-beta.md): the sigma-delta's domain from the
		// target; float / 16-bit targets are not a code domain it serves.
		// Planner 2026-10-16 evening: regressions for the two optical-flow fixes. (1) The
		// engine's textures were freed before it unregistered them, a fail-fast (0xC0000409)
		// on NVIDIA at teardown and on a size change - here: two sizes, back, then destroy.
		// (2) The replace path ran the backward block search over the engine's flow (an
		// operator-precedence bug) - here: with the engine replacing, or seeding, the searches
		// no block-search pass runs, and the engine's backward vectors are the ones kept.
		TEST_METHOD(OpticalFlowEngineSurvivesResizeAndTeardownAndReplacesTheSearches)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			static constexpr int dx = 12, dy = 6;
			auto frames = [&](int width, int height, DejudderGpuPlanes& pa, DejudderGpuPlanes& pb) {
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
					[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
				UploadDejudderFrame(gpu, a, pa);
				UploadDejudderFrame(gpu, b, pb);
			};
			DejudderGpuPlanes a1, b1, a2, b2;
			frames(512, 256, a1, b1);
			frames(768, 384, a2, b2);
			bool engine = false;
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = true;
				settings.opticalFlowSeed = true;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				engine = warp.OpticalFlowUsed();
				if (!engine)
				{
					Logger::WriteMessage(("optical flow engine unavailable here (" + warp.OpticalFlowError() + "); regressions not run\n").c_str());
					return;
				}
				Assert::IsTrue(warp.Estimate(a2.planes, b2.planes), L"the engine across a size change");
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), L"and back");
				Assert::IsTrue(warp.OpticalFlowUsed(), L"the engine still in use after the size changes");
			}   // destroyed here: the engine first (a fail-fast would end the test process)
			Logger::WriteMessage("teardown after two size changes: survived\n");
			for (const bool seed : { false, true })
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = seed;   // false: the engine replaces the searches
				settings.opticalFlowSeed = seed;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				warp.SetPassTiming(true, true);
				Assert::IsTrue(warp.Estimate(a1.planes, b1.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				Assert::IsTrue(warp.OpticalFlowUsed());
				std::string passes;
				bool searched = false;
				for (const auto& entry : warp.PassMs())
				{
					passes += entry.first + "; ";
					searched = searched || entry.first.find("block search") != std::string::npos;
				}
				Assert::IsFalse(searched, (std::wstring(seed ? L"seed" : L"replace") + L": no block search runs: " +
					std::wstring(passes.begin(), passes.end())).c_str());
				// The backward field is the engine's pan, (-6, -3) half-size pixels.
				const DejudderField bwd = DownloadField(gpu, warp, warp.Backward());
				int interior = 0, right = 0;
				for (int by = 2; by < bwd.blocksY - 2; ++by)
					for (int bx = 2; bx < bwd.blocksX - 2; ++bx)
					{
						++interior;
						right += std::abs(bwd.vx(bx, by) + 6) < 0.6f && std::abs(bwd.vy(bx, by) + 3) < 0.6f ? 1 : 0;
					}
				Assert::IsTrue(right * 100 >= interior * 90, (std::wstring(seed ? L"seed" : L"replace") + L": the backward field holds the pan (" +
					std::to_wstring(right) + L" of " + std::to_wstring(interior) + L")").c_str());
			}
		}

		// Planner Q1 (2026-10-16): the whole-frame motion (D1) from the global search and from the
		// engine's field, against the true pan, and whether each is marked usable (z < 0.9) -
		// the taste's roll-off and the camera read it.
		TEST_METHOD(EngineGlobalMatchesTheSearchOnKnownPans)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			auto readGlobal = [&](const Dejudder::MotionWarp& warp, float out[4]) {
				pl_tex t = warp.GlobalVector();
				Assert::IsNotNull(t);
				const size_t texel = t->params.format->texel_size;
				std::vector<uint8_t> raw(texel);
				pl_tex_transfer_params d{}; d.tex = t; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"the global vector is not host-readable");
				for (int k = 0; k < 4; ++k)
				{
					if (texel == 16) { float f; std::memcpy(&f, &raw[static_cast<size_t>(k) * 4], 4); out[k] = f; }
					else { uint16_t h; std::memcpy(&h, &raw[static_cast<size_t>(k) * 2], 2); out[k] = HalfBits(h); }
				}
			};
			int worse = 0;
			bool engine = true;
			for (const int dx : { 6, 12, 20, 32 })
			{
				const int width = 768, height = 384;
				const DejudderFrame a = MakeDejudderFrame(width, height,
					[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
				const DejudderFrame b = MakeDejudderFrame(width, height,
					[dx](int x, int y) { return DejudderTexture(x - dx, y); },
					[dx](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
				DejudderGpuPlanes pa, pb;
				UploadDejudderFrame(gpu, a, pa);
				UploadDejudderFrame(gpu, b, pb);
				float g[2][4] = {};
				for (int mode = 0; mode < 2; ++mode)
				{
					Dejudder::Settings settings = DejudderTestSettings();
					settings.opticalFlow = true;
					settings.opticalFlowCandidate = true;
					settings.opticalFlowSeed = true;
					settings.globalPrior = true;   // D1 on, as live
					settings.estCandidates = true;   // the two sweeps the seed needs
					settings.globalTwo = true;
					settings.opticalFlowGlobal = mode == 1;
					settings.opticalFlowGlobalReadback = mode == 1;
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
					Assert::IsTrue(warp.Estimate(pa.planes, pb.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
					if (!warp.OpticalFlowUsed()) { engine = false; break; }
					if (mode == 1) Assert::IsTrue(warp.GlobalFromEngine(), L"the engine's global was used");
					readGlobal(warp, g[mode]);
				}
				if (!engine) { Logger::WriteMessage("optical flow engine unavailable: not run\n"); return; }
				char line[240];
				snprintf(line, sizeof(line), "pan %2d px: search (%.2f, %.2f) px z %.2f; engine (%.2f, %.2f) px z %.2f\n",
					dx, g[0][0] * 8.0f, g[0][1] * 8.0f, g[0][2], g[1][0] * 8.0f, g[1][1] * 8.0f, g[1][2]);
				Logger::WriteMessage(line);
				const float errSearch = std::fabs(g[0][0] * 8.0f - dx), errEngine = std::fabs(g[1][0] * 8.0f - dx);
				if (errEngine > errSearch + 1.0f || (g[1][2] >= 0.9f && g[0][2] < 0.9f)) ++worse;
			}
			Assert::AreEqual(0, worse, L"the engine's whole-frame motion is as close to the true pan and as usable as the search's");
		}

		TEST_METHOD(EngineCostLowersTrustAndSwitchOffIsIdentical)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 768, height = 384;
			static constexpr int dx = 8;   // source px; 4 half-size px
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
			// Unsure cells 40-55 x 16-31 = blocks 20-27 x 8-15.
			enum Mode { Off, OnSure, OnUnsure, OnNoPair };
			auto run = [&](Mode mode, bool& used, bool& weighted) {
				Dejudder::Settings settings = DejudderTestSettings();
				settings.opticalFlow = true;
				settings.opticalFlowCandidate = true;
				settings.opticalFlowSeed = true;
				settings.estCandidates = true;
				settings.globalPrior = true;
				settings.opticalFlowCostTrust = mode != Off;
				settings.opticalFlowFbCheck = mode != Off;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				if (mode != OnNoPair)
					warp.SetExternalFlow(SyntheticAheadFlow(width, height, dx / 2, mode == OnUnsure ? 250 : 10, 40, 16, 56, 32));
				Assert::IsTrue(warp.Estimate(pa.planes, pb.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
				used = warp.ExternalFlowUsed();
				weighted = warp.EngineWeightsUsed();
				return DownloadField(gpu, warp);
			};
			bool used[4] = {}, weighted[4] = {};
			const DejudderField off = run(Off, used[0], weighted[0]);
			if (!used[0]) { Logger::WriteMessage("the pair computed ahead was not taken here (no D3D11 engine textures): not run\n"); return; }
			const DejudderField sure = run(OnSure, used[1], weighted[1]);
			const DejudderField unsure = run(OnUnsure, used[2], weighted[2]);
			const DejudderField noPair = run(OnNoPair, used[3], weighted[3]);
			Assert::IsFalse(weighted[0], L"switched off: no weights");
			Assert::IsTrue(weighted[1] && weighted[2], L"switched on with a pair ahead: weights applied");
			Assert::IsFalse(weighted[3], L"no pair ahead: no weights");
			// Every block as sure as usual: exactly the field without the extras.
			Assert::IsTrue(off.texels.size() == sure.texels.size());
			size_t differ = 0;
			for (size_t i = 0; i < off.texels.size(); ++i) differ += off.texels[i] != sure.texels[i] ? 1 : 0;
			Assert::AreEqual(static_cast<size_t>(0), differ, L"weights of 1 change nothing");
			// Unsure blocks (the region's interior; its rim may be re-trusted by the smoothing's
			// neighbour rule) are not trusted; the rest of the picture is unchanged.
			int inside = 0, insideTrustedOff = 0, insideTrusted = 0, outsideChanged = 0;
			for (int by = 0; by < off.blocksY; ++by)
				for (int bx = 0; bx < off.blocksX; ++bx)
				{
					const bool interior = bx >= 21 && bx < 27 && by >= 9 && by < 15;
					const bool nearRegion = bx >= 18 && bx < 30 && by >= 6 && by < 18;
					if (interior)
					{
						++inside;
						insideTrustedOff += off.trusted(bx, by) ? 1 : 0;
						insideTrusted += unsure.trusted(bx, by) ? 1 : 0;
					}
					else if (!nearRegion && off.trusted(bx, by) != unsure.trusted(bx, by))
						++outsideChanged;
				}
			char line[200];
			snprintf(line, sizeof(line), "unsure region: trusted %d / %d without the cost, %d / %d with it; changed elsewhere %d\n",
				insideTrustedOff, inside, insideTrusted, inside, outsideChanged);
			Logger::WriteMessage(line);
			Assert::IsTrue(insideTrustedOff >= inside * 9 / 10, L"the true pan is trusted without the cost");
			Assert::AreEqual(0, insideTrusted, L"the engine's unsure blocks are not trusted");
			Assert::AreEqual(0, outsideChanged, L"elsewhere unchanged");
			// No pair ahead (on WARP the engine is missing: the searches): the same as switched off
			// with the searches - compared against a matcher-only run.
			Dejudder::Settings matcher = DejudderTestSettings();
			matcher.opticalFlow = true;
			matcher.opticalFlowCandidate = true;
			matcher.opticalFlowSeed = true;
			matcher.estCandidates = true;
			matcher.globalPrior = true;
			matcher.opticalFlowCostTrust = false;
			matcher.opticalFlowFbCheck = false;
			Dejudder::MotionWarp reference(fixture.Log(), gpu, matcher);
			Assert::IsTrue(reference.Estimate(pa.planes, pb.planes));
			const DejudderField referenceField = DownloadField(gpu, reference);
			if (!reference.OpticalFlowUsed())
			{
				differ = 0;
				for (size_t i = 0; i < referenceField.texels.size(); ++i) differ += referenceField.texels[i] != noPair.texels[i] ? 1 : 0;
				Assert::AreEqual(static_cast<size_t>(0), differ, L"no pair ahead: exactly as without the switches");
			}
			else
				Logger::WriteMessage("the engine ran at render time here (no weights either way)\n");
		}

		TEST_METHOD(EngineGlobalFallsBackToTheSearchWhenUnclear)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 768, height = 384;
			static constexpr int dx = 8;
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y, 7 + c); });
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
			for (int clear = 0; clear < 2; ++clear)
				for (int fallback = 0; fallback < 2; ++fallback)
				{
					Dejudder::Settings settings = DejudderTestSettings();
					settings.opticalFlow = true;
					settings.opticalFlowCandidate = true;
					settings.opticalFlowSeed = true;
					settings.estCandidates = true;
					settings.globalPrior = true;
					settings.opticalFlowGlobal = true;
					settings.opticalFlowGlobalFallback = fallback == 1;
					Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
					auto flow = SyntheticAheadFlow(width, height, dx / 2, 10, 0, 0, 0, 0);
					if (!clear)
						for (int d = 0; d < 2; ++d) flow->global[d][2] = 1.0f;   // not clear (too few cells agree)
					warp.SetExternalFlow(flow);
					Assert::IsTrue(warp.Estimate(pa.planes, pb.planes), std::wstring(warp.LastError().begin(), warp.LastError().end()).c_str());
					if (!warp.ExternalFlowUsed()) { Logger::WriteMessage("the pair computed ahead was not taken here: not run\n"); return; }
					const bool expectEngine = clear || !fallback;
					Assert::AreEqual(expectEngine, warp.GlobalFromEngine(), clear ? L"clear: the engine's camera" :
						fallback ? L"unclear: the global search" : L"fallback off: the engine's value as before");
					Assert::AreEqual(expectEngine ? 0ull : 1ull, warp.GlobalFallbacks());
				}
		}

		TEST_METHOD(EstimatorNeighbourCandidatesBeatATextureOutlier)
		{
			// E1: block (5, 3)'s true landing in B carries noise (still a good
			// match), and an exact copy of its window sits 44 px further. With a
			// first sweep that found the copy, the second sweep keeps the copy at
			// lambda 0 (lower SAD) and takes the neighbours' pan at lambda 0.5.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, bx = 5, by = 3, wx = 44;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			// The block's window (16 + 2 x 4 source px) at its true landing: noise.
			for (int y = by * 16 - 4 + dy; y < by * 16 + 20 + dy; ++y)
				for (int x = bx * 16 - 4 + dx; x < bx * 16 + 20 + dx; ++x)
					b.luma[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(
						b.luma[static_cast<size_t>(y) * width + x] + NoiseCode(x, y, 3, 512));
			// An exact copy of A's window, 44 px right of the true landing.
			for (int y = by * 16 - 4; y < by * 16 + 20; ++y)
				for (int x = bx * 16 - 4; x < bx * 16 + 20; ++x)
					b.luma[static_cast<size_t>(y + dy) * width + x + dx + wx] = a.luma[static_cast<size_t>(y) * width + x];
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
			DejudderField first = DownloadField(gpu, warp, warp.Forward());
			Assert::IsTrue(std::fabs(first.vx(bx - 1, by) - dx * 0.5f) < 0.6f && std::fabs(first.vx(bx + 1, by) - dx * 0.5f) < 0.6f,
				L"the neighbours should have found the pan");
			// The first sweep as if it had found the copy.
			float* t = &first.texels[(by * first.blocksX + bx) * 4];
			t[0] = (dx + wx) * 0.5f;
			t[1] = dy * 0.5f;
			pl_tex sweep = UploadVectors(gpu, first.texels, first.blocksX, first.blocksY);
			Assert::IsNotNull(sweep);
			auto second = [&](float lambda)
			{
				Dejudder::Settings s2 = settings;
				s2.estLambda = lambda;
				warp.SetSettings(s2);
				Assert::IsTrue(warp.SecondSweep(sweep), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				const DejudderField f = DownloadField(gpu, warp, warp.Forward());
				return std::make_pair(f.vx(bx, by) * 2.0f, f.vy(bx, by) * 2.0f);   // source pixels
			};
			const auto plain = second(0.0f);
			const auto penalised = second(0.5f);
			pl_tex_destroy(gpu, &sweep);
			const std::wstring message = L"lambda 0: (" + std::to_wstring(plain.first) + L", " + std::to_wstring(plain.second) +
				L"); lambda 0.5: (" + std::to_wstring(penalised.first) + L", " + std::to_wstring(penalised.second) + L"); log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(std::fabs(plain.first - (dx + wx)) <= 1.0f, (L"without the penalty the outlier wins; " + message).c_str());
			Assert::IsTrue(std::fabs(penalised.first - dx) <= 1.0f && std::fabs(penalised.second - dy) <= 1.0f,
				(L"with the penalty the neighbours' pan wins; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorQuarterPelRefinesASubPixelPan)
		{
			// E3: a pan of 1.5 source px per frame (B linearly resampled). The
			// half-pel search lands on whole source pixels (1 or 2: 0.5 off);
			// the quarter-pel step reaches 1.5. Both warps then move the picture
			// closer to B at shift 1.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			// A smooth, aperiodic texture (several incommensurate periods),
			// shifted analytically: sub-pixel offsets are then exact.
			auto smooth = [](double x, double y, double phase)
			{
				const double k = 6.283185307179586;
				// Gentle enough (a few codes per pixel) that the half-pel result
				// is trusted, so the quarter-pel step applies.
				return static_cast<uint16_t>(std::lround(30000.0 + 1800.0 * std::sin(k * x / 13.0 + phase) +
					1500.0 * std::sin(k * (0.7 * x + y) / 23.0 + 2.0 * phase) + 1500.0 * std::sin(k * (1.3 * y - 0.4 * x) / 37.0) +
					1000.0 * std::sin(k * y / 11.0 + 1.0)));
			};
			const DejudderFrame a = MakeDejudderFrame(width, height, [&](int x, int y) { return smooth(x, y, 0.0); },
				[&](int x, int y, int c) { return smooth(x * 2.0, y, 1.0 + c); });
			const DejudderFrame b = MakeDejudderFrame(width, height, [&](int x, int y) { return smooth(x - 1.5, y, 0.0); },
				[&](int x, int y, int c) { return smooth(x * 2.0 - 1.5, y, 1.0 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			std::string lastVectors;
			auto run = [&](bool qpel, double& vectorError, double& sourceError, double& renderedError)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.estQpel = qpel;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes));
				const DejudderField f = DownloadField(gpu, warp);
				double sum = 0.0;
				int n = 0;
				for (int y = 2; y < f.blocksY - 2; ++y)
					for (int x = 2; x < f.blocksX - 2; ++x)
						if (f.trusted(x, y)) { sum += std::fabs(f.vx(x, y) - 1.5) + std::fabs(f.vy(x, y)); ++n; }
				vectorError = n ? sum / n : 1e9;
				std::string vs;
				for (int x = 4; x < 9; ++x)
					vs += std::to_string(f.vx(x, 3)) + "/" + std::to_string(f.trusted(x, 3)) + " ";
				Logger::WriteMessage(vs.c_str());
				lastVectors = vs;
				const BothPaths warped = WarpBothPaths(fixture, warp, a, planesA.planes, 1.0f);
				auto meanAbs = [&](const std::vector<float>& image)
				{
					double e = 0.0;
					int m = 0;
					for (int y = 24; y < height - 24; ++y)
						for (int x = 24; x < width - 24; ++x)
						{
							e += std::fabs(image[static_cast<size_t>(y) * width + x] - b.luma[static_cast<size_t>(y) * width + x]);
							++m;
						}
					return e / m;
				};
				sourceError = meanAbs(warped.source);
				renderedError = meanAbs(warped.rendered);
			};
			double vOff = 0, sOff = 0, rOff = 0, vOn = 0, sOn = 0, rOn = 0;
			run(false, vOff, sOff, rOff);
			const std::string vectorsOff = lastVectors;
			run(true, vOn, sOn, rOn);
			const std::string vectorsOn = lastVectors;
			const std::wstring message = L"vector error (source px) " + std::to_wstring(vOff) + L" -> " + std::to_wstring(vOn) +
				L"; warp error vs B, source path " + std::to_wstring(sOff) + L" -> " + std::to_wstring(sOn) +
				L", rendered path " + std::to_wstring(rOff) + L" -> " + std::to_wstring(rOn) + L" (16-bit codes); vectors off " +
				ExtendedRangeGpuProbe::ToWide(vectorsOff) + L" on " + ExtendedRangeGpuProbe::ToWide(vectorsOn) + L"; log: " +
				std::wstring(log.begin(), log.end());
			Assert::IsTrue(vOff >= 0.35 && vOn <= 0.25, message.c_str());
			Assert::IsTrue(sOn < sOff && rOn < rOff, message.c_str());
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorGateKeepsTheFieldOnStillScenes)
		{
			// E4 (with D1): a still pair after a still pair keeps the previous
			// frame's searches (its SADs come back unchanged on new content); a
			// 3 px pan and a cut search again; off searches every time.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			auto still = [&](unsigned salt, unsigned noiseSeed, int shift = 0)
			{
				return MakeDejudderFrame(width, height,
					[=](int x, int y) { return static_cast<uint16_t>(DejudderTexture(x - shift, y, salt) + NoiseCode(x, y, noiseSeed, 256)); },
					[=](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c + salt); });
			};
			const DejudderFrame a0 = still(0, 0), a1 = still(0, 1), c0 = still(5, 0), c1 = still(5, 2), c2 = still(5, 0, 3),
				d0 = still(99, 0);
			DejudderGpuPlanes pa0, pa1, pc0, pc1, pc2, pd0;
			UploadDejudderFrame(gpu, a0, pa0);
			UploadDejudderFrame(gpu, a1, pa1);
			UploadDejudderFrame(gpu, c0, pc0);
			UploadDejudderFrame(gpu, c1, pc1);
			UploadDejudderFrame(gpu, c2, pc2);
			UploadDejudderFrame(gpu, d0, pd0);
			auto sads = [&](Dejudder::MotionWarp& warp)
			{
				const DejudderField f = DownloadField(gpu, warp, warp.Forward());
				std::vector<float> z;
				for (int i = 0; i < f.blocksX * f.blocksY; ++i)
					z.push_back(f.texels[i * 4 + 2]);
				return z;
			};
			for (bool gate : { true, false })
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.globalPrior = true;
				settings.estGate = gate;
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(pa0.planes, pa1.planes));
				const std::vector<float> first = sads(warp);
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				const std::vector<float> second = sads(warp);
				const bool kept = first == second;
				Assert::AreEqual(gate, kept, gate ? L"the gate should keep the previous searches on a still scene" :
					L"without the gate the still scene is searched again");
				if (!gate)
					continue;
				// A 3 px pan resumes the search.
				Assert::IsTrue(warp.Estimate(pc0.planes, pc2.planes));
				const DejudderField pan = DownloadField(gpu, warp, warp.Forward());
				int right = 0, total = 0;
				for (int y = 2; y < pan.blocksY - 2; ++y)
					for (int x = 2; x < pan.blocksX - 2; ++x)
					{
						++total;
						right += std::fabs(pan.vx(x, y) - 1.5f) <= 0.5f ? 1 : 0;   // half-resolution pixels
					}
				Assert::IsTrue(right * 100 >= total * 90, (L"the 3 px pan should be searched: " + std::to_wstring(right) +
					L" of " + std::to_wstring(total)).c_str());
				// Still again (arms the gate: the pan's mean |v| is above the
				// threshold, so this one searches), then a cut searches too.
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				Assert::IsTrue(warp.Estimate(pc0.planes, pc1.planes));
				const std::vector<float> armed = sads(warp);
				Assert::IsTrue(warp.Estimate(pc0.planes, pd0.planes));
				Assert::IsFalse(sads(warp) == armed, L"a cut should be searched");
			}
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorSplitOffStateLeavesEveryConsumerAlone)
		{
			// E2 off-state (planner 2026-10-13): with a threshold nothing reaches,
			// the split on leaves every consumer of the field reading the parent
			// exactly as with it off: the field, the occlusion mask, the grain
			// error and both warps, byte for byte; off allocates no sub-field.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6;
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height, [](int x, int y) { return DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			struct Seen { std::vector<float> field, occlusion, grain; BothPaths warped; };
			auto run = [&](bool split, bool& subFieldPresent)
			{
				Dejudder::Settings settings = DejudderTestSettings();
				settings.estSplit = split;
				settings.estSplitSad = 1e9f;   // nothing splits (the edge blocks of a pan would)
				Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, 0, true), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
				subFieldPresent = warp.SubField() != nullptr;
				Seen seen;
				seen.field = DownloadField(gpu, warp).texels;
				seen.occlusion = DownloadField(gpu, warp, warp.Occlusion(0)).texels;
				seen.grain = DownloadField(gpu, warp, warp.GrainError()).texels;
				seen.warped = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
				return seen;
			};
			bool offPresent = true, onPresent = false;
			const Seen off = run(false, offPresent), on = run(true, onPresent);
			Assert::IsFalse(offPresent, L"split off: no sub-field");
			Assert::IsTrue(onPresent, L"split on: a sub-field");
			Assert::IsTrue(off.field == on.field, L"field");
			Assert::IsTrue(off.occlusion == on.occlusion, L"occlusion mask");
			Assert::IsTrue(off.grain == on.grain, L"grain error");
			Assert::IsTrue(off.warped.source == on.warped.source, L"source warp");
			Assert::IsTrue(off.warped.rendered == on.warped.rendered, L"rendered warp");
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(EstimatorSplitFollowsASmallObject)
		{
			// E2: an 8x8 object moving (14, 6) inside a pan of (12, 6) fills one
			// quarter of block (6, 3). The block matches poorly as a whole; split,
			// the object's quarter takes (14, 6), trusted, and the others the
			// pan; both warps then move the object by half its motion.
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 256, height = 128;
			static constexpr int dx = 12, dy = 6, ox = 104, oy = 56, odx = 14;
			auto inA = [](int x, int y) { return x >= ox && x < ox + 8 && y >= oy && y < oy + 8; };
			auto inB = [](int x, int y) { return x >= ox + odx && x < ox + odx + 8 && y >= oy + dy && y < oy + dy + 8; };
			// Per-pixel detail: a 2 px shift of the object does not match itself.
			auto object = [](int x, int y) { return static_cast<uint16_t>(32000 + NoiseCode(x, y, 77, 24000)); };
			auto chroma = [](int x, int y, int c) { return DejudderTexture(x * 2, y, 7 + c); };
			const DejudderFrame a = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inA(x, y) ? object(x - ox, y - oy) : DejudderTexture(x, y); }, chroma);
			const DejudderFrame b = MakeDejudderFrame(width, height,
				[&](int x, int y) { return inB(x, y) ? object(x - ox - odx, y - oy - dy) : DejudderTexture(x - dx, y - dy); },
				[](int x, int y, int c) { return DejudderTexture((x - dx / 2) * 2, y - dy, 7 + c); });
			DejudderGpuPlanes planesA, planesB;
			UploadDejudderFrame(gpu, a, planesA);
			UploadDejudderFrame(gpu, b, planesB);
			Dejudder::Settings settings = DejudderTestSettings();
			settings.estSplit = true;
			settings.estSplitSad = 1.0f;   // the pan matches at ~0; the object's block does not
			Dejudder::MotionWarp warp(fixture.Log(), gpu, settings);
			Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes), ExtendedRangeGpuProbe::ToWide(warp.LastError()).c_str());
			// The sub-field: two texels per block each way.
			DejudderField sub;
			{
				Assert::IsNotNull(warp.SubField());
				const int sw = warp.SubField()->params.w, sh = warp.SubField()->params.h;
				std::vector<uint16_t> half(static_cast<size_t>(sw) * sh * 4);
				pl_tex_transfer_params d{};
				d.tex = warp.SubField();
				d.ptr = half.data();
				Assert::IsTrue(pl_tex_download(gpu, &d), L"the sub-field is not host-readable");
				sub.blocksX = sw;
				sub.blocksY = sh;
				for (uint16_t h : half)
					sub.texels.push_back(HalfBits(h));
			}
			auto describe = [&](int sx, int sy)
			{
				const float* t = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx) * 4];
				return L"(" + std::to_wstring(t[0]) + L", " + std::to_wstring(t[1]) + L") trust " + std::to_wstring(t[2]) +
					L" split " + std::to_wstring(t[3]);
			};
			const int sx = ox / 8, sy = oy / 8;   // the object's sub-block
			const float* objectSub = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx) * 4];
			const float* panSub = &sub.texels[(static_cast<size_t>(sy) * sub.blocksX + sx - 1) * 4];
			const std::wstring message = L"object sub-block " + describe(sx, sy) + L"; its neighbour " + describe(sx - 1, sy) +
				L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(objectSub[3] > 0.5f && objectSub[2] > 0.5f && std::fabs(objectSub[0] - odx) <= 0.6f &&
				std::fabs(objectSub[1] - dy) <= 0.6f, (L"the object's sub-block follows the object; " + message).c_str());
			Assert::IsTrue(panSub[3] > 0.5f && std::fabs(panSub[0] - dx) <= 0.6f, (L"the surround follows the pan; " + message).c_str());
			// Both warps (backward: each output pixel takes A at its own position
			// minus its own vector) use the object's vector inside its sub-block:
			// there the split output is closer to A moved by (7, 3) than the
			// output without the split.
			const BothPaths split = WarpBothPaths(fixture, warp, a, planesA.planes, 0.5f);
			Dejudder::Settings plainSettings = DejudderTestSettings();
			Dejudder::MotionWarp plain(fixture.Log(), gpu, plainSettings);
			Assert::IsTrue(plain.Estimate(planesA.planes, planesB.planes));
			const BothPaths unsplit = WarpBothPaths(fixture, plain, a, planesA.planes, 0.5f);
			auto errorIn = [&](const std::vector<float>& image)
			{
				double e = 0.0;
				int n = 0;
				for (int y = oy + 1; y < oy + 7; ++y)
					for (int x = ox + 1; x < ox + 7; ++x)
					{
						e += std::fabs(image[static_cast<size_t>(y) * width + x] -
							static_cast<float>(a.luma[static_cast<size_t>(y - 3) * width + x - 7]));
						++n;
					}
				return e / n;
			};
			const double sSplit = errorIn(split.source), sPlain = errorIn(unsplit.source);
			const double rSplit = errorIn(split.rendered), rPlain = errorIn(unsplit.rendered);
			Assert::IsTrue(sSplit < sPlain && rSplit < rPlain, (L"inside the object's sub-block, error vs A moved by (7, 3): source " +
				std::to_wstring(sPlain) + L" -> " + std::to_wstring(sSplit) + L", rendered " + std::to_wstring(rPlain) + L" -> " +
				std::to_wstring(rSplit) + L"; " + message).c_str());
			AssertCleanLibplaceboLog(log);
		}
	};
}
