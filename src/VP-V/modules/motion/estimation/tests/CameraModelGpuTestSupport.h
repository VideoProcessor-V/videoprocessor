// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Test helper (header-only): the synthetic scenarios shared by the GPU label pass mirror tests (CPU) and the
// WARP test, and the near-threshold band in which float and double arithmetic may legitimately disagree.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraModelGpu.h>
#include "SyntheticCameraFields.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace CameraGpuSupport
	{
		using namespace CameraModel;
		using namespace CameraSynth;

		inline const double PairSeconds() { return 1.0 / 24; }

		struct Scenario
		{
			std::string name;
			std::shared_ptr<Field> field;
		};

		inline Scenario MakeScenario(const char* name, const Params& truth, uint64_t seed)
		{
			Scenario s;
			s.name = name;
			s.field = std::make_shared<Field>(Grid4K());
			s.field->FromModel(truth);
			s.field->AddNoise(0.05, seed);
			return s;
		}

		inline std::vector<Scenario> MakeScenarios()
		{
			std::vector<Scenario> v;
			const Params pan = PanModel(12.3, -4.7);
			v.push_back(MakeScenario("pan", pan, 1));
			v.push_back(MakeScenario("zoom", ZoomModel(1.02), 2));
			v.push_back(MakeScenario("perspective", PerspectiveModel(3840.0, 3.0, -2.0), 3));
			{
				Scenario s = MakeScenario("objects", pan, 4);
				s.field->AddObject(100, 60, 6, 4, -20, 15);
				v.push_back(s);
			}
			{
				Scenario s = MakeScenario("tiny mover", pan, 5);
				s.field->AddTinyObject(50, 40, 1, -90, 60);
				s.field->SetPixelResidual(70, 70, 0.05);
				s.field->SetUntrusted(120, 100, 1, 1, 1.0);
				s.field->SetPixelResidual(120, 100, 0.05);
				v.push_back(s);
			}
			{
				Scenario s = MakeScenario("logo", pan, 6);
				s.field->AddLogo(10, 8, 4, 4, false);
				v.push_back(s);
			}
			{
				Scenario s = MakeScenario("letterbox", pan, 7);
				s.field->AddBars(17, 17);
				v.push_back(s);
			}
			{
				Scenario s = MakeScenario("sky + text + logo", pan, 8);
				s.field->AddObject(100, 60, 6, 4, -20, 15);
				s.field->AddLogo(10, 40, 3, 3, false);
				s.field->AddTextTiles(60, 90, 40, 3, 52);
				s.field->SetUntrusted(0, 0, s.field->gridW, s.field->gridH / 3, 0.05);
				s.field->SetUntrusted(100, 100, 2, 2, 1.0);
				v.push_back(s);
			}
			return v;
		}

		// True where the block, or one of its 4-neighbours, has a decision quantity within 1e-4 (relative) of its
		// threshold: float and double arithmetic may then legitimately differ. `out4` is the mirror's first-pass
		// or final output (only the model vector, channels 0 and 1, is read).
		inline std::vector<uint8_t> NearThresholdBand(const LabelConstants& k, const Field& f, const std::vector<float>& out4)
		{
			const size_t n = static_cast<size_t>(k.gridW) * static_cast<size_t>(k.gridH);
			std::vector<uint8_t> own(n, 0), band(n, 0);
			const double abs2 = static_cast<double>(k.absThr) * k.absThr, rel2 = static_cast<double>(k.rel) * k.rel;
			auto closeTo = [](double a, double b) { return std::fabs(a - b) <= 1e-4 * std::max(std::fabs(b), 1e-12); };
			for (size_t i = 0; i < n; ++i)
			{
				const double mx = out4[i * 4], my = out4[i * 4 + 1];
				const double mm2 = mx * mx + my * my;
				const double thr2 = std::max(abs2, rel2 * mm2);
				const double vx = f.fx[i], vy = f.fy[i];
				const double vm2 = vx * vx + vy * vy;
				const double r2 = (vx - mx) * (vx - mx) + (vy - my) * (vy - my);
				own[i] = (closeTo(r2, thr2) || closeTo(vm2, abs2) || closeTo(mm2, thr2)) ? 1 : 0;
			}
			for (int y = 0; y < k.gridH; ++y)
				for (int x = 0; x < k.gridW; ++x)
				{
					const size_t i = static_cast<size_t>(y) * k.gridW + x;
					bool b = own[i] != 0;
					if (x > 0) b = b || own[i - 1];
					if (x + 1 < k.gridW) b = b || own[i + 1];
					if (y > 0) b = b || own[i - static_cast<size_t>(k.gridW)];
					if (y + 1 < k.gridH) b = b || own[i + static_cast<size_t>(k.gridW)];
					band[i] = b ? 1 : 0;
				}
			return band;
		}
	}
}
