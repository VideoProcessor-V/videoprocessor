// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include "SyntheticCameraFields.h"
#include "CppUnitTest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	using namespace CameraModel;
	using namespace CameraSynth;

	namespace
	{
		const int kIterations = 15;
		const double kPairSeconds = 1.0 / 24;

		struct Stats
		{
			double median = 0, minimum = 0, maximum = 0;
		};

		Stats Summarise(std::vector<double> ms)
		{
			std::sort(ms.begin(), ms.end());
			Stats s;
			s.minimum = ms.front();
			s.maximum = ms.back();
			s.median = ms[ms.size() / 2];
			return s;
		}

		void Report(const char* label, const GridSpec& g, const Stats& s)
		{
			char line[256];
			snprintf(line, sizeof(line), "CAMERA_COST %s grid=%dx%d median_ms=%.3f min_ms=%.3f max_ms=%.3f n=%d",
				label, g.w, g.h, s.median, s.minimum, s.maximum, kIterations);
			Logger::WriteMessage(line);
		}

		void Require(bool ok, const std::string& what)
		{
			if (ok) return;
			Logger::WriteMessage(what.c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}

		double MsSince(std::chrono::steady_clock::time_point t0)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		}

		// Typical: a primed Fitter, each timed Fit the next pair of a slowly varying pan (H0 early exit).
		Stats Typical(const GridSpec& g)
		{
			std::vector<std::unique_ptr<Field>> fields;
			for (int i = 0; i < kIterations + 3; ++i)
			{
				fields.emplace_back(new Field(g));
				const double s = g.blockSize;
				fields.back()->FromModel(PanModel(0.77 * s + 0.01 * s * i, -0.29 * s + 0.005 * s * i), true);
				fields.back()->AddNoise(0.05, 200 + i);
			}
			Fitter fitter;
			for (int i = 0; i < 3; ++i) fitter.Fit(fields[i]->Input(kPairSeconds, 10 + i));
			std::vector<double> ms;
			for (int i = 0; i < kIterations; ++i)
			{
				const PairInput in = fields[3 + i]->Input(kPairSeconds, 13 + i);
				const auto t0 = std::chrono::steady_clock::now();
				const CameraMotion r = fitter.Fit(in);
				ms.push_back(MsSince(t0));
				Require(r.valid, "typical fit invalid: " + Describe(r));
			}
			return Summarise(ms);
		}

		// Worst (a): a cut with the next pair queued: full search of the next pair.
		Stats WorstCut(const GridSpec& g)
		{
			std::vector<std::unique_ptr<Field>> cur, nxt;
			const double s = g.blockSize;
			for (int i = 0; i < kIterations; ++i)
			{
				cur.emplace_back(new Field(g));
				cur.back()->FromModel(PanModel(0.77 * s, -0.29 * s));
				nxt.emplace_back(new Field(g));
				nxt.back()->FromModel(SimilarityModel(1.0 + 0.003 * (i % 3), 0.002 * i, -0.6 * s, 0.2 * s));
				nxt.back()->AddNoise(0.05, 300 + i);
			}
			Fitter fitter;
			std::vector<double> ms;
			for (int i = 0; i < kIterations; ++i)
			{
				const PairInput next = nxt[i]->Input(kPairSeconds, 21 + 2 * i);
				PairInput cut = cur[i]->Input(kPairSeconds, 20 + 2 * i);
				cut.cut = true;
				cut.next = &next;
				const auto t0 = std::chrono::steady_clock::now();
				const CameraMotion r = fitter.Fit(cut);
				ms.push_back(MsSince(t0));
				Require(r.valid && r.reason == Reason::PostCutSeeded, "cut fit not seeded: " + Describe(r));
			}
			return Summarise(ms);
		}

		// Worst (b): cold full search, homography allowed, objects on, perspective field with moving patches.
		Stats WorstHomographyObjects(const GridSpec& g)
		{
			std::vector<std::unique_ptr<Field>> fields;
			const double width = g.w * g.blockSize;
			for (int i = 0; i < kIterations; ++i)
			{
				fields.emplace_back(new Field(g));
				Field& f = *fields.back();
				f.FromModel(PerspectiveModel(width, 0.2 * g.blockSize, -0.1 * g.blockSize + 0.01 * i), true);
				f.AddNoise(0.1, 400 + i);
				const int pw = std::max(2, g.w / 8), ph = std::max(2, g.h / 8);
				f.AddObject(g.w / 4, g.h / 4, pw, ph, -1.2 * g.blockSize, 0.9 * g.blockSize);
				f.AddObject(g.w / 2, g.h / 2, pw, ph, 1.5 * g.blockSize, -1.0 * g.blockSize);
				f.AddObject(3 * g.w / 4, g.h / 3, pw, ph, 0.8 * g.blockSize, 1.4 * g.blockSize);
				f.AddObject(g.w / 3, 3 * g.h / 4, pw, ph, -0.9 * g.blockSize, -1.1 * g.blockSize);
			}
			Config cfg;
			cfg.objects = true;
			cfg.maxModel = ModelType::Homography;
			std::vector<double> ms;
			for (int i = 0; i < kIterations; ++i)
			{
				Fitter fitter(cfg);
				const PairInput in = fields[i]->Input(kPairSeconds, 40 + i);
				const auto t0 = std::chrono::steady_clock::now();
				const CameraMotion r = fitter.Fit(in);
				ms.push_back(MsSince(t0));
				Require(r.valid, "homography/objects fit invalid: " + Describe(r));
			}
			return Summarise(ms);
		}
	}

	TEST_CLASS(CameraCostTests)
	{
	public:
		TEST_METHOD(Cost4K)
		{
			const GridSpec g = Grid4K();
			const Stats typical = Typical(g), cut = WorstCut(g), hom = WorstHomographyObjects(g);
			Report("typical_4k", g, typical);
			Report("worst_cut_4k", g, cut);
			Report("worst_homography_objects_4k", g, hom);
			Assert::IsTrue(typical.median <= 20.0, L"typical 4K median over 20 ms");
			Assert::IsTrue(cut.median <= 20.0, L"4K cut median over 20 ms");
			Assert::IsTrue(hom.median <= 20.0, L"4K homography+objects median over 20 ms");
		}

		TEST_METHOD(Cost8K)
		{
			const GridSpec g = Grid8K();
			const Stats typical = Typical(g), cut = WorstCut(g), hom = WorstHomographyObjects(g);
			Report("typical_8k", g, typical);
			Report("worst_cut_8k", g, cut);
			Report("worst_homography_objects_8k", g, hom);
			Assert::IsTrue(typical.median <= 20.0, L"typical 8K median over 20 ms");
			Assert::IsTrue(cut.median <= 20.0, L"8K cut median over 20 ms");
			Assert::IsTrue(hom.median <= 60.0, L"8K homography+objects median over 60 ms");
		}
	};
}
