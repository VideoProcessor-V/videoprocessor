// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: the float CPU mirror of the GPU label pass against the double precision fitter labels,
// and the cpuLabels = false record. CPU only (the shader itself is covered by CameraModelGpuWarpTests.cpp).
#include "pch.h"
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessorGlsl.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelSearchHints.h>
#include "CameraModelGpuTestSupport.h"
#include "CppUnitTest.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	using namespace CameraModel;
	using namespace CameraSynth;
	using namespace CameraGpuSupport;

	TEST_CLASS(CameraModelGpuMirrorTests)
	{
	public:
		TEST_METHOD(MirrorMatchesTheCpuLabelsOnEverySyntheticCase)
		{
			for (const Scenario& s : MakeScenarios())
			{
				const Field& f = *s.field;
				const CameraModel::Config cfg;
				Fitter fitter(cfg);
				const PairInput in = f.Input(PairSeconds(), 5);
				const CameraMotion r = fitter.Fit(in);
				Check(r.valid && r.blocks != nullptr, s.name + ": fit");
				const BlockData& bd = *r.blocks;
				const LabelConstants k = MakeLabelConstants(r, cfg, in);
				std::vector<float> field, aux, out;
				PackLabelInputs(in, bd, field, aux);
				LabelGridMirror(k, field.data(), aux.data(), cfg.overlayZeroClusterMin, out);
				const std::vector<uint8_t> band = NearThresholdBand(k, f, out);
				const size_t n = f.Count();
				int bandBlocks = 0, mismatches = 0, tinyMismatches = 0, overlays = 0, objects = 0, tinies = 0;
				double worstModel = 0;
				for (size_t i = 0; i < n; ++i)
				{
					bandBlocks += band[i];
					overlays += bd.label[i] == static_cast<uint8_t>(Label::Overlay) ? 1 : 0;
					objects += bd.label[i] == static_cast<uint8_t>(Label::Object) ? 1 : 0;
					tinies += bd.tinyMover[i];
					worstModel = std::max(worstModel, std::max(std::fabs(static_cast<double>(out[i * 4]) - bd.modelX[i]),
						std::fabs(static_cast<double>(out[i * 4 + 1]) - bd.modelY[i])));
					if (band[i])
						continue;
					if (CodeLabel(out[i * 4 + 3]) != bd.label[i])
						++mismatches;
					if (CodeTiny(out[i * 4 + 3]) != bd.tinyMover[i])
						++tinyMismatches;
				}
				char msg[256];
				snprintf(msg, sizeof(msg), "CAMERA_LABEL_MIRROR %s blocks=%zu band=%d label_mismatch=%d tiny_mismatch=%d overlay=%d object=%d tiny=%d worst_model_err=%.2e\n",
					s.name.c_str(), n, bandBlocks, mismatches, tinyMismatches, overlays, objects, tinies, worstModel);
				Logger::WriteMessage(msg);
				Check(mismatches == 0, s.name + ": labels differ outside the threshold band: " + std::to_string(mismatches));
				Check(tinyMismatches == 0, s.name + ": tiny flags differ: " + std::to_string(tinyMismatches));
				Check(worstModel <= 1e-3, s.name + ": model vector error " + std::to_string(worstModel));
			}
		}

		TEST_METHOD(AbsoluteResidualMatchesTheStoredResidual)
		{
			// The texture stores |res| = sqrtf(rx*rx + ry*ry), the value the CPU accessor recomputes from resX/resY.
			Field f(Grid4K());
			f.FromModel(PanModel(12.3, -4.7));
			f.AddNoise(0.05, 11);
			f.AddObject(100, 60, 6, 4, -20, 15);
			const CameraModel::Config cfg;
			Fitter fitter(cfg);
			const PairInput in = f.Input(PairSeconds(), 5);
			const CameraMotion r = fitter.Fit(in);
			const BlockData& bd = *r.blocks;
			const LabelConstants k = MakeLabelConstants(r, cfg, in);
			std::vector<float> field, aux, out;
			PackLabelInputs(in, bd, field, aux);
			LabelGridMirror(k, field.data(), aux.data(), cfg.overlayZeroClusterMin, out);
			double worst = 0;
			for (size_t i = 0; i < f.Count(); ++i)
			{
				const float cpu = std::sqrt(bd.resX[i] * bd.resX[i] + bd.resY[i] * bd.resY[i]);
				worst = std::max(worst, std::fabs(static_cast<double>(out[i * 4 + 2]) - cpu));
			}
			Check(worst <= 1e-3, "|res| differs from the stored residual by " + std::to_string(worst));
		}

		TEST_METHOD(CpuLabelsOffLeavesTheArraysEmptyAndTheConsumersFallBack)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(12.3, -4.7));
			f.AddNoise(0.05, 12);
			CameraModel::Config off;
			off.cpuLabels = false;
			Fitter fitterOff(off);
			Fitter fitterOn;
			const PairInput in = f.Input(PairSeconds(), 5);
			const CameraMotion r = fitterOff.Fit(in);
			const CameraMotion ref = fitterOn.Fit(in);
			Check(r.valid && r.blocks != nullptr, "fit with GPU labels");
			Check(r.labelsOnGpu, "labelsOnGpu set");
			Check(!ref.labelsOnGpu, "labelsOnGpu clear by default");
			const BlockData& b = *r.blocks;
			Check(b.gridW == f.gridW && b.gridH == f.gridH && b.blockSize == f.blockSize, "grid fields kept");
			Check(b.label.empty() && b.resX.empty() && b.resY.empty() && b.modelX.empty() && b.modelY.empty() &&
				b.tinyMover.empty() && b.objectId.empty(), "per-block arrays empty");
			Check(b.excluded.size() == f.Count(), "excluded mask kept");
			Check(b.absThresholdPx == ref.blocks->absThresholdPx && b.relThreshold == ref.blocks->relThreshold && b.absThresholdPx > 0.0f,
				"thresholds kept");
			Check(r.confidence == ref.confidence && r.params.type == ref.params.type &&
				std::memcmp(r.params.h, ref.params.h, sizeof(r.params.h)) == 0, "the fit itself is unchanged");

			// Accessor: the caller's value comes back unchanged (and the weight can be full).
			Policy on;
			on.enabled = true;
			BlendState state;
			for (int i = 0; i < 20; ++i)
				state.Update(on, &r);
			Check(state.Weight() == 1.0, "weight reaches 1");
			const float inputs[][2] = { { 1.5f, -2.25f }, { 0.0f, 0.0f }, { 123.456f, 7.0f } };
			for (const auto& v : inputs)
				for (int trusted = 0; trusted < 2; ++trusted)
				{
					float ox = 99, oy = 99;
					const Choice c = FieldVector(on, state, &r, 3, 4, trusted != 0, v[0], v[1], ox, oy);
					Check(std::memcmp(&ox, &v[0], 4) == 0 && std::memcmp(&oy, &v[1], 4) == 0, "accessor returns the input bits");
					Check(c == (trusted ? Choice::Measured : Choice::Hold), "accessor choice");
				}

			// Hints: short label arrays mean a full search everywhere, no narrow window.
			const size_t n = f.Count();
			std::vector<float> px(n, 12.3f), py(n, -4.7f);
			const SearchHints h = BuildHints(HintConfig(), &r, px.data(), py.data(), px.data(), py.data(), false, false, false, 1.0, 0.0, 0.0, 0.0);
			Check(h.hint.size() == n && h.fullBlocks == n && h.narrowBlocks == 0 && h.finerBlocks == 0, "hints: full search everywhere");
			for (size_t i = 0; i < n; ++i)
				if (h.hint[i] != static_cast<uint8_t>(BlockHint::FullSearch))
					Check(false, "hint is not FullSearch");
			// The shader constants for the same record say usable (the GPU holds the labels).
			const AccessorConstants ac = MakeAccessorConstants(on, state, &r);
			Check(ac.enabled == 1 && ac.weight == 1.0f && ac.gridW == f.gridW && ac.gridH == f.gridH && ac.absThr > 0.0f, "accessor constants");
		}

		TEST_METHOD(ObjectsForceCpuLabels)
		{
			Field f(Grid4K());
			f.FromModel(PanModel(12.3, -4.7));
			f.AddNoise(0.05, 13);
			f.AddObject(30, 20, 24, 16, -25, 14);
			CameraModel::Config cfg;
			cfg.cpuLabels = false;
			cfg.objects = true;
			Fitter fitter(cfg);
			const CameraMotion r = fitter.Fit(f.Input(PairSeconds(), 5));
			Check(r.valid && r.blocks != nullptr, "fit");
			Check(!r.labelsOnGpu, "objects force CPU labels");
			Check(r.blocks->label.size() == f.Count() && r.blocks->modelX.size() == f.Count(), "arrays present");
		}

		TEST_METHOD(DefaultConfigKeepsCpuLabels)
		{
			Check(CameraModel::Config().cpuLabels, "cpuLabels defaults to true");
			Check(!CameraMotion().labelsOnGpu, "labelsOnGpu defaults to false");
		}

	private:
		static void Check(bool ok, const std::string& what)
		{
			if (ok)
				return;
			Logger::WriteMessage((what + "\n").c_str());
			Assert::Fail(std::wstring(what.begin(), what.end()).c_str());
		}
	};
}
